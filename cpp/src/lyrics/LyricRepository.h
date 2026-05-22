#pragma once

#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/OnlineLyrics.h"

#include <filesystem>
#include <string>
#include <vector>

namespace smtc::lyrics {

struct LyricLoadResult {
    std::vector<std::uint8_t> lrcBytes;
    LyricSource source = LyricSource::Local;
    std::filesystem::path localPath;
    bool fromCache = false;
};

class LyricRepository {
public:
    LyricRepository(std::filesystem::path lyricsDirectory, cache::LyricCache& cache, OnlineLyrics online = {});

    LyricLoadResult loadForKeyword(std::wstring_view keyword, const config::AppConfig& config, bool ignoreCache = false);
    std::vector<std::uint8_t> loadLocal(std::wstring_view keyword, std::filesystem::path* matchedPath = nullptr) const;

private:
    std::filesystem::path exactLocalPath(std::wstring_view keyword) const;
    std::vector<std::uint8_t> fetchOnline(LyricSource source, std::wstring_view keyword, const config::AppConfig& config) const;

    std::filesystem::path lyricsDirectory_;
    cache::LyricCache& cache_;
    OnlineLyrics online_;
};

std::wstring makeKeyword(std::wstring_view artist, std::wstring_view title);

}
