#include "config/Config.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cwchar>

namespace smtc::config {
namespace {

std::wstring toWide(std::wstring_view text) {
    return std::wstring(text.begin(), text.end());
}

int clampSourcePriority(int value, int fallback) {
    if (value < 0 || value > 3) return fallback;
    return value + 1;
}

}

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

AppConfig ConfigStore::load() const {
    AppConfig config;
    config.font.name = readString(L"字体", L"字体名称", config.font.name);
    config.font.size = readInt(L"字体", L"字体大小", config.font.size);
    config.font.bold = readBool(L"字体", L"字体加粗", config.font.bold);
    config.font.italic = readBool(L"字体", L"字体倾斜", config.font.italic);
    config.font.underline = readBool(L"字体", L"字体下划线", config.font.underline);

    config.normal.color1 = static_cast<COLORREF>(readInt(L"字体", L"文字颜色1", static_cast<int>(config.normal.color1)));
    config.normal.color2 = static_cast<COLORREF>(readInt(L"字体", L"文字颜色2", static_cast<int>(config.normal.color2)));
    config.normal.border = static_cast<COLORREF>(readInt(L"字体", L"文字边框颜色", static_cast<int>(config.normal.border)));
    config.normal.gradientMode = readInt(L"字体", L"渐变模式", config.normal.gradientMode);
    config.highlight.color1 = static_cast<COLORREF>(readInt(L"字体", L"高亮文字颜色1", static_cast<int>(config.highlight.color1)));
    config.highlight.color2 = static_cast<COLORREF>(readInt(L"字体", L"高亮文字颜色2", static_cast<int>(config.highlight.color2)));
    config.highlight.border = static_cast<COLORREF>(readInt(L"字体", L"高亮文字边框颜色", static_cast<int>(config.highlight.border)));
    config.highlight.gradientMode = readInt(L"字体", L"高亮文字渐变模式", config.highlight.gradientMode);

    config.cookie = readString(L"账号", L"cookie", L"");
    config.lyricOffsetSeconds = readInt(L"歌词", L"微调", 0);
    config.smtcMode = std::clamp(readInt(L"SMTC", L"SMTC", 1), 1, 2);
    config.displayMode = std::clamp(readInt(L"显示方式", L"显示方式", 1), 1, 3);

    config.sourcePriority = {
        clampSourcePriority(readInt(L"歌词源", L"优先级1", 0), 0),
        clampSourcePriority(readInt(L"歌词源", L"优先级2", 1), 1),
        clampSourcePriority(readInt(L"歌词源", L"优先级3", 2), 2),
        clampSourcePriority(readInt(L"歌词源", L"优先级4", 3), 3),
    };

    const auto top = readString(L"歌词窗口", L"顶边", L"");
    if (!top.empty()) {
        config.window.top = readInt(L"歌词窗口", L"顶边", 0);
        config.window.height = readInt(L"歌词窗口", L"高度", 150);
        config.window.width = readInt(L"歌词窗口", L"宽度", 0);
        config.window.left = readInt(L"歌词窗口", L"左边", 0);
        config.window.hasPosition = config.window.width > 0 && config.window.height > 0;
    }

    return config;
}

void ConfigStore::save(const AppConfig& config) const {
    writeString(L"字体", L"字体名称", config.font.name);
    writeString(L"字体", L"字体大小", std::to_wstring(config.font.size));
    writeString(L"字体", L"字体加粗", config.font.bold ? L"真" : L"假");
    writeString(L"字体", L"字体倾斜", config.font.italic ? L"真" : L"假");
    writeString(L"字体", L"字体下划线", config.font.underline ? L"真" : L"假");
    writeString(L"字体", L"文字颜色1", std::to_wstring(config.normal.color1));
    writeString(L"字体", L"文字颜色2", std::to_wstring(config.normal.color2));
    writeString(L"字体", L"文字边框颜色", std::to_wstring(config.normal.border));
    writeString(L"字体", L"渐变模式", std::to_wstring(config.normal.gradientMode));
    writeString(L"字体", L"高亮文字颜色1", std::to_wstring(config.highlight.color1));
    writeString(L"字体", L"高亮文字颜色2", std::to_wstring(config.highlight.color2));
    writeString(L"字体", L"高亮文字边框颜色", std::to_wstring(config.highlight.border));
    writeString(L"字体", L"高亮文字渐变模式", std::to_wstring(config.highlight.gradientMode));
    writeString(L"账号", L"cookie", config.cookie);
    writeString(L"歌词", L"微调", std::to_wstring(config.lyricOffsetSeconds));
    for (std::size_t i = 0; i < config.sourcePriority.size() && i < 4; ++i) {
        writeString(L"歌词源", L"优先级" + std::to_wstring(i + 1), std::to_wstring(std::clamp(config.sourcePriority[i], 1, 4) - 1));
    }
    writeString(L"SMTC", L"SMTC", std::to_wstring(std::clamp(config.smtcMode, 1, 2)));
    writeString(L"显示方式", L"显示方式", std::to_wstring(std::clamp(config.displayMode, 1, 3)));
    if (config.window.hasPosition) {
        saveWindow(config.window);
    }
}

void ConfigStore::saveWindow(const WindowConfig& window) const {
    writeString(L"歌词窗口", L"顶边", std::to_wstring(window.top));
    writeString(L"歌词窗口", L"高度", std::to_wstring(window.height));
    writeString(L"歌词窗口", L"宽度", std::to_wstring(window.width));
    writeString(L"歌词窗口", L"左边", std::to_wstring(window.left));
}

void ConfigStore::saveDisplayMode(int mode) const {
    writeString(L"显示方式", L"显示方式", std::to_wstring(std::clamp(mode, 1, 3)));
}

void ConfigStore::saveSmtcMode(int mode) const {
    writeString(L"SMTC", L"SMTC", std::to_wstring(std::clamp(mode, 1, 2)));
}

std::wstring ConfigStore::readString(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const {
    std::wstring buffer(1024, L'\0');
    const DWORD chars = GetPrivateProfileStringW(toWide(section).c_str(), toWide(key).c_str(), toWide(fallback).c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), path_.c_str());
    buffer.resize(chars);
    return buffer;
}

int ConfigStore::readInt(std::wstring_view section, std::wstring_view key, int fallback) const {
    const auto text = util::trim(readString(section, key, std::to_wstring(fallback)));
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 10);
    return end == text.c_str() ? fallback : static_cast<int>(value);
}

bool ConfigStore::readBool(std::wstring_view section, std::wstring_view key, bool fallback) const {
    const auto text = util::trim(readString(section, key, fallback ? L"真" : L"假"));
    if (text == L"真" || text == L"true" || text == L"TRUE" || text == L"1") return true;
    if (text == L"假" || text == L"false" || text == L"FALSE" || text == L"0") return false;
    return fallback;
}

void ConfigStore::writeString(std::wstring_view section, std::wstring_view key, std::wstring_view value) const {
    WritePrivateProfileStringW(toWide(section).c_str(), toWide(key).c_str(), toWide(value).c_str(), path_.c_str());
}

}
