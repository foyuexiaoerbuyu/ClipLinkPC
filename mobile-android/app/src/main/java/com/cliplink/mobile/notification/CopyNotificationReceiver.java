package com.cliplink.mobile.notification;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import com.cliplink.mobile.R;
import com.cliplink.mobile.clipboard.ClipboardWriteResult;
import com.cliplink.mobile.sync.SyncManager;

/**
 * 远程剪贴板通知「复制」按钮的接收器（《PC 远程剪贴板通知》spec §8-§9）。
 *
 * <p>行为契约：
 * <ul>
 *   <li>action = {@link RemoteClipboardNotificationManager#ACTION_COPY_REMOTE}；</li>
 *   <li>Intent extra 含 eventId / content / notificationId；</li>
 *   <li>只把 content 写入系统剪贴板，来源标记为
 *       {@code ClipboardSource.NOTIFICATION}：不上传服务器、不生成 eventId、
 *       不入库、不广播、不启动 MainActivity；</li>
 *   <li>写入成功后把该通知更新为「已复制」（移除「复制」按钮）。</li>
 * </ul>
 *
 * <p>防回环：写入统一走 SyncManager -> ClipboardHelper 程序写入通道
 * （suppress 标志 + lastProgrammaticClipboardHash 双保护），
 * 因此这次写入不会被前台/无障碍监听当成 USER 事件重新上传。
 *
 * <p>该接收器在 manifest 中 exported=false：只接受本应用（Notification
 * PendingIntent）发来的 Intent，外部应用无法伪造触发。
 */
public class CopyNotificationReceiver extends BroadcastReceiver {

    private static final String TAG = "ClipLink";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !RemoteClipboardNotificationManager.ACTION_COPY_REMOTE
                .equals(intent.getAction())) {
            return;
        }
        if (context == null) {
            return;
        }
        String eventId = intent.getStringExtra(
                RemoteClipboardNotificationManager.EXTRA_EVENT_ID);
        int notificationId = intent.getIntExtra(
                RemoteClipboardNotificationManager.EXTRA_NOTIFICATION_ID, -1);

        RemoteClipboardNotificationManager manager =
                RemoteClipboardNotificationManager.getInstance(context);
        // 优先取进程内保存的完整正文（Intent extra 有长度上限）
        String content = manager.getPendingContent(notificationId);
        if (content == null || content.isEmpty()) {
            content = intent.getStringExtra(
                    RemoteClipboardNotificationManager.EXTRA_CONTENT);
        }
        if (content == null || content.isEmpty()) {
            Log.w(TAG, "通知「复制」失败：通知内容缺失, eventId=" + eventId);
            return;
        }

        // NOTIFICATION 来源：只写剪贴板，不入库/不上行/不生成 eventId/不广播
        ClipboardWriteResult result =
                SyncManager.getInstance().copyFromNotification(context, content);
        if (result == ClipboardWriteResult.SUCCESS) {
            Log.i(TAG, "防回环：NOTIFICATION 本地复制成功"
                    + "（未生成 eventId、未上行、未广播），eventId=" + eventId);
            manager.markCopied(eventId, notificationId, content);
            showToast(context, R.string.notification_copy_done);
        } else if (result == ClipboardWriteResult.UNVERIFIED) {
            // 用户主动点击「复制」是明确意图：写入调用已被系统接受，
            // 后台焦点限制导致无法回读校验，按"已复制"更新通知并移除按钮
            Log.i(TAG, "防回环：NOTIFICATION 本地复制完成，但结果无法校验"
                    + "(UNVERIFIED：后台焦点限制，回读被拒，写入调用已被系统接受)；"
                    + "按已复制处理（未生成 eventId、未上行、未广播），eventId="
                    + eventId);
            manager.markCopied(eventId, notificationId, content);
            showToast(context, R.string.notification_copy_done);
        } else {
            // FAILED / NOT_ALLOWED：确定失败，保持原通知（仍带「复制」按钮），提示用户可重试
            Log.w(TAG, "NOTIFICATION 本地复制失败（确定失败）result=" + result
                    + ", eventId=" + eventId + "，通知保持带「复制」按钮");
            showToast(context, R.string.notification_copy_failed);
        }
    }

    private void showToast(final Context context, final int resId) {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    Toast.makeText(context.getApplicationContext(),
                            resId, Toast.LENGTH_SHORT).show();
                } catch (RuntimeException e) {
                    Log.w(TAG, "提示展示失败: " + e.getMessage());
                }
            }
        });
    }
}
