// ---------------------------------------------------------------------------
// WebSocket 客户端实现（纯 Winsock2，RFC 6455）
// 见 WebSocketClient.h 注释；对应需求 §37-§40 / §56
// ---------------------------------------------------------------------------

#include "sync/WebSocketClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#include "sync/WsCommon.h"
#include "util/Logger.h"

using cliplink::util::Logger;

namespace cliplink {
namespace sync {

namespace {

constexpr const char* kTag = "WebSocketClient";

// 单条消息上限 8MB（文本上限 1MB + 协议开销，§73-§74）
constexpr size_t kMaxMessageBytes = 8u * 1024u * 1024u;
// 重连退避序列：1s / 2s / 5s / 10s / 30s，之后维持 30s（§39）
constexpr int kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr int kConnectTimeoutMs = 5000;
constexpr int kSendTimeoutMs = 10000;

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parseWsUrl(const std::string& url, ParsedUrl& out) {
    const std::string prefix = "ws://";
    if (url.rfind(prefix, 0) != 0) {
        return false;  // MVP 仅支持明文 ws://（wss 留待后续，§37）
    }
    const std::string rest = url.substr(prefix.size());
    const size_t slash = rest.find('/');
    const std::string hostPort = rest.substr(
        0, slash == std::string::npos ? rest.size() : slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostPort.empty()) return false;

    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostPort.substr(0, colon);
        const std::string portStr = hostPort.substr(colon + 1);
        if (portStr.empty()) return false;
        out.port = std::atoi(portStr.c_str());
        if (out.port <= 0 || out.port > 65535) return false;
    } else {
        out.host = hostPort;
        out.port = 80;
    }
    return !out.host.empty();
}

std::string stateName(ConnState s) {
    switch (s) {
        case ConnState::Connected:    return "已连接";
        case ConnState::Connecting:   return "连接中";
        case ConnState::Disconnected:
        default:                      return "断开";
    }
}

}  // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    stop();
}

void WebSocketClient::setUrl(const std::string& url) {
    url_ = url;
}

void WebSocketClient::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    stateCallback_ = std::move(cb);
}

void WebSocketClient::setMessageCallback(MessageCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    messageCallback_ = std::move(cb);
}

void WebSocketClient::setHeartbeatIntervalMs(int ms) {
    heartbeatMs_.store(ms > 0 ? ms : 0);
}

bool WebSocketClient::start() {
    if (running_.load()) return true;
    if (url_.empty()) {
        setError("服务器地址为空");
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void WebSocketClient::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    wakeCv_.notify_all();
    closeCurrentSocket();  // 打断阻塞中的 connect/recv/handshake
    if (thread_.joinable()) thread_.join();
    closeCurrentSocket();  // 清理循环退出后可能残留的 socket
}

bool WebSocketClient::sendText(const std::string& text) {
    if (!isConnected()) return false;
    return sendFrame(0x1, text.data(), text.size());  // Text 帧
}

std::string WebSocketClient::lastError() const {
    std::lock_guard<std::mutex> lock(errMutex_);
    return lastError_;
}

// ---- 内部实现 -------------------------------------------------------------

void WebSocketClient::setState(ConnState s) {
    ConnState expected = state_.load();
    while (!state_.compare_exchange_weak(expected, s)) {
        if (expected == s) return;  // 已是目标状态，去重
    }
    invokeStateCallback(s);
}

void WebSocketClient::invokeStateCallback(ConnState s) {
    StateCallback cb;
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        cb = stateCallback_;
    }
    if (cb) cb(s);
}

void WebSocketClient::setError(const std::string& err) {
    std::lock_guard<std::mutex> lock(errMutex_);
    lastError_ = err;
}

void WebSocketClient::sleepInterruptible(int ms) {
    std::unique_lock<std::mutex> lock(wakeMutex_);
    wakeCv_.wait_for(lock, std::chrono::milliseconds(ms),
                     [this] { return !running_.load(); });
}

void WebSocketClient::closeCurrentSocket() {
    const uintptr_t raw = sock_.exchange(static_cast<uintptr_t>(-1));
    if (raw != static_cast<uintptr_t>(-1)) {
        ::closesocket(static_cast<SOCKET>(raw));
    }
}

bool WebSocketClient::sendBytes(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    const uintptr_t raw = sock_.load();
    if (raw == static_cast<uintptr_t>(-1)) return false;
    return ws::sendAll(static_cast<SOCKET>(raw),
                       static_cast<const char*>(data), len);
}

bool WebSocketClient::sendFrame(uint8_t opcode, const void* payload,
                                size_t len) {
    const auto frame = ws::encodeFrame(static_cast<ws::Opcode>(opcode),
                                       static_cast<const uint8_t*>(payload),
                                       len, /*fin=*/true, /*masked=*/true);
    return sendBytes(frame.data(), frame.size());
}

bool WebSocketClient::connectAndHandshake(std::string& leftover,
                                          std::string& err) {
    ParsedUrl url;
    if (!parseWsUrl(url_, url)) {
        err = "非法服务器地址（仅支持 ws://host[:port][/path]）: " + url_;
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;  // MVP 局域网 IPv4（§70）
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(url.port);
    if (::getaddrinfo(url.host.c_str(), portStr.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        err = "域名解析失败: " + url.host;
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // 非阻塞 connect + select 超时，避免长时间卡死（§72 网络异常不退出）
        u_long nonBlock = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlock);
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfds;
            fd_set efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            timeval tv{};
            tv.tv_sec = kConnectTimeoutMs / 1000;
            tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
            const int r = ::select(0, nullptr, &wfds, &efds, &tv);
            if (r > 0) {
                int soErr = 0;
                int optLen = sizeof(soErr);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&soErr), &optLen);
                if (soErr == 0) break;  // 连接成功
            }
        }
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(result);

    if (s == INVALID_SOCKET) {
        err = "连接失败: " + url.host + ":" + portStr;
        return false;
    }

    u_long block = 0;
    ::ioctlsocket(s, FIONBIO, &block);  // 恢复阻塞模式
    ws::setSocketTimeouts(s, kSendTimeoutMs, kConnectTimeoutMs);
    sock_.store(static_cast<uintptr_t>(s));

    // ---- RFC 6455 握手：GET Upgrade + Sec-WebSocket-Key -------------------
    const std::string wsKey = ws::randomWebSocketKey();
    const std::string request =
        ws::buildClientHandshakeRequest(url.host, url.port, url.path, wsKey);
    if (!ws::sendAll(s, request.data(), request.size())) {
        err = "握手请求发送失败";
        return false;
    }

    // 读取响应头直到 \r\n\r\n（上限 16KB，防止无界读取）
    std::string head;
    char buf[2048];
    while (head.find("\r\n\r\n") == std::string::npos) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            err = "握手响应读取失败（对端关闭或超时）";
            return false;
        }
        head.append(buf, static_cast<size_t>(n));
        if (head.size() > 16 * 1024) {
            err = "握手响应头过大";
            return false;
        }
    }
    const size_t headEnd = head.find("\r\n\r\n") + 4;
    const std::string headBlock = head.substr(0, headEnd);
    leftover = head.substr(headEnd);

    ws::HttpHeaders headers;
    if (!ws::parseHttpHeaders(headBlock, headers)) {
        err = "握手响应解析失败";
        return false;
    }
    if (headers.firstLine.find(" 101") == std::string::npos) {
        err = "服务器未返回 101 Switching Protocols: " + headers.firstLine;
        return false;
    }
    const std::string upgrade = headers.value("upgrade");
    if (ws::toLowerAscii(upgrade) != "websocket") {
        err = "握手响应缺少 Upgrade: websocket";
        return false;
    }
    if (!ws::headerContainsToken(headers.value("connection"), "upgrade")) {
        err = "握手响应缺少 Connection: Upgrade";
        return false;
    }
    const std::string expected = ws::computeAcceptKey(wsKey);
    if (headers.value("sec-websocket-accept") != expected) {
        err = "Sec-WebSocket-Accept 校验失败";
        return false;
    }

    // 心跳期间降低读超时敏感度：recv 走 select 驱动
    ws::setSocketTimeouts(s, kSendTimeoutMs, 0);
    return true;
}

bool WebSocketClient::recvLoop(const std::string& leftover) {
    const uintptr_t raw = sock_.load();
    if (raw == static_cast<uintptr_t>(-1)) return false;
    const SOCKET s = static_cast<SOCKET>(raw);

    ws::MessageReader reader(kMaxMessageBytes);
    if (!leftover.empty()) {
        reader.append(leftover.data(), leftover.size());
    }

    const int hbMs = heartbeatMs_.load();
    auto lastActivity = std::chrono::steady_clock::now();
    auto lastPing = std::chrono::steady_clock::now();
    char buf[8192];

    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        // ---- 心跳与超时（§40：ping 间隔 45s；2 个周期无响应判定断线）----
        if (hbMs > 0 && now - lastActivity >
                            std::chrono::milliseconds(2ll * hbMs)) {
            setError("心跳超时（" + std::to_string(2 * hbMs / 1000) + "s 无数据）");
            return false;
        }
        if (hbMs > 0 && now - lastPing >= std::chrono::milliseconds(hbMs)) {
            if (!sendFrame(0x9, nullptr, 0)) {  // Ping
                setError("心跳 Ping 发送失败");
                return false;
            }
            lastPing = now;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;  // 500ms 轮询 select（阻塞等待，非轮询网络）
        const int r = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (r == SOCKET_ERROR) {
            setError("select 失败, err=" + std::to_string(WSAGetLastError()));
            return false;
        }
        if (r == 0) continue;

        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            setError("连接被对端关闭, err=" + std::to_string(WSAGetLastError()));
            return false;
        }
        lastActivity = std::chrono::steady_clock::now();
        reader.append(buf, static_cast<size_t>(n));

        ws::MessageReader::Item item;
        std::string payload;
        while (reader.next(item, payload)) {
            switch (item) {
                case ws::MessageReader::Item::Text: {
                    MessageCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(cbMutex_);
                        cb = messageCallback_;
                    }
                    if (cb) cb(payload);
                    break;
                }
                case ws::MessageReader::Item::Ping:
                    if (!sendFrame(0xA, payload.data(), payload.size())) {
                        return false;  // Pong 应答失败
                    }
                    break;
                case ws::MessageReader::Item::Pong:
                    lastActivity = std::chrono::steady_clock::now();
                    break;
                case ws::MessageReader::Item::Close: {
                    // 回 Close 后结束连接（§72 对端关闭不崩溃）
                    sendFrame(0x8, payload.data(),
                              payload.size() > 125 ? 0 : payload.size());
                    setError("收到 Close 帧");
                    return false;
                }
                case ws::MessageReader::Item::Binary:
                    // MVP 仅 TEXT（§10），二进制消息忽略
                    CLIPLOG_WARN(kTag, "忽略二进制消息, bytes=" +
                                           std::to_string(payload.size()));
                    break;
                case ws::MessageReader::Item::Error:
                default:
                    setError("协议错误: " + payload);
                    return false;
            }
            if (!running_.load()) return false;
        }
        if (reader.failed()) {
            setError("协议错误: " + reader.errorMessage());
            return false;
        }
    }
    return false;
}

void WebSocketClient::loop() {
    ws::ensureWinsock();
    int backoffIdx = 0;

    while (running_.load()) {
        setState(ConnState::Connecting);
        std::string leftover;
        std::string err;
        if (connectAndHandshake(leftover, err)) {
            backoffIdx = 0;  // 连接成功重置退避序列（§39）
            if (!running_.load()) {
                closeCurrentSocket();
                break;
            }
            setState(ConnState::Connected);
            CLIPLOG_INFO(kTag, "已连接 " + url_);
            recvLoop(leftover);
            closeCurrentSocket();
            if (!running_.load()) break;
            CLIPLOG_WARN(kTag, "连接断开: " + lastError());
        } else {
            if (!running_.load()) break;
            setError(err);
            CLIPLOG_WARN(kTag, "连接失败: " + err);
        }
        setState(ConnState::Disconnected);
        if (!running_.load()) break;

        // 退避等待：1s/2s/5s/10s/30s，封顶 30s（§39），stop() 可打断
        const int waitMs =
            kBackoffMs[std::min<int>(backoffIdx,
                                     static_cast<int>(sizeof(kBackoffMs) /
                                                       sizeof(kBackoffMs[0])) -
                                         1)];
        if (backoffIdx <
            static_cast<int>(sizeof(kBackoffMs) / sizeof(kBackoffMs[0])) - 1) {
            ++backoffIdx;
        }
        CLIPLOG_INFO(kTag, "将在 " + std::to_string(waitMs / 1000) +
                               "s 后重连");
        sleepInterruptible(waitMs);
    }

    setState(ConnState::Disconnected);
    ws::releaseWinsock();
    (void)stateName;  // 保留辅助函数避免未使用告警
}

}  // namespace sync
}  // namespace cliplink
