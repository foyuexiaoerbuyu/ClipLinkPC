#pragma once

// ---------------------------------------------------------------------------
// 托盘图标（见需求 §4 / §64-§66 / §97）
// - 图标常驻通知区：左键/双击打开主窗口（§4）
// - 右键菜单（§66）：ClipLink、●连接状态、打开主界面、暂停同步、
//   设置、启动服务、退出
// - Tooltip：ClipLink / 服务器状态 / 设备名（§38）
// - 拥有独立隐藏消息窗口，与业务层仅通过 std::function 回调交互（§62）
// ---------------------------------------------------------------------------

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace cliplink {
namespace tray {

class TrayIcon {
public:
    // UI 行为回调（main 装配；均在 UI 线程执行）
    struct Handlers {
        std::function<void()> openMain;      // 打开主界面 / 左键双击
        std::function<void()> openSettings;  // 设置
        std::function<void()> onExit;        // 退出
    };

    static TrayIcon& instance();

    bool create(HINSTANCE instance, Handlers handlers);
    void destroy();

    // 刷新 Tooltip 与状态菜单项（连接状态/暂停/启动服务变化时调用）
    void updateStatus();

    HWND hwnd() const { return hwnd_; }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

private:
    TrayIcon() = default;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void showContextMenu();
    void applyToggleServer();
    std::wstring buildTooltip() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    Handlers handlers_;
    bool added_ = false;
};

}  // namespace tray
}  // namespace cliplink
