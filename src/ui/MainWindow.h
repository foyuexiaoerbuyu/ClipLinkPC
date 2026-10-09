#pragma once

// ---------------------------------------------------------------------------
// 主窗口（见需求 §6-§9 / §62-§64 / §75-§76）
// - 纯 Win32 固定基础尺寸 200×500（允许 DPI 缩放，§6）
// - 右上角 ⚙ 按钮 -> 设置窗口（§7）
// - 历史列表：置顶项优先、其余 created_at DESC 排序，内容预览截断（§75），
//   仅在 Repository 变更通知（kMsgHistoryChanged）时刷新，
//   禁止 UI 定时轮询 SELECT（§63）
// - 单击历史项 -> 写入系统剪贴板（HISTORY），
//   不发 WebSocket、不新增历史（§9 / §12 / §76）
// - 双击历史项 -> 写入剪贴板后，模拟 Ctrl+V 粘贴到「上次光标所在输入框」
//   （即 ClipLink 抢焦点前最后一个非本进程的前台窗口，见 ForegroundTracker）
// - 右键历史项 -> 弹出「置顶 / 取消置顶」菜单；置顶为纯本地属性，
//   置顶项在列表排最前，取消置顶后按时间回落到原位置
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

    // 窗口置顶开关（主界面「置顶 / 取消置顶」入口，可来回切换）：
    // 置顶态由 SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST) 落地，
    // 与按钮文案保持同步（UI 线程内调用）
    bool topMost() const { return topMost_; }
    void setTopMost(bool on);
    void toggleTopMost();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

private:
    MainWindow() = default;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void onCommand(WPARAM wParam, LPARAM lParam);
    void copySelectedItem();
    // 双击历史项：写入剪贴板 + 模拟 Ctrl+V 粘贴到上次光标所在输入框
    void pasteSelectedItem();
    // 右键历史项：弹出「置顶 / 取消置顶」上下文菜单
    void showItemContextMenu(LPARAM lParam);
    // 列表刷新后按 eventId 恢复选中（置顶重排后保持焦点不跳）
    void reselectByEventId(const std::string& eventId);
    void layoutChildren();

    // ---- 贴边隐藏（拖动到屏幕顶部隐藏 / 鼠标滑到顶部唤出 / 离开后再隐藏）----
    // 应用配置开关（启动/停止边缘监测定时器，关闭时立即还原窗口位置）
    void applyEdgeHideConfig();
    // 边缘监测：唤出与离开判定（低频定时器回调，非高频轮询）
    void onEdgeTimer();
    // 贴顶：把主界面停靠到工作区顶部，仅在工作区内露出一条窄边
    void dockToEdge();
    // 还原：从贴边状态滑出到屏幕顶部对齐位置；armAutoHide 控制是否开启
    // "鼠标离开即隐藏"判定（用户主动从托盘唤出时不开启，避免误隐藏）
    void restoreFromEdge(bool armAutoHide = true);

    // 历史内容预览：换行/制表符压成空格、宽字符截断，防撑爆列表（§8）
    static std::wstring makePreview(const std::string& utf8);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND statusText_ = nullptr;
    HWND settingsBtn_ = nullptr;
    HWND headingText_ = nullptr;
    HWND pinBtn_ = nullptr;  // 「置顶 / 取消置顶」一键切换按钮
    HWND listBox_ = nullptr;
    HFONT font_ = nullptr;
    HFONT gearFont_ = nullptr;

    bool topMost_ = false;  // 当前窗口是否处于置顶态

    // ---- 贴边隐藏状态 ----
    bool edgeHideEnabled_ = false;   // 配置项：贴边隐藏是否开启
    bool edgeHidden_ = false;        // 当前是否已被贴边隐藏（窗口在屏幕外）
    bool edgeShown_ = false;         // 是否由贴边唤出（用于"离开即隐藏"判定）
    bool edgeHovered_ = false;       // 贴边唤出后鼠标是否已进入过窗口
    bool dragging_ = false;          // 用户是否正在拖动/缩放窗口
    bool edgeRevealArmed_ = false;   // 贴边隐藏后鼠标已离开顶部边缘、允许再次唤出
    bool edgeTopMostSuppressed_ = false;  // 隐藏期间为避让顶部任务栏临时取消过置顶
    ULONGLONG edgeShownTick_ = 0;    // 贴边唤出时刻（毫秒），用于离开延时防抖
    int edgeRestoreLeft_ = 0;        // 贴边隐藏前的窗口左边坐标（还原用）

    static constexpr UINT_PTR kEdgeTimerId = 1;
    static constexpr UINT kEdgeTimerIntervalMs = 120;

    std::function<void()> settingsHandler_;
    std::vector<ClipboardEvent> rows_;  // 与列表项一一对应的完整内容
};

}  // namespace ui
}  // namespace cliplink
