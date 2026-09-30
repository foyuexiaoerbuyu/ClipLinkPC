package com.cliplink.mobile.clipboard;

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
     * <p>与旧实现的关键差异：不再以"setPrimaryClip 未抛异常"判定成功，
     * 而是写入后回读 {@link ClipboardManager} 当前内容与待写文本比对：
     * <ul>
     *   <li>{@link ClipboardWriteResult#SUCCESS}：回读内容与写入内容完全一致；
     *       Android 10+ 后台读取被系统限制（回读为空但 hasPrimaryClip()=true）
     *       时按写入成功处理，并在日志中标注"回读受限"；</li>
     *   <li>{@link ClipboardWriteResult#FAILED}：写入异常、内容过大/为空、
     *       回读内容与写入内容不一致、回读为空且剪贴板为空、等待主线程超时；</li>
     *   <li>{@link ClipboardWriteResult#NOT_ALLOWED}：系统拒绝写入
     *       （SecurityException / 剪贴板服务不可用）。</li>
     * </ul>
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
     * 回读系统剪贴板与写入内容比对（spec §4）。
     *
     * <p>Android 10+ 对后台应用读取剪贴板有限制，回读可能为空：
     * 此时若 hasPrimaryClip()=true（说明写入动作确实改了剪贴板），
     * 按写入成功处理并记录"回读受限"日志，避免把真实成功误判为失败。
     */
    private ClipboardWriteResult verifyWritten(String expected) {
        String readBack = readTextWithRetry();
        if (readBack == null || readBack.isEmpty()) {
            // 部分 ROM 写入后存在极短可见窗口，延迟后再读一次
            try {
                Thread.sleep(80);
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
            }
            readBack = readTextWithRetry();
        }
        if (readBack != null && readBack.equals(expected)) {
            return ClipboardWriteResult.SUCCESS;
        }
        if (readBack != null && !readBack.isEmpty()) {
            Log.w(TAG, "剪贴板回读内容与写入内容不一致，判定写入失败");
            return ClipboardWriteResult.FAILED;
        }
        if (hasPrimaryClip()) {
            Log.w(TAG, "剪贴板回读受限（Android 10+ 后台读取被拒），"
                    + "写入未抛异常且剪贴板非空，按写入成功处理");
            return ClipboardWriteResult.SUCCESS;
        }
        Log.e(TAG, "剪贴板回读为空且 hasPrimaryClip=false，判定写入失败");
        return ClipboardWriteResult.FAILED;
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

    private static int utf8Length(String text) {
        return text.getBytes(StandardCharsets.UTF_8).length;
    }
}
