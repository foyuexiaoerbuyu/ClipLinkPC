// ---------------------------------------------------------------------------
// 主窗口实现（§6-§9 / §63 / §64 / §75-§76）
// ---------------------------------------------------------------------------

#include "ui/MainWindow.h"

#include <algorithm>

#include "clipboard/ClipboardMonitor.h"
#include "database/ClipboardRepository.h"
#include "sync/SyncManager.h"
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

// 单屏展示行数上限（超出的更旧记录不渲染，避免超长列表拖慢 UI）
constexpr int kMaxDisplayRows = 200;
// 内容预览最大宽字符数（§8 长文本截断）
constexpr size_t kPreviewChars = 60;
// 逻辑设计尺寸（§6，随后按 DPI 缩放）
constexpr int kDesignWidth = 200;
constexpr int kDesignHeight = 500;

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

    auto& repo = database::ClipboardRepository::instance();
    rows_ = repo.queryHistory(0, kMaxDisplayRows);

    SendMessageW(listBox_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(listBox_, LB_RESETCONTENT, 0, 0);
    for (const auto& ev : rows_) {
        const std::wstring preview = makePreview(ev.content);
        SendMessageW(listBox_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(preview.c_str()));
    }
    SendMessageW(listBox_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listBox_, nullptr, TRUE);
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
        // §76：单击选择与双击均执行"复制到系统剪贴板"
        if (code == LBN_SELCHANGE || code == LBN_DBLCLK) {
            copySelectedItem();
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
        case WM_COMMAND:
            onCommand(wParam, lParam);
            return 0;
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
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace ui
}  // namespace cliplink
