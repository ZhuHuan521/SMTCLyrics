#include "util/Base64.h"

#include <array>

namespace smtc::util {
namespace {

// 标准 Base64 字母表。
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 把一个 Base64 字符还原成 0..63；非法字符返回 -1。
int decodeValue(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// 以三个字节为一组编码成四个字符，末尾不足时用 '=' 补齐。
std::string base64EncodeBytes(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t a = data[i];
        const std::uint32_t b = i + 1 < size ? data[i + 1] : 0;
        const std::uint32_t c = i + 2 < size ? data[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? alphabet[triple & 0x3F] : '=');
    }
    return out;
}
}

std::string base64Encode(std::string_view text) {
    // string_view 版本直接把文本按原始字节编码。
    return base64EncodeBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    // vector 版本用于接口返回体或摘要字节。
    return base64EncodeBytes(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> base64Decode(std::string_view text) {
    // 解码时忽略非 Base64 字符，方便处理接口偶尔带换行的内容。
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = -8;
    for (unsigned char ch : text) {
        if (ch == '=') break;
        const int decoded = decodeValue(ch);
        if (decoded < 0) continue;
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string base64DecodeToString(std::string_view text) {
    // 歌词接口通常返回 UTF-8 文本，这里只负责字节到 string 的搬运。
    const auto bytes = base64Decode(text);
    return {bytes.begin(), bytes.end()};
}

}
