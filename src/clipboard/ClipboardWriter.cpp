// ---------------------------------------------------------------------------
// 剪贴板写入实现（见需求 §58）
// ---------------------------------------------------------------------------

#include "clipboard/ClipboardWriter.h"

#include <cstring>
#include <string>

#include "clipboard/ClipboardReader.h"
#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace clipboard {

namespace {
constexpr const char* kTag = "ClipboardWriter";
}  // namespace

bool ClipboardWriter::write(const std::string& utf8, HWND owner) {
    if (utf8.empty()) {
        return false;  // 不写入空内容，避免制造空历史
    }

    const std::wstring wide = util::utf8ToWide(utf8);
    if (wide.empty()) {
        CLIPLOG_ERROR(kTag, "UTF-8 -> UTF-16 转换失败，写入取消");
        return false;
    }

    if (!ClipboardReader::openWithRetry(owner)) {
        CLIPLOG_ERROR(kTag, "写入失败：OpenClipboard 被占用");
        return false;
    }

    if (!EmptyClipboard()) {
        CLIPLOG_ERROR(kTag,
                      "EmptyClipboard 失败, errno=" +
                          std::to_string(GetLastError()));
        CloseClipboard();
        return false;
    }

    // Windows Clipboard 内存所有权（§58）：
    // GlobalAlloc 分配 -> SetClipboardData 成功后系统接管（不可 Free），
    // 失败则由本函数释放
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem == nullptr) {
        CLIPLOG_ERROR(kTag, "GlobalAlloc 失败");
        CloseClipboard();
        return false;
    }

    void* dst = GlobalLock(hMem);
    if (dst == nullptr) {
        GlobalFree(hMem);
        CloseClipboard();
        CLIPLOG_ERROR(kTag, "GlobalLock 失败");
        return false;
    }
    memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(hMem);

    if (SetClipboardData(CF_UNICODETEXT, hMem) == nullptr) {
        // 系统未接管内存，需自行释放
        GlobalFree(hMem);
        CLIPLOG_ERROR(kTag,
                      "SetClipboardData 失败, errno=" +
                          std::to_string(GetLastError()));
        CloseClipboard();
        return false;
    }

    // 成功：内存所有权已移交系统，不得 GlobalFree
    CloseClipboard();
    return true;
}

}  // namespace clipboard
}  // namespace cliplink
