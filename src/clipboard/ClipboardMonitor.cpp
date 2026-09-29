// ---------------------------------------------------------------------------
// 剪贴板监控与事件主流程实现（见需求 §14-§16 / §81）
// ---------------------------------------------------------------------------

#include "clipboard/ClipboardMonitor.h"

#include "clipboard/ClipboardReader.h"
#include "clipboard/ClipboardWriter.h"
#include "config/ConfigManager.h"
#include "database/ClipboardRepository.h"
#include "device/DeviceManager.h"
#include "util/Logger.h"
#include "util/sha256.h"
#include "util/uuid.h"

namespace cliplink {
namespace clipboard {

namespace {

constexpr const char* kTag = "ClipboardMonitor";

// suppress 标志有效期：程序写入到 WM_CLIPBOARDUPDATE 送达通常为毫秒级，
// 5 秒视为安全上限，过期后不再吞并更新，防止极端场景误吞用户复制（§14）
constexpr uint32_t kSuppressTtlMs = 5000;

}  // namespace

ClipboardMonitor& ClipboardMonitor::instance() {
    static ClipboardMonitor inst;
    return inst;
}

bool ClipboardMonitor::attach(HWND hwnd) {
    if (hwnd == nullptr) return false;
    if (attached_.load()) return true;

    if (!AddClipboardFormatListener(hwnd)) {
        CLIPLOG_ERROR(kTag,
                      "AddClipboardFormatListener 失败, errno=" +
                          std::to_string(GetLastError()));
        return false;
    }
    hwnd_ = hwnd;
    attached_.store(true);
    CLIPLOG_INFO(kTag, "剪贴板监听已注册（AddClipboardFormatListener）");
    return true;
}

void ClipboardMonitor::detach() {
    if (!attached_.exchange(false)) return;
    if (hwnd_ != nullptr) {
        RemoveClipboardFormatListener(hwnd_);
        hwnd_ = nullptr;
    }
    CLIPLOG_INFO(kTag, "剪贴板监听已移除");
}

bool ClipboardMonitor::attached() const {
    return attached_.load();
}

void ClipboardMonitor::setUserEventCallback(UserEventCallback cb) {
    std::lock_guard<std::mutex> g(callbackMutex_);
    userEventCallback_ = std::move(cb);
}

bool ClipboardMonitor::writeToClipboard(const std::string& utf8) {
    if (!attached_.load()) {
        CLIPLOG_ERROR(kTag, "程序写入被拒绝：监听未注册");
        return false;
    }

    // §14-§15 写入前置两层防循环标记
    const std::string hash = util::Sha256::hashHex(utf8);
    {
        std::lock_guard<std::mutex> g(programmaticHashMutex_);
        lastProgrammaticClipboardHash_ = hash;
    }
    suppressNextClipboardUpdate_.store(true);
    suppressSetTick_.store(GetTickCount());

    if (!ClipboardWriter::write(utf8, hwnd_)) {
        // 写入失败：回滚两层标记，避免吞掉后续真实用户复制
        suppressNextClipboardUpdate_.store(false);
        std::lock_guard<std::mutex> g(programmaticHashMutex_);
        lastProgrammaticClipboardHash_.clear();
        return false;
    }
    CLIPLOG_INFO(kTag, "程序写入剪贴板成功, hash=" + hash.substr(0, 16) +
                           "...");
    return true;
}

void ClipboardMonitor::onClipboardUpdate() {
    if (!attached_.load()) return;

    // ---- 第一层保护：suppress 标志（§14）------------------------------
    if (suppressNextClipboardUpdate_.exchange(false)) {
        const uint32_t setTick = suppressSetTick_.load();
        const DWORD now = GetTickCount();
        if (now - setTick <= kSuppressTtlMs) {
            CLIPLOG_INFO(kTag, "忽略程序写入触发的更新（suppress 标志，§14）");
            return;
        }
        // 标记过期：按正常流程继续，避免误吞真实用户复制
        CLIPLOG_DEBUG(kTag, "suppress 标记已过期，按正常流程处理");
    }

    // ---- 读取 CF_UNICODETEXT（§10 / §57 / §77）-------------------------
    const ClipboardReader::Result rr = ClipboardReader::read(hwnd_);
    if (rr.openFailed || rr.tooLarge || !rr.ok) {
        // 空剪贴板 / 超限 / 被占用：均不产生事件，已在 Reader 记录日志（§72）
        return;
    }

    // ---- SHA-256 内容 Hash（§17）---------------------------------------
    const std::string contentHash = util::Sha256::hashHex(rr.text);

    // ---- 第二层保护：lastProgrammaticClipboardHash（§15）---------------
    {
        std::lock_guard<std::mutex> g(programmaticHashMutex_);
        if (contentHash == lastProgrammaticClipboardHash_) {
            CLIPLOG_INFO(kTag,
                         "忽略程序写入内容（hash 匹配，§15）, hash=" +
                             contentHash.substr(0, 16) + "...");
            return;
        }
    }

    // ---- 生成 USER 事件：UUID eventId -> 写本地历史 -> 通知 Sync（§81）--
    ClipboardEvent ev;
    ev.eventId    = util::uuidV4();
    ev.deviceId   = DeviceManager::instance().deviceId();
    ev.deviceType = constants::kDeviceTypeWindows;
    ev.deviceName = DeviceManager::instance().deviceName();
    ev.contentType = ContentType::TEXT;
    ev.contentHash = contentHash;
    ev.content     = rr.text;                 // UTF-8 原样（§78）
    ev.clientTime  = nowUnixMillis();         // UTC 毫秒（§80）
    ev.createdAt   = ev.clientTime;
    ev.source      = ClipboardSource::USER;
    ev.syncStatus  = SyncStatus::PENDING;     // server_seq IS NULL（§86）

    auto& repo = database::ClipboardRepository::instance();
    const database::InsertOutcome outcome = repo.insertEvent(ev);

    switch (outcome) {
        case database::InsertOutcome::Inserted: {
            CLIPLOG_INFO(kTag, "USER 事件已入库, eventId=" + ev.eventId +
                                   ", bytes=" +
                                   std::to_string(ev.content.size()));

            // §28 历史清理：条数 / 天数满足任意条件即清理
            const AppConfig cfg = ConfigManager::instance().get();
            repo.cleanup(cfg.maxHistoryCount, cfg.maxHistoryDays);

            // 回调通知 Sync 层（std::function，Sync 实现由后续模块提供）
            UserEventCallback cb;
            {
                std::lock_guard<std::mutex> g(callbackMutex_);
                cb = userEventCallback_;
            }
            if (cb) {
                cb(ev);
            }
            break;
        }
        case database::InsertOutcome::AdjacentDuplicate:
            // §29：与最近一次内容相同，不产生新事件、不发送服务器
            CLIPLOG_DEBUG(kTag, "相邻重复内容，跳过（§29）");
            break;
        case database::InsertOutcome::DuplicateEventId:
            CLIPLOG_WARN(kTag, "eventId 重复，幂等跳过: " + ev.eventId);
            break;
        case database::InsertOutcome::Failed:
            CLIPLOG_ERROR(kTag, "USER 事件入库失败: " + ev.eventId);
            break;
    }
}

}  // namespace clipboard
}  // namespace cliplink
