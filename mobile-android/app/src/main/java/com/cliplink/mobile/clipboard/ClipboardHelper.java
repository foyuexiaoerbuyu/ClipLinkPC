package com.cliplink.mobile.clipboard;

import android.app.ActivityManager;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.cliplink.mobile.protocol.HashUtil;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * 系统剪贴板监听与读写（Android 端对应 PC 端 ClipboardReader/ClipboardWriter，
 * 见需求 §11-§17 / §57-§58 / §73 / §77）。
 *
 * - 事件监听：ClipboardManager.OnPrimaryClipChangedListener，禁止轮询（§103）
 * - 读取：加锁 + 短暂重试，失败不崩溃（§57）
 * - 空剪贴板忽略，不创建空历史（§77）
 * - UTF-8 上限 1MB，超过不实时同步（§73）
 * - 防循环双保护（§14-§16）：
 *   1) suppressNext 标志：程序写入前置位，下次系统回调消费并忽略；
 *   2) lastProgrammaticClipboardHash：当前内容 hash 命中程序写入 hash 则忽略。
 * - 程序写入：ClipData.newPlainText（§58 对应的 Android 写入方式）
 */
public class ClipboardHelper implements ClipboardManager.OnPrimaryClipChangedListener {

    private static final String TAG = "ClipLink";

    /** 文本内容上限：1MB（UTF-8 字节，见需求 §73） */
    public static final int MAX_TEXT_BYTES = 1024 * 1024;

    /**
     * 程序写入等待主线程完成的超时（毫秒）。
     *
     * <p>写入结果需要同步返回给调用方（网络线程），但剪贴板操作统一在主线程执行，
     * 故用 latch 等待；超时按失败处理，绝不无限阻塞同步线程（§62 / §72）。
     */
    private static final long WRITE_TIMEOUT_MS = 1500L;

    /** 用户真实复制（USER 来源）回调，主线程触发 */
    public interface UserClipListener {
        void onUserClip(String text);
    }

    private final Context appContext;
    private final ClipboardManager clipboardManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    /** 防循环状态锁：标志与 hash 的读写-写入需原子（§14-§15） */
    private final Object lock = new Object();
    private boolean suppressNext = false;
    private String lastProgrammaticHash = "";

    private volatile UserClipListener listener;
    private volatile boolean listening = false;

    public ClipboardHelper(Context context) {
        Context app = context.getApplicationContext();
        appContext = app;
        clipboardManager =
                (ClipboardManager) app.getSystemService(Context.CLIPBOARD_SERVICE);
    }

    /**
     * 开始监听系统剪贴板变化（事件驱动，非轮询）。
     */
    public void startListening(UserClipListener userClipListener) {
        listener = userClipListener;
        if (clipboardManager == null) {
            Log.e(TAG, "ClipboardManager 不可用，无法监听剪贴板");
            return;
        }
        if (!listening) {
            clipboardManager.addPrimaryClipChangedListener(this);
            listening = true;
        }
    }

    /**
     * 停止监听系统剪贴板变化。
     */
    public void stopListening() {
        if (listening && clipboardManager != null) {
            clipboardManager.removePrimaryClipChangedListener(this);
            listening = false;
        }
    }

    @Override
    public void onPrimaryClipChanged() {
        handleSystemCallback();
    }

    /**
     * 系统剪贴板变化回调（Android 对应 PC 端 WM_CLIPBOARDUPDATE）：
     * 读取 -> 判定是否程序写入（防循环双保护）-> USER 才回调上层。
     */
    private void handleSystemCallback() {
        String text = readTextWithRetry();
        if (text == null || text.isEmpty()) {
            // 空剪贴板忽略，不创建空历史（§77）
            return;
        }
        if (utf8Length(text) > MAX_TEXT_BYTES) {
            // 超过 1MB：不入库不实时同步（§73）
            Log.w(TAG, "剪贴板内容过大，未同步");
            return;
        }
        synchronized (lock) {
            // 第一层保护：suppress 标志，消费一次即清除（§14）
            if (suppressNext) {
                suppressNext = false;
                // 防回环日志关键字（真机验证时 grep "防回环" 即可确认程序写入未被上行）
                Log.i(TAG, "防回环：命中程序写入 suppress 标志，"
                        + "忽略本次系统剪贴板回调，不产生 USER 事件");
                return;
            }
            // 第二层保护：当前 hash == 最近程序写入 hash 则忽略（§15）
            String currentHash = HashUtil.sha256Hex(text);
            if (currentHash.equals(lastProgrammaticHash)) {
                Log.i(TAG, "防回环：内容命中程序写入 hash，"
                        + "忽略本次系统剪贴板回调，不产生 USER 事件");
                return;
            }
        }
        UserClipListener l = listener;
        if (l != null) {
            l.onUserClip(text);
        }
    }

    /**
     * 读取当前剪贴板文本：立即尝试 + 短延迟重试（共 3 次，见需求 §57）。
     *
     * @return 文本内容；无文本、空或始终读取失败返回 null
     */
    private String readTextWithRetry() {
        if (clipboardManager == null) {
            return null;
        }
        for (int attempt = 0; attempt < 3; attempt++) {
            try {
                ClipData clip = clipboardManager.getPrimaryClip();
                if (clip != null && clip.getItemCount() > 0) {
                    // MVP 仅支持文本（§10）：非文本项 getText 为 null，忽略
                    CharSequence cs = clip.getItemAt(0).getText();
                    if (cs != null) {
                        return cs.toString();
                    }
                }
            } catch (Exception e) {
                // 剪贴板被占用/读取异常：短暂重试，不崩溃（§57 / §72）
                Log.w(TAG, "读取剪贴板失败: " + e.getMessage());
            }
            if (attempt < 2) {
                try {
                    Thread.sleep(30);
                } catch (InterruptedException ie) {
                    Thread.currentThread().interrupt();
                    return null;
                }
            }
        }
        return null;
    }

    /**
     * 直读系统剪贴板文本（供后台无障碍通道复用，含短暂重试，见需求 §57）。
     *
     * <p>Android 10+ 起后台应用直读剪贴板可能被系统拒绝（返回 null/空），
     * 此时由调用方回退其它通道（无障碍：事件源节点的选中文本）。
     *
     * @return 文本内容；无文本 / 空 / 读取被拒时返回 null
     */
    public String readClipboardText() {
        return readTextWithRetry();
    }

    /**
     * 判断文本是否与本应用最近一次程序写入内容一致（§15 / §47 防循环）。
     *
     * <p>供后台无障碍通道复用：PC 下发 / 历史回填写入剪贴板后，
     * 无障碍侧读到的同一内容不得再作为 USER 事件上行。
     */
    public boolean isProgrammaticText(String text) {
        if (text == null || text.isEmpty()) {
            return false;
        }
        synchronized (lock) {
            return HashUtil.sha256Hex(text).equals(lastProgrammaticHash);
        }
    }

    /**
     * 程序写入系统剪贴板并回读校验，返回明确写入结果
     * （REMOTE 下发 / HISTORY 点击 / NOTIFICATION 复制共用，见需求 §46 / §58 / §83
     * 与《PC 远程剪贴板通知》spec §4）。
     *
     * <p>判定顺序（真机 Android 14 实测修正，spec §2）：
     * <ol>
     *   <li>setPrimaryClip 抛 SecurityException -&gt; {@link ClipboardWriteResult#NOT_ALLOWED}；
     *       抛其它异常 -&gt; {@link ClipboardWriteResult#FAILED}；</li>
     *   <li>回读成功且内容与写入一致 -&gt; {@link ClipboardWriteResult#SUCCESS}；</li>
     *   <li>回读成功但内容不一致或为空 -&gt; {@link ClipboardWriteResult#FAILED}；</li>
     *   <li>回读被系统拒绝 / 返回 null（后台焦点限制，例如抛 SecurityException
     *       或 hasPrimaryClip 不可用）-&gt; {@link ClipboardWriteResult#UNVERIFIED}，
     *       日志写「回读受限，无法校验（后台焦点限制），写入调用已被系统接受」。</li>
     * </ol>
     *
     * <p>关键修正：旧实现在"回读为空且 hasPrimaryClip=false"时一律判 FAILED，
     * 但 Android 10+ 后台应用回读剪贴板会被系统伪装成"空剪贴板"（实测后台下发时
     * setPrimaryClip 已生效、系统自动填充服务已检测到变化），故该情形必须判
     * UNVERIFIED（无法校验）而非 FAILED（确定失败）。
     *
     * <p>写入前置 suppress 标志并记录 lastProgrammaticClipboardHash（§14-§15），
     * 保证随后的系统回调被识别为程序写入而忽略，不再次发送服务器（§47 防回环）。
     * 事件来源语义由调用方（SyncManager 事件路由）决定。
     *
     * <p>可从任意线程调用：主线程直接执行；其它线程投递主线程并同步等待结果
     * （超时 {@link #WRITE_TIMEOUT_MS}，避免阻塞同步线程）。
     */
    public ClipboardWriteResult writeProgrammatic(final String text) {
        if (text == null || text.isEmpty()) {
            return ClipboardWriteResult.FAILED;
        }
        if (clipboardManager == null) {
            Log.e(TAG, "ClipboardManager 不可用，无法写入剪贴板");
            return ClipboardWriteResult.NOT_ALLOWED;
        }
        if (utf8Length(text) > MAX_TEXT_BYTES) {
            Log.w(TAG, "待写入内容过大，已跳过");
            return ClipboardWriteResult.FAILED;
        }
        // 剪贴板操作统一投递主线程（§62 解耦，网络线程不直接碰系统服务）
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return doWriteOnMainThread(text);
        }
        final ClipboardWriteResult[] holder = new ClipboardWriteResult[1];
        final CountDownLatch latch = new CountDownLatch(1);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                try {
                    holder[0] = doWriteOnMainThread(text);
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            if (!latch.await(WRITE_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "写入剪贴板等待超时（主线程未及时响应），按失败处理");
                return ClipboardWriteResult.FAILED;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return ClipboardWriteResult.FAILED;
        }
        return holder[0] == null ? ClipboardWriteResult.FAILED : holder[0];
    }

    /** 主线程内执行的实际写入 + 回读校验（见 writeProgrammatic 说明） */
    private ClipboardWriteResult doWriteOnMainThread(String text) {
        synchronized (lock) {
            suppressNext = true;
            lastProgrammaticHash = HashUtil.sha256Hex(text);
            try {
                clipboardManager.setPrimaryClip(
                        ClipData.newPlainText("ClipLink", text));
            } catch (SecurityException e) {
                // 系统拒绝写入（权限/策略限制）：本次剪贴板未变化，回收标志
                suppressNext = false;
                Log.e(TAG, "写入剪贴板被系统拒绝(SecurityException): "
                        + e.getMessage());
                return ClipboardWriteResult.NOT_ALLOWED;
            } catch (Exception e) {
                // 写入失败时回收标志，避免吞掉下一次用户复制（§72）
                suppressNext = false;
                Log.e(TAG, "写入剪贴板失败: " + e.getMessage());
                return ClipboardWriteResult.FAILED;
            }
        }
        // 回读校验：setPrimaryClip 未抛异常不等于写入成功（spec §4）
        return verifyWritten(text);
    }

    /**
     * 回读系统剪贴板与写入内容比对（spec §2 判定顺序）。
     *
     * <p>关键：必须区分「回读成功但内容为空/不一致」（确定失败 FAILED）
     * 与「回读被系统拒绝或返回 null」（后台焦点限制，写入调用已被系统接受
     * 但无法校验 UNVERIFIED）。Android 10+ 后台应用读取剪贴板会被系统
     * 伪装成"空剪贴板"（getPrimaryClip 返回 null、hasPrimaryClip 返回 false），
     * 因此当本应用不在前台时，读不到内容一律判 UNVERIFIED，绝不误报 FAILED。
     */
    private ClipboardWriteResult verifyWritten(String expected) {
        ReadBack first = readBackForVerify();
        ClipboardWriteResult verdict = judgeReadBack(first, expected);
        if (verdict != null) {
            return verdict;
        }
        // 部分 ROM 写入后存在极短可见窗口：稍等后再读一次
        sleepQuietly(80);
        ReadBack second = readBackForVerify();
        verdict = judgeReadBack(second, expected);
        if (verdict != null) {
            return verdict;
        }
        if (first.denied || second.denied || !isAppInForeground()) {
            // 后台焦点限制：写入调用未抛异常（已被系统接受），但无法回读校验
            Log.w(TAG, "回读受限，无法校验（后台焦点限制），写入调用已被系统接受"
                    + "；最终结果=UNVERIFIED，reason=readBackDenied，"
                    + "foreground=" + isAppInForeground()
                    + ", denied=" + (first.denied || second.denied)
                    + ", hasPrimaryClip=" + hasPrimaryClip());
            return ClipboardWriteResult.UNVERIFIED;
        }
        Log.e(TAG, "最终结果=FAILED，reason=前台回读为空且剪贴板无内容，写入未生效");
        return ClipboardWriteResult.FAILED;
    }

    /**
     * 单次回读结果的判定：能得出确定结论时返回结果，否则返回 null
     * （表示"读不到内容且非确定失败"，交由 verifyWritten 结合前台状态裁决）。
     */
    private ClipboardWriteResult judgeReadBack(ReadBack readBack, String expected) {
        if (readBack.text != null && readBack.text.equals(expected)) {
            Log.i(TAG, "最终结果=SUCCESS，reason=回读内容与写入内容一致，len="
                    + expected.length());
            return ClipboardWriteResult.SUCCESS;
        }
        if (!readBack.denied && readBack.text != null && !readBack.text.isEmpty()) {
            // 回读成功但内容不一致：确定失败（内容被其它应用抢占 / 写入未生效）
            Log.e(TAG, "最终结果=FAILED，reason=回读内容与写入内容不一致，readLen="
                    + readBack.text.length() + ", expectedLen=" + expected.length());
            return ClipboardWriteResult.FAILED;
        }
        return null;
    }

    /** 回读结果：区分「读到文本」「读到空剪贴板」「系统拒绝读取」三种情形 */
    private static final class ReadBack {
        /** 读到的文本；null 表示无内容 */
        final String text;
        /** true 表示系统拒绝本应用读取剪贴板（后台焦点限制） */
        final boolean denied;

        ReadBack(String text, boolean denied) {
            this.text = text;
            this.denied = denied;
        }
    }

    /**
     * 专用于写入校验的回读（不吞掉"被系统拒绝"这一关键信息，
     * 与 {@link #readTextWithRetry()} 的语义不同），短重试后返回。
     */
    private ReadBack readBackForVerify() {
        if (clipboardManager == null) {
            return new ReadBack(null, true);
        }
        boolean denied = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            try {
                ClipData clip = clipboardManager.getPrimaryClip();
                if (clip != null && clip.getItemCount() > 0) {
                    CharSequence cs = clip.getItemAt(0).getText();
                    return new ReadBack(cs == null ? "" : cs.toString(), false);
                }
                // 剪贴板为空：读取动作本身成功，是"读到空"而非"被拒绝"
                return new ReadBack(null, false);
            } catch (SecurityException e) {
                denied = true;
                Log.w(TAG, "回读剪贴板被系统拒绝(SecurityException，后台焦点限制): "
                        + e.getMessage());
            } catch (Exception e) {
                Log.w(TAG, "回读剪贴板异常: " + e.getMessage());
            }
            if (attempt == 0) {
                sleepQuietly(40);
            }
        }
        return new ReadBack(null, denied);
    }

    /**
     * 本应用当前是否处于前台（用于区分"后台焦点限制读不到"与"前台确实没写进去"）。
     *
     * <p>拿不到进程状态时返回 false（保守：按后台处理，宁可判 UNVERIFIED
     * 也不误报 FAILED）。
     */
    private boolean isAppInForeground() {
        if (appContext == null) {
            return false;
        }
        try {
            ActivityManager am = (ActivityManager)
                    appContext.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) {
                return false;
            }
            ActivityManager.RunningAppProcessInfo info =
                    new ActivityManager.RunningAppProcessInfo();
            ActivityManager.getMyMemoryState(info);
            return info.importance
                    <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
        } catch (Throwable t) {
            return false;
        }
    }

    /** 当前剪贴板是否存在内容（读取失败按 false 处理，不抛出） */
    private boolean hasPrimaryClip() {
        if (clipboardManager == null) {
            return false;
        }
        try {
            return clipboardManager.hasPrimaryClip();
        } catch (Exception e) {
            Log.w(TAG, "查询剪贴板状态失败: " + e.getMessage());
            return false;
        }
    }

    /** 忽略中断的短暂休眠（回读校验用，不抛异常） */
    private static void sleepQuietly(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException ie) {
            Thread.currentThread().interrupt();
        }
    }

    private static int utf8Length(String text) {
        return text.getBytes(StandardCharsets.UTF_8).length;
    }
}
