#include "lyrics/LyricRepository.h"

#include "util/Encoding.h"

#include <algorithm>

namespace smtc::lyrics {
namespace {

// 过短的内容通常是接口错误、空文件或提示文本，不作为有效歌词。
bool usefulLyrics(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 10;
}

// 本地歌词宽松匹配时忽略大小写、空白和常见分隔符。
std::wstring lowerNoSpace(std::wstring text) {
    std::wstring out;
    for (wchar_t ch : text) {
        if (iswspace(ch) || ch == L'-' || ch == L'_' || ch == L'.') continue;
        out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

// 有些播放器会把 artist 写成“歌手—专辑”，这里只取歌手部分生成关键字。
std::wstring artistForKeyword(std::wstring_view artist) {
    const auto cleanArtist = util::trim(artist);
    const auto dash = cleanArtist.find(L'\u2014');
    if (dash == std::wstring::npos) {
        return cleanArtist;
    }

    const auto singer = util::trim(std::wstring_view(cleanArtist).substr(0, dash));
    const auto album = util::trim(std::wstring_view(cleanArtist).substr(dash + 1));
    if (singer.empty() || album.empty()) {
        return cleanArtist;
    }
    return singer;
}

}

LyricRepository::LyricRepository(std::filesystem::path lyricsDirectory, cache::LyricCache& cache, OnlineLyrics online)
    : lyricsDirectory_(std::move(lyricsDirectory)), cache_(cache), online_(std::move(online)) {}

LyricLoadResult LyricRepository::loadForKeyword(std::wstring_view keyword, const config::AppConfig& config, bool ignoreCache) {
    LyricLoadResult result;
    const auto keywordUtf8 = util::wideToUtf8(keyword);

    // 本地 lyrics 目录优先级最高，方便用户手动修正歌词。
    result.lrcBytes = loadLocal(keyword, &result.localPath);
    result.source = LyricSource::Local;
    if (usefulLyrics(result.lrcBytes)) {
        return result;
    }
    result.localPath.clear();

    if (!ignoreCache) {
        // 命中缓存源时只试这个源一次；失败后仍会按当前优先级完整搜索。
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

    // 缓存没有命中或显式忽略缓存时，按配置顺序逐个在线源尝试。
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
        // 先找精确文件名：lyrics/<标题 - 歌手>.lrc。
        const auto exact = exactLocalPath(keyword);
        if (std::filesystem::is_regular_file(exact)) {
            if (matchedPath) *matchedPath = exact;
            return util::readFileBytes(exact);
        }

        // 再做宽松扫描，兼容用户手写文件名略有差异的情况。
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
        // 本地目录遍历失败不阻断在线歌词搜索。
    }
    return {};
}

std::filesystem::path LyricRepository::exactLocalPath(std::wstring_view keyword) const {
    return lyricsDirectory_ / (std::wstring(keyword) + L".lrc");
}

std::vector<std::uint8_t> LyricRepository::fetchOnline(LyricSource source, std::wstring_view keyword, const config::AppConfig& config) const {
    try {
        // 当前在线抓取只需要关键字；config 参数保留给未来账号/区域等设置。
        const auto keywordUtf8 = util::wideToUtf8(keyword);
        (void)config;
        return online_.fetch(source, keywordUtf8);
    } catch (...) {
        // 单个歌词源失败时返回空，让调用方继续尝试下一个源。
        return {};
    }
}

std::wstring makeKeyword(std::wstring_view artist, std::wstring_view title) {
    // 统一生成“标题 - 歌手”，同一个关键字同时用于本地文件名和缓存 key。
    const auto cleanTitle = util::trim(title);
    const auto cleanArtist = artistForKeyword(artist);
    if (!cleanArtist.empty() && !cleanTitle.empty()) {
        return cleanTitle + L" - " + cleanArtist;
    }
    return cleanTitle.empty() ? cleanArtist : cleanTitle;
}

}
