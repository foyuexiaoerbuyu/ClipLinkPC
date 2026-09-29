package com.cliplink.mobile.sync;

import android.util.Log;

import org.java_websocket.client.WebSocketClient;
import org.java_websocket.framing.CloseFrame;
import org.java_websocket.framing.Framedata;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * WebSocket 连接监督器（见需求 §38-§40 / §72 / §91）。
 *
 * <p>本类不再是 WebSocketClient 子类，而是一个长生命周期的"连接监督器"：
 * 内部持有的 {@link Connection}（WebSocketClient 子类）每次连接尝试都新建。
 * 原因：org.java-websocket 的 WebSocketClient 实例不可复用
 * （{@code connect()} 对已启动实例抛 IllegalStateException
 * "WebSocketClient objects are not reuseable"），旧实现复用同一实例重连会
 * 直接抛异常并陷入"退避 -> 抛异常 -> 再退避"的死循环，永远连不上。
 *
 * <p>断链自愈（不依赖单次 onClose 回调）：
 * <ul>
 *   <li>连接态：监督线程每 {@link #SUPERVISE_MS} 校验存活，超过
 *       {@link #LIVENESS_TIMEOUT_MS} 未收到任何入站数据（Pong / 业务消息）
 *       即判定僵尸连接，强制重建；</li>
 *   <li>断开态：监督线程发现无连接在途且无待执行重连时兜底补一次重连，
 *       覆盖"被冻结/被后台限制导致 onClose 从未触发"的场景；</li>
 *   <li>onError 也主动调度重连，不再假设 onClose 一定会到达。</li>
 * </ul>
 *
 * <p>并发重复连接防护：
 * <ul>
 *   <li>{@link #connectInFlight} 原子量保证同一时刻最多一次连接尝试；</li>
 *   <li>{@link #generation} 代际号使被替换的旧连接回调全部失效（含其 onClose）；</li>
 *   <li>重连任务经 {@link #scheduleReconnect} 去重，退避 1/2/5/10/30 秒封顶
 *       （§39，禁止 100ms 无限重连）。</li>
 * </ul>
 *
 * <p>心跳：每 {@link #HEARTBEAT_MS} 一次 Ping（§40 要求 30~60 秒一次），
 * 库内置连接丢失检测已关闭（{@code setConnectionLostTimeout(0)}），
 * 存活判定统一由本类监督线程负责，避免双重心跳与不可控的库内关闭时机。
 *
 * <p>线程模型：所有调度都在单线程 {@link #scheduler} 上串行执行，
 * 回调（onOpen/onClose/onError/onMessage）由库的读写线程触发，
 * 共享状态用 volatile + 同步块保护，UI 通知由 SyncManager 切主线程。
 */
public class WebSocketClientWrapper {

    /** 连接状态（见需求 §38） */
    public enum State {
        DISCONNECTED,
        CONNECTING,
        CONNECTED
    }

    /** 状态与消息回调（由 SyncManager 实现） */
    public interface Callback {
        void onStateChanged(State state);

        void onConnected();

        void onMessage(String text);

        void onDisconnected(int code, String reason);
    }

    private static final String TAG = "ClipLink";

    /** 退避序列：1s / 2s / 5s / 10s / 30s，之后按 30s 封顶（§39） */
    private static final long[] BACKOFF_MS = {1000L, 2000L, 5000L, 10000L, 30000L};

    /** 心跳间隔 45 秒（§40：30~60 秒一次即可） */
    private static final long HEARTBEAT_MS = 45_000L;

    /** 监督周期：5 秒一次体检（存活校验 / 挂死连接兜底 / 断线兜底重连） */
    private static final long SUPERVISE_MS = 5_000L;

    /** 单次连接尝试上限：超过视为挂死，废弃该连接并重新退避 */
    private static final long CONNECT_TIMEOUT_MS = 30_000L;

    /** 存活判定窗口：连续两个心跳周期无任何入站数据即判定断链 */
    private static final long LIVENESS_TIMEOUT_MS = HEARTBEAT_MS * 2 + 15_000L;

    private final URI serverUri;
    private final Callback callback;
    private final ScheduledExecutorService scheduler;

    /** 连接尝试在途标记：保证同一时刻只有一个连接（禁止并发重复连接） */
    private final AtomicBoolean connectInFlight = new AtomicBoolean(false);

    /** 调度互斥锁：保护 reconnectTask 的去重判定 */
    private final Object scheduleLock = new Object();

    private volatile State state = State.DISCONNECTED;
    private volatile boolean stopped = false;
    private volatile int backoffIndex = 0;

    /** 连接代际号：每次新建连接自增，旧代际回调一律丢弃 */
    private volatile long generation = 0L;

    private volatile Connection connection;

    private volatile long connectStartedMs = 0L;
    private volatile long lastInboundMs = 0L;
    private volatile long heartbeatNextMs = 0L;

    private ScheduledFuture<?> reconnectTask;
    private ScheduledFuture<?> superviseTask;

    public WebSocketClientWrapper(URI serverUri, Callback callback) {
        this.serverUri = serverUri;
        this.callback = callback;
        this.scheduler = Executors.newSingleThreadScheduledExecutor(
                new ThreadFactory() {
                    @Override
                    public Thread newThread(Runnable r) {
                        Thread t = new Thread(r, "cliplink-ws-scheduler");
                        t.setDaemon(true);
                        return t;
                    }
                });
    }

    // ---- 对外 API ------------------------------------------------------------

    /**
     * 启动监督并立即发起一次连接（可重复调用，幂等）。
     */
    public synchronized void start() {
        stopped = false;
        if (superviseTask == null) {
            startSuperviseLoop();
        }
        attemptConnect();
    }

    /**
     * 停止：关闭当前连接并禁止后续自动重连（本地主动停止）。
     */
    public synchronized void stop() {
        stopped = true;
        generation++; // 使在途连接的后续回调全部失效
        cancelTask(reconnectTask);
        reconnectTask = null;
        cancelTask(superviseTask);
        superviseTask = null;
        connectInFlight.set(false);
        retireConnection();
        setState(State.DISCONNECTED);
        scheduler.shutdownNow();
    }

    /**
     * 主动催一次连接自愈（网络恢复 / 回到前台 / 用户点击状态条时调用）：
     * 已连接或连接在途时不动，否则取消等待中的退避并立即重连。
     */
    public synchronized void ensureConnected() {
        if (stopped || state == State.CONNECTED || connectInFlight.get()) {
            return;
        }
        synchronized (scheduleLock) {
            cancelTask(reconnectTask);
            reconnectTask = null;
        }
        backoffIndex = 0; // 外部明确事件触发，跳过剩余退避等待，但仍保留退避序列
        Log.i(TAG, "外部触发连接自愈，立即重连");
        attemptConnect();
    }

    /**
     * 发送文本消息（UTF-8 语义由字符串编码保证，见需求 §79）。
     *
     * @return true 表示已交给连接发送；未连接或异常返回 false
     */
    public boolean sendText(String text) {
        if (text == null) {
            return false;
        }
        Connection c = connection;
        if (c == null || !c.isOpen()) {
            return false;
        }
        try {
            c.send(text);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "WebSocket 发送失败: " + e.getMessage());
            // 写失败视为断链，交由监督线程强制重建（不等待 onClose）
            markBroken("send failed");
            return false;
        }
    }

    public boolean isOpen() {
        Connection c = connection;
        return c != null && c.isOpen();
    }

    public State getState() {
        return state;
    }

    // ---- 监督循环（单线程串行）------------------------------------------------

    private void startSuperviseLoop() {
        schedule(new Runnable() {
            @Override
            public void run() {
                supervise();
            }
        }, SUPERVISE_MS, SUPERVISE_MS, true);
    }

    /** 周期性体检：心跳 + 存活判定 + 挂死连接兜底 + 断线兜底重连 */
    private void supervise() {
        if (stopped) {
            return;
        }
        long now = System.currentTimeMillis();

        if (state == State.CONNECTED) {
            heartbeatTick(now);
            long silent = now - lastInboundMs;
            if (silent > LIVENESS_TIMEOUT_MS) {
                Log.w(TAG, "心跳/数据静默 " + silent + "ms 超过阈值，判定断链并强制重建");
                markBroken("liveness timeout");
            }
            return;
        }

        if (connectInFlight.get()) {
            long elapsed = now - connectStartedMs;
            if (elapsed > CONNECT_TIMEOUT_MS) {
                Log.w(TAG, "连接尝试 " + elapsed + "ms 未完成，判定挂死并重新调度");
                generation++;
                connectInFlight.set(false);
                retireConnection();
                setState(State.DISCONNECTED);
                scheduleReconnect();
            }
            return;
        }

        // 断开且无连接在途：不依赖 onClose，监督线程兜底补一次重连
        Log.i(TAG, "监督发现当前无连接且无待执行重连，兜底调度重连");
        scheduleReconnect();
    }

    /** 心跳：每 HEARTBEAT_MS 发一次 Ping（Pong 在 onWebsocketPong 中记活） */
    private void heartbeatTick(long now) {
        if (now < heartbeatNextMs) {
            return;
        }
        heartbeatNextMs = now + HEARTBEAT_MS;
        Connection c = connection;
        if (c == null || !c.isOpen()) {
            return;
        }
        try {
            c.sendPing(); // Ping/Pong 心跳（§40）
        } catch (Exception e) {
            Log.w(TAG, "心跳发送失败: " + e.getMessage());
            markBroken("ping failed");
        }
    }

    /** 判定当前连接不可用：废弃连接并立即重建（不等待 onClose） */
    private synchronized void markBroken(String reason) {
        if (stopped) {
            return;
        }
        if (state == State.DISCONNECTED && !connectInFlight.get()) {
            return; // 已经不在连接态且无在途尝试，无需重复处理
        }
        Log.i(TAG, "连接判定失效（" + reason + "），执行强制重建");
        generation++;
        connectInFlight.set(false);
        retireConnection();
        setState(State.DISCONNECTED);
        backoffIndex = 0;
        scheduleReconnect(0L); // 明确的断链事件：立即重连，不再等待退避
    }

    // ---- 连接建立 -----------------------------------------------------------

    private synchronized void attemptConnect() {
        if (stopped) {
            return;
        }
        if (!connectInFlight.compareAndSet(false, true)) {
            return; // 已有连接尝试在途：禁止并发重复连接
        }
        if (state == State.CONNECTED) {
            connectInFlight.set(false);
            return;
        }
        long gen = ++generation;
        retireConnection(); // 丢弃可能存在的僵尸连接（其回调因代际过期被忽略）

        Connection c = new Connection(this, gen);
        connection = c;
        connectStartedMs = System.currentTimeMillis();
        lastInboundMs = connectStartedMs;
        setState(State.CONNECTING);
        try {
            c.connect(); // 异步连接；结果经 onOpen/onClose/onError 回调
        } catch (Exception e) {
            Log.e(TAG, "WebSocket 连接发起失败: " + e.getMessage());
            connectInFlight.set(false);
            setState(State.DISCONNECTED);
            scheduleReconnect();
        }
    }

    /** 关闭并丢弃当前连接（代际号已先行自增时，其回调会被忽略） */
    private void retireConnection() {
        Connection c = connection;
        connection = null;
        if (c == null) {
            return;
        }
        try {
            c.closeConnection(CloseFrame.ABNORMAL_CLOSE, "cliplink retry");
        } catch (Exception e) {
            Log.w(TAG, "关闭旧连接异常: " + e.getMessage());
        }
        try {
            c.close(); // 兜底：极端状态下未建立完成时确保释放
        } catch (Exception ignored) {
            // 忽略：连接已废弃
        }
    }

    // ---- 重连调度（退避 + 去重）----------------------------------------------

    private void scheduleReconnect() {
        scheduleReconnect(-1L);
    }

    /**
     * @param fixedDelayMs >= 0 时使用固定延迟（0 表示立即）；-1 表示按退避序列
     */
    private void scheduleReconnect(long fixedDelayMs) {
        if (stopped) {
            return;
        }
        synchronized (scheduleLock) {
            if (reconnectTask != null && !reconnectTask.isDone()) {
                return; // 已有待执行重连，避免重复调度（防并发重复连接）
            }
            long delay;
            if (fixedDelayMs >= 0) {
                delay = fixedDelayMs;
            } else {
                delay = BACKOFF_MS[Math.min(backoffIndex, BACKOFF_MS.length - 1)];
                if (backoffIndex < BACKOFF_MS.length - 1) {
                    backoffIndex++;
                }
            }
            Log.i(TAG, "WebSocket 将在 " + delay + "ms 后重连（退避档 "
                    + backoffIndex + "）");
            reconnectTask = schedule(new Runnable() {
                @Override
                public void run() {
                    attemptConnect();
                }
            }, delay, -1L, false);
        }
    }

    private ScheduledFuture<?> schedule(Runnable task, long initialDelayMs,
                                        long periodMs, boolean periodic) {
        try {
            if (periodic) {
                // 固定延迟：进程被冻结后不会补跑积压任务（避免恢复瞬间心跳/体检风暴）
                return scheduler.scheduleWithFixedDelay(task, initialDelayMs,
                        periodMs, TimeUnit.MILLISECONDS);
            }
            return scheduler.schedule(task, initialDelayMs, TimeUnit.MILLISECONDS);
        } catch (RejectedExecutionException e) {
            Log.w(TAG, "连接监督调度被拒绝（已停止）: " + e.getMessage());
            return null;
        }
    }

    private void setState(State newState) {
        synchronized (scheduleLock) {
            if (state == newState) {
                return;
            }
            state = newState;
        }
        callback.onStateChanged(newState);
    }

    private static void cancelTask(ScheduledFuture<?> task) {
        if (task != null && !task.isDone()) {
            task.cancel(false);
        }
    }

    // ---- 单次连接实例（不可复用的 WebSocketClient）----------------------------

    /**
     * 一次连接尝试的实现：每次重连都新建实例（WebSocketClient 实例不可复用）。
     * 所有回调先校验代际，代际过期的旧连接事件一律丢弃。
     */
    private static final class Connection extends WebSocketClient {

        private final WebSocketClientWrapper owner;
        private final long gen;

        Connection(WebSocketClientWrapper owner, long gen) {
            super(owner.serverUri);
            this.owner = owner;
            this.gen = gen;
            // 关闭库内置连接丢失检测：存活判定统一由监督线程负责（§40 心跳）
            setConnectionLostTimeout(0);
            setTcpNoDelay(true);
        }

        private boolean stale() {
            return owner.stopped || gen != owner.generation;
        }

        @Override
        public void onOpen(ServerHandshake handshake) {
            if (stale()) {
                return;
            }
            owner.connectInFlight.set(false);
            owner.backoffIndex = 0; // 连接成功重置退避（§39）
            long now = System.currentTimeMillis();
            owner.lastInboundMs = now;
            owner.heartbeatNextMs = now + HEARTBEAT_MS;
            owner.setState(State.CONNECTED);
            Log.i(TAG, "WebSocket 已连接: " + getURI());
            owner.callback.onConnected();
        }

        @Override
        public void onMessage(String message) {
            if (stale()) {
                return;
            }
            owner.lastInboundMs = System.currentTimeMillis(); // 业务数据同样算存活
            owner.callback.onMessage(message);
        }

        @Override
        public void onClose(int code, String reason, boolean remote) {
            if (stale()) {
                return;
            }
            owner.connectInFlight.set(false);
            owner.setState(State.DISCONNECTED);
            Log.i(TAG, "WebSocket 断开: code=" + code + ", reason=" + reason
                    + ", remote=" + remote);
            owner.callback.onDisconnected(code, reason);
            owner.scheduleReconnect(); // 退避重连（§39）
        }

        @Override
        public void onError(Exception ex) {
            if (stale()) {
                return;
            }
            Log.w(TAG, "WebSocket 错误: " + ex.getMessage());
            // §72 不崩溃；§91 不依赖后置 onClose：立即调度重连，监督线程兜底
            owner.connectInFlight.set(false);
            owner.setState(State.DISCONNECTED);
            owner.scheduleReconnect();
        }

        @Override
        public void onWebsocketPong(org.java_websocket.WebSocket conn,
                                    Framedata f) {
            super.onWebsocketPong(conn, f);
            if (stale()) {
                return;
            }
            owner.lastInboundMs = System.currentTimeMillis(); // Pong 记活（§40）
        }
    }
}
