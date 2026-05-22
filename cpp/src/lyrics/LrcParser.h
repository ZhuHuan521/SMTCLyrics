#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smtc::lyrics {

struct LrcLine {
    std::int64_t timeMs = 0;
    std::wstring text;
};

struct LyricFrame {
    std::wstring text;
    int highlightPercent = 0;
};

class LrcParser {
public:
    bool parseUtf8(std::string_view lrcUtf8);
    bool parseBytes(const std::vector<std::uint8_t>& bytes);
    LyricFrame frameAt(std::int64_t positionMs, int displayMode, int offsetSeconds) const;
    bool empty() const { return lines_.empty(); }
    const std::vector<LrcLine>& lines() const { return lines_; }

private:
    static std::optional<std::int64_t> parseTimestamp(std::wstring_view token);
    int findCurrentIndex(std::int64_t positionMs) const;
    std::optional<std::wstring> lineTextNear(int index, int direction, int maxDistance) const;

    std::vector<LrcLine> lines_;
};

}
