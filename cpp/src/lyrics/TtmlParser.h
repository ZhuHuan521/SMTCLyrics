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
    // 从 TTML 中提取 replacement 文本；音译、translation 和 x-bg 不参与主歌词。
    const std::vector<LrcLine>& lines() const { return lines_; }
    bool empty() const { return lines_.empty(); }

private:
    std::vector<LrcLine> lines_;
};

}
