#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smtc::lyrics {

struct LyricSegment {
    std::int64_t offsetMs = 0;
    std::int64_t durationMs = 0;
    std::size_t textStart = 0;
    std::size_t textEnd = 0;
};

struct LrcLine {
    std::int64_t timeMs = 0;
    std::int64_t durationMs = 0;
    std::wstring text;
    std::vector<LyricSegment> segments;
};

struct LyricFrame {
    std::wstring text;
    int highlightPercent = 0;
    int highlightLine = 0;
};

struct VisibleLyricLine {
    int index = -1;
    std::wstring text;
};

class LrcParser {
public:
    bool parseUtf8(std::string_view lrcUtf8);
    bool parseBytes(const std::vector<std::uint8_t>& bytes);
    LyricFrame frameAt(std::int64_t positionMs, int displayMode) const;
    bool empty() const { return lines_.empty(); }
    const std::vector<LrcLine>& lines() const { return lines_; }

private:
    static std::optional<std::int64_t> parseTimestamp(std::wstring_view token);
    static bool parseKrcLine(std::wstring_view rawLine, LrcLine& out);
    int findCurrentIndex(std::int64_t positionMs) const;
    int highlightPercentForLine(int index, std::int64_t positionMs) const;
    std::optional<VisibleLyricLine> visibleLineNear(int index, int direction, int maxDistance, std::wstring_view excludedText = {}) const;

    std::vector<LrcLine> lines_;
};

}
