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
    std::string key(language);
    const auto dash = key.find('-');
    if (dash != std::string::npos) key.resize(dash);
    auto found = localizations.find(key);
    if (found == localizations.end()) {
        for (auto it = localizations.begin(); it != localizations.end(); ++it) {
            if (lowerAscii(it.key()) == lowerAscii(key)) {
                found = it;
                break;
            }
        }
    }
    return found == localizations.end() ? nullptr : &*found;
}

std::string pickTtml(const nlohmann::json& response, std::string_view requestedLanguage) {
    const auto* attrs = at(at(at(&response, "data"), std::size_t{0}), "attributes");
    if (!attrs) return {};
    const auto* localized = at(attrs, "ttmlLocalizations");
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
bool verifiedCandidate(const OnlineTrackVerifier& candidate, const OnlineTrackVerifier& wanted) {
    if (normalizedName(util::utf8ToWide(candidate.title)) != normalizedName(util::utf8ToWide(wanted.title))) return false;
    if (!wanted.artist.empty() && !artistMatches(wanted.artist, candidate.artist)) return false;
    if (modifiedVersion(candidate.title) && !modifiedVersion(wanted.title)) return false;
    return wanted.durationMs <= 0 || candidate.durationMs <= 0 || std::abs(wanted.durationMs - candidate.durationMs) <= 3000;
}
} // namespace

std::string appleMusicLyricQuery(std::string_view language) {
    std::string query = "?extend=ttmlLocalizations";
    if (language.empty()) return query;
    query += "&l%5Blyrics%5D=" + util::urlEncode(language);
    // Same derivation as am-ttml-fetch: zh-Hans-CN -> zh-Hans.
    const auto dash = language.find('-');
    if (dash != std::string_view::npos) {
        const auto second = language.find('-', dash + 1);
        query += "&l%5Bscript%5D=" + util::urlEncode(language.substr(0, second == std::string_view::npos ? language.size() : second));
    }
    return query;
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
    } else if (at(&response, "attributes")) {
        add(response);
    }
    return result;
}
OnlineTrackVerifier parseAppleMusicTrackCandidate(std::string_view json) {
    const auto all = parseAppleMusicTrackCandidates(json);
    return all.empty() ? OnlineTrackVerifier{} : all.front();
}
bool appleMusicRecordingMatches(std::string_view title, std::string_view artist, std::string_view json, std::int64_t durationMs) {
    const auto response = parseJson(json);
    const auto* item = at(at(&response, "data"), std::size_t{0});
    if (!item) item = &response;
    const auto* attrs = at(item, "attributes");
    return attrs && candidateMatches(*attrs, title, artist, durationMs);
}
bool isLocalizedTtml(std::string_view ttml) {
    return ttml.find("type=\"replacement\"") != std::string_view::npos ||
           ttml.find("type=\"translation\"") != std::string_view::npos ||
           ttml.find("type=\"subtitle\"") != std::string_view::npos ||
           ttml.find("type=\"transliteration\"") != std::string_view::npos;
}

bool containsTargetLanguage(std::string_view ttml, std::string_view language) {
    if (language.empty()) return true;
    std::string primary(language);
    const auto dash = primary.find('-');
    if (dash != std::string::npos) primary.resize(dash);
    std::string lower(ttml);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const auto marker = std::string("xml:lang=\"") + lowerAscii(primary);
    return lower.find(marker) != std::string::npos;
}
AppleMusicLyricsClient::AppleMusicLyricsClient(http::HttpClient client) : client_(std::move(client)) {}

AppleMusicFetchResult AppleMusicLyricsClient::fetch(std::string_view keywordUtf8, const OnlineTrackVerifier& track,
                                                    const AppleMusicTokenSettings& settings) const {
    AppleMusicFetchResult result;
    for (const auto language : kAppleMusicLyricsLanguages) {
        result = fetch(keywordUtf8, track, settings, language);
        if (result.status == AppleMusicStatus::Fetched || result.status == AppleMusicStatus::LineOnly) return result;
        if (result.status == AppleMusicStatus::AuthRequired || result.status == AppleMusicStatus::AuthRejected ||
            result.status == AppleMusicStatus::MalformedResponse) {
            return result;
        }
    }
    return result;
}
AppleMusicFetchResult AppleMusicLyricsClient::fetch(std::string_view keywordUtf8, const OnlineTrackVerifier& track, const AppleMusicTokenSettings& settings, std::string_view fixedLanguage) const {
    AppleMusicFetchResult result;
    const auto developerToken = tokenBearer(settings.developerToken);
    const auto mediaUserToken = trim(settings.mediaUserToken);
    if (developerToken.empty() || mediaUserToken.empty()) {
        result.status = AppleMusicStatus::AuthRequired;
        result.diagnostic = "Apple Music 官方歌词需要在设置中填写开发者 Token 和 Media-User-Token";
        return result;
    }
    const auto apiHeaders = [&] {
        auto headers = browserHeaders();
        headers.emplace_back("Authorization", "Bearer " + developerToken);
        headers.emplace_back("Media-User-Token", mediaUserToken);
        return headers;
    };
    const auto get = [&](std::string_view path) { return client_.get(std::string(kAppleMusicApi) + std::string(path), apiHeaders()); };
    // 用户填写地区时使用该值；否则从当前账号读取真实曲库地区。
    // 不按公网 IP 或 cn 猜测，以免账号曲库中的 catalog ID 返回 404。
    auto storefront = storefrontName(settings.storefront);
    if (trim(settings.storefront).empty()) {
        const auto account = get("/me/storefront");
        if (authFailure(account.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "无法读取 Apple Music 账号地区，凭证无效或已过期 (HTTP " + std::to_string(account.statusCode) + ")";
            return result;
        }
        const auto detected = parseAppleMusicAccountStorefront(account.text());
        if (!detected) {
            result.diagnostic = "Apple Music 未返回有效账号地区，请在设置中填写地区";
            return result;
        }
        storefront = *detected;
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
    // 若从内部播放对象获得 ISRC，先按 ISRC 精确反查；catalog ID 是公开 WinMD 能可靠取得的精确身份。
    const bool bridgeIsrc = !wanted.catalogId.empty() && !wanted.isrc.empty();
    // 专用模式传入 catalog ID，直接查询当前账号地区中的同一曲目。
    if (!wanted.catalogId.empty() && !bridgeIsrc) {
        const auto detail = get("/catalog/" + storefront + "/songs/" + wanted.catalogId);
        if (authFailure(detail.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "Apple Music 凭证被拒绝 (HTTP " + std::to_string(detail.statusCode) + ")";
            return result;
        }
        if (detail) {
            selectedId = normalizedCatalogId(firstId(detail.text())).value_or(wanted.catalogId);
            candidates = parseAppleMusicTrackCandidates(detail.text());
        }
    }
    // ISRC 跨地区共享；若有 ISRC，优先用它精确对应同一录音。
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
    // 普通 SMTC 才按名称搜索；专用模式已知身份时，不用同名候选顶替。
    if (wanted.catalogId.empty() && wanted.isrc.empty()) {
        const auto search = get("/catalog/" + storefront + "/search?term=" + util::urlEncode(keywordUtf8) + "&types=songs&limit=25");
        if (authFailure(search.statusCode)) {
            result.status = AppleMusicStatus::AuthRejected;
            result.diagnostic = "Apple Music 凭证被拒绝 (HTTP " + std::to_string(search.statusCode) + ")";
            return result;
        }
        candidates = parseAppleMusicTrackCandidates(search.text());
    }
    if (!candidates.empty()) {
        std::erase_if(candidates, [&](const OnlineTrackVerifier& item) { return !verifiedCandidate(item, wanted); });
        if (!candidates.empty()) selectedId = candidates.front().catalogId;
    }
    if (selectedId.empty()) {
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "Apple Music 未找到经歌名、艺人及版本校验后一致的曲目";
        return result;
    }
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
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "该曲目在当前 Apple Music 地区没有逐字歌词 (HTTP 404)";
        return result;
    }
    if (!lyrics) {
        result.diagnostic = "Apple Music 歌词接口请求失败 (HTTP " + std::to_string(lyrics.statusCode) + ")";
        return result;
    }
    result.ttml = pickTtml(parseJson(lyrics.text()), fixedLanguage);
    if (result.ttml.empty() && !fixedLanguage.empty()) {
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "该曲目在此 Apple Music 地区没有请求语言的翻译";
        return result;
    }
    if (fixedLanguage.empty() && !isLocalizedTtml(result.ttml)) {
        // 原文不能冒充翻译；用户要求的是中文翻译，没有对应语言就继续下一源。
        result.ttml.clear();
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "Apple Music 只返回了原文歌词，没有简体中文翻译";
        return result;
    }
    if (fixedLanguage.empty() && !containsTargetLanguage(result.ttml, settings.preferredLyricsLanguage)) {
        result.ttml.clear();
        result.status = AppleMusicStatus::NoMatch;
        result.diagnostic = "Apple Music 没有返回简体中文歌词";
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
    result.status = TtmlParser::hasSyllableTiming(result.ttml) ? AppleMusicStatus::Fetched : AppleMusicStatus::LineOnly;
    return result;
}
}
