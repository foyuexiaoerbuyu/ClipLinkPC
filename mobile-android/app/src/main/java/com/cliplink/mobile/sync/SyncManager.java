package com.cliplink.mobile.sync;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.cliplink.mobile.clipboard.ClipboardHelper;
import com.cliplink.mobile.clipboard.ClipboardSource;
import com.cliplink.mobile.data.ClipboardRepository;
import com.cliplink.mobile.data.ConfigRepository;
import com.cliplink.mobile.protocol.ClipboardEvent;
import com.cliplink.mobile.protocol.HashUtil;
import com.cliplink.mobile.protocol.SyncProtocol;
import com.cliplink.mobile.protocol.UuidUtil;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.net.URI;
import java.util.List;

/**
 * 同步编排单例（Android 端对应 PC 端 SyncManager，见需求 §41-§54 /
 * §60 / §65 / §81-§87 / §92-§93）。
 *
 * 职责：
 * - 连接建立后发送 register + sync_request(lastServerSeq)（§42 / §50）
 * - 上行 USER 事件：clipboard 消息 -> clipboard_ack 更新 serverSeq / sync_status（§43-§44）
 * - 下行 clipboard：eventId 幂等 + contentHash 校验 + 单调 serverSeq 入库，
 *   并写入系统剪贴板（REMOTE，§46-§47）
 * - sync_response 补历史：只入库、推进游标，不覆盖当前剪贴板（§51-§52 / §93）
 * - 断线期间事件经 queryPendingSync 于重连后补传（§84-§86）
 * - setPaused 暂停同步：不发不收，恢复时补拉（§65）
 * - 事件路由区分 ClipboardSource：USER 入库+上行；REMOTE 写剪贴板不回发；
 *   HISTORY 只写剪贴板不入库不上行（§12 / §83）
 */
public final class SyncManager {

    private static final String TAG = "ClipLink";

    /** 每次补传批量上限（离线事件，§84） */
    private static final int PENDING_BATCH_LIMIT = 500;

    /**
     * 状态监听（静态监听器通知 UI，见需求 §63；回调统一切主线程）。
     */
    public interface StateListener {
        /** 连接状态变化（§38） */
        void onConnectionStateChanged(WebSocketClientWrapper.State state);

        /** 暂停同步开关变化（§65） */
        void onSyncPausedChanged(boolean paused);

        /** 本地历史有增删改（入库/ACK/清理），UI 应刷新列表（§63） */
        void onHistoryChanged();
    }

    private static volatile SyncManager instance;

    public static SyncManager getInstance() {
        if (instance == null) {
            synchronized (SyncManager.class) {
                if (instance == null) {
                    instance = new SyncManager();
                }
            }
        }
        return instance;
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private volatile Context appContext;
    private volatile ClipboardRepository repository;
    private volatile ConfigRepository config;
    private volatile ClipboardHelper clipboardHelper;
    private volatile WebSocketClientWrapper webSocket;
    private volatile StateListener listener;

    private volatile boolean started = false;
    private volatile boolean paused = false;

    /** 网络变化监听：网络恢复/切换时主动触发连接自愈（§91 后台常驻） */
    private volatile ConnectivityManager.NetworkCallback networkCallback;

    private SyncManager() {
    }

    // ---- 生命周期 -----------------------------------------------------------

    /**
     * 启动同步：初始化数据层、开启剪贴板监听（事件驱动）、建立 WebSocket
     * 连接（见需求 §81）。幂等，重复调用无副作用。
     */
    public synchronized void start(Context context) {
        if (started) {
            return;
        }
        appContext = context.getApplicationContext();
        repository = new ClipboardRepository(appContext);
        config = new ConfigRepository(appContext);

        // 启动时按配置清理历史（§28）
        try {
            repository.cleanup(config.getMaxHistoryCount(),
                    config.getMaxHistoryDays());
        } catch (RuntimeException e) {
            Log.w(TAG, "历史清理失败: " + e.getMessage());
        }

        // 剪贴板监听（OnPrimaryClipChangedListener 事件监听，非轮询，§103）
        ClipboardHelper helper = new ClipboardHelper(appContext);
        helper.startListening(new ClipboardHelper.UserClipListener() {
            @Override
            public void onUserClip(String text) {
                // helper 已完成防循环双保护判定，回调必为 USER 来源（§14-§16）
                handleClipboardEvent(ClipboardSource.USER, text);
            }
        });
        clipboardHelper = helper;

        started = true;
        openSocket();
        registerNetworkCallback();
        Log.i(TAG, "SyncManager 已启动, server=" + config.getServerUrl());
    }

    /**
     * 停止同步：断开连接、关闭剪贴板监听（Service onDestroy 调用）。
     */
    public synchronized void stop() {
        if (!started) {
            return;
        }
        started = false;
        unregisterNetworkCallback();
        ClipboardHelper helper = clipboardHelper;
        if (helper != null) {
            helper.stopListening();
        }
        WebSocketClientWrapper ws = webSocket;
        if (ws != null) {
            ws.stop();
            webSocket = null;
        }
        Log.i(TAG, "SyncManager 已停止");
    }

    /** UI 注册静态状态监听（§63：Repository/同步 -> 通知 UI，不轮询数据库） */
    public void setStateListener(StateListener stateListener) {
        listener = stateListener;
    }

    /**
     * 暂停/恢复同步（见需求 §65）：
     * 暂停期间仍监听剪贴板并保存本地历史，但不发送、不接收远程剪贴板；
     * 恢复时补拉 sync_request + 重传待同步事件。
     */
    public void setPaused(boolean newPaused) {
        if (paused == newPaused) {
            return;
        }
        paused = newPaused;
        final StateListener l = listener;
        if (l != null) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    l.onSyncPausedChanged(paused);
                }
            });
        }
        if (!newPaused) {
            // 恢复同步：补拉缺口 + 补传离线事件（§65 / §84）
            sendRegister();
            sendSyncRequest();
            flushPending();
        }
    }

    public boolean isPaused() {
        return paused;
    }

    /**
     * 立即重连（UI 状态条点击 / 保存服务器设置后调用，见需求 §38 / §101）：
     * 关闭当前连接并按 ConfigRepository 最新配置重建 WebSocket。
     * 仅重建连接，不中断剪贴板监听与历史保存。
     */
    public synchronized void reconnect() {
        if (!started) {
            return;
        }
        WebSocketClientWrapper ws = webSocket;
        if (ws != null) {
            ws.stop(); // 旧 wrapper 本地停止，禁止其继续退避重连
            webSocket = null;
        }
        openSocket(); // 读取 config.getServerUrl() 最新地址（§101 不写死）
        Log.i(TAG, "手动重连: " + (config == null ? "" : config.getServerUrl()));
    }

    /**
     * 主动触发一次连接自愈（网络恢复 / 回到前台 / 自检提示后调用，§91）：
     * 已连接或连接在途时为空操作，由 WebSocketClientWrapper 保证不产生并发重复连接。
     */
    public void ensureConnected() {
        WebSocketClientWrapper ws = webSocket;
        if (!started || paused || ws == null) {
            return;
        }
        ws.ensureConnected();
    }

    // ---- 网络变化监听（§91：网络切换/恢复后立即自愈）--------------------------

    private void registerNetworkCallback() {
        if (networkCallback != null || appContext == null) {
            return;
        }
        try {
            ConnectivityManager cm = (ConnectivityManager)
                    appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) {
                return;
            }
            ConnectivityManager.NetworkCallback cb =
                    new ConnectivityManager.NetworkCallback() {
                        @Override
                        public void onAvailable(Network network) {
                            Log.i(TAG, "网络可用，触发连接自愈");
                            ensureConnected();
                        }

                        @Override
                        public void onLost(Network network) {
                            // 断网：连接将由 WebSocketClientWrapper 监督线程判定并退避重连
                            Log.i(TAG, "网络断开: " + network);
                        }
                    };
            cm.registerDefaultNetworkCallback(cb);
            networkCallback = cb;
            Log.i(TAG, "网络状态监听已注册");
        } catch (RuntimeException e) {
            Log.w(TAG, "网络状态监听注册失败: " + e.getMessage());
        }
    }

    private void unregisterNetworkCallback() {
        ConnectivityManager.NetworkCallback cb = networkCallback;
        networkCallback = null;
        if (cb == null || appContext == null) {
            return;
        }
        try {
            ConnectivityManager cm = (ConnectivityManager)
                    appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null) {
                cm.unregisterNetworkCallback(cb);
                Log.i(TAG, "网络状态监听已注销");
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "网络状态监听注销失败: " + e.getMessage());
        }
    }

    public WebSocketClientWrapper.State getConnectionState() {
        WebSocketClientWrapper ws = webSocket;
        return ws == null ? WebSocketClientWrapper.State.DISCONNECTED
                : ws.getState();
    }

    /** 数据层访问（历史列表 UI 使用） */
    public ClipboardRepository getRepository() {
        if (repository == null && appContext != null) {
            repository = new ClipboardRepository(appContext);
        }
        return repository;
    }

    // ---- 事件路由（§12 ClipboardSource）-------------------------------------

    /**
     * 统一事件路由入口（见需求 §12）：
     *
     * USER   - 用户真实复制：入库 + 发送服务器（§81 同步流程）
     * REMOTE - 服务器下发：写入系统剪贴板（suppress 防回发，§46-§47）；
     *          入库/游标已在下行协议处理中完成
     * HISTORY - 点击历史项：仅写系统剪贴板，不入库、不发送（§83）
     */
    public void handleClipboardEvent(ClipboardSource source, String text) {
        if (text == null || text.isEmpty()) {
            return; // 空内容忽略（§77）
        }
        switch (source) {
            case USER:
                onUserCopied(text);
                break;
            case REMOTE:
                // §46-§47：写入本地剪贴板，由 ClipboardHelper 设置 suppress +
                // lastProgrammaticClipboardHash 保证不回发
                writeRemote(text);
                break;
            case HISTORY:
                // §83：只写剪贴板，不产生新历史、不发送 WebSocket
                writeRemote(text);
                break;
            default:
                break;
        }
    }

    private void writeRemote(String text) {
        ClipboardHelper helper = clipboardHelper;
        if (helper != null) {
            helper.writeProgrammatic(text);
        }
    }

    // ---- 上行：USER 事件（§81 / §43-§44）------------------------------------

    private void onUserCopied(String text) {
        ClipboardRepository repo = repository;
        ConfigRepository cfg = config;
        if (repo == null || cfg == null) {
            return;
        }

        ClipboardEvent ev = new ClipboardEvent();
        ev.setEventId(UuidUtil.uuidV4());                    // §18 每次新事件 UUID
        ev.setDeviceId(cfg.getDeviceId());                   // §19 持久化 deviceId
        ev.setDeviceType(SyncProtocol.DEVICE_TYPE_ANDROID);  // §21
        ev.setDeviceName(cfg.getDeviceName());               // §20
        ev.setContentType(ClipboardEvent.CONTENT_TYPE_TEXT); // §10 MVP 仅 TEXT
        ev.setContentHash(HashUtil.sha256Hex(text));         // §17 SHA-256
        ev.setContent(text);
        ev.setClientTime(System.currentTimeMillis());        // §80 UTC 毫秒
        ev.setCreatedAt(System.currentTimeMillis());
        ev.setSyncStatus(ClipboardEvent.SYNC_STATUS_PENDING);// §85

        ClipboardRepository.InsertOutcome outcome = repo.insertEvent(ev);
        if (outcome == ClipboardRepository.InsertOutcome.ADJACENT_DUPLICATE) {
            // §29 相邻最近一次相同内容去重：不入库也不发送
            return;
        }
        if (outcome == ClipboardRepository.InsertOutcome.DUPLICATE_EVENT_ID) {
            return; // 新 UUID 不应重复，防御性返回
        }
        if (outcome == ClipboardRepository.InsertOutcome.FAILED) {
            Log.e(TAG, "USER 事件入库失败");
            return;
        }
        notifyHistoryChanged();
        cleanupHistory();

        // §65 暂停同步时仍保存本地历史，但不发送
        if (paused || !cfg.getAutoSync()) {
            return;
        }
        sendUpstream(ev);
    }

    /**
     * 发送单个上行 clipboard 事件；失败标记 FAILED 待重试（§85）。
     */
    private void sendUpstream(ClipboardEvent ev) {
        WebSocketClientWrapper ws = webSocket;
        if (ws == null || !ws.isOpen()) {
            return; // 断线：保持 PENDING，重连后 queryPendingSync 补传（§84）
        }
        boolean ok = false;
        try {
            String msg = SyncProtocol.buildClipboardMessage(ev);
            ok = ws.sendText(msg);
        } catch (JSONException e) {
            Log.e(TAG, "clipboard 消息构建失败: " + e.getMessage());
        }
        if (!ok) {
            repository.markSyncFailed(ev.getEventId()); // §85 FAILED 保持可重试
        }
    }

    /**
     * 重连/恢复后补传离线事件（§84-§86）。
     */
    private void flushPending() {
        WebSocketClientWrapper ws = webSocket;
        ClipboardRepository repo = repository;
        if (paused || ws == null || !ws.isOpen() || repo == null) {
            return;
        }
        List<ClipboardEvent> pending = repo.queryPendingSync(PENDING_BATCH_LIMIT);
        for (ClipboardEvent ev : pending) {
            sendUpstream(ev);
        }
        if (!pending.isEmpty()) {
            Log.i(TAG, "补传离线事件 " + pending.size() + " 条");
        }
    }

    // ---- WebSocket 回调（网络线程）------------------------------------------

    private final WebSocketClientWrapper.Callback wsCallback =
            new WebSocketClientWrapper.Callback() {
                @Override
                public void onStateChanged(WebSocketClientWrapper.State state) {
                    final StateListener l = listener;
                    if (l != null) {
                        mainHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                l.onConnectionStateChanged(state);
                            }
                        });
                    }
                }

                @Override
                public void onConnected() {
                    // §42：连接后第一条消息 register；
                    // §50：随后 sync_request(lastServerSeq) 增量补历史；
                    // §84：并补传断线期间的离线事件
                    sendRegister();
                    sendSyncRequest();
                    flushPending();
                }

                @Override
                public void onMessage(String text) {
                    handleServerMessage(text);
                }

                @Override
                public void onDisconnected(int code, String reason) {
                    // §39 重连由 WebSocketClientWrapper 退避调度；此处仅记录
                    Log.i(TAG, "连接断开: code=" + code + ", reason=" + reason);
                }
            };

    private synchronized void openSocket() {
        if (!started) {
            return;
        }
        ConfigRepository cfg = config;
        String url = cfg.getServerUrl();
        try {
            webSocket = new WebSocketClientWrapper(URI.create(url), wsCallback);
            webSocket.start(); // 异步连接，退避重连由 wrapper 管理（§39）
        } catch (RuntimeException e) {
            Log.e(TAG, "连接地址无效: " + url + ", " + e.getMessage());
            webSocket = null;
        }
    }

    private void sendRegister() {
        WebSocketClientWrapper ws = webSocket;
        ConfigRepository cfg = config;
        if (ws == null || !ws.isOpen() || cfg == null || paused) {
            return;
        }
        try {
            ws.sendText(SyncProtocol.buildRegisterMessage(
                    cfg.getDeviceId(), cfg.getDeviceName()));
        } catch (JSONException e) {
            Log.e(TAG, "register 构建失败: " + e.getMessage());
        }
    }

    private void sendSyncRequest() {
        WebSocketClientWrapper ws = webSocket;
        ClipboardRepository repo = repository;
        if (paused || ws == null || !ws.isOpen() || repo == null) {
            return;
        }
        try {
            ws.sendText(SyncProtocol.buildSyncRequestMessage(
                    repo.getLastServerSeq())); // §48 / §50 游标增量
        } catch (JSONException e) {
            Log.e(TAG, "sync_request 构建失败: " + e.getMessage());
        }
    }

    // ---- 下行消息处理 --------------------------------------------------------

    private void handleServerMessage(String text) {
        SyncProtocol.ParsedMessage msg;
        try {
            msg = SyncProtocol.parseMessage(text); // §100 结构化解析
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "非法消息忽略: " + e.getMessage());
            return;
        }
        // §65 暂停期间不接收处理同步消息
        if (paused) {
            return;
        }
        try {
            switch (msg.type) {
                case CLIPBOARD:
                    onRemoteClipboard(msg.data);       // §46-§47 实时下发
                    break;
                case CLIPBOARD_ACK:
                    onClipboardAck(msg.data);          // §44
                    break;
                case SYNC_RESPONSE:
                    onSyncResponse(msg.data);          // §50-§51 补历史
                    break;
                case ERROR:
                    Log.w(TAG, "服务器错误: " + msg.data.toString());
                    break;
                default:
                    // REGISTER/SEND 等上行类型服务器不下发，忽略
                    break;
            }
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "消息校验失败: " + e.getMessage());
        } catch (RuntimeException e) {
            Log.e(TAG, "消息处理异常: " + e.getMessage());
        }
    }

    /** §46：收到远程 clipboard -> 校验 -> 入库 -> 写系统剪贴板（REMOTE） */
    private void onRemoteClipboard(JSONObject data) {
        ClipboardEvent ev = SyncProtocol.eventFromJson(data); // 必填校验

        // §17 / §46：hash 完整性校验，不匹配丢弃
        if (!HashUtil.sha256Hex(ev.getContent()).equals(ev.getContentHash())) {
            Log.w(TAG, "contentHash 校验失败，丢弃事件 " + ev.getEventId());
            return;
        }
        // §45 防御：服务器不应向来源设备回发自身事件
        if (ev.getDeviceId().equals(config.getDeviceId())) {
            Log.i(TAG, "忽略本机来源事件回发");
            return;
        }

        ClipboardRepository repo = repository;
        ClipboardRepository.InsertOutcome outcome = repo.insertEvent(ev); // §87 幂等
        if (outcome == ClipboardRepository.InsertOutcome.FAILED) {
            Log.e(TAG, "REMOTE 事件入库失败");
            return;
        }
        if (outcome == ClipboardRepository.InsertOutcome.DUPLICATE_EVENT_ID) {
            advanceCursor(ev.getServerSeq()); // 重复广播：仅推进游标，不重复写入
            return;
        }
        advanceCursor(ev.getServerSeq());    // §48 单调推进
        if (outcome == ClipboardRepository.InsertOutcome.INSERTED) {
            notifyHistoryChanged();
            cleanupHistory();
        }
        // §46-§47：写入系统剪贴板（REMOTE），防循环双保护保证不再回发
        handleClipboardEvent(ClipboardSource.REMOTE, ev.getContent());
    }

    /** §44：clipboard_ack 更新 serverSeq / serverTime / sync_status=SYNCED */
    private void onClipboardAck(JSONObject data) {
        String eventId = data.optString("eventId", "");
        long serverSeq = data.optLong("serverSeq", 0L);
        long serverTime = data.optLong("serverTime", 0L);
        if (eventId.isEmpty() || serverSeq <= 0) {
            Log.w(TAG, "clipboard_ack 字段非法，忽略");
            return;
        }
        ClipboardRepository repo = repository;
        if (repo.updateServerAck(eventId, serverSeq, serverTime)) {
            advanceCursor(serverSeq);
            notifyHistoryChanged();
        }
    }

    /**
     * §50-§52：sync_response 增量补历史——全部入库并推进游标，
     * 但不写入系统剪贴板（补历史不覆盖当前剪贴板，§51/§93）。
     */
    private void onSyncResponse(JSONObject data) {
        JSONArray events = data.optJSONArray("events");
        if (events == null || events.length() == 0) {
            return; // 无缺口
        }
        ClipboardRepository repo = repository;
        boolean anyInserted = false;
        for (int i = 0; i < events.length(); i++) {
            JSONObject item = events.optJSONObject(i);
            if (item == null) {
                continue;
            }
            ClipboardEvent ev;
            try {
                ev = SyncProtocol.eventFromJson(item);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "补历史事件非法，跳过: " + e.getMessage());
                continue;
            }
            // §17 hash 校验；不匹配仅跳过该条，不推进其游标
            if (!HashUtil.sha256Hex(ev.getContent())
                    .equals(ev.getContentHash())) {
                Log.w(TAG, "补历史事件 hash 不匹配，跳过 " + ev.getEventId());
                continue;
            }
            ClipboardRepository.InsertOutcome outcome = repo.insertEvent(ev);
            if (outcome == ClipboardRepository.InsertOutcome.FAILED) {
                Log.e(TAG, "补历史入库失败 " + ev.getEventId());
                continue;
            }
            if (outcome == ClipboardRepository.InsertOutcome.INSERTED) {
                anyInserted = true;
            }
            advanceCursor(ev.getServerSeq()); // §48 / §50 serverSeq 单调递增
            // §51：绝不写入系统剪贴板，仅入库成为历史
        }
        if (anyInserted) {
            notifyHistoryChanged();
            cleanupHistory();
        }
    }

    // ---- 内部工具 ------------------------------------------------------------

    /**
     * 单调推进同步游标 lastServerSeq（§22 / §48）：仅接受更大的值。
     */
    private void advanceCursor(long serverSeq) {
        if (serverSeq <= 0) {
            return;
        }
        ClipboardRepository repo = repository;
        if (repo != null && serverSeq > repo.getLastServerSeq()) {
            repo.setLastServerSeq(serverSeq);
        }
    }

    private void cleanupHistory() {
        ClipboardRepository repo = repository;
        ConfigRepository cfg = config;
        if (repo == null || cfg == null) {
            return;
        }
        try {
            repo.cleanup(cfg.getMaxHistoryCount(), cfg.getMaxHistoryDays());
        } catch (RuntimeException e) {
            Log.w(TAG, "历史清理失败: " + e.getMessage());
        }
    }

    private void notifyHistoryChanged() {
        final StateListener l = listener;
        if (l != null) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    l.onHistoryChanged();
                }
            });
        }
    }
}
