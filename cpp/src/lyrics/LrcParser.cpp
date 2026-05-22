#include "lyrics/LrcParser.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace smtc::lyrics {
namespace {

std::vector<std::wstring> splitLines(std::wstring text) {
    text = util::replaceAll(std::move(text), L"\r\n", L"\n");
    text = util::replaceAll(std::move(text), L"\r", L"\n");
    std::vector<std::wstring> lines;
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line, L'\n')) {
        lines.push_back(line);
    }
    return lines;
}

bool hasVisibleText(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return !std::iswspace(ch); });
}

}

bool LrcParser::parseUtf8(std::string_view lrcUtf8) {
    lines_.clear();
    const auto text = util::utf8ToWide(lrcUtf8);
    for (const auto& rawLine : splitLines(text)) {
        std::vector<std::int64_t> times;
        std::size_t pos = 0;
        while (pos < rawLine.size() && rawLine[pos] == L'[') {
            const auto end = rawLine.find(L']', pos + 1);
            if (end == std::wstring::npos) break;
            const auto token = rawLine.substr(pos + 1, end - pos - 1);
            auto timestamp = parseTimestamp(token);
            if (!timestamp) break;
            times.push_back(*timestamp);
            pos = end + 1;
        }
        if (times.empty()) continue;
        auto lyric = rawLine.substr(pos);
        for (const auto time : times) {
            lines_.push_back({time, lyric});
        }
    }
    std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
    return !lines_.empty();
}

bool LrcParser::parseBytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        lines_.clear();
        return false;
    }
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return parseUtf8(text);
}

LyricFrame LrcParser::frameAt(std::int64_t positionMs, int displayMode, int offsetSeconds) const {
    LyricFrame frame;
    if (lines_.empty()) return frame;

    const auto adjusted = positionMs + static_cast<std::int64_t>(offsetSeconds) * 1000;
    const int index = findCurrentIndex(adjusted);
    if (index < 0) return frame;

    const auto current = lineTextNear(index, +1, 2);
    if (!current) return frame;

    if (displayMode == 2) {
        if (auto next = lineTextNear(index + 1, +1, 3)) {
            frame.text = *current + L"\n" + *next;
        } else {
            frame.text = *current;
        }
    } else if (displayMode == 3) {
        if (auto previous = lineTextNear(index - 1, -1, 3)) {
            frame.text = *previous + L"\n" + *current;
        } else {
            frame.text = *current;
        }
    } else {
        frame.text = *current;
    }
    return frame;
}

std::optional<std::int64_t> LrcParser::parseTimestamp(std::wstring_view token) {
    const auto colon = token.find(L':');
    if (colon == std::wstring::npos) return std::nullopt;
    const auto minuteText = std::wstring(token.substr(0, colon));
    const auto secondText = std::wstring(token.substr(colon + 1));
    if (minuteText.empty() || secondText.empty()) return std::nullopt;

    wchar_t* end = nullptr;
    const long minutes = std::wcstol(minuteText.c_str(), &end, 10);
    if (end == minuteText.c_str()) return std::nullopt;

    end = nullptr;
    const double seconds = std::wcstod(secondText.c_str(), &end);
    if (end == secondText.c_str()) return std::nullopt;

    return static_cast<std::int64_t>(minutes) * 60'000 + static_cast<std::int64_t>(seconds * 1000.0 + 0.5);
}

int LrcParser::findCurrentIndex(std::int64_t positionMs) const {
    auto it = std::upper_bound(lines_.begin(), lines_.end(), positionMs, [](std::int64_t value, const LrcLine& line) { return value < line.timeMs; });
    if (it == lines_.begin()) return -1;
    return static_cast<int>(std::distance(lines_.begin(), std::prev(it)));
}

std::optional<std::wstring> LrcParser::lineTextNear(int index, int direction, int maxDistance) const {
    for (int i = 0; i <= maxDistance; ++i) {
        const int candidate = index + i * direction;
        if (candidate < 0 || candidate >= static_cast<int>(lines_.size())) continue;
        auto text = util::trim(lines_[candidate].text);
        if (hasVisibleText(text)) {
            return text;
        }
    }
    return std::nullopt;
}

}
