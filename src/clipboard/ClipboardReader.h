#pragma once

// ---------------------------------------------------------------------------
// 剪贴板读取（见需求 §10 / §57 / §73 / §77-§79）
// - 仅支持 CF_UNICODETEXT（MVP 范围，§10）
// - OpenClipboard 短延迟有限重试（立即 / 10ms / 30ms / 60ms 共 4 次，
//   属于 §57 的有限重试，不是轮询）
// - UTF-16 -> UTF-8 转换，1MB 上限标记（§73），空内容标记（§77）
// - 文本原样返回：空格 / 换行 / Tab / Emoji 均不清理（§78）
// ---------------------------------------------------------------------------

#include <string>

#include <windows.h>

namespace cliplink {
namespace clipboard {

class ClipboardReader {
public:
    struct Result {
        bool ok = false;        // 成功读到非空文本
        bool empty = false;     // 无 CF_UNICODETEXT 或内容为空（§77 忽略）
        bool tooLarge = false;  // 超过 1MB 上限（§73 不同步）
        bool openFailed = false;// OpenClipboard 重试耗尽（§72 不崩溃）
        std::string text;       // UTF-8 原样文本
    };

    // owner：OpenClipboard 的窗口句柄（传监控窗口，减少被占用概率）
    static Result read(HWND owner = nullptr);

    // 有限重试打开剪贴板（§57）：立即尝试 -> 失败短延迟 -> 再试
    static bool openWithRetry(HWND owner);
};

}  // namespace clipboard
}  // namespace cliplink
