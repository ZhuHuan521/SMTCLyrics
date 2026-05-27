#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::lyrics {

// 逐字/逐词歌词片段：记录某一段文本在本行内的高亮时间范围。
struct LyricSegment {
    std::int64_t offsetMs = 0;
    std::int64_t durationMs = 0;
    std::size_t textStart = 0;
    std::size_t textEnd = 0;
};

// 已解析的一行歌词；segments 为空时按整行时间估算高亮进度。
struct LrcLine {
    std::int64_t timeMs = 0;
    std::int64_t durationMs = 0;
    std::wstring text;
    std::vector<LyricSegment> segments;
};

// 当前播放位置对应的渲染帧：要显示的文本、百分比高亮和高亮行号。
struct LyricFrame {
    std::wstring text;
    int highlightPercent = 0;
    int highlightLine = 0;
};

// 寻找当前行附近可见歌词时返回的轻量视图。
struct VisibleLyricLine {
    int index = -1;
    std::wstring_view text;
};

// 支持普通 LRC、酷狗 KRC、QQ QRC、网易云 YRC 的歌词解析器。
class LrcParser {
public:
    // 解析 UTF-8 文本或原始字节；成功后 lines_ 按时间排序。
    bool parseUtf8(std::string_view lrcUtf8);
    bool parseBytes(const std::vector<std::uint8_t>& bytes);
    // 根据播放位置生成当前应显示的歌词文本和高亮进度。
    LyricFrame frameAt(std::int64_t positionMs, int displayMode) const;
    void frameAt(std::int64_t positionMs, int displayMode, LyricFrame& frame) const;
    bool empty() const { return lines_.empty(); }
    const std::vector<LrcLine>& lines() const { return lines_; }

private:
    // 解析不同歌词格式的内部工具。
    static std::optional<std::int64_t> parseTimestamp(std::wstring_view token);
    static bool parseKrcLine(std::wstring_view rawLine, LrcLine& out);
    static bool parseYrcContent(std::wstring_view text, std::vector<LrcLine>& out);
    static bool parseQrcContent(std::wstring_view text, std::vector<LrcLine>& out);
    // 时间轴查询和高亮计算。
    int findCurrentIndex(std::int64_t positionMs) const;
    int highlightPercentForLine(int index, std::int64_t positionMs) const;
    std::optional<VisibleLyricLine> visibleLineNear(int index, int direction, int maxDistance, std::wstring_view excludedText = {}) const;

    std::vector<LrcLine> lines_;
};

}
