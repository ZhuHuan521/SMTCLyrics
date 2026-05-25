#include "config/Config.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cwchar>
#include <iomanip>
#include <sstream>

namespace smtc::config {
namespace {

// 读取 INI 时用于区分“缺失”和“用户真的写了 fallback 值”的哨兵。
constexpr wchar_t kMissingValue[] = L"\xFDD0__SMTCLYRICS_MISSING__";

// Win32 INI API 需要以零结尾的 std::wstring。
std::wstring toWide(std::wstring_view text) {
    return std::wstring(text.begin(), text.end());
}

// 宽字符整数解析，失败时返回调用方给定的默认值。
int parseInt(std::wstring_view text, int fallback) {
    const auto trimmed = util::trim(text);
    if (trimmed.empty()) return fallback;
    wchar_t* end = nullptr;
    const long value = std::wcstol(trimmed.c_str(), &end, 10);
    return end == trimmed.c_str() ? fallback : static_cast<int>(value);
}

// 支持新版 #RRGGBB，也兼容旧版直接保存 COLORREF 整数。
COLORREF parseColor(std::wstring_view text, COLORREF fallback) {
    const auto trimmed = util::trim(text);
    if (trimmed.empty()) return fallback;

    if (trimmed.size() == 7 && trimmed[0] == L'#') {
        const std::wstring hex(trimmed.substr(1));
        wchar_t* end = nullptr;
        const long value = std::wcstol(hex.c_str(), &end, 16);
        if (end != hex.c_str()) {
            const BYTE red = static_cast<BYTE>((value >> 16) & 0xFF);
            const BYTE green = static_cast<BYTE>((value >> 8) & 0xFF);
            const BYTE blue = static_cast<BYTE>(value & 0xFF);
            return RGB(red, green, blue);
        }
    }

    return static_cast<COLORREF>(parseInt(trimmed, static_cast<int>(fallback)));
}

// 统一把颜色写成可读的 #RRGGBB，避免 COLORREF 的 BGR 数值不直观。
std::wstring colorText(COLORREF color) {
    std::wostringstream out;
    out << L'#' << std::uppercase << std::hex << std::setfill(L'0')
        << std::setw(2) << static_cast<int>(GetRValue(color))
        << std::setw(2) << static_cast<int>(GetGValue(color))
        << std::setw(2) << static_cast<int>(GetBValue(color));
    return out.str();
}

// 歌词源新版配置是 1..4。
int clampSourcePriority(int value, int fallback) {
    if (value < 1 || value > 4) return fallback;
    return value;
}

// 旧版歌词源配置是 0..3，读取时转换成新版 1..4。
int clampLegacySourcePriority(int value, int fallback) {
    if (value < 0 || value > 3) return fallback;
    return value + 1;
}

// 轮询间隔限制在 UI 允许的范围内。
int clampSmtcPollIntervalMs(int value) {
    return std::clamp(value, 500, 2000);
}

}

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

AppConfig ConfigStore::load() const {
    // 读取时优先使用新版英文键名，同时继续兼容旧版中文键名。
    AppConfig config;
    config.font.name = readStringAny(L"Font", L"name", L"字体", L"字体名称", config.font.name);
    config.font.size = readIntAny(L"Font", L"size", L"字体", L"字体大小", config.font.size);
    config.font.bold = readBoolAny(L"Font", L"bold", L"字体", L"字体加粗", config.font.bold);
    config.font.italic = readBoolAny(L"Font", L"italic", L"字体", L"字体倾斜", config.font.italic);
    config.font.underline = readBoolAny(L"Font", L"underline", L"字体", L"字体下划线", config.font.underline);

    config.normal.color1 = readColorAny(L"Lyrics", L"normalColor1", L"字体", L"文字颜色1", config.normal.color1);
    config.normal.color2 = readColorAny(L"Lyrics", L"normalColor2", L"字体", L"文字颜色2", config.normal.color2);
    config.normal.border = readColorAny(L"Lyrics", L"normalBorderColor", L"字体", L"文字边框颜色", config.normal.border);
    config.normal.gradientMode = std::clamp(readIntAny(L"Lyrics", L"normalGradientMode", L"字体", L"渐变模式", config.normal.gradientMode), 0, 2);
    config.highlight.color1 = readColorAny(L"Lyrics", L"highlightColor1", L"字体", L"高亮文字颜色1", config.highlight.color1);
    config.highlight.color2 = readColorAny(L"Lyrics", L"highlightColor2", L"字体", L"高亮文字颜色2", config.highlight.color2);
    config.highlight.border = readColorAny(L"Lyrics", L"highlightBorderColor", L"字体", L"高亮文字边框颜色", config.highlight.border);
    config.highlight.gradientMode = std::clamp(readIntAny(L"Lyrics", L"highlightGradientMode", L"字体", L"高亮文字渐变模式", config.highlight.gradientMode), 0, 2);

    config.highlight2.color1 = readColorAny(L"Lyrics", L"highlight2Color1", L"字体", L"高亮文字颜色1_2", config.highlight2.color1);
    config.highlight2.color2 = readColorAny(L"Lyrics", L"highlight2Color2", L"字体", L"高亮文字颜色2_2", config.highlight2.color2);
    config.highlight2.border = readColorAny(L"Lyrics", L"highlight2BorderColor", L"字体", L"高亮文字边框颜色_2", config.highlight2.border);
    config.highlight2.gradientMode = std::clamp(readIntAny(L"Lyrics", L"highlight2GradientMode", L"字体", L"高亮文字渐变模式_2", config.highlight2.gradientMode), 0, 2);

    config.lyricOffsetMs = readIntAny(L"Lyrics", L"offsetMs", L"歌词", L"微调", 0);
    config.smtcMode = std::clamp(readIntAny(L"SMTC", L"mode", L"SMTC", L"SMTC", 1), 1, 2);
    config.smtcPollIntervalMs = clampSmtcPollIntervalMs(readInt(L"SMTC", L"pollIntervalMs", config.smtcPollIntervalMs));
    config.displayMode = std::clamp(readIntAny(L"Display", L"mode", L"显示方式", L"显示方式", 1), 1, 3);

    config.sourcePriority = {
        // 每个优先级独立读取，缺项时回到默认的 QQ/酷狗/酷我/网易云顺序。
        readSourcePriority(L"priority1", L"优先级1", 1),
        readSourcePriority(L"priority2", L"优先级2", 2),
        readSourcePriority(L"priority3", L"优先级3", 3),
        readSourcePriority(L"priority4", L"优先级4", 4),
    };

    const auto top = readStringAny(L"Window", L"top", L"歌词窗口", L"顶边", L"");
    if (!top.empty()) {
        // 只有读到 top 才认为旧配置里保存过窗口几何。
        config.window.top = readIntAny(L"Window", L"top", L"歌词窗口", L"顶边", 0);
        config.window.height = readIntAny(L"Window", L"height", L"歌词窗口", L"高度", 150);
        config.window.width = readIntAny(L"Window", L"width", L"歌词窗口", L"宽度", 0);
        config.window.left = readIntAny(L"Window", L"left", L"歌词窗口", L"左边", 0);
        config.window.hasPosition = config.window.width > 0 && config.window.height > 0;
    }

    return config;
}

void ConfigStore::save(const AppConfig& config) const {
    // 保存新版配置前清理旧版 section/key，避免用户看到两套配置相互混淆。
    deleteSection(L"字体");
    deleteSection(L"账号");
    deleteSection(L"歌词");
    deleteSection(L"歌词源");
    deleteSection(L"歌词窗口");
    deleteSection(L"显示方式");
    deleteSection(L"Account");
    deleteKey(L"SMTC", L"SMTC");

    // 新版配置统一写英文 section/key，但保留中文字体名等真实值。
    writeString(L"Font", L"name", config.font.name);
    writeString(L"Font", L"size", std::to_wstring(config.font.size));
    writeString(L"Font", L"bold", config.font.bold ? L"true" : L"false");
    writeString(L"Font", L"italic", config.font.italic ? L"true" : L"false");
    writeString(L"Font", L"underline", config.font.underline ? L"true" : L"false");
    writeString(L"Lyrics", L"offsetMs", std::to_wstring(config.lyricOffsetMs));
    writeString(L"Lyrics", L"normalColor1", colorText(config.normal.color1));
    writeString(L"Lyrics", L"normalColor2", colorText(config.normal.color2));
    writeString(L"Lyrics", L"normalBorderColor", colorText(config.normal.border));
    writeString(L"Lyrics", L"normalGradientMode", std::to_wstring(std::clamp(config.normal.gradientMode, 0, 2)));
    writeString(L"Lyrics", L"highlightColor1", colorText(config.highlight.color1));
    writeString(L"Lyrics", L"highlightColor2", colorText(config.highlight.color2));
    writeString(L"Lyrics", L"highlightBorderColor", colorText(config.highlight.border));
    writeString(L"Lyrics", L"highlightGradientMode", std::to_wstring(std::clamp(config.highlight.gradientMode, 0, 2)));
    writeString(L"Lyrics", L"highlight2Color1", colorText(config.highlight2.color1));
    writeString(L"Lyrics", L"highlight2Color2", colorText(config.highlight2.color2));
    writeString(L"Lyrics", L"highlight2BorderColor", colorText(config.highlight2.border));
    writeString(L"Lyrics", L"highlight2GradientMode", std::to_wstring(std::clamp(config.highlight2.gradientMode, 0, 2)));
    for (std::size_t i = 0; i < config.sourcePriority.size() && i < 4; ++i) {
        writeString(L"Sources", L"priority" + std::to_wstring(i + 1), std::to_wstring(std::clamp(config.sourcePriority[i], 1, 4)));
    }
    writeString(L"SMTC", L"mode", std::to_wstring(std::clamp(config.smtcMode, 1, 2)));
    writeString(L"SMTC", L"pollIntervalMs", std::to_wstring(clampSmtcPollIntervalMs(config.smtcPollIntervalMs)));
    writeString(L"Display", L"mode", std::to_wstring(std::clamp(config.displayMode, 1, 3)));
    if (config.window.hasPosition) {
        saveWindow(config.window);
    }
}

void ConfigStore::saveWindow(const WindowConfig& window) const {
    // 窗口位置变化频繁，单独保存可避免重写整个配置。
    deleteSection(L"歌词窗口");
    writeString(L"Window", L"top", std::to_wstring(window.top));
    writeString(L"Window", L"height", std::to_wstring(window.height));
    writeString(L"Window", L"width", std::to_wstring(window.width));
    writeString(L"Window", L"left", std::to_wstring(window.left));
}

void ConfigStore::saveDisplayMode(int mode) const {
    // 兼容旧版显示方式 section，同时只写入新版键。
    deleteSection(L"显示方式");
    writeString(L"Display", L"mode", std::to_wstring(std::clamp(mode, 1, 3)));
}

void ConfigStore::saveSmtcMode(int mode) const {
    // 旧版把 mode 存在 SMTC/SMTC，保存新版时删掉它。
    deleteKey(L"SMTC", L"SMTC");
    writeString(L"SMTC", L"mode", std::to_wstring(std::clamp(mode, 1, 2)));
}

std::wstring ConfigStore::readString(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const {
    // GetPrivateProfileStringW 会在 key 缺失时直接返回 fallback。
    std::wstring buffer(1024, L'\0');
    const DWORD chars = GetPrivateProfileStringW(toWide(section).c_str(), toWide(key).c_str(), toWide(fallback).c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), path_.c_str());
    buffer.resize(chars);
    return buffer;
}

std::wstring ConfigStore::readStringAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, std::wstring_view fallback) const {
    // 新版键存在时不再读取旧版键，保证迁移后新版配置优先。
    const auto current = readString(section, key, kMissingValue);
    if (current != kMissingValue) return current;
    return readString(legacySection, legacyKey, fallback);
}

COLORREF ConfigStore::readColorAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, COLORREF fallback) const {
    // 颜色的默认值也以 #RRGGBB 传入，和新版保存格式一致。
    return parseColor(readStringAny(section, key, legacySection, legacyKey, colorText(fallback)), fallback);
}

int ConfigStore::readSourcePriority(std::wstring_view key, std::wstring_view legacyKey, int fallback) const {
    // 歌词源优先级是迁移差异最大的字段，因此单独处理新旧索引范围。
    const auto current = readString(L"Sources", key, kMissingValue);
    if (current != kMissingValue) {
        return clampSourcePriority(parseInt(current, fallback), fallback);
    }
    return clampLegacySourcePriority(readInt(L"歌词源", legacyKey, fallback - 1), fallback);
}

int ConfigStore::readInt(std::wstring_view section, std::wstring_view key, int fallback) const {
    return parseInt(readString(section, key, std::to_wstring(fallback)), fallback);
}

int ConfigStore::readIntAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, int fallback) const {
    return parseInt(readStringAny(section, key, legacySection, legacyKey, std::to_wstring(fallback)), fallback);
}

bool ConfigStore::readBool(std::wstring_view section, std::wstring_view key, bool fallback) const {
    // 旧版布尔值使用中文“真/假”，新版使用 true/false。
    const auto text = util::trim(readString(section, key, fallback ? L"真" : L"假"));
    if (text == L"真" || text == L"true" || text == L"TRUE" || text == L"1") return true;
    if (text == L"假" || text == L"false" || text == L"FALSE" || text == L"0") return false;
    return fallback;
}

bool ConfigStore::readBoolAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, bool fallback) const {
    // 新版键存在但内容无法识别时，直接返回默认值，避免错误配置继续传播。
    const auto current = readString(section, key, kMissingValue);
    if (current != kMissingValue) {
        const auto text = util::trim(current);
        if (text == L"true" || text == L"TRUE" || text == L"1" || text == L"真") return true;
        if (text == L"false" || text == L"FALSE" || text == L"0" || text == L"假") return false;
        return fallback;
    }
    return readBool(legacySection, legacyKey, fallback);
}

void ConfigStore::writeString(std::wstring_view section, std::wstring_view key, std::wstring_view value) const {
    // WritePrivateProfileStringW 同时负责创建文件和写入 section/key。
    WritePrivateProfileStringW(toWide(section).c_str(), toWide(key).c_str(), toWide(value).c_str(), path_.c_str());
}

void ConfigStore::deleteKey(std::wstring_view section, std::wstring_view key) const {
    // value 传 nullptr 表示删除指定 key。
    WritePrivateProfileStringW(toWide(section).c_str(), toWide(key).c_str(), nullptr, path_.c_str());
}

void ConfigStore::deleteSection(std::wstring_view section) const {
    // key/value 都为 nullptr 表示删除整个 section。
    WritePrivateProfileStringW(toWide(section).c_str(), nullptr, nullptr, path_.c_str());
}

}
