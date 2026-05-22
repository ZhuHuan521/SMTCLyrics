#include "util/Encoding.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace smtc::util {
namespace {

std::wstring multiByteToWide(std::string_view text, unsigned codePage) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        const int fallbackSize = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (fallbackSize <= 0) {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(fallbackSize), L'\0');
        MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), fallbackSize);
        return result;
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string wideToMultiByte(std::wstring_view text, unsigned codePage) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool looksLikeUtf8(const std::vector<std::uint8_t>& bytes) {
    int remaining = 0;
    for (const auto byte : bytes) {
        if (remaining == 0) {
            if ((byte & 0x80) == 0) {
                continue;
            }
            if ((byte & 0xE0) == 0xC0) {
                remaining = 1;
            } else if ((byte & 0xF0) == 0xE0) {
                remaining = 2;
            } else if ((byte & 0xF8) == 0xF0) {
                remaining = 3;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            --remaining;
        }
    }
    return remaining == 0;
}

int hexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

}

std::wstring utf8ToWide(std::string_view text) {
    return multiByteToWide(text, CP_UTF8);
}

std::string wideToUtf8(std::wstring_view text) {
    return wideToMultiByte(text, CP_UTF8);
}

std::wstring ansiToWide(std::string_view text, unsigned codePage) {
    return multiByteToWide(text, codePage);
}

std::string wideToAnsi(std::wstring_view text, unsigned codePage) {
    return wideToMultiByte(text, codePage);
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::wstring readTextAuto(const std::filesystem::path& path) {
    auto bytes = readFileBytes(path);
    if (bytes.empty()) {
        return {};
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return utf8ToWide(std::string_view(reinterpret_cast<const char*>(bytes.data() + 3), bytes.size() - 3));
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    }
    std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (looksLikeUtf8(bytes)) {
        return utf8ToWide(raw);
    }
    return ansiToWide(raw, 936);
}

std::string readUtf8Auto(const std::filesystem::path& path) {
    return wideToUtf8(readTextAuto(path));
}

std::string urlEncode(std::string_view utf8) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : utf8) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string htmlDecodeUtf8(std::string_view utf8) {
    std::wstring input = utf8ToWide(utf8);
    std::wstring out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != L'&') {
            out.push_back(input[i]);
            continue;
        }
        const auto semicolon = input.find(L';', i + 1);
        if (semicolon == std::wstring::npos) {
            out.push_back(input[i]);
            continue;
        }
        const auto entity = input.substr(i + 1, semicolon - i - 1);
        if (entity == L"amp") out.push_back(L'&');
        else if (entity == L"lt") out.push_back(L'<');
        else if (entity == L"gt") out.push_back(L'>');
        else if (entity == L"quot") out.push_back(L'\"');
        else if (entity == L"apos") out.push_back(L'\'');
        else if (!entity.empty() && entity[0] == L'#') {
            int value = 0;
            if (entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X')) {
                for (std::size_t j = 2; j < entity.size(); ++j) {
                    const int n = hexValue(entity[j]);
                    if (n < 0) { value = 0; break; }
                    value = value * 16 + n;
                }
            } else {
                for (std::size_t j = 1; j < entity.size(); ++j) {
                    if (entity[j] < L'0' || entity[j] > L'9') { value = 0; break; }
                    value = value * 10 + entity[j] - L'0';
                }
            }
            if (value > 0) out.push_back(static_cast<wchar_t>(value));
        } else {
            out.append(L"&").append(entity).push_back(L';');
        }
        i = semicolon;
    }
    return wideToUtf8(out);
}

std::wstring trim(std::wstring_view text) {
    const auto begin = std::find_if_not(text.begin(), text.end(), [](wchar_t ch) { return iswspace(ch) != 0; });
    const auto end = std::find_if_not(text.rbegin(), text.rend(), [](wchar_t ch) { return iswspace(ch) != 0; }).base();
    if (begin >= end) return {};
    return {begin, end};
}

std::string trimUtf8(std::string_view text) {
    return wideToUtf8(trim(utf8ToWide(text)));
}

std::wstring replaceAll(std::wstring text, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return text;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) return text;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

}
