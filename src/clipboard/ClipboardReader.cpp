// ---------------------------------------------------------------------------
// 剪贴板读取实现（见需求 §57 / §73 / §77-§79）
// ---------------------------------------------------------------------------

#include "clipboard/ClipboardReader.h"

#include <vector>

#include "common/DataTypes.h"
#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace clipboard {

namespace {

constexpr const char* kTag = "ClipboardReader";

// §57：立即尝试，失败后短延迟再试；总耗时约 100ms，非轮询
constexpr int kRetryDelaysMs[] = {0, 10, 30, 60};

}  // namespace

bool ClipboardReader::openWithRetry(HWND owner) {
    for (int delay : kRetryDelaysMs) {
        if (delay > 0) Sleep(static_cast<DWORD>(delay));
        if (OpenClipboard(owner)) {
            return true;
        }
    }
    return false;
}

ClipboardReader::Result ClipboardReader::read(HWND owner) {
    Result r;

    // §77：没有 CF_UNICODETEXT 直接视为空，不创建空历史
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        r.empty = true;
        return r;
    }

    if (!openWithRetry(owner)) {
        r.openFailed = true;
        CLIPLOG_WARN(kTag,
                     "OpenClipboard 失败（剪贴板被其他程序占用），"
                     "本次更新跳过");
        return r;
    }

    HGLOBAL hMem =
        static_cast<HGLOBAL>(GetClipboardData(CF_UNICODETEXT));
    if (hMem == nullptr) {
        CloseClipboard();
        r.empty = true;
        return r;
    }

    const wchar_t* wide =
        static_cast<const wchar_t*>(GlobalLock(hMem));
    if (wide == nullptr) {
        CloseClipboard();
        r.empty = true;
        return r;
    }

    // CF_UNICODETEXT 以 UTF-16 双 null 结尾，按第一个 null 取长度
    size_t wideLen = 0;
    while (wide[wideLen] != L'\0') {
        ++wideLen;
    }
    const size_t wideBytes = wideLen * sizeof(wchar_t);

    // §73 粗筛：UTF-8 字节数 >= UTF-16 字节数 / 2，粗筛可提前拦截超大文本
    if (wideBytes / 2 > constants::kMaxTextBytes) {
        GlobalUnlock(hMem);
        CloseClipboard();
        r.tooLarge = true;
        CLIPLOG_WARN(kTag, "剪贴板内容过大（超过 1MB），未同步");
        return r;
    }

    // UTF-16 -> UTF-8（§79），文本原样不清理（§78）
    const std::wstring content(wide, wideLen);
    GlobalUnlock(hMem);
    CloseClipboard();

    std::string utf8 = util::wideToUtf8(content);
    if (utf8.size() > constants::kMaxTextBytes) {
        r.tooLarge = true;
        CLIPLOG_WARN(kTag, "剪贴板内容过大（超过 1MB），未同步");
        return r;
    }
    if (utf8.empty()) {
        r.empty = true;
        return r;
    }

    r.text = std::move(utf8);
    r.ok = true;
    return r;
}

}  // namespace clipboard
}  // namespace cliplink
