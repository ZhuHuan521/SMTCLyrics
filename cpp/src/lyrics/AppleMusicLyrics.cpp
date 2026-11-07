#include "lyrics/AppleMusicLyrics.h"
#include "json.hpp"
#include "util/Base64.h"
#include "util/Encoding.h"
#include "lyrics/LrcParser.h"
#include "lyrics/TtmlParser.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <set>
#include <string>
namespace smtc::lyrics {
namespace {
constexpr std::string_view kAppleMusicOrigin = "https://music.apple.com";
constexpr std::string_view kAppleMusicApi = "https://amp-api.music.apple.com/v1";

nlohmann::json parseJson(std::string_view text) {
    try { return nlohmann::json::parse(text); } catch (...) { return {}; }
}
std::string jsonText(const nlohmann::json* value) {
    if (!value) return {};
    if (value->is_string()) return value->get<std::string>();
    if (value->is_number_integer()) return std::to_string(value->get<long long>());
    if (value->is_number_unsigned()) return std::to_string(value->get<unsigned long long>());
    if (value->is_number_float()) {
        const auto number = value->get<double>();
        return std::isfinite(number) ? std::to_string(static_cast<long long>(number + 0.5)) : std::string{};
    }
    return {};
}
const nlohmann::json* at(const nlohmann::json* value, std::string_view key) {
    if (!value || !value->is_object()) return nullptr;
    const auto found = value->find(std::string(key));
    return found == value->end() ? nullptr : &*found;
}
const nlohmann::json* at(const nlohmann::json* value, std::size_t index) {
    if (!value || !value->is_array() || index >= value->size()) return nullptr;
    return &(*value)[index];
}
std::string trim(std::string_view value) { return util::trimUtf8(value); }
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
std::wstring normalizedName(std::wstring_view value) {
    std::wstring result;
    for (const auto ch : util::trim(value)) {
        if (std::iswspace(ch) || std::iswpunct(ch)) continue;
        result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return result;
}
bool isBase64Url(char ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
}
std::string decodeBase64Url(std::string_view text) {
    std::string padded(text);
    while (padded.size() % 4 != 0) padded.push_back(L'=');
    std::replace(padded.begin(), padded.end(), L'-', L'+');
    std::replace(padded.begin(), padded.end(), L'_', L'/');
    const auto bytes = util::base64Decode(padded);
    return {bytes.begin(), bytes.end()};
}
std::int64_t intValue(const nlohmann::json* value) {
    const auto text = jsonText(value);
    if (text.empty()) return 0;
    try { return static_cast<std::int64_t>(std::llround(std::stod(text))); } catch (...) { return 0; }
}
std::string titleOf(const nlohmann::json& attrs) { return jsonText(at(&attrs, "name")); }
std::string artistOf(const nlohmann::json& attrs) { return jsonText(at(&attrs, "artistName")); }
std::int64_t durationOf(const nlohmann::json& attrs) { return intValue(at(&attrs, "durationInMillis")); }
bool artistMatches(std::string_view wanted, std::string_view actual) {
    const auto wantedKey = normalizedName(util::utf8ToWide(wanted));
    const auto actualKey = normalizedName(util::utf8ToWide(actual));
    return !wantedKey.empty() && !actualKey.empty() && actualKey.find(wantedKey) != std::wstring::npos;
}
bool modifiedVersion(std::string_view title) {
    const auto lower = lowerAscii(std::string(title));
    return lower.find("live") != std::string::npos || lower.find("remix") != std::string::npos ||
           lower.find("karaoke") != std::string::npos || title.find("伴奏") != std::string_view::npos ||
           title.find("翻唱") != std::string_view::npos || title.find("现场") != std::string_view::npos;
}
bool candidateMatches(const nlohmann::json& attrs, std::string_view title, std::string_view artist, std::int64_t durationMs) {
    const auto actualTitle = titleOf(attrs);
    if (actualTitle.empty() || normalizedName(util::utf8ToWide(actualTitle)) != normalizedName(util::utf8ToWide(title))) return false;
    if (!artist.empty() && !artistMatches(artist, artistOf(attrs))) return false;
    if (modifiedVersion(actualTitle) && !modifiedVersion(title)) return false;
    const auto actualDuration = durationOf(attrs);
    return durationMs <= 0 || actualDuration <= 0 || std::abs(durationMs - actualDuration) <= 3000;
}
// 返回请求语言的 TTML；缺失时返回空串，由调用方继续尝试下一语言，
// 不把 ttmlLocalizations 中其他地区或语言的内容当作当前翻译。
const nlohmann::json* localizationFor(const nlohmann::json& localizations, std::string_view language) {
    if (localizations.is_string()) return &localizations;
    if (!localizations.is_object()) return nullptr;
    const auto findKey = [&](std::string_view candidate) -> const nlohmann::json* {
        const auto exact = localizations.find(std::string(candidate));
        if (exact != localizations.end()) return &*exact;
        for (auto it = localizations.begin(); it != localizations.end(); ++it) {
            if (lowerAscii(it.key()) == lowerAscii(std::string(candidate))) return &*it;
        }
        return nullptr;
    };
    if (const auto* exact = findKey(language)) return exact;
    std::string key(language);
    const auto dash = key.find('-');
    if (dash != std::string::npos) {
        const auto second = key.find('-', dash + 1);
        if (second != std::string::npos) key.resize(second);
    }
    return findKey(key);
}

std::string pickTtml(const nlohmann::json& response, std::string_view requestedLanguage) {
    const auto* attrs = at(at(at(&response, "data"), std::size_t{0}), "attributes");
    if (!attrs) return {};
    const auto* localized = at(attrs, "ttmlLocalizations");
    if (requestedLanguage.empty()) {
        const auto original = jsonText(at(attrs, "ttml"));
        if (!original.empty()) return original;
        return localized && localized->is_string() ? localized->get<std::string>() : std::string{};
    }
    if (localized && !localized->is_null() && !localized->empty()) {
        const auto* selected = localizationFor(*localized, requestedLanguage);
        return selected && selected->is_string() ? selected->get<std::string>() : std::string{};
    }
    return jsonText(localized ? localized : at(attrs, "ttml"));
}
std::vector<http::HttpClient::Header> browserHeaders() {
    return {
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"},
        {"Accept", "application/json"},
        {"Origin", std::string(kAppleMusicOrigin)},
        {"Referer", std::string(kAppleMusicOrigin) + "/"},
    };
}
std::string tokenBearer(const std::string& raw) {
    auto token = trim(raw);
    if (token.size() >= 7 && lowerAscii(token.substr(0, 7)) == "bearer ") token = trim(std::string_view(token).substr(7));
    return token;
}
std::string storefrontName(std::string_view value) {
    const auto result = lowerAscii(trim(value));
    if (result.empty()) return "cn";
    if (result.size() < 2 || result.size() > 3 || !std::all_of(result.begin(), result.end(), [](unsigned char ch) { return std::isalpha(ch) != 0; })) return "cn";
    return result;
}
std::string firstId(std::string_view json) {
    const auto response = parseJson(json);
    const auto* first = at(at(&response, "data"), 0);
    return jsonText(at(first, "id"));
}
bool authFailure(unsigned status) { return status == 401 || status == 403; }
std::string networkFailureDetail(const http::HttpResponse& response) {
    if (response.networkError == 0) return "无法连接 Apple Music";
    return "网络请求失败 (WinHTTP " + std::to_string(response.networkError) + ")";
}
std::vector<http::HttpClient::Header> apiHeaders(std::string_view developerToken, std::string_view mediaUserToken) {
    auto headers = browserHeaders();
    headers.emplace_back("Authorization", "Bearer " + std::string(developerToken));
    headers.emplace_back("Media-User-Token", std::string(mediaUserToken));
    return headers;
}
bool verifiedCandidate(const OnlineTrackVerifier& candidate, const OnlineTrackVerifier& wanted) {
    if (normalizedName(util::utf8ToWide(candidate.title)) != normalizedName(util::utf8ToWide(wanted.title))) return false;
    if (!wanted.artist.empty() && !artistMatches(wanted.artist, candidate.artist)) return false;
    if (modifiedVersion(candidate.title) && !modifiedVersion(wanted.title)) return false;
    return wanted.durationMs <= 0 || candidate.durationMs <= 0 || std::abs(wanted.durationMs - candidate.durationMs) <= 3000;
}
} // namespace

std::string appleMusicLyricQuery(std::string_view language) {
    // 原文必须直接取 attrs.ttml。带 extend=ttmlLocalizations 时接口只返回
    // ttmlLocalizations 一个 TTML 字符串，attrs.ttml 为空，不能把它误作原文。
    if (language.empty()) return {};
    std::string query = "?extend=ttmlLocalizations";
    query += "&l%5Blyrics%5D=" + util::urlEncode(language);
    // Same derivation as am-ttml-fetch: zh-Hans-CN -> zh-Hans.
    const auto dash = language.find('-');
    if (dash != std::string_view::npos) {
        const auto second = language.find('-', dash + 1);
        query += "&l%5Bscript%5D=" + util::urlEncode(language.substr(0, second == std::string_view::npos ? language.size() : second));
    }
    return query;
}

std::string appleMusicOriginalTtml(std::string_view responseJson) {
    const auto response = parseJson(responseJson);
    const auto* attrs = at(at(at(&response, "data"), std::size_t{0}), "attributes");
    if (!attrs) return {};
    const auto original = jsonText(at(attrs, "ttml"));
    if (!original.empty()) return original;
    // 兼容仍然返回 ttmlLocalizations 字符串的接口；字符串本身就是 TTML。
    const auto* localized = at(attrs, "ttmlLocalizations");
    return localized && localized->is_string() ? localized->get<std::string>() : std::string{};
}

AppleMusicLyricSelection selectAppleMusicLyrics(std::string_view responseJson, std::string_view requestedLanguage) {
    AppleMusicLyricSelection selection;
    const auto response = parseJson(responseJson);
    const auto* attrs = at(at(at(&response, "data"), std::size_t{0}), "attributes");
    if (!attrs) return selection;

    const auto* localizations = at(attrs, "ttmlLocalizations");
    selection.originalTtml = appleMusicOriginalTtml(responseJson);
    TtmlParser originalTtmlParser;
    selection.originalHasEmbeddedTranslation =
        originalTtmlParser.parseUtf8(selection.originalTtml) && originalTtmlParser.hasTranslationsBlock();
    if (requestedLanguage.empty()) return selection;

    if (!localizations || !localizations->is_object()) return selection;
    const auto* selected = localizationFor(*localizations, requestedLanguage);
    std::string translated;
    if (selected && selected->is_string()) {
        translated = selected->get<std::string>();
    } else if (selected && selected->is_object()) {
        const auto* nested = at(selected, "ttml");
        if (nested && nested->is_string()) translated = nested->get<std::string>();
    }
    if (!selected || translated.empty() || translated == selection.originalTtml) return selection;

    selection.translatedTtml = translated;
    selection.requestedLanguage = std::string(requestedLanguage);
    return selection;
}

std::string appleMusicTtmlForLanguage(std::string_view responseJson, std::string_view requestedLanguage) {
    const auto selection = selectAppleMusicLyrics(responseJson, requestedLanguage);
    if (requestedLanguage.empty()) return selection.originalTtml;
    return selection.translatedTtml;
}

std::optional<std::string> parseAppleMusicAccountStorefront(std::string_view json) {
    const auto body = parseJson(json);
    const auto* item = at(at(&body, "data"), std::size_t{0});
    const auto id = lowerAscii(trim(jsonText(at(item, "id"))));
    if (id.size() < 2 || id.size() > 3 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char ch) { return std::isalpha(ch) != 0; })) {
        return std::nullopt;
    }
    return id;
}

std::string appleMusicStorefrontFailure(const AppleMusicFetchResult& result) {
    if (!result.diagnostic.empty()) return result.diagnostic;
    switch (result.status) {
    case AppleMusicStatus::AuthRequired:
        return "Apple Music 地区检测需要填写 Authorization 和 Media-User-Token";
    case AppleMusicStatus::AuthRejected:
        return "Apple Music 凭证被拒绝，无法读取账号地区";
    default:
        return "无法自动读取 Apple Music 账号地区";
    }
}

std::int64_t appleMusicTokenExpiry(std::string_view token) {
    const auto raw = tokenBearer(std::string(token));
    const auto firstDot = raw.find(L'.');
    const auto secondDot = raw.find(L'.', firstDot == std::string::npos ? 0 : firstDot + 1);
    if (firstDot == std::string::npos || secondDot == std::string::npos) return 0;
    const auto payload = parseJson(decodeBase64Url(std::string_view(raw).substr(firstDot + 1, secondDot - firstDot - 1)));
    return intValue(at(&payload, "exp"));
}
std::optional<std::string> normalizedCatalogId(std::string_view value) {
    const auto id = trim(value);
    if (id.empty() || id.size() > 20 || !std::all_of(id.begin(), id.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) return std::nullopt;
    return id;
}
std::vector<OnlineTrackVerifier> parseAppleMusicTrackCandidates(std::string_view json) {
    std::vector<OnlineTrackVerifier> result;
    const auto response = parseJson(json);
    auto add = [&](const nlohmann::json& item) {
        const auto id = normalizedCatalogId(jsonText(at(&item, "id")));
        const auto* attrs = at(&item, "attributes");
        if (!id || !attrs) return;
        OnlineTrackVerifier candidate;
        candidate.catalogId = *id;
        candidate.title = titleOf(*attrs);
        candidate.artist = artistOf(*attrs);
        candidate.album = jsonText(at(attrs, "albumName"));
        candidate.isrc = jsonText(at(attrs, "isrc"));
        candidate.durationMs = durationOf(*attrs);
        if (!candidate.title.empty() && !candidate.artist.empty()) result.push_back(std::move(candidate));
    };
    const auto* data = at(&response, "data");
    if (data && data->is_array()) {
        for (const auto& item : *data) add(item);
    } else {
        // 搜索接口的结构是 results.songs.data，不是顶层 data。
        // 兼容这种结构后，普通 SMTC 模式才能按歌名和歌手找到真实 catalog ID。
        const auto* searchSongs = at(at(at(&response, "results"), "songs"), "data");
        if (searchSongs && searchSongs->is_array()) {
            for (const auto& item : *searchSongs) add(item);
        } else if (at(&response, "attributes")) {
            add(response);
        }
    }
    return result;
}
OnlineTrackVerifier parseAppleMusicTrackCandidate(std::string_view json) {
    const auto all = parseAppleMusicTrackCandidates(json);
    return all.empty() ? OnlineTrackVerifier{} : all.front();
}
bool appleMusicRecordingMatches(std::string_view title, std::string_view artist, std::string_view json, std::int64_t durationMs) {
    const auto candidates = parseAppleMusicTrackCandidates(json);
    return !candidates.empty() && candidateMatches(
        nlohmann::json{{"name", candidates.front().title},
                       {"artistName", candidates.front().artist},
                       {"durationInMillis", candidates.front().durationMs}},
        title, artist, durationMs);
}
AppleMusicLyricsClient::AppleMusicLyricsClient(http::HttpClient client) : client_(std::move(client)) {}

AppleMusicFetchResult AppleMusicLyricsClient::detectStorefront(const AppleMusicTokenSettings& settings) const {
    AppleMusicFetchResult result;
    const auto developerToken = tokenBearer(settings.developerToken);
    const auto mediaUserToken = trim(settings.mediaUserToken);
    if (developerToken.empty() || mediaUserToken.empty()) {
        result.status = AppleMusicStatus::AuthRequired;
        result.diagnostic = "Apple Music 官方歌词需要在设置中填写 Authorization 和 Media-User-Token";
        return result;
    }
    const auto account = client_.get(
        std::string(kAppleMusicApi) + "/me/storefront", apiHeaders(developerToken, mediaUserToken));
    if (authFailure(account.statusCode)) {
        result.status = AppleMusicStatus::AuthRejected;
        result.diagnostic = "Apple Music 凭证无法访问账号地区 (HTTP " + std::to_string(account.statusCode) + ")";
        return result;
    }
    const auto detected = parseAppleMusicAccountStorefront(account.text());
    if (!detected) {
        result.diagnostic = account.statusCode == 0
            ? "Apple Music 无法自动读取账号地区：" + networkFailureDetail(account)
            : "Apple Music 账号地区响应无效 (HTTP " + std::to_string(account.statusCode) + ")";
        return result;
    }
    result.status = AppleMusicStatus::Fetched;
    result.detectedStorefront = *detected;
    return result;
}

bool AppleMusicLyricsClient::validStorefront(std::string_view storefront) {
    const auto value = trim(storefront);
    return value.empty() || (value.size() >= 2 && value.size() <= 3 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isalpha(ch) != 0; }));
}

AppleMusicFetchResult AppleMusicLyricsClient::fetch(std::string_view keywordUtf8, const OnlineTrackVerifier& track,
                                                    const AppleMusicTokenSettings& settings) const {
    AppleMusicFetchResult result;
    // 先取得原文，并按原文字种决定默认显示和译文请求。
    auto original = fetch(keywordUtf8, track, settings, "");
    if (original.status == AppleMusicStatus::AuthRequired || original.status == AppleMusicStatus::AuthRejected ||
        original.status == AppleMusicStatus::MalformedResponse) {
        return original;
    }
    if (original.status != AppleMusicStatus::Fetched && original.status != AppleMusicStatus::LineOnly) return original;
    LrcParser originalParser;
    const auto originalBytes = std::vector<std::uint8_t>(original.ttml.begin(), original.ttml.end());
    if (!originalParser.parseBytes(originalBytes)) return original;
    TtmlParser sourceTtml;
    const bool sourceIsCjk = sourceTtml.parseUtf8(original.ttml) &&
        mostlyCjkLyrics(sourceTtml.originalLines());

    // 中文歌依次请求简体、台湾繁中、香港繁中；外文歌只额外请求简体，
    // 并继续默认显示原文。Apple 没有译文时 translatedTtml 保持为空。
    const auto languages = sourceIsCjk
        ? std::span<const std::string_view>(kAppleMusicLyricsLanguages, 3)
        : std::span<const std::string_view>(kAppleMusicLyricsLanguages).subspan(4, 1);
    for (const auto language : languages) {
        if (language.empty()) break;
        const auto translated = fetch(keywordUtf8, track, settings, language);
        if (translated.translatedTtml.empty()) continue;
        TtmlParser translatedTtml;
        if (translatedTtml.parseUtf8(translated.translatedTtml) && translatedTtml.translatedLines().empty()) {
            // Apple 在没有该歌词语言时会回传原文 TTML。没有本地化翻译文本时
            // 不能把原文当成可切换译文，继续寻找；否则界面仍应提示“无翻译”。
            continue;
        }
        original.translatedTtml = translated.translatedTtml;
        original.detectedStorefront = translated.detectedStorefront.empty()
            ? original.detectedStorefront : translated.detectedStorefront;
        return original;
    }
    original.translatedTtml.clear();
    return original;
}
AppleMusicFetchResult AppleMusicLyricsClient::fetch(std::string_view keywordUtf8, const OnlineTrackVerifier& track, const AppleMusicTokenSettings& settings, std::string_view fixedLanguage) const {
    AppleMusicFetchResult result;
    const auto developerToken = tokenBearer(settings.developerToken);
    const auto mediaUserToken = trim(settings.mediaUserToken);
    if (developerToken.empty() || mediaUserToken.empty()) {
        result.status = AppleMusicStatus::AuthRequired;
        result.diagnostic = "Apple Music 官方歌词需要在设置中填写 Authorization 和 Media-User-Token";
        return result;
    }
    const auto requestHeaders = apiHeaders(developerToken, mediaUserToken);
    const auto get = [&](std::string_view path) {
        return client_.get(std::string(kAppleMusicApi) + std::string(path), requestHeaders);
    };
    // 用户填写地区时使用该值；否则从当前账号读取真实曲库地区。
    // 不按公网 IP 或 cn 猜测，以免账号曲库中的 catalog ID 返回 404。
    auto storefront = storefrontName(settings.storefront);
    std::string detectedStorefront;
    if (trim(settings.storefront).empty()) {
        const auto detected = detectStorefront(settings);
        if (detected.status != AppleMusicStatus::Fetched) {
            result.status = detected.status;
            result.diagnostic = detected.diagnostic + "；请在设置中填写地区（如 tr、cn、us）";
            return result;
        }
        detectedStorefront = detected.detectedStorefront;
        result.detectedStorefront = detectedStorefront;
        storefront = detected.detectedStorefront;
    }
    OnlineTrackVerifier wanted = track;
    wanted.catalogId = normalizedCatalogId(track.catalogId).value_or(std::string{});
    wanted.isrc = trim(track.isrc);
    wanted.title = trim(track.title.empty() ? keywordUtf8 : track.title);
    wanted.artist = trim(track.artist);
    if (wanted.title.empty()) {
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "Apple Music 搜索缺少歌曲名";
        return result;
    }
    std::string selectedId;
    std::vector<OnlineTrackVerifier> candidates;
    // catalog ID 是内部播放对象的精确身份；先查询详情，作为后续匹配的锚点。
    if (!wanted.catalogId.empty()) {
        const auto detail = get("/catalog/" + storefront + "/songs/" + wanted.catalogId);
        if (authFailure(detail.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "Apple Music 凭证被拒绝 (HTTP " + std::to_string(detail.statusCode) + ")";
            return result;
        }
        if (detail) {
            selectedId = normalizedCatalogId(firstId(detail.text())).value_or(wanted.catalogId);
            candidates = parseAppleMusicTrackCandidates(detail.text());
        } else if (detail.statusCode == 0) {
            result.diagnostic = "Apple Music 无法验证当前曲目：" + networkFailureDetail(detail);
            return result;
        }
    }
    // ISRC 跨地区共享；若查询成功，优先用它精确对应同一录音。
    if (!wanted.isrc.empty()) {
        const auto response = get("/catalog/" + storefront + "/songs?filter%5Bisrc%5D=" + util::urlEncode(wanted.isrc));
        if (authFailure(response.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "Apple Music 凭证被拒绝 (HTTP " + std::to_string(response.statusCode) + ")";
            return result;
        }
        auto isrcCandidates = parseAppleMusicTrackCandidates(response.text());
        std::erase_if(isrcCandidates, [&](const OnlineTrackVerifier& item) {
            // ISRC 查询若无结果，Apple 可能返回不限筛选的候选。不能拿别的录音顶替当前曲目。
            return item.isrc.empty() || lowerAscii(item.isrc) != lowerAscii(wanted.isrc);
        });
        if (!isrcCandidates.empty()) {
            candidates = std::move(isrcCandidates);
            selectedId.clear();
        }
    }
    // catalog/ISRC 都未命中时再按名称搜索；搜索结果仍须通过标题、艺人和版本校验。
    if (candidates.empty() && selectedId.empty()) {
        const auto search = get("/catalog/" + storefront + "/search?term=" + util::urlEncode(keywordUtf8) + "&types=songs&limit=25");
        if (authFailure(search.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "Apple Music 凭证被拒绝 (HTTP " + std::to_string(search.statusCode) + ")";
            return result;
        }
        candidates = parseAppleMusicTrackCandidates(search.text());
    }
    // 搜索按歌名得到候选；后续歌词请求仍绑定这些真实 catalog ID。
    if (candidates.empty() && selectedId.empty()) {
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "Apple Music 未找到匹配的曲目";
        return result;
    }
    if (selectedId.empty() && !candidates.empty()) selectedId = candidates.front().catalogId;
    auto lyrics = get("/catalog/" + storefront + "/songs/" + selectedId + "/syllable-lyrics" + appleMusicLyricQuery(fixedLanguage));
    if (lyrics.statusCode == 404 && !wanted.isrc.empty()) {
        const auto response = get("/catalog/" + storefront + "/songs?filter%5Bisrc%5D=" + util::urlEncode(wanted.isrc));
        for (const auto& alternative : parseAppleMusicTrackCandidates(response.text())) {
            if (alternative.catalogId == selectedId || !verifiedCandidate(alternative, wanted)) continue;
            lyrics = get("/catalog/" + storefront + "/songs/" + alternative.catalogId + "/syllable-lyrics" + appleMusicLyricQuery(fixedLanguage));
            if (lyrics) break;
        }
    }
    if (authFailure(lyrics.statusCode)) {
        result.status = AppleMusicStatus::AuthRejected;
        result.diagnostic = "Apple Music 凭证无效、已过期或无此歌词权限 (HTTP " + std::to_string(lyrics.statusCode) + ")";
        return result;
    }
    if (lyrics.statusCode == 404) {
        result.status = AppleMusicStatus::Unavailable;
        result.diagnostic = "该曲目在当前 Apple Music 地区没有逐字歌词 (HTTP 404)";
        result.detectedStorefront = detectedStorefront;
        return result;
    }
    // 已检测到账号地区时，任何歌词请求结果都应带回该值；自动地区不能因首曲失败而丢失。
    result.detectedStorefront = detectedStorefront;
    if (!lyrics) {
        result.diagnostic = lyrics.statusCode == 0
            ? "Apple Music 歌词接口请求失败：" + networkFailureDetail(lyrics)
            : "Apple Music 歌词接口请求失败 (HTTP " + std::to_string(lyrics.statusCode) + ")";
        return result;
    }
    const auto selection = selectAppleMusicLyrics(lyrics.text(), fixedLanguage);
    result.ttml = selection.originalTtml;
    // 仅请求一种本地化语言时，响应只含 ttmlLocalizations，不含 attrs.ttml。
    // 此时返回的 TTML 就是该语言的翻译，不能因为 translatedTtml 字段为空而
    // 把已经获取到的翻译文本丢弃。
    result.translatedTtml = selection.translatedTtml.empty() && !fixedLanguage.empty()
        ? selection.originalTtml : selection.translatedTtml;
    if (result.ttml.empty()) {
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = fixedLanguage.empty()
            ? "Apple Music 接口未返回原文歌词"
            : "该曲目在此 Apple Music 地区没有请求语言的翻译";
        return result;
    }
    if (result.ttml.empty() || result.ttml.find("<tt") == std::string::npos) {
        result.status = AppleMusicStatus::MalformedResponse;
        result.diagnostic = "Apple Music 接口未返回 TTML 歌词";
        return result;
    }
    // Both Word and Line TTML are valid fallbacks; the repository decides whether
    // to continue looking for syllable timing and marks line-only results accordingly.
    LrcParser parser;
    const std::vector<std::uint8_t> bytes(result.ttml.begin(), result.ttml.end());
    if (!parser.parseBytes(bytes) || parser.lines().empty()) {
        result.status = AppleMusicStatus::MalformedResponse;
        result.diagnostic = "Apple Music TTML 没有可显示歌词行";
        return result;
    }
    result.status = TtmlParser::hasUsableSyllableTiming(result.ttml) ? AppleMusicStatus::Fetched : AppleMusicStatus::LineOnly;
    return result;
}
}
