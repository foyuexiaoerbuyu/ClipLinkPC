#pragma once

// ---------------------------------------------------------------------------
// 设置窗口（见需求 §36 / §37 / §67 / §95-§97）
// - 字段：服务器地址 / 设备名称 / 启动服务 / 服务端口 / 连接状态 /
//   当前连接数 / 保存记录最大条数 / 保存最长时间(天) /
//   ☑开机自动启动 / ☑自动同步剪贴板 / [保存]（§36 全字段）
// - 保存：写入 ConfigManager 并应用生效（自启动写注册表、
//   网络参数变化时重启 SyncManager，§5 / §67）
// - 连接状态事件驱动 + 窗口可见时 2s 低频刷新连接数
//   （WebSocketServer 无连接变更回调，仅可见期低频读取，§3 不做高频轮询）
// - UI 与业务解耦：只经业务层公开接口调用（§62）
// ---------------------------------------------------------------------------

#include <windows.h>

#include <string>

namespace cliplink {
namespace ui {

class SettingsWindow {
public:
    static SettingsWindow& instance();

    bool create(HINSTANCE instance);
    void show();
    void hide();
    bool visible() const;
    HWND hwnd() const { return hwnd_; }
    void destroy();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

private:
    SettingsWindow() = default;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                     int x, int y, int w, int h, int id);
    void layoutControls();
    void loadFromConfig();
    void saveToConfig();
    void updateLiveStatus();   // 连接状态 + 当前连接数（可见时低频）
    void setHint(const wchar_t* text);

    static std::wstring getText(HWND edit);
    static bool parseInt(const std::wstring& s, int* out);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND editAddr_ = nullptr;
    HWND editDeviceName_ = nullptr;
    HWND chkServerEnabled_ = nullptr;
    HWND editPort_ = nullptr;
    HWND textConnState_ = nullptr;
    HWND textConnCount_ = nullptr;
    HWND editMaxCount_ = nullptr;
    HWND editMaxDays_ = nullptr;
    HWND chkAutoStart_ = nullptr;
    HWND chkSyncEnabled_ = nullptr;
    HWND btnSave_ = nullptr;
    HWND textHint_ = nullptr;
    HFONT font_ = nullptr;

    static constexpr UINT_PTR kTimerId = 1;
    static constexpr UINT kTimerIntervalMs = 2000;
};

}  // namespace ui
}  // namespace cliplink
