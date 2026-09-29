// ---------------------------------------------------------------------------
// SyncManager 实现：网络同步层编排核心
// 上行 §81 / 下行 §82 / 注册 §42 / 离线重传 §84-§86 / 增量补历史 §49-§51
// ---------------------------------------------------------------------------

#include "sync/SyncManager.h"

#include <string>
#include <vector>

#include "clipboard/ClipboardMonitor.h"
#include "config/ConfigManager.h"
#include "database/ClipboardRepository.h"
#include "database/DatabaseManager.h"
#include "database/StateRepository.h"
#include "device/DeviceManager.h"
#include "sync/SyncProtocol.h"
#include "sync/WebSocketClient.h"
#include "sync/WebSocketServer.h"
#include "util/Logger.h"
#include "util/sha256.h"

namespace cliplink {
namespace sync {

using clipboard::ClipboardMonitor;
using database::ClipboardRepository;
using database::InsertOutcome;
using database::StateRepository;
using util::Sha256;

namespace {

constexpr const char* kTag = "SyncManager";
constexpr int kPendingUploadLimit = 1000;  // 单次重传批量上限

}  // namespace

SyncManager& SyncManager::instance() {
    static SyncManager mgr;
    return mgr;
}

SyncManager::~SyncManager() {
    stop();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void SyncManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    const AppConfig cfg = ConfigManager::instance().get();
    syncEnabled_.store(cfg.syncEnabled);
    deviceId_ = DeviceManager::instance().deviceId();
    deviceName_ = DeviceManager::instance().deviceName();

    CLIPLOG_INFO(kTag,
                 "启动同步层, 服务端=" +
                     std::string(cfg.serverEnabled ? "开" : "关") +
                     ", 服务器=" + cfg.serverAddress +
                     ", 同步=" +
                     std::string(cfg.syncEnabled ? "开" : "暂停"));

    // USER 事件对接：ClipboardMonitor -> SyncManager（§60 解耦）
    ClipboardMonitor::instance().setUserEventCallback(
        [this](const ClipboardEvent& ev) { onLocalUserEvent(ev); });

    // ---- 本机服务端（§34/§67：端口占用只提示不崩溃）-------------------
    if (cfg.serverEnabled) {
        server_ = new WebSocketServer();
        server_->setPort(cfg.port > 0 ? cfg.port : constants::kDefaultPort);
        if (!server_->start()) {
            CLIPLOG_ERROR(kTag,
                          "服务端启动失败，继续以客户端运行: " +
                              server_->lastError());
            delete server_;
            server_ = nullptr;
        }
    }

    // ---- 客户端（§39 自动重连由 WebSocketClient 内部退避实现）----------
    client_ = new WebSocketClient();
    client_->setUrl(cfg.serverAddress);
    client_->setStateCallback(
        [this](ConnState s) { onStateChanged(s); });
    client_->setMessageCallback(
        [this](const std::string& text) { onMessage(text); });
    client_->start();
}

void SyncManager::stop() {
    if (!running_.exchange(false)) return;

    // 先停线程再析构对象；回调只读 running_，不会死锁
    if (client_ != nullptr) client_->stop();
    if (server_ != nullptr) server_->stop();
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        delete client_;
        client_ = nullptr;
        delete server_;
        server_ = nullptr;
    }
    ClipboardMonitor::instance().setUserEventCallback(nullptr);
    CLIPLOG_INFO(kTag, "同步层已停止");
}

bool SyncManager::isConnected() {
    std::lock_guard<std::mutex> lock(opMutex_);
    return client_ != nullptr && client_->isConnected();
}

ConnState SyncManager::connectionState() {
    std::lock_guard<std::mutex> lock(opMutex_);
    if (client_ == nullptr) return ConnState::Disconnected;
    return client_->state();
}

void SyncManager::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(opMutex_);
    stateCb_ = std::move(cb);
}

size_t SyncManager::serverConnectionCount() {
    std::lock_guard<std::mutex> lock(opMutex_);
    return server_ != nullptr ? server_->connectionCount() : 0;
}

size_t SyncManager::serverOnlineDeviceCount() {
    std::lock_guard<std::mutex> lock(opMutex_);
    return server_ != nullptr ? server_->onlineDeviceCount() : 0;
}

std::string SyncManager::serverLastError() const {
    std::lock_guard<std::mutex> lock(opMutex_);
    return server_ != nullptr ? server_->lastError() : std::string();
}

// ---------------------------------------------------------------------------
// 暂停同步开关（§65）
// ---------------------------------------------------------------------------
void SyncManager::setPaused(bool paused) {
    const bool enable = !paused;
    const bool changed = (syncEnabled_.exchange(enable) != enable);
    if (changed) {
        AppConfig cfg = ConfigManager::instance().get();
        cfg.syncEnabled = enable;
        ConfigManager::instance().update(cfg);
        CLIPLOG_INFO(kTag, enable ? "同步已恢复" : "同步已暂停");
    }
    if (!enable || !running_.load()) return;

    // 恢复同步：注册 + 补历史 + 重传（等价一次重连语义）
    std::lock_guard<std::mutex> lock(opMutex_);
    if (client_ != nullptr && client_->isConnected()) {
        sendRegister();
        requestHistory();
        uploadPending();
    }
}

// ---------------------------------------------------------------------------
// WebSocketClient 回调（均在 WebSocket 线程）
// ---------------------------------------------------------------------------
void SyncManager::onStateChanged(ConnState state) {
    StateCallback cb;
    WebSocketClient* clientPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        cb = stateCb_;
        clientPtr = client_;
        if (state == ConnState::Connected) {
            // §42 上线注册；§50/§84 重连后补历史 + 重传离线事件
            sendRegister();
            if (syncEnabled_.load()) {
                requestHistory();
                uploadPending();
            }
        }
        // Disconnected：断线期间新事件自然保持 PENDING（§85）
    }
    // UI 回调在锁外执行，避免重入死锁；clientPtr 在 stop() join 前恒有效
    if (cb && clientPtr != nullptr) cb(*clientPtr);
}

void SyncManager::onMessage(const std::string& text) {
    if (!running_.load()) return;

    ParsedMessage msg;
    std::string err;
    if (!parseMessage(text, msg, &err)) {
        CLIPLOG_WARN(kTag, "下行消息解析失败: " + err);
        return;
    }

    std::lock_guard<std::mutex> lock(opMutex_);
    if (!syncEnabled_.load()) {
        // §65 暂停：不接收处理远程剪贴板（连接与心跳保持存活）
        CLIPLOG_DEBUG(kTag,
                      "同步暂停中，忽略下行消息: " +
                          std::string(messageTypeToString(msg.type)));
        return;
    }
    switch (msg.type) {
        case MessageType::Clipboard:
            handleRemoteClipboard(msg.data);
            break;
        case MessageType::ClipboardAck:
            handleClipboardAck(msg.data);
            break;
        case MessageType::SyncResponse:
            handleSyncResponse(msg.data);
            break;
        case MessageType::Error:
            handleServerError(msg.data);
            break;
        default:
            CLIPLOG_WARN(kTag,
                         "未预期的下行消息: " +
                             std::string(messageTypeToString(msg.type)));
            break;
    }
}

// ---------------------------------------------------------------------------
// 上行
// ---------------------------------------------------------------------------
void SyncManager::onLocalUserEvent(const ClipboardEvent& ev) {
    if (!running_.load() || !syncEnabled_.load()) return;
    std::lock_guard<std::mutex> lock(opMutex_);
    // 离线：保持 PENDING（§84-§86），重连后由 uploadPending 补传
    if (client_ == nullptr || !client_->isConnected()) return;
    sendEvent(ev);
}

void SyncManager::sendEvent(const ClipboardEvent& ev) {
    if (ev.content.size() > constants::kMaxTextBytes) {
        CLIPLOG_WARN(kTag, "内容超过 1MB 上限，不同步（eventId=" +
                               ev.eventId + "）");
        return;
    }
    if (!client_->sendText(buildClipboardMessage(ev))) {
        ClipboardRepository::instance().markSyncFailed(ev.eventId);
        CLIPLOG_DEBUG(kTag,
                      "发送失败标记 FAILED，等待重试: " + ev.eventId);
    }
}

void SyncManager::sendRegister() {
    client_->sendText(
        buildRegisterMessage(deviceId_, constants::kDeviceTypeWindows,
                             deviceName_));
    CLIPLOG_DEBUG(kTag, "已发送 register: " + deviceName_);
}

void SyncManager::requestHistory() {
    const int64_t last =
        StateRepository::instance().getLastServerSeq();
    client_->sendText(buildSyncRequestMessage(last));
    CLIPLOG_DEBUG(kTag,
                  "已发送 sync_request, lastServerSeq=" +
                      std::to_string(last));
}

void SyncManager::uploadPending() {
    const std::vector<ClipboardEvent> pending =
        ClipboardRepository::instance().queryPendingSync(
            kPendingUploadLimit);
    if (pending.empty()) return;
    int sent = 0;
    for (const auto& ev : pending) {
        if (client_->sendText(buildClipboardMessage(ev))) ++sent;
    }
    CLIPLOG_INFO(kTag,
                 "离线重传: 待同步 " + std::to_string(pending.size()) +
                     " 条, 已发送 " + std::to_string(sent) + " 条");
}

// ---------------------------------------------------------------------------
// 下行处理
// ---------------------------------------------------------------------------
void SyncManager::handleRemoteClipboard(const util::JsonValue& data) {
    ClipboardEvent remote;
    std::string err;
    if (!eventFromJson(data, remote, &err)) {
        CLIPLOG_WARN(kTag, "远端 clipboard 非法: " + err);
        return;
    }
    // §82 检查顺序：eventId/serverSeq 单调 -> hash
    if (Sha256::hashHex(remote.content) != remote.contentHash) {
        CLIPLOG_WARN(kTag,
                     "hash 校验失败，丢弃远端事件: " + remote.eventId);
        return;
    }
    if (remote.serverSeq > 0) {
        const int64_t cursor =
            StateRepository::instance().getLastServerSeq();
        if (remote.serverSeq <= cursor) {
            // 已同步过（重复广播/补历史重放）：不重复入库、不重复写剪贴板
            CLIPLOG_DEBUG(kTag,
                          "serverSeq 未前进，忽略重复事件 seq=" +
                              std::to_string(remote.serverSeq));
            return;
        }
    }

    remote.source = ClipboardSource::REMOTE;
    remote.syncStatus = SyncStatus::SYNCED;
    if (remote.createdAt <= 0) remote.createdAt = nowUnixMillis();

    const auto outcome =
        ClipboardRepository::instance().insertEvent(remote);
    if (outcome == InsertOutcome::Failed) {
        CLIPLOG_ERROR(kTag, "远端事件入库失败: " + remote.eventId);
        return;
    }
    if (remote.serverSeq > 0) {
        StateRepository::instance().setLastServerSeq(remote.serverSeq);
    }

    // DuplicateEventId = 幂等命中（同一事件重复投递）：不重写剪贴板。
    // Inserted / AdjacentDuplicate：写入本地剪贴板（REMOTE，§46-§47，
    // 经 ClipboardMonitor 内部置 suppress+hash 双重防回发）
    if (outcome != InsertOutcome::DuplicateEventId) {
        if (!ClipboardMonitor::instance().writeToClipboard(
                remote.content)) {
            CLIPLOG_WARN(kTag,
                         "写入本地剪贴板失败（已保存历史）: " +
                             remote.eventId);
        }
    }
    CLIPLOG_DEBUG(kTag,
                  "远端事件已处理 seq=" +
                      std::to_string(remote.serverSeq) +
                      ", 写入剪贴板=" +
                      (outcome == InsertOutcome::
                                       DuplicateEventId
                           ? "否(幂等)"
                           : "是"));
}

void SyncManager::handleClipboardAck(const util::JsonValue& data) {
    const std::string eventId = data.get("eventId").asString();
    const int64_t serverSeq = data.get("serverSeq").asInt64(0);
    const int64_t serverTime = data.get("serverTime").asInt64(0);
    if (eventId.empty() || serverSeq <= 0) {
        CLIPLOG_WARN(kTag, "clipboard_ack 字段缺失/非法");
        return;
    }
    if (!ClipboardRepository::instance().updateServerAck(
            eventId, serverSeq, serverTime)) {
        CLIPLOG_WARN(kTag, "ack 更新本地历史失败: " + eventId);
        return;
    }
    StateRepository::instance().setLastServerSeq(serverSeq);
    CLIPLOG_DEBUG(kTag,
                  "ack: eventId=" + eventId +
                      " seq=" + std::to_string(serverSeq));
}

void SyncManager::handleSyncResponse(const util::JsonValue& data) {
    const util::JsonValue& events = data.get("events");
    if (!events.isArray()) {
        CLIPLOG_WARN(kTag, "sync_response 缺少 events 数组");
        return;
    }
    const int64_t before =
        StateRepository::instance().getLastServerSeq();
    int applied = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        ClipboardEvent ev;
        std::string err;
        if (!eventFromJson(events.at(i), ev, &err)) {
            CLIPLOG_WARN(kTag, "补历史事件非法: " + err);
            continue;
        }
        if (Sha256::hashHex(ev.content) != ev.contentHash) {
            CLIPLOG_WARN(kTag,
                         "补历史 hash 不匹配，丢弃: " + ev.eventId);
            continue;
        }
        ev.source = ClipboardSource::REMOTE;
        ev.syncStatus = SyncStatus::SYNCED;
        if (ev.createdAt <= 0) ev.createdAt = nowUnixMillis();

        const auto outcome =
            ClipboardRepository::instance().insertEvent(ev);
        if (outcome == InsertOutcome::Failed) {
            continue;
        }
        // §51：补历史只入库，绝不写入系统剪贴板、不覆盖当前剪贴板
        if (outcome == InsertOutcome::
                           DuplicateEventId &&
            ev.serverSeq > 0) {
            // 自身事件因 ack 丢失未标 SYNCED：借补历史修复（§86）
            ClipboardRepository::instance().updateServerAck(
                ev.eventId, ev.serverSeq, ev.serverTime);
        }
        if (ev.serverSeq > 0) {
            StateRepository::instance().setLastServerSeq(ev.serverSeq);
        }
        ++applied;
    }
    const int64_t after =
        StateRepository::instance().getLastServerSeq();
    CLIPLOG_INFO(kTag,
                 "补历史完成: 应用 " + std::to_string(applied) + " 条, " +
                     "lastServerSeq " + std::to_string(before) + " -> " +
                     std::to_string(after));
}

void SyncManager::handleServerError(const util::JsonValue& data) {
    CLIPLOG_WARN(kTag,
                 "服务端错误: code=" + data.get("code").asString() +
                     ", message=" + data.get("message").asString());
}

}  // namespace sync
}  // namespace cliplink
