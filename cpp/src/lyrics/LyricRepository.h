#pragma once

#include "cache/Cache.h"
#include "config/Config.h"
#include <cstdint>

#include "lyrics/AppleMusicLyrics.h"
#include "lyrics/OnlineLyrics.h"

#include <filesystem>
#include <string>
#include <vector>

namespace smtc::lyrics {

// 一次歌词加载的结果，可能来自本地文件、缓存命中的在线源或按优先级搜索的在线源。
struct LyricLoadResult {
    std::vector<std::uint8_t> lrcBytes;
    // Apple Music 译文为单独的一套 TTML；其他歌词源通常为空。
    std::vector<std::uint8_t> translatedLrcBytes;
    LyricSource source = LyricSource::Local;
    std::filesystem::path localPath;
    bool fromCache = false;
    // 仅手动“换源”的成功结果允许写回 cache.json；自动加载始终为 false。
    bool recordSource = false;
};

// 歌词仓库：统一“本地优先、缓存源其次、在线源兜底”的加载策略。
class LyricRepository {
public:
    LyricRepository(std::filesystem::path lyricsDirectory, cache::LyricCache& cache, OnlineLyrics online = {});

    // 按歌曲关键字加载歌词；本地可用时始终优先，之后的在线搜索才受缓存参数影响。
    LyricLoadResult loadForKeyword(std::wstring_view keyword, const config::AppConfig& config, bool ignoreCache = false,
        const OnlineTrackVerifier& appleMusic = {});
    // 手动换源跳过本地文件，沿配置顺序从当前在线源向后切换。
    LyricLoadResult loadNextOnlineSource(std::wstring_view keyword, const config::AppConfig& config,
        const OnlineTrackVerifier& appleMusic = {});
    // 注入专用的 Apple Music 客户端，用于测试和无网络加载路径。
    void setAppleMusicClient(AppleMusicLyricsClient client);
    // 只查本地 lyrics 目录，支持精确文件名和宽松匹配。
    std::vector<std::uint8_t> loadLocal(std::wstring_view keyword, std::filesystem::path* matchedPath = nullptr) const;
    // 只按缓存中的在线源重试；行级缓存会放弃命中，重新按优先级寻找逐字歌词。
    LyricLoadResult loadCachedOnline(std::wstring_view keyword, const config::AppConfig& config,
        const OnlineTrackVerifier& appleMusic = {}) const;

private:
    std::vector<std::uint8_t> fetchOnline(LyricSource source, std::wstring_view keyword, const config::AppConfig& config,
        const OnlineTrackVerifier& appleMusic, std::vector<std::uint8_t>* translatedBytes = nullptr) const;

    std::filesystem::path lyricsDirectory_;
    cache::LyricCache& cache_;
    OnlineLyrics online_;
    AppleMusicLyricsClient appleMusicClient_;
};

// 把 SMTC 的歌手/标题组合成统一文件名/缓存关键字。
std::wstring makeKeyword(std::wstring_view artist, std::wstring_view title);

// 至少两行包含多个时间片段才视为可靠的逐字歌词；普通 LRC/行级 TTML 为 false。
bool hasWordLevelTiming(const std::vector<std::uint8_t>& bytes);

}
