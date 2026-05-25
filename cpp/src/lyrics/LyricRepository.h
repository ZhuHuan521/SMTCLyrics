#pragma once

#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/OnlineLyrics.h"

#include <filesystem>
#include <string>
#include <vector>

namespace smtc::lyrics {

// 一次歌词加载的结果，可能来自本地文件、缓存命中的在线源或按优先级搜索的在线源。
struct LyricLoadResult {
    std::vector<std::uint8_t> lrcBytes;
    LyricSource source = LyricSource::Local;
    std::filesystem::path localPath;
    bool fromCache = false;
};

// 歌词仓库：统一“本地优先、缓存源其次、在线源兜底”的加载策略。
class LyricRepository {
public:
    LyricRepository(std::filesystem::path lyricsDirectory, cache::LyricCache& cache, OnlineLyrics online = {});

    // 按歌曲关键字加载歌词；ignoreCache=true 时强制重新按优先级搜索在线源。
    LyricLoadResult loadForKeyword(std::wstring_view keyword, const config::AppConfig& config, bool ignoreCache = false);
    // 只查本地 lyrics 目录，支持精确文件名和宽松匹配。
    std::vector<std::uint8_t> loadLocal(std::wstring_view keyword, std::filesystem::path* matchedPath = nullptr) const;

private:
    std::filesystem::path exactLocalPath(std::wstring_view keyword) const;
    std::vector<std::uint8_t> fetchOnline(LyricSource source, std::wstring_view keyword, const config::AppConfig& config) const;

    std::filesystem::path lyricsDirectory_;
    cache::LyricCache& cache_;
    OnlineLyrics online_;
};

// 把 SMTC 的歌手/标题组合成统一文件名/缓存关键字。
std::wstring makeKeyword(std::wstring_view artist, std::wstring_view title);

}
