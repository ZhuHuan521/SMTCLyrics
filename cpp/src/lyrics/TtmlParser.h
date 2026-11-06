#pragma once

#include "lyrics/LrcParser.h"

#include <string_view>
#include <vector>

namespace smtc::lyrics {

// Apple Music official TTML (syllable-lyrics). Kept separate from LRC so
// namespaced timing and nested spans are parsed without guessing the format.
class TtmlParser {
public:
    bool parseUtf8(std::string_view xml);
    // 单测可检查 TTML 是否包含逐字 span 时间轴。
    static bool hasSyllableTiming(std::string_view xml);
    // 字级时间轴至少覆盖两行，避免英文行级 TTML 的单词包装被误判。
    static bool hasUsableSyllableTiming(std::string_view xml);
    // Replacement/subtitle are displayed text; x-bg and transliteration are not.
    const std::vector<LrcLine>& lines() const { return lines_; }
    bool empty() const { return lines_.empty(); }
    int wordTimedLineCount() const { return wordTimedLineCount_; }

private:
    std::vector<LrcLine> lines_;
    bool hasWordTiming_ = false;
    int wordTimedLineCount_ = 0;
};

}
