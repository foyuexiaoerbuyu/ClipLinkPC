package com.cliplink.mobile.accessibility;

import android.accessibilityservice.AccessibilityService;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import androidx.core.content.ContextCompat;

import com.cliplink.mobile.clipboard.ClipboardHelper;
import com.cliplink.mobile.protocol.HashUtil;
import com.cliplink.mobile.service.SyncForegroundService;
import com.cliplink.mobile.sync.SyncManager;

import java.util.List;

/**
 * 后台复制/剪切捕获通道（无障碍服务，见全局任务 2/3）。
 *
 * <p>要解决的问题：Android 10+ 起应用退到后台后，{@link ClipboardHelper} 的
 * OnPrimaryClipChangedListener 仍会被触发，但后台直读剪贴板可能被系统拒绝
 * （返回 null/空），导致"后台复制/剪切同步到 PC"失效。本服务以无障碍事件作为
 * 第二触发源，并在直读失败时回退读取事件源节点的选中文本。
 *
 * <p>捕获判定链路（多路兜底 + 三道去重）：
 * <ol>
 *   <li><b>触发</b>：TYPE_VIEW_TEXT_SELECTION_CHANGED（文本选择变化）与
 *       TYPE_WINDOW_CONTENT_CHANGED（窗口内容变化），后者要求源节点/事件文本
 *       与文本相关（过滤列表滚动等纯界面刷新）；</li>
 *   <li><b>合并</b>：同一批连续事件在 CAPTURE_DEBOUNCE_MS 内合并为一次捕获，
 *       并在 CAPTURE_FOLLOW_UP_MS 后复读一次（覆盖"先选中、稍后点复制"的时间差）；</li>
 *   <li><b>取文本（多路兜底）</b>：优先 ClipboardManager 直读；直读返回 null/空
 *       （Android 10+ 后台限制）时，回退读取事件源节点的选中文本；</li>
 *   <li><b>去重</b>：① 本服务内"内容未变化"判定 + 程序写入判定（PC 下发/历史回填
 *       的内容不再上行）；② {@link SyncManager} 双通道闸门（内容哈希 + 时间窗，
 *       与前台监听共用同一提交入口）；③ 仓库相邻重复去重。</li>
 * </ol>
 *
 * <p>捕获到的文本一律经 {@link SyncManager#submitAccessibilityCapture(String)}
 * 提交，与前台 OnPrimaryClipChangedListener 走完全相同的入库 + 上行链路，
 * 不新增协议、不改数据库结构（§10-§21 / §81-§87）。
 */
public class ClipLinkAccessibilityService extends AccessibilityService {

    private static final String TAG = "ClipLink";

    /** 事件合并窗口：复制/剪切会连发多个事件，合并为一次捕获 */
    private static final long CAPTURE_DEBOUNCE_MS = 260L;

    /** 选择事件后的二次复读延迟：用户"选中文本"与"点复制"之间有 1 秒左右时间差 */
    private static final long CAPTURE_FOLLOW_UP_MS = 1200L;

    /** 选择类事件最小间隔（连续拖动选择时避免无意义捕获） */
    private static final long SELECTION_THROTTLE_MS = 120L;

    /** 窗口内容变化类事件最小间隔（该类事件最频繁，限流更严格） */
    private static final long CONTENT_CHANGE_THROTTLE_MS = 600L;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    /** 最近一次事件的候选文本（事件源节点选中文本），供剪贴板直读失败时兜底 */
    private volatile String candidateText;

    private long lastSelectionEventAt;
    private long lastContentChangeAt;

    /** 上次直读到的剪贴板内容 hash：内容未变化说明不是新的复制/剪切 */
    private volatile String lastSeenClipboardHash = "";

    private final Runnable captureTask = new Runnable() {
        @Override
        public void run() {
            captureNow(false);
        }
    };

    private final Runnable followUpTask = new Runnable() {
        @Override
        public void run() {
            captureNow(true);
        }
    };

    // ---- 生命周期 -----------------------------------------------------------

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        Log.i(TAG, "无障碍服务已连接，后台复制/剪切捕获通道就绪");
        // 无障碍服务由系统常驻绑定，进程存活有保障：补启同步链路（幂等）
        ensureSyncRunning();
        // 记录当前剪贴板基线，避免服务刚连上就把历史遗留内容当成新复制
        primeClipboardHash();
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        if (event == null) {
            return;
        }
        try {
            switch (event.getEventType()) {
                case AccessibilityEvent.TYPE_VIEW_TEXT_SELECTION_CHANGED:
                    onTextSelectionChanged(event);
                    break;
                case AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED:
                    onWindowContentChanged(event);
                    break;
                default:
                    // 其余事件类型（窗口切换 / 焦点变化等）与复制无关，忽略
                    break;
            }
        } catch (RuntimeException e) {
            // 无障碍事件处理绝不能让服务崩溃（§72）
            Log.w(TAG, "无障碍事件处理异常: " + e.getMessage());
        }
    }

    @Override
    public void onInterrupt() {
        // 系统临时中断（用户关闭无障碍或系统回收）：不自行重启，等待系统重连（§72）
        Log.w(TAG, "无障碍服务被系统中断，等待系统重新连接");
    }

    @Override
    public boolean onUnbind(Intent intent) {
        Log.i(TAG, "无障碍服务已解绑");
        return super.onUnbind(intent);
    }

    @Override
    public void onDestroy() {
        mainHandler.removeCallbacks(captureTask);
        mainHandler.removeCallbacks(followUpTask);
        candidateText = null;
        Log.i(TAG, "无障碍服务已销毁，后台捕获停止");
        super.onDestroy();
    }

    // ---- 事件入口 -----------------------------------------------------------

    /**
     * 文本选择变化：用户划选文本（复制/剪切的前置动作）。
     * 无选中内容（仅光标移动）时直接忽略，避免误判为复制。
     */
    private void onTextSelectionChanged(AccessibilityEvent event) {
        AccessibilityNodeInfo source = event.getSource();
        if (source == null) {
            return; // 无源节点：既无法判定选择，也无回退数据源
        }
        String selected;
        try {
            selected = extractSelectedText(source, event);
        } finally {
            recycle(source);
        }
        if (selected == null || selected.isEmpty()) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (now - lastSelectionEventAt < SELECTION_THROTTLE_MS) {
            return; // 高频选择事件限流
        }
        lastSelectionEventAt = now;
        scheduleCapture(selected, "文本选择变化");
    }

    /**
     * 窗口内容变化：部分应用（WebView / 长按菜单消失等）复制后只发该类事件。
     * 仅当事件或源节点与文本相关时才作为捕获触发，过滤列表滚动等噪声。
     */
    private void onWindowContentChanged(AccessibilityEvent event) {
        long now = SystemClock.uptimeMillis();
        if (now - lastContentChangeAt < CONTENT_CHANGE_THROTTLE_MS) {
            return;
        }
        AccessibilityNodeInfo source = event.getSource();
        boolean textBearing = false;
        String selected = null;
        try {
            textBearing = isTextBearing(source, event);
            if (textBearing) {
                selected = extractSelectedText(source, event);
            }
        } finally {
            recycle(source);
        }
        if (!textBearing) {
            return; // 纯界面刷新（滚动/重绘）不触发捕获
        }
        lastContentChangeAt = now;
        scheduleCapture(selected, "窗口内容变化");
    }

    // ---- 捕获调度 -----------------------------------------------------------

    /**
     * 排队一次捕获：合并窗口内的事件只做一次，并在复读点再确认一次
     * （覆盖"选中 -> 稍后点复制"的时间差）。
     *
     * @param text    事件源节点候选文本，可为 null（此时仅走剪贴板直读）
     * @param trigger 触发来源，仅用于日志诊断
     */
    private void scheduleCapture(String text, String trigger) {
        candidateText = (text != null && !text.isEmpty()) ? text : null;
        mainHandler.removeCallbacks(captureTask);
        mainHandler.removeCallbacks(followUpTask);
        mainHandler.postDelayed(captureTask, CAPTURE_DEBOUNCE_MS);
        mainHandler.postDelayed(followUpTask, CAPTURE_FOLLOW_UP_MS);
        Log.i(TAG, "无障碍捕获已排队, trigger=" + trigger
                + ", 候选文本=" + (candidateText == null ? "无" : candidateText.length() + " 字"));
    }

    private void captureNow(boolean followUp) {
        try {
            doCapture(followUp);
        } finally {
            if (followUp) {
                // 复读结束清理候选，避免陈旧选中文本被后续事件兜底使用
                candidateText = null;
            }
        }
    }

    /** 多路兜底取文本 + 去重后提交同步链路 */
    private void doCapture(boolean followUp) {
        ClipboardHelper helper = SyncManager.getInstance().getClipboardHelper();
        if (helper == null) {
            ensureSyncRunning();
            helper = SyncManager.getInstance().getClipboardHelper();
            if (helper == null) {
                Log.w(TAG, "同步链路未启动，本次无障碍捕获放弃");
                return;
            }
        }

        String text = helper.readClipboardText(); // ① 优先剪贴板直读
        String channel;
        if (text != null && !text.isEmpty()) {
            channel = "剪贴板直读";
            String hash = HashUtil.sha256Hex(text);
            if (helper.isProgrammaticText(text)) {
                // ② 程序写入（PC 下发 / 历史回填）不回发，避免循环同步（§15 / §47）
                lastSeenClipboardHash = hash;
                Log.i(TAG, "无障碍捕获忽略程序写入内容（PC 下发/历史回填）");
                return;
            }
            if (hash.equals(lastSeenClipboardHash)) {
                Log.d(TAG, "无障碍捕获忽略：剪贴板内容未变化");
                return; // 非新的复制/剪切
            }
            lastSeenClipboardHash = hash;
        } else {
            // ③ 直读被 Android 10+ 后台限制拒绝：回退事件源节点选中文本
            text = candidateText;
            channel = "节点选中文本";
            if (text == null || text.isEmpty()) {
                Log.i(TAG, "剪贴板直读为空且无节点选中文本，忽略本次事件");
                return;
            }
        }

        SyncManager.getInstance().submitAccessibilityCapture(text);
        Log.i(TAG, "无障碍通道捕获到文本(" + channel
                + (followUp ? "/复读" : "") + "), len=" + text.length());
    }

    // ---- 文本提取与工具 -----------------------------------------------------

    /**
     * 读取事件源节点的"选中文本"（直读被拒时的回退通道）：
     * ① 节点选区范围（selectionStart < selectionEnd）内的文本；
     * ② 取不到选区时退化为事件自带文本，但仅限非可编辑节点——可编辑节点
     *    在"点击输入框/剪切后"会拿到与实际复制内容不符的整段文本。
     */
    private String extractSelectedText(AccessibilityNodeInfo node,
                                       AccessibilityEvent event) {
        if (node != null) {
            CharSequence text = node.getText();
            int start = node.getTextSelectionStart();
            int end = node.getTextSelectionEnd();
            if (text != null && start >= 0 && end > start && end <= text.length()) {
                return text.subSequence(start, end).toString();
            }
        }
        if (node == null || !node.isEditable()) {
            List<CharSequence> texts = event.getText();
            if (texts != null) {
                for (CharSequence cs : texts) {
                    if (cs != null && cs.length() > 0) {
                        return cs.toString();
                    }
                }
            }
        }
        return null;
    }

    /** 事件或源节点是否与文本相关（用于过滤窗口内容变化类噪声事件） */
    private boolean isTextBearing(AccessibilityNodeInfo node,
                                  AccessibilityEvent event) {
        List<CharSequence> texts = event.getText();
        if (texts != null) {
            for (CharSequence cs : texts) {
                if (cs != null && cs.length() > 0) {
                    return true;
                }
            }
        }
        if (node == null) {
            return false;
        }
        if (node.isEditable()) {
            return true;
        }
        CharSequence text = node.getText();
        if (text == null || text.length() == 0) {
            return false;
        }
        CharSequence cls = node.getClassName();
        String name = cls == null ? "" : cls.toString();
        return name.contains("EditText") || name.contains("TextView")
                || name.contains("WebView");
    }

    /**
     * 保证同步链路在运行：无障碍服务由系统常驻绑定（进程存活有保障），
     * 因此这里可补启同步；SyncManager.start 幂等，不会与前台服务重复启动。
     */
    private void ensureSyncRunning() {
        SyncManager manager = SyncManager.getInstance();
        if (manager.getClipboardHelper() != null) {
            return; // 同步已在运行
        }
        try {
            ContextCompat.startForegroundService(this,
                    new Intent(this, SyncForegroundService.class));
            Log.i(TAG, "无障碍服务已补启同步前台服务");
        } catch (RuntimeException e) {
            // Android 12+ 后台启动前台服务可能受限：退化为进程内启动同步
            // （进程由无障碍服务保活），捕获与发送链路不受影响（§72 不崩溃）
            Log.w(TAG, "补启前台服务受限，改为进程内启动同步: " + e.getMessage());
            manager.start(getApplicationContext());
        }
    }

    /** 记录当前剪贴板内容作为基线，避免把历史遗留内容当成新复制 */
    private void primeClipboardHash() {
        ClipboardHelper helper = SyncManager.getInstance().getClipboardHelper();
        if (helper == null) {
            return;
        }
        String text = helper.readClipboardText();
        if (text != null && !text.isEmpty()) {
            lastSeenClipboardHash = HashUtil.sha256Hex(text);
            Log.i(TAG, "无障碍服务剪贴板基线已记录");
        }
    }

    private void recycle(AccessibilityNodeInfo node) {
        if (node == null) {
            return;
        }
        try {
            node.recycle();
        } catch (RuntimeException e) {
            // 节点已被系统回收：忽略即可
            Log.d(TAG, "无障碍节点回收异常: " + e.getMessage());
        }
    }
}
