#include "lyrics/LrcParser.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>

namespace smtc::lyrics {
namespace {

// 判断一行是否含有非空白字符，避免空行参与显示。
bool hasVisibleText(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return !std::iswspace(ch); });
}

// 对 string_view 做首尾空白裁剪，不分配新字符串。
std::wstring_view trimView(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// 解析有符号整数，并做溢出保护。
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

// 解析 start,duration 形式；KRC 部分标签还允许尾随第三字段。
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

// YRC 单词标签常见格式是 (start,duration,0)，这里只取前两个时间字段。
bool parseIntegerTripleFirstTwo(std::wstring_view text, std::int64_t& first, std::int64_t& second) {
    const auto firstComma = text.find(L',');
    if (firstComma == std::wstring_view::npos) return false;
    const auto secondComma = text.find(L',', firstComma + 1);
    if (secondComma == std::wstring_view::npos) return false;
    const auto parsedFirst = parseInteger(text.substr(0, firstComma));
    const auto parsedSecond = parseInteger(text.substr(firstComma + 1, secondComma - firstComma - 1));
    const auto parsedThird = parseInteger(text.substr(secondComma + 1));
    if (!parsedFirst || !parsedSecond || !parsedThird) return false;
    first = *parsedFirst;
    second = *parsedSecond;
    return true;
}

// 去除行首行尾换行符，保留歌词内部空白。
std::wstring_view trimLineBreaks(std::wstring_view text) {
    while (!text.empty() && (text.front() == L'\r' || text.front() == L'\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// QRC/YRC 的词时间有时是绝对时间，有时是相对行首；这里做兼容归一化。
std::int64_t qrcWordOffset(std::int64_t lineStartMs, std::int64_t lineDurationMs, std::int64_t wordStartMs) {
    const auto lineEndMs = lineStartMs + std::max<std::int64_t>(0, lineDurationMs);
    if (wordStartMs >= lineStartMs - 100 && wordStartMs <= lineEndMs + 1000) {
        return std::max<std::int64_t>(0, wordStartMs - lineStartMs);
    }
    return std::max<std::int64_t>(0, wordStartMs);
}

// 在 YRC 行体中寻找下一个 (start,duration,...) 单词时间标签。
bool findYrcWordTag(std::wstring_view text, std::size_t start, std::size_t& tagBegin, std::size_t& tagEnd, std::int64_t& wordStartMs, std::int64_t& wordDurationMs) {
    std::size_t search = start;
    while (search < text.size()) {
        const auto open = text.find(L'(', search);
        if (open == std::wstring_view::npos) return false;
        const auto close = text.find(L')', open + 1);
        if (close == std::wstring_view::npos) return false;
        if (parseIntegerTripleFirstTwo(text.substr(open + 1, close - open - 1), wordStartMs, wordDurationMs) && wordDurationMs >= 0) {
            tagBegin = open;
            tagEnd = close + 1;
            return true;
        }
        search = open + 1;
    }
    return false;
}

// YRC 某些歌词片段末尾还会附带 [start,duration] 内联标签，显示前要剥掉。
std::wstring_view removeTrailingYrcInlineTags(std::wstring_view text) {
    for (;;) {
        text = trimLineBreaks(text);
        if (text.empty() || text.back() != L']') return text;
        const auto open = text.rfind(L'[');
        if (open == std::wstring_view::npos) return text;
        std::int64_t startMs = 0;
        std::int64_t durationMs = 0;
        if (!parseIntegerPair(text.substr(open + 1, text.size() - open - 2), startMs, durationMs)) {
            return text;
        }
        text = text.substr(0, open);
    }
}

// 解析网易云 YRC 的单行：[行开始,行时长](词开始,词时长,0)词...
bool parseYrcLine(std::wstring_view rawLine, LrcLine& out) {
    rawLine = trimLineBreaks(rawLine);
    if (rawLine.empty() || rawLine.front() != L'[') return false;
    const auto headerEnd = rawLine.find(L']');
    if (headerEnd == std::wstring_view::npos) return false;

    std::int64_t lineStartMs = 0;
    std::int64_t lineDurationMs = 0;
    if (!parseIntegerPair(rawLine.substr(1, headerEnd - 1), lineStartMs, lineDurationMs)) return false;

    const auto body = rawLine.substr(headerEnd + 1);
    out = {};
    out.timeMs = lineStartMs;
    out.durationMs = std::max<std::int64_t>(0, lineDurationMs);
    out.text.reserve(body.size());

    std::size_t tagBegin = 0;
    std::size_t tagEnd = 0;
    std::int64_t wordStartMs = 0;
    std::int64_t wordDurationMs = 0;
    if (!findYrcWordTag(body, 0, tagBegin, tagEnd, wordStartMs, wordDurationMs)) {
        out.text.assign(body.begin(), body.end());
        return hasVisibleText(out.text);
    }

    bool foundWordTiming = false;
    for (;;) {
        std::size_t nextBegin = body.size();
        std::size_t nextEnd = body.size();
        std::int64_t nextStartMs = 0;
        std::int64_t nextDurationMs = 0;
        const bool hasNext = findYrcWordTag(body, tagEnd, nextBegin, nextEnd, nextStartMs, nextDurationMs);

        const auto wordText = removeTrailingYrcInlineTags(body.substr(tagEnd, nextBegin - tagEnd));
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

        if (!hasNext) break;
        tagBegin = nextBegin;
        tagEnd = nextEnd;
        wordStartMs = nextStartMs;
        wordDurationMs = nextDurationMs;
    }

    return foundWordTiming && hasVisibleText(out.text);
}

// QRC 行标签记录每一行歌词在原文中的范围和时间。
struct QrcLineTag {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
};

// QQ QRC 的行级时间标签形如 [start,duration]。
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

// 解析 QRC 行体：普通文本与 (start,duration) 词级时间标签交错出现。
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
    // 解析入口会依次尝试 YRC、QRC，最后回退到普通 LRC/KRC 行解析。
    lines_.clear();
    const auto text = util::utf8ToWide(lrcUtf8);
    const std::wstring_view textView(text);
    lines_.reserve(static_cast<std::size_t>(std::count(text.begin(), text.end(), L'\n') + 1));
    if (parseYrcContent(textView, lines_)) {
        // 所有格式最终都按时间排序，frameAt 才能二分查找。
        std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
        return true;
    }
    if (parseQrcContent(textView, lines_)) {
        std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
        return true;
    }

    std::size_t lineStart = 0;
    while (lineStart <= textView.size()) {
        // 普通 LRC 支持一行多个 [mm:ss.xx] 标签，共用同一段歌词文本。
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
    // 在线源和本地歌词最终都以字节形式进入这里。
    if (bytes.empty()) {
        lines_.clear();
        return false;
    }
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        // 去掉 UTF-8 BOM，避免第一行标签解析失败。
        text.erase(0, 3);
    }
    return parseUtf8(text);
}

LyricFrame LrcParser::frameAt(std::int64_t positionMs, int displayMode) const {
    // displayMode: 1=当前行，2=当前+下一行，3=上一行+当前。
    LyricFrame frame;
    if (lines_.empty()) return frame;

    const auto adjusted = positionMs;
    const int index = findCurrentIndex(adjusted);
    if (index < 0) return frame;

    const auto current = visibleLineNear(index, +1, 2);
    if (!current) return frame;

    if (displayMode == 2) {
        // 两句向后：当前行高亮，下一句用第二行样式显示。
        if (auto next = visibleLineNear(current->index + 1, +1, 3, current->text)) {
            frame.text.reserve(current->text.size() + 1 + next->text.size());
            frame.text.append(current->text.begin(), current->text.end()).push_back(L'\n');
            frame.text.append(next->text.begin(), next->text.end());
            frame.highlightLine = 0;
        } else {
            frame.text.assign(current->text.begin(), current->text.end());
        }
    } else if (displayMode == 3) {
        // 两句向前：上一句在第一行，当前行在第二行高亮。
        if (auto previous = visibleLineNear(current->index - 1, -1, 3, current->text)) {
            frame.text.reserve(previous->text.size() + 1 + current->text.size());
            frame.text.append(previous->text.begin(), previous->text.end()).push_back(L'\n');
            frame.text.append(current->text.begin(), current->text.end());
            frame.highlightLine = 1;
        } else {
            frame.text.assign(current->text.begin(), current->text.end());
        }
    } else {
        frame.text.assign(current->text.begin(), current->text.end());
    }
    frame.highlightPercent = highlightPercentForLine(current->index, adjusted);
    return frame;
}

std::optional<std::int64_t> LrcParser::parseTimestamp(std::wstring_view token) {
    // 普通 LRC 时间标签形如 mm:ss、mm:ss.xx 或 mm:ss.xxx。
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
        // 超过三位小数时做四舍五入，保持毫秒精度。
        ++fractionMs;
        if (fractionMs >= 1000) {
            ++seconds;
            fractionMs = 0;
        }
    }

    return minutes * 60'000 + seconds * 1000 + fractionMs;
}

bool LrcParser::parseKrcLine(std::wstring_view rawLine, LrcLine& out) {
    // 酷狗 KRC 单行格式：[start,duration]<offset,duration,...>词...
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

bool LrcParser::parseYrcContent(std::wstring_view text, std::vector<LrcLine>& out) {
    // 只有确认至少解析到词级时间时，才把整段文本当作 YRC。
    std::vector<LrcLine> parsed;
    bool hasTimedWords = false;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const auto lineEnd = text.find(L'\n', lineStart);
        auto rawLine = text.substr(lineStart, lineEnd == std::wstring_view::npos ? std::wstring_view::npos : lineEnd - lineStart);
        if (!rawLine.empty() && rawLine.back() == L'\r') {
            rawLine.remove_suffix(1);
        }

        LrcLine line;
        if (parseYrcLine(rawLine, line)) {
            hasTimedWords = hasTimedWords || !line.segments.empty();
            parsed.push_back(std::move(line));
        }

        if (lineEnd == std::wstring_view::npos) break;
        lineStart = lineEnd + 1;
    }

    if (!hasTimedWords) return false;
    out = std::move(parsed);
    return !out.empty();
}

bool LrcParser::parseQrcContent(std::wstring_view text, std::vector<LrcLine>& out) {
    // QRC 先找所有行级标签，再用相邻标签之间的文本作为当前行体。
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
    // upper_bound 找到第一个晚于当前位置的行，前一行就是当前行。
    auto it = std::upper_bound(lines_.begin(), lines_.end(), positionMs, [](std::int64_t value, const LrcLine& line) { return value < line.timeMs; });
    if (it == lines_.begin()) return -1;
    return static_cast<int>(std::distance(lines_.begin(), std::prev(it)));
}

int LrcParser::highlightPercentForLine(int index, std::int64_t positionMs) const {
    // 有词级片段时按片段内进度高亮，否则用整行 duration 估算。
    if (index < 0 || index >= static_cast<int>(lines_.size())) return 0;
    const auto& line = lines_[index];
    const auto textLength = line.text.size();
    if (textLength == 0) return 0;

    if (!line.segments.empty()) {
        const auto localMs = positionMs - line.timeMs;
        if (localMs <= line.segments.front().offsetMs) return 0;

        const auto& lastSegment = line.segments.back();
        if (localMs >= lastSegment.offsetMs + std::max<std::int64_t>(1, lastSegment.durationMs)) {
            return 100;
        }

        const auto next = std::upper_bound(
            // 找到当前位置所在的词级片段。
            line.segments.begin(),
            line.segments.end(),
            localMs,
            [](std::int64_t value, const LyricSegment& segment) { return value < segment.offsetMs; });
        const auto& segment = *std::prev(next);
        const auto segmentStart = segment.offsetMs;
        const auto segmentEnd = segment.offsetMs + std::max<std::int64_t>(1, segment.durationMs);
        double coveredUnits = static_cast<double>(segment.textEnd);
        if (localMs < segmentEnd) {
            const auto segmentUnits = static_cast<double>(segment.textEnd - segment.textStart);
            const double progress = static_cast<double>(localMs - segmentStart) / static_cast<double>(segmentEnd - segmentStart);
            coveredUnits = static_cast<double>(segment.textStart) + segmentUnits * std::clamp(progress, 0.0, 1.0);
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
    // 向前或向后寻找最近的非空行，跳过与当前行重复的文本。
    for (int i = 0; i <= maxDistance; ++i) {
        const int candidate = index + i * direction;
        if (candidate < 0 || candidate >= static_cast<int>(lines_.size())) continue;
        const auto text = trimView(lines_[candidate].text);
        if (!text.empty() && text != excludedText) {
            return VisibleLyricLine{candidate, text};
        }
    }
    return std::nullopt;
}

}
