#pragma once

// ---------------------------------------------------------------------------
// 剪贴板监控与事件主流程（见需求 §11-§17 / §81）
// - attach(hwnd)：AddClipboardFormatListener，收 WM_CLIPBOARDUPDATE；
//   程序退出 detach() 移除监听（§11），全程事件驱动、禁止轮询（§2 / §103）
// - 主流程：WM_CLIPBOARDUPDATE -> 读取 CF_UNICODETEXT -> SHA-256 ->
//   suppress 标志 + lastProgrammaticClipboardHash 双重防循环（§14-§16）->
//   生成 UUID eventId -> 写本地历史 -> std::function 回调通知 Sync 层（§81）
// - writeToClipboard()：REMOTE / HISTORY 程序写入前置两层防循环标记（§82-§83）
// - 空剪贴板忽略（§77）、超过 1MB 不同步（§73）、相邻重复不产生新事件（§29）
// ---------------------------------------------------------------------------

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <windows.h>

#include "common/DataTypes.h"

namespace cliplink {
namespace clipboard {

class ClipboardMonitor {
public:
    // Sync 层接入点：USER 事件已入库后回调（Sync 实现由后续模块提供）
    using UserEventCallback = std::function<void(const ClipboardEvent&)>;

    static ClipboardMonitor& instance();

    // 在隐藏宿主窗口上注册剪贴板监听（§11）
    bool attach(HWND hwnd);
    // 程序退出时移除监听（§11）
    void detach();
    bool attached() const;

    // WM_CLIPBOARDUPDATE 处理入口（由宿主窗口 WndProc 分发，§16 流程）
    void onClipboardUpdate();

    // 程序写入系统剪贴板（REMOTE 远程同步 / HISTORY 历史点击共用）：
    // 写入前设置 suppress 标志 + lastProgrammaticClipboardHash（§14-§15）
    bool writeToClipboard(const std::string& utf8);

    // 注册 Sync 层回调（可多次调用，最后一次生效；置空则仅写历史）
    void setUserEventCallback(UserEventCallback cb);

    ClipboardMonitor(const ClipboardMonitor&) = delete;
    ClipboardMonitor& operator=(const ClipboardMonitor&) = delete;

private:
    ClipboardMonitor() = default;

    // 第一层保护：程序写入后的下一次更新（§14）
    std::atomic<bool> suppressNextClipboardUpdate_{false};
    // suppress 有效期（GetTickCount 毫秒）：过期视为陈旧标记，
    // 避免极端场景吞掉真实用户复制
    std::atomic<uint32_t> suppressSetTick_{0};

    // 第二层保护：程序写入内容的 SHA-256（§15）
    mutable std::mutex programmaticHashMutex_;
    std::string lastProgrammaticClipboardHash_;

    mutable std::mutex callbackMutex_;
    UserEventCallback userEventCallback_;

    std::atomic<bool> attached_{false};
    HWND hwnd_ = nullptr;
};

}  // namespace clipboard
}  // namespace cliplink
