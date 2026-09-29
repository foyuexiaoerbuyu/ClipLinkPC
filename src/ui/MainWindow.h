#pragma once

// ---------------------------------------------------------------------------
// 主窗口（见需求 §6-§9 / §62-§64 / §75-§76）
// - 纯 Win32 固定基础尺寸 200×500（允许 DPI 缩放，§6）
// - 右上角 ⚙ 按钮 -> 设置窗口（§7）
// - 历史列表：created_at DESC、内容预览截断（§75），
//   仅在 Repository 变更通知（kMsgHistoryChanged）时刷新，
//   禁止 UI 定时轮询 SELECT（§63）
// - 点击/双击历史项 -> 写入系统剪贴板（HISTORY），
//   不发 WebSocket、不新增历史（§9 / §12 / §76）
// - 关闭按钮默认隐藏到托盘（§64）
// - UI 与业务解耦：只经回调/接口调用业务层（§62）
// ---------------------------------------------------------------------------

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "common/DataTypes.h"

namespace cliplink {
namespace ui {

class MainWindow {
public:
    static MainWindow& instance();

    // ⚙ 按钮回调（main 装配为打开设置窗口；在 UI 线程内调用）
    void setSettingsHandler(std::function<void()> handler);

    // 创建窗口（初始隐藏）；失败返回 false，可重复调用幂等
    bool create(HINSTANCE instance);
    void show();
    void hide();
    bool visible() const;
    HWND hwnd() const { return hwnd_; }
    void destroy();

    // 事件驱动刷新（kMsgHistoryChanged 广播触发，UI 线程执行）
    void refreshHistory();
    // 连接状态刷新（kMsgConnState 广播触发，UI 线程执行）
    void updateConnStatus();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

private:
    MainWindow() = default;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void onCommand(WPARAM wParam, LPARAM lParam);
    void copySelectedItem();
    void layoutChildren();

    // 历史内容预览：换行/制表符压成空格、宽字符截断，防撑爆列表（§8）
    static std::wstring makePreview(const std::string& utf8);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND statusText_ = nullptr;
    HWND settingsBtn_ = nullptr;
    HWND headingText_ = nullptr;
    HWND listBox_ = nullptr;
    HFONT font_ = nullptr;
    HFONT gearFont_ = nullptr;

    std::function<void()> settingsHandler_;
    std::vector<ClipboardEvent> rows_;  // 与列表项一一对应的完整内容
};

}  // namespace ui
}  // namespace cliplink
