#include "lyrics/LyricRepository.h"

#include "lyrics/LrcParser.h"
#include "lyrics/QrcDecrypter.h"
#include "util/Base64.h"
#include "util/Encoding.h"
#include "util/Inflate.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <string_view>
#include <system_error>

namespace smtc::lyrics {
namespace {

constexpr std::wstring_view kPreferredLocalExtensions[] = {L".krc", L".qrc"};
constexpr std::wstring_view kFallbackLocalExtensions[] = {L".lrc"};
constexpr std::array<std::uint8_t, 10> kQrcLocalMagic{
    0x98, 0x25, 0xB0, 0xAC, 0xE3, 0x02, 0x83, 0x68, 0xE8, 0xFC
};

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

bool extensionMatches(const std::filesystem::path& path, std::wstring_view expectedExtension) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

    std::wstring expected(expectedExtension);
    std::transform(expected.begin(), expected.end(), expected.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return extension == expected;
}

bool parsesAsLyrics(const std::vector<std::uint8_t>& bytes) {
    if (!usefulLyrics(bytes)) return false;
    LrcParser parser;
    return parser.parseBytes(bytes);
}

std::vector<std::uint8_t> decryptKrcBytes(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 4> kHeader{'k', 'r', 'c', '1'};
    constexpr std::array<std::uint8_t, 16> kKey{64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105};
    if (bytes.size() <= kHeader.size() || !std::equal(kHeader.begin(), kHeader.end(), bytes.begin())) {
        return {};
    }

    std::vector<std::uint8_t> zlibBytes(bytes.begin() + static_cast<std::ptrdiff_t>(kHeader.size()), bytes.end());
    for (std::size_t i = 0; i < zlibBytes.size(); ++i) {
        zlibBytes[i] ^= kKey[i % kKey.size()];
    }
    return util::inflateZlib(zlibBytes);
}

bool hasQrcLocalMagic(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() > kQrcLocalMagic.size() &&
           std::equal(kQrcLocalMagic.begin(), kQrcLocalMagic.end(), bytes.begin());
}

void qmc1DecryptInPlace(std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<std::uint8_t, 128> kPrivKey{
        0xc3, 0x4a, 0xd6, 0xca, 0x90, 0x67, 0xf7, 0x52, 0xd8, 0xa1, 0x66, 0x62, 0x9f, 0x5b, 0x09, 0x00,
        0xc3, 0x5e, 0x95, 0x23, 0x9f, 0x13, 0x11, 0x7e, 0xd8, 0x92, 0x3f, 0xbc, 0x90, 0xbb, 0x74, 0x0e,
        0xc3, 0x47, 0x74, 0x3d, 0x90, 0xaa, 0x3f, 0x51, 0xd8, 0xf4, 0x11, 0x84, 0x9f, 0xde, 0x95, 0x1d,
        0xc3, 0xc6, 0x09, 0xd5, 0x9f, 0xfa, 0x66, 0xf9, 0xd8, 0xf0, 0xf7, 0xa0, 0x90, 0xa1, 0xd6, 0xf3,
        0xc3, 0xf3, 0xd6, 0xa1, 0x90, 0xa0, 0xf7, 0xf0, 0xd8, 0xf9, 0x66, 0xfa, 0x9f, 0xd5, 0x09, 0xc6,
        0xc3, 0x1d, 0x95, 0xde, 0x9f, 0x84, 0x11, 0xf4, 0xd8, 0x51, 0x3f, 0xaa, 0x90, 0x3d, 0x74, 0x47,
        0xc3, 0x0e, 0x74, 0xbb, 0x90, 0xbc, 0x3f, 0x92, 0xd8, 0x7e, 0x11, 0x13, 0x9f, 0x23, 0x95, 0x5e,
        0xc3, 0x00, 0x09, 0x5b, 0x9f, 0x62, 0x66, 0xa1, 0xd8, 0x52, 0xf7, 0x67, 0x90, 0xca, 0xd6, 0x4a,
    };

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto keyIndex = i > 0x7FFF ? ((i % 0x7FFF) & 0x7F) : (i & 0x7F);
        bytes[i] ^= kPrivKey[keyIndex];
    }
}

std::vector<std::uint8_t> decryptLocalQrcBytes(const std::vector<std::uint8_t>& bytes) {
    if (!hasQrcLocalMagic(bytes)) return {};

    std::vector<std::uint8_t> decrypted(bytes);
    qmc1DecryptInPlace(decrypted);
    if (decrypted.size() <= 11) return {};

    const std::string encryptedHex(reinterpret_cast<const char*>(decrypted.data() + 11), decrypted.size() - 11);
    const auto xml = decryptQrc(encryptedHex);
    return {xml.begin(), xml.end()};
}

std::vector<std::uint8_t> preprocessLocalLyrics(const std::filesystem::path& path, const std::vector<std::uint8_t>& rawBytes) {
    if (parsesAsLyrics(rawBytes)) {
        return rawBytes;
    }

    if (extensionMatches(path, L".krc")) {
        const std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
        const auto decoded = util::base64Decode(rawText);
        if (parsesAsLyrics(decoded)) {
            return decoded;
        }
        if (auto decrypted = decryptKrcBytes(decoded); parsesAsLyrics(decrypted)) {
            return decrypted;
        }
        if (auto decrypted = decryptKrcBytes(rawBytes); parsesAsLyrics(decrypted)) {
            return decrypted;
        }
    }

    if (extensionMatches(path, L".qrc")) {
        const std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
        const auto decoded = util::base64Decode(rawText);
        if (parsesAsLyrics(decoded)) {
            return decoded;
        }
        if (auto decrypted = decryptLocalQrcBytes(decoded); parsesAsLyrics(decrypted)) {
            return decrypted;
        }
        if (auto decrypted = decryptLocalQrcBytes(rawBytes); parsesAsLyrics(decrypted)) {
            return decrypted;
        }
    }

    return {};
}

bool maybeUseLocalFile(const std::filesystem::path& path, std::filesystem::path* matchedPath, std::vector<std::uint8_t>& outBytes) {
    const auto rawBytes = util::readFileBytes(path);
    const auto processedBytes = preprocessLocalLyrics(path, rawBytes);
    if (!parsesAsLyrics(processedBytes)) return false;
    if (matchedPath) *matchedPath = path;
    outBytes = processedBytes;
    return true;
}

template <std::size_t N>
bool tryExactLocalFiles(
    const std::filesystem::path& lyricsDirectory,
    std::wstring_view keyword,
    const std::wstring_view (&extensions)[N],
    std::filesystem::path* matchedPath,
    std::vector<std::uint8_t>& outBytes) {
    for (const auto extension : extensions) {
        const auto path = lyricsDirectory / (std::wstring(keyword) + std::wstring(extension));
        if (!std::filesystem::is_regular_file(path)) continue;
        if (maybeUseLocalFile(path, matchedPath, outBytes)) return true;
    }
    return false;
}

template <std::size_t N>
bool tryFuzzyLocalFiles(
    const std::filesystem::path& lyricsDirectory,
    std::wstring_view keyword,
    const std::wstring_view (&extensions)[N],
    std::filesystem::path* matchedPath,
    std::vector<std::uint8_t>& outBytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(lyricsDirectory, ec)) return false;

    const auto normalizedKeyword = lowerNoSpace(std::wstring(keyword));
    for (const auto& entry : std::filesystem::directory_iterator(lyricsDirectory, ec)) {
        if (ec || !entry.is_regular_file()) continue;

        bool allowedExtension = false;
        for (const auto extension : extensions) {
            if (extensionMatches(entry.path(), extension)) {
                allowedExtension = true;
                break;
            }
        }
        if (!allowedExtension) continue;

        const auto stem = lowerNoSpace(entry.path().stem().wstring());
        if (stem != normalizedKeyword && stem.find(normalizedKeyword) == std::wstring::npos && normalizedKeyword.find(stem) == std::wstring::npos) {
            continue;
        }

        if (maybeUseLocalFile(entry.path(), matchedPath, outBytes)) return true;
    }
    return false;
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
        std::vector<std::uint8_t> bytes;
        if (tryExactLocalFiles(lyricsDirectory_, keyword, kPreferredLocalExtensions, matchedPath, bytes)) {
            return bytes;
        }
        if (tryFuzzyLocalFiles(lyricsDirectory_, keyword, kPreferredLocalExtensions, matchedPath, bytes)) {
            return bytes;
        }
        if (tryExactLocalFiles(lyricsDirectory_, keyword, kFallbackLocalExtensions, matchedPath, bytes)) {
            return bytes;
        }
        if (tryFuzzyLocalFiles(lyricsDirectory_, keyword, kFallbackLocalExtensions, matchedPath, bytes)) {
            return bytes;
        }
    } catch (...) {
    }
    return {};
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
    const auto cleanTitle = util::trim(title);
    const auto cleanArtist = artistForKeyword(artist);
    if (!cleanArtist.empty() && !cleanTitle.empty()) {
        return cleanTitle + L" - " + cleanArtist;
    }
    return cleanTitle.empty() ? cleanArtist : cleanTitle;
}

}
