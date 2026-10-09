// ---------------------------------------------------------------------------
// 前台输入窗口跟踪器实现
// ---------------------------------------------------------------------------

#include "ui/ForegroundTracker.h"

#include "util/Logger.h"

namespace cliplink {
namespace ui {

namespace {

constexpr const char* kTag = "ForegroundTracker";

// 系统外壳 / 非输入目标窗口类名（桌面、任务栏、窗口切换器等）：
// 这些窗口不接受 Ctrl+V，记录下来只会导致粘贴落空
bool isShellWindowClass(const wchar_t* cls) {
    static const wchar_t* kDeny[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
        L"TaskListThumbnailWnd",
        L"TaskSwitcherWnd",
        L"ForegroundStaging",
        L"EdgeUiInputTopWndClass",
    };
    for (const wchar_t* deny : kDeny) {
        if (lstrcmpW(cls, deny) == 0) return true;
    }
    return false;
}

}  // namespace

ForegroundTracker& ForegroundTracker::instance() {
    static ForegroundTracker tracker;
    return tracker;
}

bool ForegroundTracker::start() {
    if (hook_ != nullptr) return true;

    // WINEVENT_SKIPOWNPROCESS：本进程窗口成为前台时不回调，
    // 从而保留「ClipLink 之前的最后一个他进程前台窗口」
    hook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &ForegroundTracker::winEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hook_ == nullptr) {
        CLIPLOG_WARN(kTag, "SetWinEventHook 注册失败, errno=" +
                               std::to_string(GetLastError()));
        return false;
    }
    CLIPLOG_INFO(kTag, "前台窗口跟踪已启动（双击粘贴目标定位）");
    return true;
}

void ForegroundTracker::stop() {
    if (hook_ == nullptr) return;
    UnhookWinEvent(hook_);
    hook_ = nullptr;
    lastForeground_.store(nullptr);
    CLIPLOG_INFO(kTag, "前台窗口跟踪已停止");
}

HWND ForegroundTracker::lastForeground() const {
    HWND h = lastForeground_.load();
    if (h != nullptr && !IsWindow(h)) return nullptr;
    return h;
}

bool ForegroundTracker::isPasteTarget(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;

    // 双保险排除本进程窗口（hook 已带 WINEVENT_SKIPOWNPROCESS）
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;

    // 必须是顶层窗口（含被拥有的弹出窗口），排除子控件
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, 128) <= 0) return false;
    if (isShellWindowClass(cls)) return false;

    // 尺寸为 0 的占位/隐藏窗口不作为目标
    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) return false;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;

    return true;
}

void CALLBACK ForegroundTracker::winEventProc(HWINEVENTHOOK /*hook*/,
                                              DWORD event, HWND hwnd,
                                              LONG idObject, LONG idChild,
                                              DWORD /*eventThread*/,
                                              DWORD /*eventTime*/) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!isPasteTarget(hwnd)) return;

    instance().lastForeground_.store(hwnd);
    CLIPLOG_DEBUG(kTag, "记录前台目标窗口 hwnd=" +
                            std::to_string(reinterpret_cast<intptr_t>(hwnd)));
}

}  // namespace ui
}  // namespace cliplink
