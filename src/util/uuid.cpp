// rand_s 声明受 _CRT_RAND_S 控制，必须在任何标准头之前定义
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif

#include "uuid.h"

#include <windows.h>
#include <objbase.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdlib.h>

namespace cliplink {
namespace util {

namespace {

// 将 GUID 格式化为小写标准 UUID 字符串
std::string formatGuid(const GUID& g) {
    char buf[37];
    std::snprintf(
        buf, sizeof(buf),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(g.Data1),
        static_cast<unsigned>(g.Data2),
        static_cast<unsigned>(g.Data3),
        static_cast<unsigned>(g.Data4[0]),
        static_cast<unsigned>(g.Data4[1]),
        static_cast<unsigned>(g.Data4[2]),
        static_cast<unsigned>(g.Data4[3]),
        static_cast<unsigned>(g.Data4[4]),
        static_cast<unsigned>(g.Data4[5]),
        static_cast<unsigned>(g.Data4[6]),
        static_cast<unsigned>(g.Data4[7]));
    // 统一为小写（MinGW 下 %lx 已小写，此处保险）
    for (char* p = buf; *p; ++p) {
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return std::string(buf, 36);
}

// 兜底：操作系统 API 不可用时使用系统随机源构造 UUID v4
std::string randomUuidV4() {
    unsigned char bytes[16] = {0};
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) {
            ok = false;
            break;
        }
        bytes[i] = static_cast<unsigned char>(v & 0xffu);
    }
    if (!ok) {
        std::random_device rd;
        for (int i = 0; i < 16; ++i) {
            bytes[i] = static_cast<unsigned char>(rd() & 0xffu);
        }
    }
    // 版本 4（variant 10）
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fu) | 0x80u);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0fu]);
    }
    return out;
}

}  // namespace

std::string uuidV4() {
    GUID guid;
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        return formatGuid(guid);
    }
    return randomUuidV4();
}

}  // namespace util
}  // namespace cliplink
