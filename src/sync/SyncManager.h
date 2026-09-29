#pragma once

// ---------------------------------------------------------------------------
// SyncManager：网络同步层编排核心（见需求 §60 / §81-§87）
// - 上行：ClipboardMonitor USER 回调 -> 组 clipboard 消息发送 ->
//   收 clipboard_ack 后更新本地 serverSeq/sync_status（§44）
// - 下行：远端 clipboard -> 校验 hash / eventId 幂等 / serverSeq 单调 ->
//   入本地历史 -> ClipboardMonitor::writeToClipboard 写入(REMOTE 不回发, §14-§15/§47)
// - 连接生命周期：register 注册（§42）/ 断线 PENDING 重传（§84-§86）/
//   重连后 sync_request(lastServerSeq) 补历史（§49-§51，只入库不覆盖当前剪贴板）
// - 与 ClipboardMonitor 通过回调对接；支持暂停同步开关（§65）
// - 单线程编排：全部回调在 WebSocket 线程内串行执行，内部互斥保护状态
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "common/DataTypes.h"
#include "sync/WebSocketClient.h"
#include "util/Json.h"

namespace cliplink {
namespace sync {

class WebSocketClient;
class WebSocketServer;

class SyncManager {
public:
    // ---- 连接状态回调（供 UI/托盘显示 已连接/连接中/断开, §38）--------
    using StateCallback = std::function<void(WebSocketClient& client)>;

    static SyncManager& instance();

    // 启动：读取 ConfigManager（serverEnabled / serverAddress / syncEnabled）
    // 与 DeviceManager（deviceId / deviceName）；可重复调用幂等
    void start();
    void stop();

    bool running() const { return running_.load(); }
    bool isConnected();
    // 客户端连接状态（§38：已连接/连接中/断开），供 UI/托盘显示；
    // 未启动或客户端未创建时返回 Disconnected
    ConnState connectionState();
    bool paused() const { return !syncEnabled_.load(); }

    // 暂停/恢复同步（§65）：持久化到 config.syncEnabled；
    // 暂停时上行不发送、下行远端事件不接收处理；恢复后补传 + 补历史
    void setPaused(bool paused);

    // 连接状态变化回调（调用发生在 WebSocket 线程）
    void setStateCallback(StateCallback cb);

    // 供 UI 查询（在线设备数来自本机 Server, §36）
    size_t serverConnectionCount();
    size_t serverOnlineDeviceCount();
    std::string serverLastError() const;

    SyncManager(const SyncManager&) = delete;
    SyncManager& operator=(const SyncManager&) = delete;

private:
    SyncManager() = default;
    ~SyncManager();

    // ---- WebSocketClient 回调（均在 WebSocket 线程内执行）------------
    void onStateChanged(ConnState state);
    void onMessage(const std::string& text);

    // ---- 下行处理 -------------------------------------------------------
    void handleRemoteClipboard(const util::JsonValue& data);
    void handleClipboardAck(const util::JsonValue& data);
    void handleSyncResponse(const util::JsonValue& data);
    void handleServerError(const util::JsonValue& data);

    // ---- 上行辅助 -------------------------------------------------------
    void sendRegister();                       // §42
    void requestHistory();                     // §50 sync_request(lastServerSeq)
    void uploadPending();                      // §84-§86 重连重传
    void sendEvent(const ClipboardEvent& ev);  // 组 clipboard 消息并发送

    // 与 ClipboardMonitor 的 USER 事件对接（由 start 注册）
    void onLocalUserEvent(const ClipboardEvent& ev);

    std::atomic<bool> running_{false};
    std::atomic<bool> syncEnabled_{true};      // §65 暂停同步开关

    WebSocketClient* client_ = nullptr;        // start 时创建，stop 销毁
    WebSocketServer* server_ = nullptr;        // serverEnabled 时创建

    mutable std::mutex opMutex_;               // 串行化回调与状态操作
    StateCallback stateCb_;                    // 由 opMutex_ 保护
    std::string deviceId_;
    std::string deviceName_;
};

}  // namespace sync
}  // namespace cliplink
