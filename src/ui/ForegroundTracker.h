#pragma once

// ---------------------------------------------------------------------------
// 前台输入窗口跟踪器
// 用途：为主界面「双击历史项 -> 粘贴到上次光标所在输入框」提供目标窗口定位。
//
// - 通过 SetWinEventHook(EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT)
//   跟踪系统前台窗口变化，记录「最近一个非本进程」的前台顶层窗口句柄；
//   ClipLink 自身窗口（主窗口 / 设置窗口）不参与记录，因此该句柄即用户在
//   ClipLink 抢走焦点之前正在使用、光标所在的那个窗口。
// - WINEVENT_OUTOFCONTEXT：回调投递到注册线程（主线程）的消息循环，
//   无需 DLL 注入，不干扰目标进程。
// - 本类只读窗口句柄，不做任何写入 / 注入；句柄失效由调用方以 IsWindow
//   校验后降级为「仅复制到剪贴板」。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <atomic>

namespace cliplink {
namespace ui {

class ForegroundTracker {
public:
    static ForegroundTracker& instance();

    // 在带消息循环的 UI 线程调用一次；重复调用幂等
    bool start();
    void stop();

    // 最近一个非本进程的前台顶层窗口；无记录或窗口已销毁时返回 nullptr
    HWND lastForeground() const;

    ForegroundTracker(const ForegroundTracker&) = delete;
    ForegroundTracker& operator=(const ForegroundTracker&) = delete;

private:
    ForegroundTracker() = default;

    static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event,
                                      HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD eventThread, DWORD eventTime);

    // 过滤：仅接受可作为粘贴目标的可见顶层窗口
    static bool isPasteTarget(HWND hwnd);

    std::atomic<HWND> lastForeground_{nullptr};
    HWINEVENTHOOK hook_ = nullptr;
};

}  // namespace ui
}  // namespace cliplink
