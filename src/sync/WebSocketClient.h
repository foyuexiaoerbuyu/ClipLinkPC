#pragma once

// ---------------------------------------------------------------------------
// WebSocket 客户端（见需求 §37-§40 / §39 重连退避 / §56 线程模型）
// - 纯 Winsock2 ws:// 明文客户端：RFC 6455 HTTP Upgrade 握手、
//   SHA-1 + Base64 Accept、掩码帧收发、ping/pong 心跳、分片与大帧重组
// - 独立 WebSocket 线程：连接 -> 握手 -> 收发 -> 断开退避重连（1/2/5/10/30s，
//   连接成功立即重置，§39；禁止高频重连）
// - 连接状态回调 Disconnected / Connecting / Connected（§38 连接状态）
// - 为避免 winsock2.h 与 windows.h 头文件冲突，socket 以 uintptr_t 存储，
//   头文件不暴露 winsock 定义，可被任意业务头安全包含
// ---------------------------------------------------------------------------

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cliplink {
namespace sync {

// 连接状态（§38：已连接 / 连接中 / 断开）
enum class ConnState {
    Disconnected,  // 断开（未连接或等待重连）
    Connecting,    // 连接中（含握手）
    Connected      // 已连接（握手完成）
};

class WebSocketClient {
public:
    using StateCallback   = std::function<void(ConnState)>;
    using MessageCallback = std::function<void(const std::string& text)>;

    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // 目标地址：ws://host[:port][/path]（MVP 仅支持 ws://，§37）
    void setUrl(const std::string& url);
    void setStateCallback(StateCallback cb);
    void setMessageCallback(MessageCallback cb);
    // 心跳间隔（毫秒）：默认 45000（45s，§40 要求 30~60s 一次）
    void setHeartbeatIntervalMs(int ms);

    // 启动后台线程（幂等）；stop() 等待线程退出后返回
    bool start();
    void stop();

    bool isRunning() const { return running_.load(); }
    ConnState state() const { return state_.load(); }
    bool isConnected() const { return state_.load() == ConnState::Connected; }

    // 发送文本消息（内部按 RFC 6455 掩码成帧）；未连接返回 false
    bool sendText(const std::string& text);

    std::string lastError() const;

private:
    void loop();
    bool connectAndHandshake(std::string& leftover, std::string& err);
    bool recvLoop(const std::string& leftover);
    bool sendFrame(uint8_t opcode, const void* payload, size_t len);
    bool sendBytes(const void* data, size_t len);
    void setState(ConnState s);
    void setError(const std::string& err);
    void sleepInterruptible(int ms);
    void closeCurrentSocket();
    void invokeStateCallback(ConnState s);

    std::string url_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};
    // INVALID_SOCKET（全 1）= 无连接；与 uintptr_t 天然对齐
    std::atomic<uintptr_t> sock_{static_cast<uintptr_t>(-1)};

    mutable std::mutex cbMutex_;
    StateCallback stateCallback_;
    MessageCallback messageCallback_;

    mutable std::mutex errMutex_;
    std::string lastError_;

    std::mutex sendMutex_;  // 帧写入串行化（发送与心跳共用）

    std::atomic<int> heartbeatMs_{45000};

    std::mutex wakeMutex_;  // stop() 打断退避等待
    std::condition_variable wakeCv_;

    std::thread thread_;
};

}  // namespace sync
}  // namespace cliplink
