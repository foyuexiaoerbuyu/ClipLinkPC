#pragma once

// ---------------------------------------------------------------------------
// UI 广播分发器（见需求 §62 / §63）
// - Repository / SyncManager 等业务线程只允许 PostMessage 广播，
//   绝不在回调里直接操作控件；各窗口收到消息后在自身 UI 线程刷新
// - 事件驱动刷新历史列表，禁止 UI 定时轮询 SELECT（§63）
// - broadcast 线程安全：工作线程与 UI 线程均可调用
// ---------------------------------------------------------------------------

#include <windows.h>

#include <mutex>
#include <vector>

namespace cliplink {
namespace ui {

// 历史数据变更（Repository insert/cleanup 后触发 -> 主窗口刷新列表）
constexpr UINT kMsgHistoryChanged = WM_APP + 0x310;
// WebSocket 连接状态变化（§38 已连接/连接中/断开 -> 主窗口与托盘刷新）
constexpr UINT kMsgConnState = WM_APP + 0x311;
// 配置变更（设置保存 / 托盘切换暂停同步、启动服务 -> 托盘与窗口刷新）
constexpr UINT kMsgConfigChanged = WM_APP + 0x312;

class UiDispatcher {
public:
    static UiDispatcher& instance();

    // 窗口创建时注册、销毁时注销（重复注册同一句柄无副作用）
    void registerWindow(HWND hwnd);
    void unregisterWindow(HWND hwnd);

    // 向所有已注册窗口 PostMessage（失败的句柄静默忽略，线程安全）
    void broadcast(UINT msg, WPARAM wParam = 0, LPARAM lParam = 0);

    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;

private:
    UiDispatcher() = default;

    std::mutex mutex_;
    std::vector<HWND> windows_;
};

}  // namespace ui
}  // namespace cliplink
