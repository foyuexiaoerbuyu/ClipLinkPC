package com.cliplink.mobile.clipboard;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.cliplink.mobile.protocol.HashUtil;

import java.nio.charset.StandardCharsets;

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
                return;
            }
            // 第二层保护：当前 hash == 最近程序写入 hash 则忽略（§15）
            String currentHash = HashUtil.sha256Hex(text);
            if (currentHash.equals(lastProgrammaticHash)) {
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
     * 程序写入系统剪贴板（REMOTE 下发与 HISTORY 点击共用，见需求 §46 / §58 / §83）。
     *
     * 写入前置 suppress 标志并记录 lastProgrammaticClipboardHash（§14-§15），
     * 保证随后的系统回调被识别为程序写入而忽略，不再次发送服务器（§47）。
     * 事件来源语义由调用方（SyncManager 事件路由）决定。
     */
    public void writeProgrammatic(final String text) {
        if (text == null || text.isEmpty() || clipboardManager == null) {
            return;
        }
        if (utf8Length(text) > MAX_TEXT_BYTES) {
            Log.w(TAG, "待写入内容过大，已跳过");
            return;
        }
        // 剪贴板操作统一投递主线程（§62 解耦，网络线程不直接碰系统服务）
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                synchronized (lock) {
                    suppressNext = true;
                    lastProgrammaticHash = HashUtil.sha256Hex(text);
                    try {
                        clipboardManager.setPrimaryClip(ClipData.newPlainText(
                                "ClipLink", text));
                    } catch (Exception e) {
                        // 写入失败时回收标志，避免吞掉下一次用户复制（§72）
                        suppressNext = false;
                        Log.e(TAG, "写入剪贴板失败: " + e.getMessage());
                    }
                }
            }
        });
    }

    private static int utf8Length(String text) {
        return text.getBytes(StandardCharsets.UTF_8).length;
    }
}
