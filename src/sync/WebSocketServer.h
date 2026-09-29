#pragma once

// ---------------------------------------------------------------------------
// WebSocket 服务端（纯 Winsock2，RFC 6455）（见需求 §34-§36 / §45 / §67-§68 /
// §74 / §84-§87）
// - 监听 0.0.0.0:端口（默认 9000，§34/§67）；端口占用时记录错误并返回
//   false，绝不崩溃（§67 / §72）
// - 维护在线设备表：register 后 deviceId -> 连接（§42/§55）
// - 广播 clipboard 事件时绝不回发给来源设备（§45/§68/§112）
// - 服务端协议处理：clipboard -> serverSeq 分配 + clipboard_ack + 广播，
//   eventId 幂等防重（§87）；sync_request 增量补历史（§50）；
//   非法/超限请求回 error 消息（§72/§74），单条消息 8MB、内容 1MB 上限
// - pimpl 设计：头文件不暴露 winsock 定义
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace cliplink {
namespace sync {

class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    void setPort(int port);  // 必须在 start() 之前调用
    // 启动监听；失败返回 false 并写入 lastError()（端口占用等），不崩溃
    bool start();
    void stop();

    bool isRunning() const;
    int port() const;

    // 已完成 WebSocket 握手的连接数（设置页"当前连接数"，§36）
    size_t connectionCount() const;
    // 已完成 register 的在线设备数（§55 在线状态）
    size_t onlineDeviceCount() const;

    std::string lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sync
}  // namespace cliplink
