#include "lyrics/LrcParser.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>

namespace smtc::lyrics {
namespace {

bool hasVisibleText(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return !std::iswspace(ch); });
}

std::wstring_view trimView(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::int64_t> parseInteger(std::wstring_view text) {
    text = trimView(text);
    if (text.empty()) return std::nullopt;

    bool negative = false;
    if (text.front() == L'+' || text.front() == L'-') {
        negative = text.front() == L'-';
        text.remove_prefix(1);
    }
    if (text.empty() || !std::iswdigit(text.front())) return std::nullopt;

    std::int64_t value = 0;
    for (wchar_t ch : text) {
        if (!std::iswdigit(ch)) return std::nullopt;
        const auto digit = static_cast<std::int64_t>(ch - L'0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

bool parseIntegerPair(std::wstring_view text, std::int64_t& first, std::int64_t& second, bool allowTrailingFields = false) {
    const auto comma = text.find(L',');
    if (comma == std::wstring_view::npos) return false;
    auto secondPart = text.substr(comma + 1);
    if (allowTrailingFields) {
        const auto nextComma = secondPart.find(L',');
        if (nextComma != std::wstring_view::npos) {
            secondPart = secondPart.substr(0, nextComma);
        }
    }
    const auto parsedFirst = parseInteger(text.substr(0, comma));
    const auto parsedSecond = parseInteger(secondPart);
    if (!parsedFirst || !parsedSecond) return false;
    first = *parsedFirst;
    second = *parsedSecond;
    return true;
}

std::wstring_view trimLineBreaks(std::wstring_view text) {
    while (!text.empty() && (text.front() == L'\r' || text.front() == L'\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.remove_suffix(1);
    }
    return text;
}

std::int64_t qrcWordOffset(std::int64_t lineStartMs, std::int64_t lineDurationMs, std::int64_t wordStartMs) {
    const auto lineEndMs = lineStartMs + std::max<std::int64_t>(0, lineDurationMs);
    if (wordStartMs >= lineStartMs - 100 && wordStartMs <= lineEndMs + 1000) {
        return std::max<std::int64_t>(0, wordStartMs - lineStartMs);
    }
    return std::max<std::int64_t>(0, wordStartMs);
}

struct QrcLineTag {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
};

std::vector<QrcLineTag> findQrcLineTags(std::wstring_view text) {
    std::vector<QrcLineTag> tags;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto open = text.find(L'[', pos);
        if (open == std::wstring_view::npos) break;
        const auto close = text.find(L']', open + 1);
        if (close == std::wstring_view::npos) break;

        const auto token = text.substr(open + 1, close - open - 1);
        std::int64_t startMs = 0;
        std::int64_t durationMs = 0;
        if (parseIntegerPair(token, startMs, durationMs)) {
            tags.push_back({open, close + 1, startMs, durationMs});
        }

        pos = close + 1;
    }
    return tags;
}

bool parseQrcLineBody(std::int64_t lineStartMs, std::int64_t lineDurationMs, std::wstring_view rawBody, LrcLine& out) {
    const auto body = trimLineBreaks(rawBody);
    out = {};
    out.timeMs = lineStartMs;
    out.durationMs = std::max<std::int64_t>(0, lineDurationMs);
    out.text.reserve(body.size());

    bool foundWordTiming = false;
    std::size_t cursor = 0;
    std::size_t search = 0;

    while (search < body.size()) {
        const auto open = body.find(L'(', search);
        if (open == std::wstring_view::npos) break;
        const auto close = body.find(L')', open + 1);
        if (close == std::wstring_view::npos) break;

        const auto timing = body.substr(open + 1, close - open - 1);
        std::int64_t wordStartMs = 0;
        std::int64_t wordDurationMs = 0;
        if (!parseIntegerPair(timing, wordStartMs, wordDurationMs) || wordDurationMs < 0) {
            search = open + 1;
            continue;
        }

        const auto wordText = body.substr(cursor, open - cursor);
        const std::size_t textStart = out.text.size();
        out.text.append(wordText.begin(), wordText.end());
        const std::size_t textEnd = out.text.size();
        if (textEnd > textStart) {
            out.segments.push_back({
                qrcWordOffset(lineStartMs, lineDurationMs, wordStartMs),
                std::max<std::int64_t>(0, wordDurationMs),
                textStart,
                textEnd
            });
        }

        foundWordTiming = true;
        cursor = close + 1;
        search = cursor;
    }

    if (cursor < body.size()) {
        const auto tail = body.substr(cursor);
        out.text.append(tail.begin(), tail.end());
    }

    return foundWordTiming && hasVisibleText(out.text);
}

}

bool LrcParser::parseUtf8(std::string_view lrcUtf8) {
    lines_.clear();
    const auto text = util::utf8ToWide(lrcUtf8);
    const std::wstring_view textView(text);
    lines_.reserve(static_cast<std::size_t>(std::count(text.begin(), text.end(), L'\n') + 1));
    if (parseQrcContent(textView, lines_)) {
        std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
        return true;
    }

    std::size_t lineStart = 0;
    while (lineStart <= textView.size()) {
        const auto lineEnd = textView.find(L'\n', lineStart);
        auto rawLine = textView.substr(lineStart, lineEnd == std::wstring_view::npos ? std::wstring_view::npos : lineEnd - lineStart);
        if (!rawLine.empty() && rawLine.back() == L'\r') {
            rawLine.remove_suffix(1);
        }

        LrcLine krcLine;
        if (parseKrcLine(rawLine, krcLine)) {
            lines_.push_back(std::move(krcLine));
        } else {
            std::vector<std::int64_t> times;
            times.reserve(2);
            std::size_t pos = 0;
            while (pos < rawLine.size() && rawLine[pos] == L'[') {
                const auto end = rawLine.find(L']', pos + 1);
                if (end == std::wstring_view::npos) break;
                const auto token = rawLine.substr(pos + 1, end - pos - 1);
                auto timestamp = parseTimestamp(token);
                if (!timestamp) break;
                times.push_back(*timestamp);
                pos = end + 1;
            }
            if (!times.empty()) {
                const std::wstring lyric(rawLine.substr(pos));
                for (const auto time : times) {
                    LrcLine line;
                    line.timeMs = time;
                    line.text = lyric;
                    lines_.push_back(std::move(line));
                }
            }
        }

        if (lineEnd == std::wstring_view::npos) break;
        lineStart = lineEnd + 1;
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

LyricFrame LrcParser::frameAt(std::int64_t positionMs, int displayMode) const {
    LyricFrame frame;
    if (lines_.empty()) return frame;

    const auto adjusted = positionMs;
    const int index = findCurrentIndex(adjusted);
    if (index < 0) return frame;

    const auto current = visibleLineNear(index, +1, 2);
    if (!current) return frame;

    if (displayMode == 2) {
        if (auto next = visibleLineNear(current->index + 1, +1, 3, current->text)) {
            frame.text.reserve(current->text.size() + 1 + next->text.size());
            frame.text.append(current->text).push_back(L'\n');
            frame.text.append(next->text);
            frame.highlightLine = 0;
        } else {
            frame.text = current->text;
        }
    } else if (displayMode == 3) {
        if (auto previous = visibleLineNear(current->index - 1, -1, 3, current->text)) {
            frame.text.reserve(previous->text.size() + 1 + current->text.size());
            frame.text.append(previous->text).push_back(L'\n');
            frame.text.append(current->text);
            frame.highlightLine = 1;
        } else {
            frame.text = current->text;
        }
    } else {
        frame.text = current->text;
    }
    frame.highlightPercent = highlightPercentForLine(current->index, adjusted);
    return frame;
}

std::optional<std::int64_t> LrcParser::parseTimestamp(std::wstring_view token) {
    const auto colon = token.find(L':');
    if (colon == std::wstring::npos) return std::nullopt;
    auto minuteText = trimView(token.substr(0, colon));
    auto secondText = trimView(token.substr(colon + 1));
    if (minuteText.empty() || secondText.empty()) return std::nullopt;

    std::int64_t minutes = 0;
    for (wchar_t ch : minuteText) {
        if (!std::iswdigit(ch)) return std::nullopt;
        minutes = minutes * 10 + static_cast<std::int64_t>(ch - L'0');
    }

    std::int64_t seconds = 0;
    std::int64_t fractionMs = 0;
    int fractionDigits = 0;
    int roundDigit = 0;
    bool sawSecondDigit = false;
    bool inFraction = false;

    for (wchar_t ch : secondText) {
        if (ch == L'.' && !inFraction) {
            inFraction = true;
            continue;
        }
        if (!std::iswdigit(ch)) return std::nullopt;
        sawSecondDigit = true;
        const int digit = static_cast<int>(ch - L'0');
        if (!inFraction) {
            seconds = seconds * 10 + digit;
        } else if (fractionDigits < 3) {
            fractionMs = fractionMs * 10 + digit;
            ++fractionDigits;
        } else if (fractionDigits == 3) {
            roundDigit = digit;
            ++fractionDigits;
        }
    }
    if (!sawSecondDigit) return std::nullopt;
    while (fractionDigits < 3) {
        fractionMs *= 10;
        ++fractionDigits;
    }
    if (roundDigit >= 5) {
        ++fractionMs;
        if (fractionMs >= 1000) {
            ++seconds;
            fractionMs = 0;
        }
    }

    return minutes * 60'000 + seconds * 1000 + fractionMs;
}

bool LrcParser::parseKrcLine(std::wstring_view rawLine, LrcLine& out) {
    if (rawLine.empty() || rawLine.front() != L'[') return false;
    const auto headerEnd = rawLine.find(L']');
    if (headerEnd == std::wstring_view::npos) return false;

    const auto header = rawLine.substr(1, headerEnd - 1);
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
    if (!parseIntegerPair(header, startMs, durationMs, true)) return false;

    out = {};
    out.timeMs = startMs;
    out.durationMs = std::max<std::int64_t>(0, durationMs);

    const auto content = rawLine.substr(headerEnd + 1);
    out.text.reserve(content.size());
    std::size_t pos = 0;
    while (pos < content.size()) {
        if (content[pos] != L'<') {
            const auto next = content.find(L'<', pos);
            const auto textPart = content.substr(pos, next == std::wstring_view::npos ? std::wstring_view::npos : next - pos);
            out.text.append(textPart.begin(), textPart.end());
            if (next == std::wstring_view::npos) break;
            pos = next;
            continue;
        }

        const auto tagEnd = content.find(L'>', pos + 1);
        if (tagEnd == std::wstring_view::npos) break;

        const auto tag = content.substr(pos + 1, tagEnd - pos - 1);
        pos = tagEnd + 1;

        const auto nextTag = content.find(L'<', pos);
        const auto textPart = content.substr(pos, nextTag == std::wstring_view::npos ? std::wstring_view::npos : nextTag - pos);
        const std::size_t textStart = out.text.size();
        out.text.append(textPart.begin(), textPart.end());
        const std::size_t textEnd = out.text.size();

        std::int64_t offset = 0;
        std::int64_t duration = 0;
        if (textEnd > textStart && parseIntegerPair(tag, offset, duration, true)) {
            out.segments.push_back({std::max<std::int64_t>(0, offset), std::max<std::int64_t>(0, duration), textStart, textEnd});
        }

        if (nextTag == std::wstring_view::npos) break;
        pos = nextTag;
    }

    return hasVisibleText(out.text);
}

bool LrcParser::parseQrcContent(std::wstring_view text, std::vector<LrcLine>& out) {
    const auto tags = findQrcLineTags(text);
    if (tags.empty()) return false;

    std::vector<LrcLine> parsed;
    bool hasTimedWords = false;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        const auto& tag = tags[i];
        const auto bodyBegin = tag.end;
        const auto bodyEnd = i + 1 < tags.size() ? tags[i + 1].begin : text.size();
        if (bodyBegin >= bodyEnd) continue;

        LrcLine line;
        if (parseQrcLineBody(tag.startMs, tag.durationMs, text.substr(bodyBegin, bodyEnd - bodyBegin), line)) {
            hasTimedWords = hasTimedWords || !line.segments.empty();
            parsed.push_back(std::move(line));
        }
    }

    if (!hasTimedWords) return false;
    out = std::move(parsed);
    return !out.empty();
}

int LrcParser::findCurrentIndex(std::int64_t positionMs) const {
    auto it = std::upper_bound(lines_.begin(), lines_.end(), positionMs, [](std::int64_t value, const LrcLine& line) { return value < line.timeMs; });
    if (it == lines_.begin()) return -1;
    return static_cast<int>(std::distance(lines_.begin(), std::prev(it)));
}

int LrcParser::highlightPercentForLine(int index, std::int64_t positionMs) const {
    if (index < 0 || index >= static_cast<int>(lines_.size())) return 0;
    const auto& line = lines_[index];
    const auto textLength = line.text.size();
    if (textLength == 0) return 0;

    if (!line.segments.empty()) {
        const auto localMs = positionMs - line.timeMs;
        if (localMs <= line.segments.front().offsetMs) return 0;

        double coveredUnits = 0.0;
        for (const auto& segment : line.segments) {
            const auto segmentStart = segment.offsetMs;
            const auto segmentEnd = segment.offsetMs + std::max<std::int64_t>(1, segment.durationMs);
            const auto segmentUnits = static_cast<double>(segment.textEnd - segment.textStart);

            if (localMs < segmentStart) {
                break;
            }
            if (localMs >= segmentEnd) {
                coveredUnits = std::max(coveredUnits, static_cast<double>(segment.textEnd));
                continue;
            }

            const double progress = static_cast<double>(localMs - segmentStart) / static_cast<double>(segmentEnd - segmentStart);
            coveredUnits = static_cast<double>(segment.textStart) + segmentUnits * std::clamp(progress, 0.0, 1.0);
            break;
        }

        if (localMs >= line.segments.back().offsetMs + line.segments.back().durationMs) {
            coveredUnits = static_cast<double>(textLength);
        }
        return std::clamp(static_cast<int>(coveredUnits * 100.0 / static_cast<double>(textLength) + 0.5), 0, 100);
    }

    if (line.durationMs > 0) {
        const auto localMs = std::clamp<std::int64_t>(positionMs - line.timeMs, 0, line.durationMs);
        return std::clamp(static_cast<int>(localMs * 100 / line.durationMs), 0, 100);
    }

    return 0;
}

std::optional<VisibleLyricLine> LrcParser::visibleLineNear(int index, int direction, int maxDistance, std::wstring_view excludedText) const {
    for (int i = 0; i <= maxDistance; ++i) {
        const int candidate = index + i * direction;
        if (candidate < 0 || candidate >= static_cast<int>(lines_.size())) continue;
        const auto text = trimView(lines_[candidate].text);
        if (!text.empty() && text != excludedText) {
            return VisibleLyricLine{candidate, std::wstring(text)};
        }
    }
    return std::nullopt;
}

}
