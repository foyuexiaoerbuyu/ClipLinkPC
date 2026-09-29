#pragma once

// ---------------------------------------------------------------------------
// 剪贴板写入（见需求 §58 / §82-§83）
// - 标准流程：OpenClipboard -> EmptyClipboard -> SetClipboardData ->
//   CloseClipboard，仅 CF_UNICODETEXT（§10）
// - 内存所有权：SetClipboardData 成功后系统接管内存，失败时由本函数释放
// - OpenClipboard 同样使用短延迟有限重试（§57）
// ---------------------------------------------------------------------------

#include <string>

#include <windows.h>

namespace cliplink {
namespace clipboard {

class ClipboardWriter {
public:
    // 写入 UTF-8 文本（内部转 UTF-16）；owner 为 OpenClipboard 窗口句柄
    // 返回 false 表示写入失败（剪贴板被占用等，§72 不崩溃）
    static bool write(const std::string& utf8, HWND owner = nullptr);
};

}  // namespace clipboard
}  // namespace cliplink
