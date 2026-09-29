#pragma once

// ---------------------------------------------------------------------------
// RFC 6455 WebSocket 传输层公共实现（客户端 / 服务端共用）
// - 纯 Winsock2，ws:// 明文（§37 MVP 局域网）
// - HTTP Upgrade 握手：Sec-WebSocket-Key -> SHA-1 + Base64 -> Accept（§握手）
// - 帧编解码：FIN/RSV/opcode、126/127 扩展长度、客户端掩码、控制帧
//   125 字节上限、分片（continuation）重组、单条消息总长上限（§74）
// - 本文件只依赖 winsock2 / 标准库，不依赖 windows.h，
//   可被任何 .cpp 安全包含（winsock2 必须先于 windows.h，见文件尾说明）
// - 需求出处：§40 心跳、§73 单条内容上限、§74 服务端防超大消息
// ---------------------------------------------------------------------------

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace cliplink {
namespace sync {
namespace ws {

// ---- Winsock 生命周期（进程级引用计数，可重复调用）------------------------
namespace detail {
inline std::mutex& winsockMutex() {
    static std::mutex m;
    return m;
}
inline int& winsockRefcount() {
    static int c = 0;
    return c;
}
}  // namespace detail

inline void ensureWinsock() {
    std::lock_guard<std::mutex> lock(detail::winsockMutex());
    if (detail::winsockRefcount() == 0) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ++detail::winsockRefcount();
}

inline void releaseWinsock() {
    std::lock_guard<std::mutex> lock(detail::winsockMutex());
    if (detail::winsockRefcount() > 0) {
        --detail::winsockRefcount();
        if (detail::winsockRefcount() == 0) WSACleanup();
    }
}

// ---- SHA-1（RFC 3174）：仅用于握手 Accept，不做安全认证--------------------
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                     0xC3D2E1F0u};
    const uint64_t totalBits = static_cast<uint64_t>(len) * 8ull;
    // 消息补齐：0x80 + 0 填充 + 8 字节大端位长，凑到 64 的倍数
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFFu));
    }

    auto rotl = [](uint32_t v, int n) {
        return static_cast<uint32_t>((v << n) | (v >> (32 - n)));
    };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp =
                rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
}

// ---- Base64 ---------------------------------------------------------------
inline std::string base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           (static_cast<uint32_t>(data[i + 2]));
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }
    const size_t rest = len - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// 客户端握手随机 Key（16 字节随机 -> Base64，RFC 6455 §4.1）
inline std::string randomWebSocketKey() {
    static std::mt19937 rng{std::random_device{}()};
    uint8_t raw[16];
    {
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        for (auto& b : raw) b = static_cast<uint8_t>(rng() & 0xFF);
    }
    return base64Encode(raw, sizeof(raw));
}

// 服务端 Accept：base64( sha1( key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
inline std::string computeAcceptKey(const std::string& clientKey) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string material = clientKey + kGuid;
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(material.data()), material.size(),
         digest);
    return base64Encode(digest, sizeof(digest));
}

// ---- HTTP 头解析 / 构建（握手）-------------------------------------------
struct HttpHeaders {
    std::string firstLine;
    // key 一律小写
    std::vector<std::pair<std::string, std::string>> fields;

    const std::string* find(const std::string& key) const {
        for (const auto& kv : fields) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    std::string value(const std::string& key,
                      const std::string& def = std::string()) const {
        const std::string* v = find(key);
        return v != nullptr ? *v : def;
    }
};

inline std::string toLowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string trimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 解析 HTTP 头块（不含空行）；失败返回 false
inline bool parseHttpHeaders(const std::string& head, HttpHeaders& out) {
    size_t pos = head.find("\r\n");
    out.firstLine = head.substr(0, pos == std::string::npos ? head.size() : pos);
    out.fields.clear();
    if (pos == std::string::npos) return !out.firstLine.empty();
    pos += 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.fields.emplace_back(toLowerAscii(trimAscii(line.substr(0, colon))),
                                trimAscii(line.substr(colon + 1)));
    }
    return true;
}

// Connection 头可能是 "Upgrade" / "keep-alive, Upgrade" 等列表（大小写不敏感）
inline bool headerContainsToken(const std::string& value,
                                const std::string& tokenLower) {
    const std::string lower = toLowerAscii(value);
    size_t pos = 0;
    while (pos <= lower.size()) {
        size_t comma = lower.find(',', pos);
        if (comma == std::string::npos) comma = lower.size();
        const std::string part =
            trimAscii(lower.substr(pos, comma - pos));
        if (part == tokenLower) return true;
        pos = comma + 1;
    }
    return false;
}

inline std::string buildClientHandshakeRequest(const std::string& host,
                                               int port,
                                               const std::string& path,
                                               const std::string& key) {
    std::string req = "GET " + (path.empty() ? std::string("/") : path) +
                      " HTTP/1.1\r\n"
                      "Host: " + host + ":" + std::to_string(port) + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + key + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "\r\n";
    return req;
}

inline std::string buildServerHandshakeResponse(const std::string& key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " +
           computeAcceptKey(key) + "\r\n"
           "\r\n";
}

// ---- 帧编解码 -------------------------------------------------------------
enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA
};

// 构造一帧；masked=true 时客户端发送必须掩码（RFC 6455 §5.3）
inline std::vector<uint8_t> encodeFrame(Opcode op, const uint8_t* payload,
                                        size_t len, bool fin = true,
                                        bool masked = false) {
    std::vector<uint8_t> frame;
    frame.reserve(len + 14);
    uint8_t b0 = static_cast<uint8_t>(op);
    if (fin) b0 |= 0x80;
    frame.push_back(b0);

    uint8_t maskBit = masked ? 0x80 : 0x00;
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(maskBit | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<uint8_t>(maskBit | 126));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(static_cast<uint8_t>(maskBit | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>(
                (static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }

    if (masked) {
        static std::mt19937 rng{std::random_device{}()};
        uint8_t key[4];
        {
            static std::mutex m;
            std::lock_guard<std::mutex> lock(m);
            for (auto& b : key) b = static_cast<uint8_t>(rng() & 0xFF);
        }
        frame.insert(frame.end(), key, key + 4);
        const size_t base = frame.size();
        frame.insert(frame.end(), payload, payload + len);
        for (size_t i = 0; i < len; ++i) {
            frame[base + i] ^= key[i % 4];
        }
    } else if (len > 0) {
        frame.insert(frame.end(), payload, payload + len);
    }
    return frame;
}

// 线路上的单帧（接收侧解码结果）
struct DecodedFrame {
    Opcode op = Opcode::Continuation;
    bool fin = false;
    std::string payload;
};

// 增量接收器：字节流 -> 完整消息（自动重组分片），单条消息超上限即报错（§74）
class MessageReader {
public:
    enum class Item {
        Text,     // 完整文本消息
        Binary,   // 完整二进制消息（MVP 仅记录忽略）
        Ping,
        Pong,
        Close,
        Error     // 协议违规 / 超限，必须断开本连接
    };

    explicit MessageReader(size_t maxMessageBytes)
        : maxMessageBytes_(maxMessageBytes) {}

    void append(const char* data, size_t len) { buf_.append(data, len); }

    // 取下一个完整项；无完整数据返回 false；出错返回 true 且 item=Error
    bool next(Item& item, std::string& payload) {
        if (error_) {
            item = Item::Error;
            payload = errMsg_;
            return true;
        }
        for (;;) {
            if (buf_.size() - pos_ < 2) return false;
            const uint8_t b0 = static_cast<uint8_t>(buf_[pos_]);
            const uint8_t b1 = static_cast<uint8_t>(buf_[pos_ + 1]);
            const bool fin = (b0 & 0x80) != 0;
            const uint8_t rsv = static_cast<uint8_t>(b0 & 0x70);
            const uint8_t opcode = static_cast<uint8_t>(b0 & 0x0F);
            const bool masked = (b1 & 0x80) != 0;
            uint64_t payloadLen = b1 & 0x7F;
            size_t head = 2;

            if (rsv != 0) return fail("RSV 位非零（不支持扩展）");
            if (payloadLen == 126) {
                if (buf_.size() - pos_ < 4) return false;
                payloadLen = (static_cast<uint64_t>(
                                  static_cast<uint8_t>(buf_[pos_ + 2]))
                              << 8) |
                             static_cast<uint8_t>(buf_[pos_ + 3]);
                head = 4;
            } else if (payloadLen == 127) {
                if (buf_.size() - pos_ < 10) return false;
                payloadLen = 0;
                for (int i = 0; i < 8; ++i) {
                    payloadLen = (payloadLen << 8) |
                                 static_cast<uint8_t>(buf_[pos_ + 2 + i]);
                }
                if ((payloadLen >> 63) != 0) {
                    return fail("64 位长度最高位非零");
                }
                head = 10;
            }

            const bool isControl = (opcode & 0x08) != 0;
            if (isControl) {
                if (!fin) return fail("控制帧不允许分片");
                if (payloadLen > 125) return fail("控制帧载荷超过 125 字节");
            }
            // 单帧与累计消息双重上限（§74：防 100MB/1GB 恶意消息）
            if (payloadLen > maxMessageBytes_) {
                return fail("单帧超过消息上限 " +
                            std::to_string(maxMessageBytes_) + " 字节");
            }
            if (!isControl && fragmenting_ &&
                fragmentBytes_ + payloadLen > maxMessageBytes_) {
                return fail("分片消息累计超过上限");
            }

            const size_t maskLen = masked ? 4 : 0;
            const size_t need = head + maskLen + static_cast<size_t>(payloadLen);
            if (buf_.size() - pos_ < need) return false;

            std::string data(buf_.data() + pos_ + head + maskLen,
                             static_cast<size_t>(payloadLen));
            if (masked) {
                const uint8_t* key =
                    reinterpret_cast<const uint8_t*>(buf_.data()) + pos_ + head;
                for (size_t i = 0; i < data.size(); ++i) {
                    data[i] = static_cast<char>(
                        static_cast<uint8_t>(data[i]) ^ key[i % 4]);
                }
            }
            pos_ += need;
            compact();

            switch (opcode) {
                case 0x0:  // Continuation
                    if (!fragmenting_) return fail("意外的 continuation 帧");
                    fragmentBuf_.append(data);
                    fragmentBytes_ += data.size();
                    if (fin) {
                        item = Item::Text;  // MVP 只发文本；二进制已忽略
                        if (fragOpcode_ == 0x2) item = Item::Binary;
                        payload.swap(fragmentBuf_);
                        fragmenting_ = false;
                        fragmentBuf_.clear();
                        fragmentBytes_ = 0;
                        return true;
                    }
                    break;
                case 0x1:  // Text
                case 0x2:  // Binary
                    if (fragmenting_) return fail("分片期间收到新消息帧");
                    if (fin) {
                        item = opcode == 0x1 ? Item::Text : Item::Binary;
                        payload.swap(data);
                        return true;
                    }
                    fragmenting_ = true;
                    fragOpcode_ = opcode;
                    fragmentBuf_.swap(data);
                    fragmentBytes_ = fragmentBuf_.size();
                    break;
                case 0x8:  // Close
                    item = Item::Close;
                    payload.swap(data);
                    return true;
                case 0x9:  // Ping
                    item = Item::Ping;
                    payload.swap(data);
                    return true;
                case 0xA:  // Pong
                    item = Item::Pong;
                    payload.swap(data);
                    return true;
                default:
                    return fail("未知 opcode " + std::to_string(opcode));
            }
        }
    }

    bool failed() const { return error_; }
    const std::string& errorMessage() const { return errMsg_; }

private:
    bool fail(const std::string& why) {
        error_ = true;
        errMsg_ = why;
        return true;
    }

    void compact() {
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        } else if (pos_ > 64 * 1024) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
    }

    size_t maxMessageBytes_;
    std::string buf_;
    size_t pos_ = 0;
    bool fragmenting_ = false;
    uint8_t fragOpcode_ = 0;
    std::string fragmentBuf_;
    uint64_t fragmentBytes_ = 0;
    bool error_ = false;
    std::string errMsg_;
};

// ---- socket 收发辅助 ------------------------------------------------------
// 全量发送：处理短写；SO_SNDTIMEO 超时返回 false（由调用方判定断线）
inline bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n == SOCKET_ERROR || n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// 统一设置收发超时（毫秒）
inline void setSocketTimeouts(SOCKET s, int sendMs, int recvMs) {
    DWORD v = static_cast<DWORD>(sendMs);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&v), sizeof(v));
    v = static_cast<DWORD>(recvMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&v), sizeof(v));
}

// 连续空闲计数（SO_RCVTIMEO 触发计一次）
inline bool isTimedOutSocket() {
    const int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
}

}  // namespace ws
}  // namespace sync
}  // namespace cliplink

// 注意：winsock2.h 与 windows.h 的包含顺序敏感——winsock2 必须在 windows.h
// 之前出现。本头已自带 winsock2；若某 .cpp 后续包含 windows.h 无冲突，
// 反向（先 windows.h 再本头）会触发 winsock.h 冲突，因此所有包含 winsock
// 的实现文件都应先包含本项目网络头，或确保 WIN32_LEAN_AND_MEAN。
