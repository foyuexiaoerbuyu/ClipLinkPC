// ---------------------------------------------------------------------------
// WebSocket 服务端实现（纯 Winsock2，RFC 6455）
// - accept 线程（select 500ms）+ 每连接一个接收线程（§56 线程模型）
// - 协议处理：register 登记在线设备表 / clipboard 分配 serverSeq 后
//   clipboard_ack 回来源、广播排除来源设备（§42-§47 / §68）/ sync_request
//   增量补历史（§50）/ 非法请求回 error（§72），eventId 幂等防重（§87）
// - 事件持久化：server_clipboard_event 表（§31，与客户端库同库不同表），
//   数据库不可用时降级内存环形缓冲，端口占用/数据库失败均不崩溃（§67/§72）
// ---------------------------------------------------------------------------

#include "sync/WebSocketServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/DataTypes.h"
#include "database/DatabaseManager.h"
#include "sync/SyncProtocol.h"
#include "sync/WsCommon.h"
#include "util/Logger.h"
#include "util/sha256.h"

namespace cliplink {
namespace sync {

using cliplink::database::DatabaseManager;
using cliplink::util::Sha256;

namespace {

constexpr const char* kTag = "WebSocketServer";

constexpr size_t kMaxMessageBytes = 8u * 1024u * 1024u;   // §74 单条消息上限
constexpr size_t kMaxContentBytes = 1024u * 1024u;        // §73 内容上限 1MB
constexpr size_t kMaxHandshakeBytes = 16u * 1024u;        // 握手头上限
constexpr size_t kSyncBatchEvents = 200;                  // 补历史单批条数
constexpr size_t kSyncBatchBytes = 512u * 1024u;          // 补历史单批字节
constexpr int kMemoryRingLimit = 1000;                    // 降级内存上限
constexpr int kAcceptTimeoutMs = 500;
constexpr int kHandshakeTimeoutMs = 10000;

// ---------------------------------------------------------------------------
// 服务端事件存储：server_seq 分配 + eventId 幂等（§87）+ 增量查询（§50）
// ---------------------------------------------------------------------------
class ServerEventStore {
public:
    static ServerEventStore& instance() {
        static ServerEventStore store;
        return store;
    }

    struct Result {
        bool inserted = false;       // false = eventId 已存在（幂等命中）
        ClipboardEvent event;        // 携带最终 serverSeq / serverTime
    };

    Result persist(const ClipboardEvent& input) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureTableLocked();

        Result res;
        res.event = input;

        // ---- 路径 A：SQLite（§31 表结构）--------------------------------
        if (DatabaseManager::instance().available()) {
            // 1) eventId 幂等：已存在直接返回既有 serverSeq（§87）
            {
                DatabaseManager::Statement q(
                    DatabaseManager::instance(),
                    "SELECT server_seq, created_at FROM server_clipboard_event"
                    " WHERE event_id = ?");
                if (q.valid()) {
                    q.bindText(1, input.eventId);
                    if (q.step() == SQLITE_ROW) {
                        res.event.serverSeq = q.columnInt64(0);
                        res.event.serverTime = q.columnInt64(1);
                        res.inserted = false;
                        return res;
                    }
                }
            }
            // 2) 分配单调递增 serverSeq（§22）
            int64_t maxSeq = 0;
            {
                DatabaseManager::Statement q(
                    DatabaseManager::instance(),
                    "SELECT COALESCE(MAX(server_seq), 0)"
                    " FROM server_clipboard_event");
                if (q.valid() && q.step() == SQLITE_ROW) {
                    maxSeq = q.columnInt64(0);
                }
            }
            const int64_t seq = maxSeq + 1;
            const int64_t serverTime = nowUnixMillis();
            {
                DatabaseManager::Statement ins(
                    DatabaseManager::instance(),
                    "INSERT INTO server_clipboard_event("
                    "server_seq, event_id, device_id, device_type, device_name,"
                    " content_type, content_hash, content, client_time,"
                    " created_at) VALUES (?,?,?,?,?,?,?,?,?,?)");
                if (!ins.valid()) break_insert_fallback();
                ins.bindInt64(1, seq);
                ins.bindText(2, input.eventId);
                ins.bindText(3, input.deviceId);
                ins.bindText(4, input.deviceType);
                ins.bindText(5, input.deviceName);
                ins.bindText(6, std::string(contentTypeToString(input.contentType)));
                ins.bindText(7, input.contentHash);
                ins.bindText(8, input.content);
                ins.bindInt64(9, static_cast<int64_t>(input.clientTime));
                ins.bindInt64(10, serverTime);
                if (ins.step() == SQLITE_DONE) {
                    res.event.serverSeq = seq;
                    res.event.serverTime = serverTime;
                    res.inserted = true;
                    return res;
                }
            }
            // INSERT 失败（磁盘满等）：降级走内存，保证同步不断（§72）
            CLIPLOG_WARN(kTag,
                         "server_clipboard_event 插入失败，降级内存存储");
        }

        // ---- 路径 B：内存降级（环形上限 1000）---------------------------
        for (const auto& e : memory_) {
            if (e.eventId == input.eventId) {
                res.event = e;
                res.inserted = false;
                return res;
            }
        }
        if (memory_.size() >= static_cast<size_t>(kMemoryRingLimit)) {
            memory_.erase(memory_.begin());
        }
        const int64_t seq =
            memory_.empty() ? 1 : memory_.back().serverSeq + 1;
        const int64_t serverTime = nowUnixMillis();
        res.event.serverSeq = seq;
        res.event.serverTime = serverTime;
        memory_.push_back(res.event);
        res.inserted = true;
        return res;
    }

    // server_seq > afterSeq 的事件，按 seq ASC，受条数/字节双限制（§50）
    std::vector<ClipboardEvent> eventsAfter(int64_t afterSeq) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureTableLocked();
        std::vector<ClipboardEvent> out;

        if (DatabaseManager::instance().available()) {
            DatabaseManager::Statement q(
                DatabaseManager::instance(),
                "SELECT event_id, device_id, device_type, device_name,"
                " content_type, content_hash, content, client_time,"
                " created_at, server_seq"
                " FROM server_clipboard_event WHERE server_seq > ?"
                " ORDER BY server_seq ASC");
            if (q.valid()) {
                q.bindInt64(1, afterSeq);
                size_t bytes = 0;
                while (q.step() == SQLITE_ROW) {
                    ClipboardEvent ev;
                    ev.eventId = q.columnText(0);
                    ev.deviceId = q.columnText(1);
                    ev.deviceType = q.columnText(2);
                    ev.deviceName = q.columnText(3);
                    ev.contentType =
                        contentTypeFromString(q.columnText(4));
                    ev.contentHash = q.columnText(5);
                    ev.content = q.columnText(6);
                    ev.clientTime = static_cast<uint64_t>(q.columnInt64(7));
                    ev.serverTime = static_cast<uint64_t>(q.columnInt64(8));
                    ev.serverSeq = q.columnInt64(9);
                    ev.source = ClipboardSource::REMOTE;
                    ev.syncStatus = SyncStatus::SYNCED;
                    bytes += ev.content.size() + 256;
                    out.push_back(std::move(ev));
                    if (out.size() >= kSyncBatchEvents ||
                        bytes >= kSyncBatchBytes) {
                        break;
                    }
                }
                return out;
            }
        }

        size_t bytes = 0;
        for (const auto& e : memory_) {
            if (e.serverSeq <= afterSeq) continue;
            bytes += e.content.size() + 256;
            out.push_back(e);
            if (out.size() >= kSyncBatchEvents || bytes >= kSyncBatchBytes) {
                break;
            }
        }
        return out;
    }

private:
    ServerEventStore() = default;

    void ensureTableLocked() {
        if (tableReady_) return;
        if (!DatabaseManager::instance().available()) return;
        std::string err;
        const bool ok = DatabaseManager::instance().exec(
            "CREATE TABLE IF NOT EXISTS server_clipboard_event ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  server_seq INTEGER NOT NULL UNIQUE,"
            "  event_id TEXT NOT NULL UNIQUE,"
            "  device_id TEXT NOT NULL,"
            "  device_type TEXT NOT NULL,"
            "  device_name TEXT,"
            "  content_type TEXT NOT NULL,"
            "  content_hash TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  client_time INTEGER,"
            "  created_at INTEGER NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_server_event_seq"
            " ON server_clipboard_event(server_seq);",
            &err);
        if (ok) {
            tableReady_ = true;
        } else {
            CLIPLOG_WARN(kTag, "建表失败（降级内存存储）: " + err);
        }
    }

    // 语法占位：编译期要求所有插入路径显式收口
    static void break_insert_fallback() {}

    std::mutex mutex_;
    bool tableReady_ = false;
    std::vector<ClipboardEvent> memory_;
};

// ---------------------------------------------------------------------------
// 连接对象（收发线程共享）
// ---------------------------------------------------------------------------
struct Conn {
    SOCKET sock = INVALID_SOCKET;
    int id = 0;
    std::mutex sendMutex;
    std::atomic<bool> closed{false};
    std::string deviceId;   // register 后绑定；空 = 未注册（§42）
    std::string deviceName;
    std::string deviceType;
    std::mutex metaMutex;   // 保护 deviceId/deviceName/deviceType
};

}  // namespace

// ---------------------------------------------------------------------------
// WebSocketServer::Impl
// ---------------------------------------------------------------------------
class WebSocketServer::Impl {
public:
    // WebSocketServer 外壳类直接访问内部状态（嵌套类访问约定）
    friend class WebSocketServer;

    bool start(int port);
    void stop();
    size_t connectionCount();
    size_t onlineDeviceCount();
    std::string lastError();

private:
    void acceptLoop();
    void serveConn(const std::shared_ptr<Conn>& conn);
    bool performHandshake(const std::shared_ptr<Conn>& conn,
                          std::string& leftover);
    void handleMessage(const std::shared_ptr<Conn>& conn,
                       const std::string& text);
    bool sendTextMessage(const std::shared_ptr<Conn>& conn,
                         const std::string& text);
    void broadcast(const std::string& text, const Conn* exclude);
    void closeConn(const std::shared_ptr<Conn>& conn);
    void setError(const std::string& err);

    std::atomic<int> port_{constants::kDefaultPort};
    std::atomic<bool> running_{false};
    SOCKET listenSock_ = INVALID_SOCKET;
    std::thread acceptThread_;

    mutable std::mutex connsMutex_;
    std::vector<std::shared_ptr<Conn>> conns_;
    std::atomic<int> nextConnId_{1};

    // 接收线程计数：stop() 等待全部退出，防止析构竞态
    std::mutex workersMutex_;
    std::condition_variable workersCv_;
    int activeWorkers_ = 0;

    mutable std::mutex errMutex_;
    std::string lastError_;
};

bool WebSocketServer::Impl::start(int port) {
    ws::ensureWinsock();
    port_.store(port);

    const SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        setError("创建监听 socket 失败, err=" +
                 std::to_string(WSAGetLastError()));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 0.0.0.0（§34）
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        const int err = WSAGetLastError();
        const std::string msg = (err == WSAEADDRINUSE || err == WSAEACCES)
            ? "端口 " + std::to_string(port) +
                  " 已被占用或无权限（请在设置中更换端口）"
            : "bind 失败, err=" + std::to_string(err);
        setError(msg);
        CLIPLOG_ERROR(kTag, msg);  // §67：提示用户，不崩溃
        ::closesocket(ls);
        return false;
    }
    if (::listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        setError("listen 失败, err=" + std::to_string(WSAGetLastError()));
        ::closesocket(ls);
        return false;
    }

    // 防 accept 阻塞在无法察觉的关闭上
    ws::setSocketTimeouts(ls, 1000, 1000);
    listenSock_ = ls;
    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    CLIPLOG_INFO(kTag, "服务端已监听 0.0.0.0:" + std::to_string(port));
    return true;
}

void WebSocketServer::Impl::stop() {
    if (!running_.exchange(false)) {
        if (acceptThread_.joinable()) acceptThread_.join();
        return;
    }
    const SOCKET ls = listenSock_;
    listenSock_ = INVALID_SOCKET;
    if (ls != INVALID_SOCKET) ::closesocket(ls);  // 打断 accept/select
    if (acceptThread_.joinable()) acceptThread_.join();

    // 关闭全部连接，唤醒接收线程
    std::vector<std::shared_ptr<Conn>> snapshot;
    {
        std::lock_guard<std::mutex> lock(connsMutex_);
        snapshot = conns_;
    }
    for (const auto& c : snapshot) closeConn(c);

    // 等待接收线程全部退出（最多 5s）
    std::unique_lock<std::mutex> lock(workersMutex_);
    workersCv_.wait_for(lock, std::chrono::seconds(5),
                        [this] { return activeWorkers_ == 0; });
    lock.unlock();
    {
        std::lock_guard<std::mutex> connsLock(connsMutex_);
        conns_.clear();
    }
    ws::releaseWinsock();
    CLIPLOG_INFO(kTag, "服务端已停止");
}

size_t WebSocketServer::Impl::connectionCount() {
    std::lock_guard<std::mutex> lock(connsMutex_);
    size_t n = 0;
    for (const auto& c : conns_) {
        if (!c->closed.load()) ++n;
    }
    return n;
}

size_t WebSocketServer::Impl::onlineDeviceCount() {
    std::lock_guard<std::mutex> lock(connsMutex_);
    size_t n = 0;
    for (const auto& c : conns_) {
        if (c->closed.load()) continue;
        std::lock_guard<std::mutex> meta(c->metaMutex);
        if (!c->deviceId.empty()) ++n;
    }
    return n;
}

std::string WebSocketServer::Impl::lastError() {
    std::lock_guard<std::mutex> lock(errMutex_);
    return lastError_;
}

void WebSocketServer::Impl::setError(const std::string& err) {
    std::lock_guard<std::mutex> lock(errMutex_);
    lastError_ = err;
}

void WebSocketServer::Impl::acceptLoop() {
    while (running_.load()) {
        const SOCKET ls = listenSock_;
        if (ls == INVALID_SOCKET) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kAcceptTimeoutMs * 1000;
        const int r = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (r == SOCKET_ERROR) {
            if (!running_.load()) break;
            const int err = WSAGetLastError();
            if (err == WSAENOTSOCK || err == WSAEINVAL) break;
            CLIPLOG_WARN(kTag, "select 失败, err=" + std::to_string(err));
            continue;
        }
        if (r == 0) continue;

        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        const SOCKET cs = ::accept(ls, reinterpret_cast<sockaddr*>(&peer),
                                   &peerLen);
        if (cs == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        if (!running_.load()) {
            ::closesocket(cs);
            break;
        }
        ws::setSocketTimeouts(cs, kHandshakeTimeoutMs, kHandshakeTimeoutMs);

        auto conn = std::make_shared<Conn>();
        conn->sock = cs;
        conn->id = nextConnId_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(connsMutex_);
            conns_.push_back(conn);
        }
        {
            std::lock_guard<std::mutex> lock(workersMutex_);
            ++activeWorkers_;
        }
        std::thread([this, conn] {
            serveConn(conn);
            closeConn(conn);
            {
                std::lock_guard<std::mutex> lock(workersMutex_);
                --activeWorkers_;
            }
            workersCv_.notify_all();
        }).detach();
    }
}

void WebSocketServer::Impl::closeConn(const std::shared_ptr<Conn>& conn) {
    bool expected = false;
    if (conn->closed.compare_exchange_strong(expected, true)) {
        const SOCKET s = conn->sock;
        conn->sock = INVALID_SOCKET;
        if (s != INVALID_SOCKET) ::closesocket(s);
        std::lock_guard<std::mutex> lock(connsMutex_);
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [&](const std::shared_ptr<Conn>& c) {
                                        return c.get() == conn.get();
                                    }),
                     conns_.end());
    }
}

bool WebSocketServer::Impl::sendTextMessage(const std::shared_ptr<Conn>& conn,
                                            const std::string& text) {
    if (conn->closed.load()) return false;
    const auto frame =
        ws::encodeFrame(ws::Opcode::Text,
                        reinterpret_cast<const uint8_t*>(text.data()),
                        text.size(), /*fin=*/true, /*masked=*/false);
    std::lock_guard<std::mutex> lock(conn->sendMutex);
    const SOCKET s = conn->sock;
    if (s == INVALID_SOCKET) return false;
    return ws::sendAll(s, reinterpret_cast<const char*>(frame.data()),
                       frame.size());
}

void WebSocketServer::Impl::broadcast(const std::string& text,
                                      const Conn* exclude) {
    const auto frame =
        ws::encodeFrame(ws::Opcode::Text,
                        reinterpret_cast<const uint8_t*>(text.data()),
                        text.size(), true, false);
    // 先摘出排除目标的 deviceId，循环内只持单锁，避免 AB-BA 锁序死锁
    // （exclude 为 const 观察指针，成员仅在锁下读取，const_cast 不改状态）
    Conn* const ex = const_cast<Conn*>(exclude);
    std::string excludeDeviceId;
    if (ex != nullptr) {
        std::lock_guard<std::mutex> exMeta(ex->metaMutex);
        excludeDeviceId = ex->deviceId;
    }
    std::vector<std::shared_ptr<Conn>> snapshot;
    {
        std::lock_guard<std::mutex> lock(connsMutex_);
        snapshot = conns_;
    }
    for (const auto& c : snapshot) {
        if (c->closed.load()) continue;
        if (exclude != nullptr && c.get() == exclude) continue;
        if (!excludeDeviceId.empty()) {
            // §45/§68：绝不回发来源设备（连接或 deviceId 任一命中即排除）
            std::lock_guard<std::mutex> meta(c->metaMutex);
            if (!c->deviceId.empty() && c->deviceId == excludeDeviceId) {
                continue;
            }
        }
        std::lock_guard<std::mutex> sendLock(c->sendMutex);
        const SOCKET s = c->sock;
        if (s == INVALID_SOCKET) continue;
        if (!ws::sendAll(s, reinterpret_cast<const char*>(frame.data()),
                         frame.size())) {
            CLIPLOG_WARN(kTag,
                         "广播发送失败, conn=" + std::to_string(c->id));
        }
    }
}

bool WebSocketServer::Impl::performHandshake(const std::shared_ptr<Conn>& conn,
                                             std::string& leftover) {
    const SOCKET s = conn->sock;
    std::string head;
    char buf[2048];
    while (head.find("\r\n\r\n") == std::string::npos) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        head.append(buf, static_cast<size_t>(n));
        if (head.size() > kMaxHandshakeBytes) {
            CLIPLOG_WARN(kTag, "握手头过大, conn=" + std::to_string(conn->id));
            return false;
        }
    }
    const size_t headEnd = head.find("\r\n\r\n") + 4;
    const std::string headBlock = head.substr(0, headEnd);
    leftover = head.substr(headEnd);

    ws::HttpHeaders headers;
    if (!ws::parseHttpHeaders(headBlock, headers)) return false;
    if (headers.firstLine.rfind("GET ", 0) != 0) return false;
    if (ws::toLowerAscii(headers.value("upgrade")) != "websocket") {
        return false;
    }
    if (!ws::headerContainsToken(headers.value("connection"), "upgrade")) {
        return false;
    }
    const std::string* key = headers.find("sec-websocket-key");
    if (key == nullptr || key->empty()) return false;

    const std::string response = ws::buildServerHandshakeResponse(*key);
    if (!ws::sendAll(s, response.data(), response.size())) return false;
    CLIPLOG_DEBUG(kTag, "握手完成, conn=" + std::to_string(conn->id));
    return true;
}

void WebSocketServer::Impl::serveConn(const std::shared_ptr<Conn>& conn) {
    const SOCKET s = conn->sock;
    if (s == INVALID_SOCKET) return;

    std::string leftover;
    if (!performHandshake(conn, leftover)) {
        CLIPLOG_WARN(kTag, "握手失败, conn=" + std::to_string(conn->id));
        return;
    }
    // 握手后进入 select 驱动读取，重置超时（服务端不做 ping，由客户端心跳）
    ws::setSocketTimeouts(s, kHandshakeTimeoutMs, 0);

    ws::MessageReader reader(kMaxMessageBytes);
    if (!leftover.empty()) reader.append(leftover.data(), leftover.size());

    while (running_.load() && !conn->closed.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kAcceptTimeoutMs * 1000;
        const int r = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (r == SOCKET_ERROR) break;
        if (r == 0) continue;

        char buf[8192];
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n == 0) {
            // 对端有序关闭（§41）：正常断开
            CLIPLOG_DEBUG(kTag,
                          "对端关闭连接, conn=" + std::to_string(conn->id));
            break;
        }
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (ws::isTimedOutSocket()) {
                // SO_RCVTIMEO 空闲接收超时（10s）：链路仍健康，仅本轮无数据。
                // 客户端心跳周期 45s > 10s，绝不能误判为断开（否则每 10s 闪断）。
                continue;
            }
            // 其余错误（WSAECONNRESET / WSAENOTSOCK 等）才是链路异常
            CLIPLOG_DEBUG(kTag,
                          "recv 失败, err=" + std::to_string(err) +
                              ", conn=" + std::to_string(conn->id));
            break;
        }
        reader.append(buf, static_cast<size_t>(n));

        ws::MessageReader::Item item;
        std::string payload;
        bool protocolError = false;
        while (reader.next(item, payload)) {
            switch (item) {
                case ws::MessageReader::Item::Text:
                    handleMessage(conn, payload);
                    break;
                case ws::MessageReader::Item::Ping: {
                    const auto pong = ws::encodeFrame(
                            ws::Opcode::Pong,
                            reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size(), true, false);
                        std::lock_guard<std::mutex> lock(conn->sendMutex);
                        if (conn->sock != INVALID_SOCKET) {
                            ws::sendAll(conn->sock,
                                        reinterpret_cast<const char*>(
                                            pong.data()),
                                        pong.size());
                        }
                    }
                    break;
                case ws::MessageReader::Item::Pong:
                    break;  // 忽略（服务端不主动发起心跳）
                case ws::MessageReader::Item::Close: {
                    const auto closeFrame = ws::encodeFrame(
                        ws::Opcode::Close, nullptr, 0, true, false);
                    std::lock_guard<std::mutex> lock(conn->sendMutex);
                    if (conn->sock != INVALID_SOCKET) {
                        ws::sendAll(conn->sock,
                                    reinterpret_cast<const char*>(
                                        closeFrame.data()),
                                    closeFrame.size());
                    }
                    return;
                }
                case ws::MessageReader::Item::Binary:
                    sendTextMessage(
                        conn, buildErrorMessage("unsupported_type",
                                                "MVP 仅支持文本消息"));
                    break;
                case ws::MessageReader::Item::Error:
                default:
                    protocolError = true;
                    break;
            }
            if (protocolError) break;
        }
        if (protocolError || reader.failed()) {
            CLIPLOG_WARN(kTag,
                         "协议错误: " + reader.errorMessage() +
                             ", conn=" + std::to_string(conn->id));
            break;  // §72：超限/非法仅断开本连接，服务器继续运行
        }
    }
}

void WebSocketServer::Impl::handleMessage(const std::shared_ptr<Conn>& conn,
                                          const std::string& text) {
    ParsedMessage msg;
    std::string parseError;
    if (!parseMessage(text, msg, &parseError)) {
        sendTextMessage(conn, buildErrorMessage("bad_message", parseError));
        return;
    }

    switch (msg.type) {
        // ---- register：登记 deviceId -> 在线设备表（§42/§55）----------
        case MessageType::Register: {
            const std::string deviceId = msg.data.get("deviceId").asString();
            if (deviceId.empty()) {
                sendTextMessage(conn,
                                buildErrorMessage("bad_register",
                                                  "缺少 deviceId"));
                return;
            }
            {
                std::lock_guard<std::mutex> meta(conn->metaMutex);
                conn->deviceId = deviceId;
                conn->deviceName = msg.data.get("deviceName").asString();
                conn->deviceType = msg.data.get("deviceType").asString();
            }
            CLIPLOG_INFO(kTag,
                         "设备上线: " +
                             msg.data.get("deviceName").asString() +
                             " (" + deviceId + ")");
            return;
        }

        // ---- clipboard：分配 serverSeq -> ack 来源 -> 广播其他设备 -----
        case MessageType::Clipboard: {
            std::string ownDeviceId;
            {
                std::lock_guard<std::mutex> meta(conn->metaMutex);
                ownDeviceId = conn->deviceId;
            }
            if (ownDeviceId.empty()) {
                sendTextMessage(conn, buildErrorMessage(
                                           "not_registered",
                                           "请先发送 register 消息"));
                return;
            }
            ClipboardEvent ev;
            std::string err;
            if (!eventFromJson(msg.data, ev, &err)) {
                sendTextMessage(
                    conn, buildErrorMessage("bad_clipboard", err));
                return;
            }
            if (ev.deviceId != ownDeviceId) {
                sendTextMessage(conn, buildErrorMessage(
                                           "device_mismatch",
                                           "deviceId 与连接注册信息不一致"));
                return;
            }
            if (ev.contentType != ContentType::TEXT) {
                sendTextMessage(conn, buildErrorMessage(
                                           "unsupported_content_type",
                                           "MVP 仅支持 TEXT（§10）"));
                return;
            }
            if (ev.content.size() > kMaxContentBytes) {
                sendTextMessage(conn, buildErrorMessage(
                                           "content_too_large",
                                           "内容超过 1MB 上限"));
                return;
            }
            // hash 校验（§17/§46）：服务器端完整性把关
            if (Sha256::hashHex(ev.content) != ev.contentHash) {
                sendTextMessage(conn, buildErrorMessage("hash_mismatch",
                                                        "contentHash 不匹配"));
                return;
            }

            const ServerEventStore::Result res =
                ServerEventStore::instance().persist(ev);
            // clipboard_ack 只回来源连接（§44）
            sendTextMessage(conn,
                            buildClipboardAckMessage(
                                res.event.eventId, res.event.serverSeq,
                                res.event.serverTime));
            if (res.inserted) {
                CLIPLOG_INFO(kTag,
                             "事件分配 serverSeq=" +
                                 std::to_string(res.event.serverSeq) +
                                 ", 来源=" + ownDeviceId);
                // 广播必须携带 serverSeq（§46 下行模型），且排除来源设备
                broadcast(buildRemoteClipboardMessage(res.event), conn.get());
            }
            // 幂等重复（§87）：仅 ack 复用旧 seq，不重复广播
            return;
        }

        // ---- sync_request：增量补历史（§49-§50），不回发当前剪贴板 ----
        case MessageType::SyncRequest: {
            const int64_t lastSeq =
                msg.data.get("lastServerSeq").asInt64(0);
            const auto events =
                ServerEventStore::instance().eventsAfter(lastSeq);
            sendTextMessage(conn, buildSyncResponseMessage(events));
            CLIPLOG_DEBUG(kTag,
                          "补历史: lastServerSeq=" +
                              std::to_string(lastSeq) +
                              ", 返回 " + std::to_string(events.size()) +
                              " 条");
            return;
        }

        // ---- 客户端 error：仅记录（§72）--------------------------------
        case MessageType::Error:
            CLIPLOG_WARN(kTag,
                         "客户端错误消息: " +
                             msg.data.get("code").asString() + " " +
                             msg.data.get("message").asString());
            return;

        case MessageType::ClipboardAck:
        case MessageType::SyncResponse:
        default:
            sendTextMessage(conn, buildErrorMessage(
                                       "unexpected_message",
                                       "服务端不接受该消息类型"));
            return;
    }
}

// ---------------------------------------------------------------------------
// WebSocketServer 外壳
// ---------------------------------------------------------------------------
WebSocketServer::WebSocketServer() : impl_(new Impl()) {}

WebSocketServer::~WebSocketServer() {
    if (impl_) impl_->stop();
}

void WebSocketServer::setPort(int port) {
    impl_->port_.store(port);
}

bool WebSocketServer::start() {
    return impl_->start(impl_->port_.load());
}

void WebSocketServer::stop() {
    impl_->stop();
}

bool WebSocketServer::isRunning() const {
    return impl_->running_.load();
}

int WebSocketServer::port() const {
    return impl_->port_.load();
}

size_t WebSocketServer::connectionCount() const {
    return impl_->connectionCount();
}

size_t WebSocketServer::onlineDeviceCount() const {
    return impl_->onlineDeviceCount();
}

std::string WebSocketServer::lastError() const {
    return impl_->lastError();
}

}  // namespace sync
}  // namespace cliplink
