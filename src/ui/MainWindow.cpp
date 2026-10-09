// ---------------------------------------------------------------------------
// 主窗口实现（§6-§9 / §63 / §64 / §75-§76）
// ---------------------------------------------------------------------------

#include "ui/MainWindow.h"

#include <algorithm>

#include "clipboard/ClipboardMonitor.h"
#include "config/ConfigManager.h"
#include "database/ClipboardRepository.h"
#include "sync/SyncManager.h"
#include "ui/ForegroundTracker.h"
#include "ui/UiDispatcher.h"
#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace ui {

namespace {

constexpr const char* kTag = "MainWindow";

constexpr wchar_t kWindowClass[] = L"ClipLinkMainWindow";
constexpr wchar_t kWindowTitle[] = L"ClipLink";

// 控件 ID
constexpr int kIdSettingsBtn = 1001;
constexpr int kIdHistoryList = 1002;
constexpr int kIdPinBtn = 1005;  // 「置顶 / 取消置顶」切换按钮

// 右键上下文菜单命令 ID（历史项置顶 / 取消置顶）
constexpr int kIdMenuTogglePin = 2001;

// 单屏展示行数上限（超出的更旧记录不渲染，避免超长列表拖慢 UI）
constexpr int kMaxDisplayRows = 200;
// 内容预览最大宽字符数（§8 长文本截断）
constexpr size_t kPreviewChars = 60;
// 逻辑设计尺寸（§6，随后按 DPI 缩放）
constexpr int kDesignWidth = 200;
constexpr int kDesignHeight = 500;

// ---- 贴边隐藏参数（逻辑像素，运行时按 DPI 缩放）--------------------------
constexpr int kEdgeDockThresholdPx = 6;    // 拖到距屏幕顶部多少像素内视为贴顶
constexpr int kEdgePeekPx = 4;             // 隐藏后在工作区内露出的窄边高度(3~5px)
constexpr int kEdgeTriggerPx = 2;          // 鼠标滑到唤出线上方多少像素内唤出
constexpr DWORD kEdgeLeaveDelayMs = 400;   // 唤出后延时判定离开，防抖

int scaleForDpi(int value) {
    HDC dc = GetDC(nullptr);
    int dpi = 96;
    if (dc != nullptr) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    if (dpi <= 0) dpi = 96;
    return MulDiv(value, dpi, 96);
}

// 模拟一次 Ctrl+V（SendInput）：按"按下 Ctrl -> 按下 V -> 抬起 V ->
// 抬起 Ctrl"的完整序列发送，避免修饰键残留导致后续输入异常
bool sendPasteKeystroke() {
    INPUT inputs[4] = {};
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = VK_CONTROL;
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = 'V';
    inputs[2].type       = INPUT_KEYBOARD;
    inputs[2].ki.wVk     = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type       = INPUT_KEYBOARD;
    inputs[3].ki.wVk     = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

}  // namespace

MainWindow& MainWindow::instance() {
    static MainWindow win;
    return win;
}

void MainWindow::setSettingsHandler(std::function<void()> handler) {
    settingsHandler_ = std::move(handler);
}

bool MainWindow::create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    instance_ = instance;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &MainWindow::wndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    // 应用图标（resources/resource.rc 提供，§97 托盘/窗口共用）
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CLIPLOG_ERROR(kTag, "窗口类注册失败, errno=" +
                                std::to_string(GetLastError()));
        return false;
    }

    const int w = scaleForDpi(kDesignWidth);
    const int h = scaleForDpi(kDesignHeight);

    hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                            nullptr, instance, this);
    if (hwnd_ == nullptr) {
        CLIPLOG_ERROR(kTag,
                      "主窗口创建失败, errno=" + std::to_string(GetLastError()));
        hwnd_ = nullptr;
        return false;
    }

    // 字体：正文 DEFAULT_GUI_FONT；⚙ 用 Segoe UI Symbol 保证字形可用
    font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    gearFont_ = CreateFontW(-scaleForDpi(16), 0, 0, 0, FW_NORMAL, FALSE,
                            FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Segoe UI Symbol");

    // 顶部：连接状态 + ⚙（§7 / §38）
    statusText_ = CreateWindowExW(
        0, L"STATIC", L"● 断开",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003)), instance,
        nullptr);
    settingsBtn_ = CreateWindowExW(
        0, L"BUTTON", L"⚙",
        WS_CHILD | WS_VISIBLE | BS_FLAT | BS_CENTER | BS_VCENTER, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSettingsBtn)),
        instance, nullptr);

    headingText_ = CreateWindowExW(
        0, L"STATIC", L"剪贴板历史", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
        0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1004)),
        instance, nullptr);

    // 标题行右侧：「置顶 / 取消置顶」一键切换（沿用 ⚙ 的扁平按钮风格）
    pinBtn_ = CreateWindowExW(
        0, L"BUTTON", L"置顶",
        WS_CHILD | WS_VISIBLE | BS_FLAT | BS_CENTER | BS_VCENTER, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPinBtn)),
        instance, nullptr);

    // 历史列表：只读 ListBox，通知式（单击/双击均触发选择通知，§76）
    listBox_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_HASSTRINGS,
        0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHistoryList)),
        instance, nullptr);

    if (statusText_ == nullptr || settingsBtn_ == nullptr ||
        headingText_ == nullptr || pinBtn_ == nullptr ||
        listBox_ == nullptr) {
        CLIPLOG_ERROR(kTag, "子控件创建失败, errno=" +
                                std::to_string(GetLastError()));
        destroy();
        return false;
    }

    SendMessageW(statusText_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                 TRUE);
    SendMessageW(headingText_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                 TRUE);
    SendMessageW(pinBtn_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(listBox_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(settingsBtn_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(gearFont_ != nullptr ? gearFont_
                                                               : font_),
                 TRUE);

    layoutChildren();
    UiDispatcher::instance().registerWindow(hwnd_);

    refreshHistory();
    updateConnStatus();
    applyEdgeHideConfig();  // 按配置决定是否开启贴边隐藏监测
    CLIPLOG_INFO(kTag, "主窗口创建完成, 逻辑尺寸 200x500");
    return true;
}

void MainWindow::layoutChildren() {
    if (hwnd_ == nullptr) return;
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    const int pad = scaleForDpi(6);
    const int gear = scaleForDpi(24);
    const int rowH = scaleForDpi(20);
    const int headH = scaleForDpi(20);
    const int pinW = scaleForDpi(56);
    const int gap = scaleForDpi(4);
    const int top = scaleForDpi(6);

    // ⚙ 右上角
    SetWindowPos(settingsBtn_, nullptr, cw - gear - pad, top, gear, gear,
                 SWP_NOZORDER);
    // 状态文本占剩余宽度
    SetWindowPos(statusText_, nullptr, pad, top + scaleForDpi(4),
                 cw - gear - pad * 3, rowH, SWP_NOZORDER);
    // 标题行：左「剪贴板历史」+ 右「置顶 / 取消置顶」切换按钮
    const int headTop = top + gear + gap;
    SetWindowPos(headingText_, nullptr, pad, headTop + scaleForDpi(2),
                 cw - pad * 2 - pinW - gap, headH, SWP_NOZORDER);
    SetWindowPos(pinBtn_, nullptr, cw - pad - pinW, headTop, pinW, headH,
                 SWP_NOZORDER);
    // 列表填满剩余区域（§7 草图）
    const int listTop = top + gear + scaleForDpi(6) + headH + scaleForDpi(4);
    SetWindowPos(listBox_, nullptr, pad, listTop, cw - pad * 2,
                 ch - listTop - pad, SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// 贴边隐藏：
//   1) 勾选后把主界面拖到屏幕顶部 -> 窗口滑出屏幕顶部（仅留一条窄边）
//   2) 鼠标再滑动到屏幕顶部该位置 -> 主界面滑出显示
//   3) 鼠标离开主界面区域 -> 再次隐藏
// 实现约束：仅用窗口自身的低频定时器读取光标位置与窗口矩形，
// 不安装全局鼠标钩子、不改动其他模块（§62 UI 与业务解耦）
// ---------------------------------------------------------------------------

void MainWindow::applyEdgeHideConfig() {
    if (hwnd_ == nullptr) return;

    const bool enabled = ConfigManager::instance().get().edgeHideEnabled;
    if (enabled == edgeHideEnabled_) return;

    edgeHideEnabled_ = enabled;
    if (enabled) {
        // 低频监测光标位置（120ms），不做高频轮询
        SetTimer(hwnd_, kEdgeTimerId, kEdgeTimerIntervalMs, nullptr);
        CLIPLOG_INFO(kTag, "贴边隐藏已开启");
    } else {
        KillTimer(hwnd_, kEdgeTimerId);
        // 关闭开关时若窗口仍在屏幕外，立即还原，避免窗口"消失"
        if (edgeHidden_) restoreFromEdge(false);
        edgeHidden_ = false;
        edgeShown_ = false;
        edgeHovered_ = false;
        edgeRevealArmed_ = false;
        edgeTopMostSuppressed_ = false;
        CLIPLOG_INFO(kTag, "贴边隐藏已关闭，窗口位置已还原");
    }
}

void MainWindow::onEdgeTimer() {
    if (hwnd_ == nullptr || !edgeHideEnabled_) return;
    // 托盘隐藏 / 最小化期间不参与贴边判定
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;

    POINT pt = {0, 0};
    if (!GetCursorPos(&pt)) return;

    RECT rc = {0, 0, 0, 0};
    if (!GetWindowRect(hwnd_, &rc)) return;

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) return;

    // 唤出触发线以工作区顶部为准：任务栏在顶部时即任务栏下沿，鼠标滑到
    // 屏幕顶部/任务栏上同样满足条件；隐藏后窗口在工作区内露出 peek 高的
    // 窄边，触发线在此基础上下移 peek 高度，保证鼠标落在露出的窄边上也能
    // 唤出。水平范围按窗口宽度判定（窄边始终可见，用户能对准隐藏位置）。
    const int workTop = mi.rcWork.top;
    const int peek = scaleForDpi(kEdgePeekPx);
    const int trigger = workTop + peek + scaleForDpi(kEdgeTriggerPx);
    const bool atEdge = pt.y <= trigger && pt.x >= rc.left && pt.x < rc.right;

    if (edgeHidden_) {
        // 隐藏后台：需鼠标先离开顶部边缘再滑回，才触发唤出，
        // 避免拖动到顶部松手时鼠标仍停留在边缘造成"隐藏即弹出"
        if (!edgeRevealArmed_) {
            if (!atEdge) edgeRevealArmed_ = true;
            return;
        }
        if (atEdge) restoreFromEdge(true);
        return;
    }

    if (dragging_) return;  // 用户拖动中不介入，防止与拖动操作抢占

    const bool inside = pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top &&
                        pt.y < rc.bottom;
    if (inside) {
        edgeHovered_ = true;
        return;
    }

    // 鼠标离开主界面区域：仅对「由贴边唤出」的窗口生效，并加延时防抖
    if (edgeShown_ && edgeHovered_ &&
        GetTickCount64() - edgeShownTick_ >= kEdgeLeaveDelayMs) {
        dockToEdge();
    }
}

void MainWindow::dockToEdge() {
    if (hwnd_ == nullptr || edgeHidden_) return;

    RECT rc = {0, 0, 0, 0};
    if (!GetWindowRect(hwnd_, &rc)) return;

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) return;

    const int height = rc.bottom - rc.top;
    const int peek = scaleForDpi(kEdgePeekPx);
    // 统一停靠公式：窗口底边 = 工作区顶边 + peek，即隐藏后在工作区内始终
    // 露出 peek 高的窄边，用户能大致看出隐藏位置；任务栏在底部/左右时
    // rcWork.top 就是屏幕顶部，效果同样是露出窄边，行为保持一致。
    const int y = mi.rcWork.top - height + peek;

    // 任务栏位于显示器顶部时，工作区顶边与屏幕顶部之间被任务栏占用。窗口
    // 矩形虽会延伸进该区域，但任务栏恒在普通窗口之上：只要隐藏期间窗口不
    // 保持置顶，重叠部分就会被任务栏盖住，既不遮挡/悬浮任务栏，又能露出窄边。
    // 因此置顶态窗口在隐藏期间临时降为普通层级，唤出时再恢复置顶。
    const bool taskbarAtTop = mi.rcWork.top > mi.rcMonitor.top;
    UINT flags = SWP_NOSIZE | SWP_NOACTIVATE;
    HWND insertAfter = nullptr;
    edgeTopMostSuppressed_ = false;
    if (taskbarAtTop && topMost_) {
        insertAfter = HWND_NOTOPMOST;
        edgeTopMostSuppressed_ = true;
    } else {
        flags |= SWP_NOZORDER;
    }

    if (!SetWindowPos(hwnd_, insertAfter, rc.left, y, 0, 0, flags)) {
        CLIPLOG_WARN(kTag, "贴边隐藏失败, errno=" +
                               std::to_string(GetLastError()));
        edgeTopMostSuppressed_ = false;
        return;
    }

    edgeRestoreLeft_ = rc.left;
    edgeHidden_ = true;
    edgeShown_ = false;
    edgeHovered_ = false;
    edgeRevealArmed_ = false;
    CLIPLOG_INFO(kTag, taskbarAtTop
                           ? "主界面已贴边隐藏（任务栏在顶部，工作区内露出窄边）"
                           : "主界面已贴边隐藏（屏幕顶部，露出窄边）");
}

void MainWindow::restoreFromEdge(bool armAutoHide) {
    if (hwnd_ == nullptr || !edgeHidden_) return;

    RECT rc = {0, 0, 0, 0};
    if (!GetWindowRect(hwnd_, &rc)) return;

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) return;

    const int width = rc.right - rc.left;
    int left = edgeRestoreLeft_;
    // 兜底：原水平位置已完全脱离工作区时贴工作区左边显示
    if (left + width <= mi.rcWork.left || left >= mi.rcWork.right) {
        left = mi.rcWork.left;
    }

    // 隐藏期间为避让顶部任务栏临时取消过置顶的窗口，唤出时恢复置顶态，
    // 保持与用户隐藏前设置的一致；其余情况维持原 Z 序不动
    UINT flags = SWP_NOSIZE | SWP_NOACTIVATE;
    HWND insertAfter = nullptr;
    if (!edgeTopMostSuppressed_) {
        flags |= SWP_NOZORDER;
    } else {
        insertAfter = HWND_TOPMOST;
    }

    if (!SetWindowPos(hwnd_, insertAfter, left, mi.rcWork.top, 0, 0, flags)) {
        CLIPLOG_WARN(kTag, "贴边唤出失败, errno=" +
                               std::to_string(GetLastError()));
        return;
    }

    if (edgeTopMostSuppressed_) {
        topMost_ = true;  // 与置顶按钮文案保持一致
        if (pinBtn_ != nullptr) SetWindowTextW(pinBtn_, L"取消置顶");
        CLIPLOG_INFO(kTag, "贴边唤出：已恢复窗口置顶态");
    }
    edgeTopMostSuppressed_ = false;

    edgeHidden_ = false;
    edgeShown_ = armAutoHide;  // 是否开启"鼠标离开即隐藏"判定
    edgeHovered_ = false;
    edgeRevealArmed_ = false;
    edgeShownTick_ = GetTickCount64();
    CLIPLOG_INFO(kTag, "主界面已从贴边状态唤出");
}

std::wstring MainWindow::makePreview(const std::string& utf8) {
    std::wstring wide = util::utf8ToWide(utf8);
    // 压平换行/制表符（仅用于预览显示，不修改真实数据，§78）
    std::wstring flat;
    flat.reserve(wide.size());
    for (wchar_t c : wide) {
        if (c == L'\r') continue;
        if (c == L'\n' || c == L'\t') {
            flat.push_back(L' ');
        } else {
            flat.push_back(c);
        }
    }
    // 控制字符替换为空格，防列表渲染异常
    for (wchar_t& c : flat) {
        if (c < 0x20) c = L' ';
    }
    if (flat.size() > kPreviewChars) {
        flat.resize(kPreviewChars);
        flat += L"...";
    }
    if (flat.empty()) flat = L"(空)";
    return flat;
}

void MainWindow::refreshHistory() {
    if (listBox_ == nullptr) return;

    // 刷新前记录选中项：置顶导致重排 / 新记录插入时，重排后恢复选中，
    // 避免用户当前关注的条目在右键置顶、取消置顶后跳走
    std::string selectedId;
    const int prevSel = static_cast<int>(
        SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
    if (prevSel != LB_ERR && prevSel >= 0 &&
        static_cast<size_t>(prevSel) < rows_.size()) {
        selectedId = rows_[static_cast<size_t>(prevSel)].eventId;
    }

    auto& repo = database::ClipboardRepository::instance();
    rows_ = repo.queryHistory(0, kMaxDisplayRows);

    SendMessageW(listBox_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(listBox_, LB_RESETCONTENT, 0, 0);
    for (const auto& ev : rows_) {
        std::wstring preview = makePreview(ev.content);
        // 置顶项加视觉前缀（置顶记录由 SQL 的 pinned DESC 排序保证在最前）
        if (ev.pinned) {
            preview = L"[置顶] " + preview;
        }
        SendMessageW(listBox_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(preview.c_str()));
    }
    SendMessageW(listBox_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listBox_, nullptr, TRUE);

    if (!selectedId.empty()) {
        reselectByEventId(selectedId);
    }
}

void MainWindow::updateConnStatus() {
    if (statusText_ == nullptr) return;

    const auto state = sync::SyncManager::instance().connectionState();
    const bool paused = sync::SyncManager::instance().paused();

    std::wstring text = L"● ";
    switch (state) {
        case sync::ConnState::Connected:
            text += L"已连接";
            break;
        case sync::ConnState::Connecting:
            text += L"连接中";
            break;
        case sync::ConnState::Disconnected:
        default:
            text += L"断开";
            break;
    }
    if (paused) text += L"(暂停)";
    SetWindowTextW(statusText_, text.c_str());
}

void MainWindow::setTopMost(bool on) {
    if (hwnd_ == nullptr) return;

    // 置顶态直接落在窗口的 WS_EX_TOPMOST 扩展样式上：
    // HWND_TOPMOST 保持在其他窗口之上，HWND_NOTOPMOST 恢复普通层级
    if (!SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        CLIPLOG_WARN(kTag, "窗口置顶状态切换失败, errno=" +
                               std::to_string(GetLastError()));
        return;
    }
    topMost_ = on;

    // 按钮文案与状态同步，保证「置顶 / 取消置顶」可来回切换
    if (pinBtn_ != nullptr) {
        SetWindowTextW(pinBtn_, topMost_ ? L"取消置顶" : L"置顶");
    }
    CLIPLOG_INFO(kTag, std::string("窗口置顶状态切换为 ") +
                           (topMost_ ? "置顶" : "取消置顶"));
}

void MainWindow::toggleTopMost() {
    setTopMost(!topMost_);
}

void MainWindow::show() {
    if (hwnd_ == nullptr) return;
    refreshHistory();
    updateConnStatus();
    // 若主界面处于贴边隐藏状态，先从屏幕顶部还原再正常显示
    // （armAutoHide=false：用户主动唤出时不立即触发"离开即隐藏"）
    if (edgeHidden_) restoreFromEdge(false);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::hide() {
    if (hwnd_ != nullptr) ShowWindow(hwnd_, SW_HIDE);
}

bool MainWindow::visible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

void MainWindow::destroy() {
    if (hwnd_ == nullptr) return;
    UiDispatcher::instance().unregisterWindow(hwnd_);
    HWND h = hwnd_;
    hwnd_ = nullptr;
    statusText_ = settingsBtn_ = headingText_ = pinBtn_ = listBox_ = nullptr;
    DestroyWindow(h);
}

void MainWindow::copySelectedItem() {
    if (listBox_ == nullptr) return;
    const int sel = static_cast<int>(
        SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR || sel < 0 ||
        static_cast<size_t>(sel) >= rows_.size()) {
        return;
    }

    // §9 / §12 / §76：HISTORY 来源写入剪贴板。
    // writeToClipboard 内置 suppress + hash 双层防循环：
    // 不发 WebSocket、不新增历史（§83）
    const ClipboardEvent& ev = rows_[static_cast<size_t>(sel)];
    if (clipboard::ClipboardMonitor::instance().writeToClipboard(
            ev.content)) {
        CLIPLOG_INFO(kTag, "历史项已写入剪贴板, eventId=" + ev.eventId);
    } else {
        CLIPLOG_WARN(kTag, "历史项写入剪贴板失败（剪贴板被占用）");
    }
}

void MainWindow::pasteSelectedItem() {
    if (listBox_ == nullptr) return;
    const int sel = static_cast<int>(
        SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR || sel < 0 ||
        static_cast<size_t>(sel) >= rows_.size()) {
        return;
    }

    // 1) 先写入系统剪贴板（与单击复制同一条路径，含 §14-§15 防循环标记）
    const ClipboardEvent ev = rows_[static_cast<size_t>(sel)];
    if (!clipboard::ClipboardMonitor::instance().writeToClipboard(ev.content)) {
        CLIPLOG_WARN(kTag,
                     "双击粘贴失败：历史项写入剪贴板失败（剪贴板被占用）");
        return;
    }

    // 2) 定位「上次光标所在输入框」：ClipLink 抢焦点前最后一个他进程前台窗口
    HWND target = ForegroundTracker::instance().lastForeground();
    if (target == nullptr) {
        CLIPLOG_WARN(kTag, "双击粘贴：无上次前台窗口记录，已仅复制到剪贴板");
        return;
    }

    // 3) 还原焦点到目标窗口后模拟 Ctrl+V
    if (!SetForegroundWindow(target)) {
        // 前台锁定 / 目标以更高完整性级别运行（UIPI）：降级为仅复制到剪贴板
        CLIPLOG_WARN(kTag, "双击粘贴：目标窗口置前台失败，已仅复制到剪贴板");
        return;
    }
    // 等待焦点真正切回目标窗口（SetForegroundWindow 异步生效，
    // 轮询上限约 200ms），否则 Ctrl+V 会发给错误的窗口
    for (int i = 0; i < 20 && GetForegroundWindow() != target; ++i) {
        Sleep(10);
    }

    if (sendPasteKeystroke()) {
        CLIPLOG_INFO(kTag,
                     "双击粘贴：已向目标窗口发送 Ctrl+V, eventId=" +
                         ev.eventId);
    } else {
        CLIPLOG_WARN(kTag, "双击粘贴：SendInput 失败, errno=" +
                               std::to_string(GetLastError()));
    }
}

void MainWindow::showItemContextMenu(LPARAM lParam) {
    if (listBox_ == nullptr || hwnd_ == nullptr) return;

    int index = -1;
    POINT pt = {0, 0};
    const bool fromKeyboard = (lParam == static_cast<LPARAM>(-1));
    if (fromKeyboard) {
        // 键盘触发（菜单键 / Shift+F10）：作用于当前选中项
        const int sel = static_cast<int>(
            SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR) return;
        index = sel;
    } else {
        // 鼠标右键：lParam 为屏幕坐标
        pt.x = static_cast<LONG>(static_cast<short>(LOWORD(lParam)));
        pt.y = static_cast<LONG>(static_cast<short>(HIWORD(lParam)));
        POINT client = pt;
        ScreenToClient(listBox_, &client);
        const LRESULT hit = SendMessageW(
            listBox_, LB_ITEMFROMPOINT, 0,
            MAKELPARAM(static_cast<WORD>(client.x),
                       static_cast<WORD>(client.y)));
        if (HIWORD(hit) != 0) return;  // 右键点在列表空白区域
        index = static_cast<int>(LOWORD(hit));
    }
    if (index < 0 || static_cast<size_t>(index) >= rows_.size()) return;

    // ListBox 右键默认不改变选择：先同步选中到被点击条目
    SendMessageW(listBox_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    const ClipboardEvent ev = rows_[static_cast<size_t>(index)];

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(kIdMenuTogglePin),
                ev.pinned ? L"取消置顶" : L"置顶");

    if (fromKeyboard) {
        // 键盘触发无鼠标坐标：贴着该项底部弹出
        RECT itemRc = {0, 0, 0, 0};
        if (SendMessageW(listBox_, LB_GETITEMRECT,
                         static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(&itemRc)) != LB_ERR) {
            pt.x = itemRc.left;
            pt.y = itemRc.bottom;
            ClientToScreen(listBox_, &pt);
        } else {
            GetCursorPos(&pt);
        }
    }

    // 弹出菜单期间窗口需处于前台，否则菜单无法正常收起
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd != static_cast<UINT>(kIdMenuTogglePin)) return;

    // 置顶 / 取消置顶：纯本地属性，写入后按 pinned DESC 重排列表（§63 事件驱动）
    auto& repo = database::ClipboardRepository::instance();
    if (repo.setPinned(ev.eventId, !ev.pinned)) {
        // 置顶状态写入成功：refreshHistory 按 pinned DESC 重排并恢复选中项
        refreshHistory();
        CLIPLOG_INFO(kTag, std::string("历史项置顶状态已切换: ") +
                               (ev.pinned ? "取消置顶" : "置顶") +
                               ", eventId=" + ev.eventId);
    } else {
        CLIPLOG_WARN(kTag, "历史项置顶状态切换失败, eventId=" + ev.eventId);
    }
}

void MainWindow::reselectByEventId(const std::string& eventId) {
    if (listBox_ == nullptr) return;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].eventId == eventId) {
            SendMessageW(listBox_, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
}

void MainWindow::onCommand(WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    const int id = LOWORD(wParam);
    const int code = HIWORD(wParam);

    if (id == kIdSettingsBtn && code == BN_CLICKED) {
        // §7 ⚙ -> 设置窗口；由 main 装配具体行为（UI/业务解耦，§62）
        if (settingsHandler_) settingsHandler_();
        return;
    }
    if (id == kIdPinBtn && code == BN_CLICKED) {
        // 快速置顶 / 取消置顶（同一按钮来回切换，无业务层依赖）
        toggleTopMost();
        return;
    }
    if (id == kIdHistoryList) {
        // §76：单击选择 -> 复制到系统剪贴板
        if (code == LBN_SELCHANGE) {
            copySelectedItem();
            return;
        }
        // 双击 -> 复制到剪贴板并粘贴到「上次光标所在输入框」
        // （双击前必然先收到一次 LBN_SELCHANGE，故两个动作互相独立）
        if (code == LBN_DBLCLK) {
            pasteSelectedItem();
            return;
        }
    }
}

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            layoutChildren();
            return 0;
        case WM_ENTERSIZEMOVE:
            // 用户开始拖动 / 缩放窗口：贴边判定延后到拖动结束，避免抢占操作
            dragging_ = true;
            return 0;
        case WM_EXITSIZEMOVE: {
            dragging_ = false;
            if (!edgeHideEnabled_ || edgeHidden_) return 0;
            // 拖动结束：窗口顶边贴住（或越过）屏幕工作区顶部 -> 贴边隐藏
            RECT rc = {0, 0, 0, 0};
            HMONITOR monitor =
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            const bool docked =
                GetWindowRect(hwnd, &rc) && monitor != nullptr &&
                GetMonitorInfoW(monitor, &mi) &&
                rc.top <= mi.rcWork.top + scaleForDpi(kEdgeDockThresholdPx);
            if (docked) {
                dockToEdge();
            } else if (edgeShown_) {
                // 用户把窗口从屏幕顶部拖走：转为普通窗口，取消"离开即隐藏"
                edgeShown_ = false;
                edgeHovered_ = false;
            }
            return 0;
        }
        case WM_TIMER:
            // 贴边隐藏：低频监测光标位置（唤出 / 离开判定）
            if (wParam == kEdgeTimerId) {
                onEdgeTimer();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case kMsgConfigChanged:
            // 设置保存后同步贴边隐藏开关，并刷新连接状态文案
            applyEdgeHideConfig();
            updateConnStatus();
            return 0;
        case WM_COMMAND:
            onCommand(wParam, lParam);
            return 0;
        case WM_CONTEXTMENU:
            // 历史列表右键：弹出「置顶 / 取消置顶」上下文菜单
            if (reinterpret_cast<HWND>(wParam) == listBox_) {
                showItemContextMenu(lParam);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case kMsgHistoryChanged:
            // §63：Repository 变更通知驱动刷新（非轮询）
            refreshHistory();
            return 0;
        case kMsgConnState:
            updateConnStatus();
            return 0;
        case WM_CLOSE:
            // §64：关闭默认隐藏到托盘，而不是退出
            hide();
            return 0;
        case WM_SETFOCUS:
            if (listBox_ != nullptr) SetFocus(listBox_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kEdgeTimerId);
            UiDispatcher::instance().unregisterWindow(hwnd);
            hwnd_ = nullptr;
            if (gearFont_ != nullptr) {
                DeleteObject(gearFont_);
                gearFont_ = nullptr;
            }
            font_ = nullptr;  // DEFAULT_GUI_FONT 为系统对象，不删除
            statusText_ = settingsBtn_ = headingText_ = pinBtn_ = listBox_ =
                nullptr;
            topMost_ = false;  // 窗口已销毁，置顶态随窗口样式一并失效
            // 贴边隐藏状态随窗口销毁一并复位
            edgeHideEnabled_ = false;
            edgeHidden_ = false;
            edgeShown_ = false;
            edgeHovered_ = false;
            edgeRevealArmed_ = false;
            edgeTopMostSuppressed_ = false;
            dragging_ = false;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace ui
}  // namespace cliplink
