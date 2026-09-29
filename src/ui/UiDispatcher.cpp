// ---------------------------------------------------------------------------
// UI 广播分发器实现（§62 / §63）
// ---------------------------------------------------------------------------

#include "ui/UiDispatcher.h"

namespace cliplink {
namespace ui {

UiDispatcher& UiDispatcher::instance() {
    static UiDispatcher disp;
    return disp;
}

void UiDispatcher::registerWindow(HWND hwnd) {
    if (hwnd == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (HWND h : windows_) {
        if (h == hwnd) return;
    }
    windows_.push_back(hwnd);
}

void UiDispatcher::unregisterWindow(HWND hwnd) {
    if (hwnd == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if (*it == hwnd) {
            windows_.erase(it);
            return;
        }
    }
}

void UiDispatcher::broadcast(UINT msg, WPARAM wParam, LPARAM lParam) {
    std::vector<HWND> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = windows_;
    }
    for (HWND h : snapshot) {
        // 跨线程投递到窗口所属 UI 线程；窗口销毁中失败则忽略
        PostMessageW(h, msg, wParam, lParam);
    }
}

}  // namespace ui
}  // namespace cliplink
