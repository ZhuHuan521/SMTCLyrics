#include "util/Base64.h"

#include <array>

namespace smtc::util {
namespace {
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeValue(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}
}

std::string base64Encode(std::string_view text) {
    return base64Encode(std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[triple & 0x3F] : '=');
    }
    return out;
}

std::vector<std::uint8_t> base64Decode(std::string_view text) {
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
    const auto bytes = base64Decode(text);
    return {bytes.begin(), bytes.end()};
}

}
