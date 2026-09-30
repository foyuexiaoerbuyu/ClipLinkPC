package com.cliplink.mobile.notification;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

import com.cliplink.mobile.MainActivity;
import com.cliplink.mobile.R;
import com.cliplink.mobile.clipboard.ClipboardWriteResult;

import java.util.LinkedHashMap;

/**
 * 远程剪贴板通知管理器（《PC 远程剪贴板通知》spec §5-§8 / §12 / §14）。
 *
 * <p>职责：
 * <ul>
 *   <li>创建「远程剪贴板」通知渠道（与前台服务常驻通知渠道分开，便于用户单独控制）；</li>
 *   <li>两类通知：自动复制成功（普通通知，正文两行「已复制 / 内容摘要」）与
 *       需要用户复制（正文「PC 剪贴板 / 内容摘要」+「复制」Action）；</li>
 *   <li>通知更新（复制成功后原地升级为"已复制"）、数量上限淘汰最旧、
 *       eventId -> notificationId 的稳定映射（禁止随机值作为唯一依据）。</li>
 * </ul>
 *
 * <p>设计要点：
 * <ul>
 *   <li><b>不依赖 MainActivity</b>：只持有 application context + NotificationManager，
 *       Activity 被关闭（用户在桌面/微信/Chrome/设置等任意界面）时通知照常展示；</li>
 *   <li><b>通知权限不是同步前置条件</b>：无通知权限时仅跳过通知展示并记录日志，
 *       绝不停同步服务 / WebSocket / 无障碍监听（spec §11）；</li>
 *   <li><b>通知不是同步来源</b>：只读展示 PC 下发内容，不写数据库、不生成 eventId、
 *       不发送 WebSocket（spec §12）。</li>
 * </ul>
 */
public final class RemoteClipboardNotificationManager {

    private static final String TAG = "ClipLink";

    /** 远程剪贴板通知渠道 id */
    public static final String CHANNEL_ID = "cliplink_remote_clipboard";

    /** 「复制」Action（点击只复制，不打开主界面，spec §8） */
    public static final String ACTION_COPY_REMOTE =
            "com.cliplink.mobile.action.COPY_REMOTE";
    public static final String EXTRA_EVENT_ID = "eventId";
    public static final String EXTRA_CONTENT = "content";
    public static final String EXTRA_NOTIFICATION_ID = "notificationId";

    /**
     * UNVERIFIED 结果在「自动路径」下的展示策略开关（spec §3）。
     *
     * <p>背景：Android 10+ 后台应用回读剪贴板会被系统伪装成"空剪贴板"，
     * 此时 {@link ClipboardWriteResult#UNVERIFIED} 表示"写入调用已被系统接受，
     * 但无法校验"——既不能证明成功也不能证明失败。
     *
     * <ul>
     *   <li>{@code false}（默认，保守策略）：仍展示带「复制」按钮的通知，
     *       用户可一键再复制一次，绝不会因为误判而让用户拿不到内容；
     *       但日志按 UNVERIFIED 记录，不写成 FAILED；</li>
     *   <li>{@code true}（乐观策略）：直接按"已复制"展示普通通知（无按钮），
     *       通知栏更干净，代价是极少数确实写入失败时会漏掉补救入口。</li>
     * </ul>
     *
     * <p>后续如需一键切换策略，只改这一处常量即可。
     */
    public static final boolean UNVERIFIED_AS_COPIED = false;

    /** 通知 id 基数：与前台服务常驻通知（1001）错开，避免互相覆盖 */
    private static final int NOTIFICATION_ID_BASE = 2000;
    /** eventId -> notificationId 映射空间（同 eventId 稳定、不同 eventId 极低概率冲突） */
    private static final int NOTIFICATION_ID_RANGE = 1000;
    /** 通知数量上限（spec §14：最多保留 5~10 条，这里取 5 条，避免通知栏堆积） */
    private static final int MAX_NOTIFICATIONS = 5;

    /** 摘要行截断长度：折叠态 / 展开态（BigTextStyle） */
    private static final int SUMMARY_SHORT_MAX = 60;
    private static final int SUMMARY_BIG_MAX = 400;

    /**
     * 传给「复制」Action 的正文上限（字符）。
     *
     * <p>Binder 事务有大小限制，超长文本直接塞进 Intent 可能抛
     * TransactionTooLargeException；完整内容同时保存在进程内
     * {@link #contentsByNotificationId} 中，Action 触发时优先取内存版本。
     */
    private static final int EXTRA_CONTENT_MAX = 8000;

    private static volatile RemoteClipboardNotificationManager instance;

    private final Context appContext;
    private final NotificationManager notificationManager;

    /** notificationId -> eventId，按插入顺序记录，用于淘汰最旧通知 */
    private final LinkedHashMap<Integer, String> activeNotifications =
            new LinkedHashMap<>();

    /** notificationId -> 完整正文（「复制」Action 取用；进程被杀后降级用 Intent extra） */
    private final LinkedHashMap<Integer, String> contentsByNotificationId =
            new LinkedHashMap<>();

    private RemoteClipboardNotificationManager(Context context) {
        this.appContext = context.getApplicationContext();
        this.notificationManager = (NotificationManager) appContext
                .getSystemService(Context.NOTIFICATION_SERVICE);
        ensureChannel();
    }

    public static RemoteClipboardNotificationManager getInstance(Context context) {
        RemoteClipboardNotificationManager local = instance;
        if (local == null) {
            synchronized (RemoteClipboardNotificationManager.class) {
                local = instance;
                if (local == null) {
                    local = new RemoteClipboardNotificationManager(context);
                    instance = local;
                }
            }
        }
        return local;
    }

    /**
     * 创建远程剪贴板通知渠道（幂等，重复调用无副作用）。
     *
     * <p>IMPORTANCE_DEFAULT：PC 下发剪贴板需要用户可见（通知栏展示 + 声音提示），
     * 但不使用高优先级横幅，避免打断用户当前操作。
     */
    public void ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O
                || notificationManager == null) {
            return;
        }
        try {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    appContext.getString(R.string.notification_channel_remote_name),
                    NotificationManager.IMPORTANCE_DEFAULT);
            channel.setDescription(
                    appContext.getString(R.string.notification_channel_remote_desc));
            notificationManager.createNotificationChannel(channel);
        } catch (RuntimeException e) {
            Log.w(TAG, "创建远程剪贴板通知渠道失败: " + e.getMessage());
        }
    }

    /**
     * eventId -> 稳定 notificationId（spec §14：禁止用随机值作为唯一依据）。
     *
     * <p>同一 eventId 永远映射到同一 id，因此同一事件重复下发只更新同一条通知；
     * 不同 eventId 在 {@link #NOTIFICATION_ID_RANGE} 空间内散列，冲突概率极低。
     */
    public static int notificationIdFor(String eventId) {
        if (eventId == null || eventId.isEmpty()) {
            return NOTIFICATION_ID_BASE;
        }
        int hash = eventId.hashCode() & 0x7fffffff;
        return NOTIFICATION_ID_BASE + (hash % NOTIFICATION_ID_RANGE);
    }

    /**
     * 远程剪贴板到达：按自动复制结果显示两类通知（spec §1 / §6 / §7）。
     *
     * @param eventId 远程事件 id（仅用于稳定映射通知 id，不参与同步）
     * @param content PC 剪贴板正文
     * @param result  自动写入结果（spec §3 分流）：
     *                <ul>
     *                  <li>SUCCESS -&gt; 「已复制」成功通知（无按钮）；</li>
     *                  <li>FAILED / NOT_ALLOWED -&gt; 带「复制」按钮的通知；</li>
     *                  <li>UNVERIFIED -&gt; 由 {@link #UNVERIFIED_AS_COPIED} 控制
     *                      （默认 false = 保守，仍带「复制」按钮），
     *                      日志按 UNVERIFIED 记录，不得写成 FAILED。</li>
     *                </ul>
     */
    public void notifyRemoteClipboard(String eventId, String content,
                                      ClipboardWriteResult result) {
        if (content == null || content.isEmpty()) {
            return;
        }
        ensureChannel();
        if (!canPostNotifications()) {
            // 通知权限关闭只影响提示，不影响同步与自动复制（spec §11）
            Log.w(TAG, "通知权限/通知开关未开启，跳过远程剪贴板通知"
                    + "（剪贴板同步与自动复制不受影响）");
            return;
        }
        int notificationId = notificationIdFor(eventId);
        ClipboardWriteResult effective = result == null
                ? ClipboardWriteResult.FAILED : result;
        final boolean copied;
        final String label;
        switch (effective) {
            case SUCCESS:
                copied = true;
                label = "自动复制成功";
                break;
            case UNVERIFIED:
                // 后台焦点限制导致无法回读校验：写入调用已被系统接受，
                // 默认保守策略仍给出「复制」入口，但语义必须是 UNVERIFIED 而非 FAILED
                copied = UNVERIFIED_AS_COPIED;
                label = copied
                        ? "自动复制无法校验（后台焦点限制），已按已复制展示(UNVERIFIED)"
                        : "需要用户复制(带复制按钮, UNVERIFIED：后台焦点限制"
                                + "无法回读校验，写入调用已被系统接受，非确定失败)";
                break;
            case NOT_ALLOWED:
                copied = false;
                label = "需要用户复制(带复制按钮, NOT_ALLOWED：系统拒绝程序写入)";
                break;
            case FAILED:
            default:
                copied = false;
                label = "需要用户复制(带复制按钮, FAILED：确定写入失败)";
                break;
        }
        synchronized (activeNotifications) {
            activeNotifications.remove(notificationId);
            activeNotifications.put(notificationId, eventId == null ? "" : eventId);
            contentsByNotificationId.remove(notificationId);
            contentsByNotificationId.put(notificationId, content);
            trimIfNeeded();
        }
        try {
            notificationManager.notify(notificationId,
                    buildNotification(eventId, notificationId, content, copied));
            Log.i(TAG, "远程剪贴板通知已展示: " + label
                    + ", notificationId=" + notificationId
                    + ", result=" + effective
                    + "（写入走 ClipboardHelper 程序写入通道，已置 suppress 防回环）");
        } catch (RuntimeException e) {
            Log.w(TAG, "展示远程剪贴板通知失败（同步不受影响）: " + e.getMessage());
        }
    }

    /**
     * 「复制」成功后把通知原地更新为已复制（移除 Action，spec §8）。
     *
     * <p>由 {@link CopyNotificationReceiver} 在写入系统剪贴板成功后调用。
     */
    public void markCopied(String eventId, int notificationId, String content) {
        if (content == null || content.isEmpty()) {
            return;
        }
        ensureChannel();
        if (!canPostNotifications()) {
            return;
        }
        int id = notificationId > 0 ? notificationId : notificationIdFor(eventId);
        synchronized (activeNotifications) {
            activeNotifications.remove(id);
            activeNotifications.put(id, eventId == null ? "" : eventId);
            contentsByNotificationId.remove(id);
            contentsByNotificationId.put(id, content);
            trimIfNeeded();
        }
        try {
            notificationManager.notify(id,
                    buildNotification(eventId, id, content, true));
            Log.i(TAG, "防回环：通知「复制」完成，通知已更新为已复制"
                    + "（移除复制按钮）, notificationId=" + id);
        } catch (RuntimeException e) {
            Log.w(TAG, "更新远程剪贴板通知失败: " + e.getMessage());
        }
    }

    /** 「复制」Action 取完整正文（内存优先，缺失时由接收方回退 Intent extra） */
    public String getPendingContent(int notificationId) {
        synchronized (activeNotifications) {
            return contentsByNotificationId.get(notificationId);
        }
    }

    // ---- 内部实现 -----------------------------------------------------------

    /**
     * 构建通知：标题统一 ClipLink；正文两行（第一行状态 / 第二行内容摘要）；
     * 展开态用 BigTextStyle 展示更长摘要；未复制时追加「复制」Action。
     */
    private Notification buildNotification(String eventId, int notificationId,
                                           String content, boolean copied) {
        String firstLine = appContext.getString(copied
                ? R.string.notification_copied
                : R.string.notification_pc_clipboard);
        String summary = summarize(content);
        String shortSummary = truncate(summary, SUMMARY_SHORT_MAX);
        String bigSummary = truncate(summary, SUMMARY_BIG_MAX);

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(appContext, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(appContext);
        }
        builder.setSmallIcon(R.drawable.ic_cliplink)
                .setContentTitle(appContext.getString(R.string.notification_title))
                .setContentText(firstLine + "\n" + shortSummary)
                .setStyle(new Notification.BigTextStyle()
                        .bigText(firstLine + "\n" + bigSummary))
                // 点击正文打开主界面；点击「复制」只复制（spec §8）
                .setContentIntent(contentIntent(notificationId))
                .setAutoCancel(true)
                .setOnlyAlertOnce(true);
        if (!copied) {
            builder.addAction(0,
                    appContext.getString(R.string.notification_action_copy),
                    copyActionIntent(eventId, notificationId, content));
        }
        return builder.build();
    }

    /** 点击通知正文 -> 打开 MainActivity（不启动任何同步动作） */
    private PendingIntent contentIntent(int notificationId) {
        Intent intent = new Intent(appContext, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        return PendingIntent.getActivity(appContext, notificationId, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    /** 「复制」Action -> 广播给 CopyNotificationReceiver（不打开 MainActivity） */
    private PendingIntent copyActionIntent(String eventId, int notificationId,
                                           String content) {
        Intent intent = new Intent(appContext, CopyNotificationReceiver.class);
        intent.setAction(ACTION_COPY_REMOTE);
        intent.putExtra(EXTRA_EVENT_ID, eventId);
        intent.putExtra(EXTRA_NOTIFICATION_ID, notificationId);
        intent.putExtra(EXTRA_CONTENT, truncate(content, EXTRA_CONTENT_MAX));
        return PendingIntent.getBroadcast(appContext, notificationId, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    /** 通知数量超上限时淘汰最旧一条（spec §14） */
    private void trimIfNeeded() {
        while (activeNotifications.size() > MAX_NOTIFICATIONS) {
            Integer oldest = null;
            for (Integer key : activeNotifications.keySet()) {
                oldest = key;
                break;
            }
            if (oldest == null) {
                return;
            }
            activeNotifications.remove(oldest);
            contentsByNotificationId.remove(oldest);
            try {
                if (notificationManager != null) {
                    notificationManager.cancel(oldest);
                }
                Log.i(TAG, "远程剪贴板通知超出上限(" + MAX_NOTIFICATIONS
                        + ")，淘汰最旧一条 notificationId=" + oldest);
            } catch (RuntimeException e) {
                Log.w(TAG, "淘汰旧通知失败: " + e.getMessage());
            }
        }
    }

    /**
     * 当前是否允许展示通知（Android 13+ 未授权 POST_NOTIFICATIONS 时返回 false）。
     *
     * <p>返回 false 只代表"不展示通知"，调用方绝不可据此停止同步链路（spec §11）。
     */
    private boolean canPostNotifications() {
        if (notificationManager == null) {
            return false;
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                return notificationManager.areNotificationsEnabled();
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "查询通知开关状态失败: " + e.getMessage());
        }
        return true;
    }

    /** 内容摘要：折叠空白字符，避免多行内容撑坏通知布局 */
    private static String summarize(String content) {
        return content.replaceAll("\\s+", " ").trim();
    }

    private static String truncate(String text, int max) {
        if (text == null || text.length() <= max) {
            return text;
        }
        return text.substring(0, max) + "…";
    }
}
