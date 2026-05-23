#include "lyrics/LyricRepository.h"

#include "util/Encoding.h"

#include <algorithm>

namespace smtc::lyrics {
namespace {

bool usefulLyrics(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 10;
}

std::wstring lowerNoSpace(std::wstring text) {
    std::wstring out;
    for (wchar_t ch : text) {
        if (iswspace(ch) || ch == L'-' || ch == L'_' || ch == L'.') continue;
        out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

}

LyricRepository::LyricRepository(std::filesystem::path lyricsDirectory, cache::LyricCache& cache, OnlineLyrics online)
    : lyricsDirectory_(std::move(lyricsDirectory)), cache_(cache), online_(std::move(online)) {}

LyricLoadResult LyricRepository::loadForKeyword(std::wstring_view keyword, const config::AppConfig& config, bool ignoreCache) {
    LyricLoadResult result;
    const auto keywordUtf8 = util::wideToUtf8(keyword);

    result.lrcBytes = loadLocal(keyword, &result.localPath);
    result.source = LyricSource::Local;
    if (usefulLyrics(result.lrcBytes)) {
        return result;
    }
    result.localPath.clear();

    if (!ignoreCache) {
        if (auto cached = cache_.sourceFor(keywordUtf8)) {
            if (auto source = sourceFromIndex(*cached)) {
                if (*source != LyricSource::Local) {
                    result.lrcBytes = fetchOnline(*source, keyword, config);
                    if (usefulLyrics(result.lrcBytes)) {
                        result.source = *source;
                        result.fromCache = true;
                        return result;
                    }
                }
            }
        }
    }

    for (const auto sourceIndex : config.sourcePriority) {
        const auto source = sourceFromIndex(sourceIndex);
        if (!source || *source == LyricSource::Local) continue;
        auto bytes = fetchOnline(*source, keyword, config);
        if (usefulLyrics(bytes)) {
            result.lrcBytes = std::move(bytes);
            result.source = *source;
            cache_.setSource(keywordUtf8, sourceIndex);
            return result;
        }
    }

    result.lrcBytes.clear();
    return result;
}

std::vector<std::uint8_t> LyricRepository::loadLocal(std::wstring_view keyword, std::filesystem::path* matchedPath) const {
    try {
        const auto exact = exactLocalPath(keyword);
        if (std::filesystem::is_regular_file(exact)) {
            if (matchedPath) *matchedPath = exact;
            return util::readFileBytes(exact);
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(lyricsDirectory_, ec)) return {};
        const auto normalizedKeyword = lowerNoSpace(std::wstring(keyword));
        for (const auto& entry : std::filesystem::directory_iterator(lyricsDirectory_, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            if (entry.path().extension() != L".lrc") continue;
            const auto stem = lowerNoSpace(entry.path().stem().wstring());
            if (stem == normalizedKeyword || stem.find(normalizedKeyword) != std::wstring::npos || normalizedKeyword.find(stem) != std::wstring::npos) {
                if (matchedPath) *matchedPath = entry.path();
                return util::readFileBytes(entry.path());
            }
        }
    } catch (...) {
    }
    return {};
}

std::filesystem::path LyricRepository::exactLocalPath(std::wstring_view keyword) const {
    return lyricsDirectory_ / (std::wstring(keyword) + L".lrc");
}

std::vector<std::uint8_t> LyricRepository::fetchOnline(LyricSource source, std::wstring_view keyword, const config::AppConfig& config) const {
    try {
        const auto keywordUtf8 = util::wideToUtf8(keyword);
        (void)config;
        return online_.fetch(source, keywordUtf8);
    } catch (...) {
        return {};
    }
}

std::wstring makeKeyword(std::wstring_view artist, std::wstring_view title) {
    const auto cleanArtist = util::trim(artist);
    const auto cleanTitle = util::trim(title);
    if (!cleanArtist.empty() && !cleanTitle.empty()) {
        return cleanArtist + L" " + cleanTitle;
    }
    return cleanTitle.empty() ? cleanArtist : cleanTitle;
}

}
