#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http/HttpClient.h"

namespace smtc::lyrics {

// 官方歌词必须通过 catalog 中的真实曲目身份取得；这类标识
// 不允许仅凭相近歌名猜出，失败时由 LyricRepository 继续尝试下一源。
struct OnlineTrackVerifier {
    std::string catalogId;
    std::string isrc;
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t durationMs = 0;
};

enum class AppleMusicStatus {
    Fetched,
    Unavailable,
    AuthRequired,
    AuthRejected,
    NoMatch,
    LineOnly,
    MalformedResponse,
};

struct AppleMusicTokenSettings {
    std::string developerToken;
    std::string mediaUserToken;
    std::string storefront;
};

struct AppleMusicFetchResult {
    std::string ttml;
    std::string diagnostic;
    AppleMusicStatus status = AppleMusicStatus::Unavailable;
    // 地区留空时由账号接口自动读取的值；仅供当前运行使用，不自动写配置。
    std::string detectedStorefront;
};

// 返回规范化 catalog ID；纯数字 ID 才能用于官方歌词路径。
std::optional<std::string> normalizedCatalogId(std::string_view value);

// 将 Apple Music 搜索/详情响应的 JSON 转为可验证的曲目候选。
std::vector<OnlineTrackVerifier> parseAppleMusicTrackCandidates(std::string_view json);
OnlineTrackVerifier parseAppleMusicTrackCandidate(std::string_view json);
bool appleMusicRecordingMatches(std::string_view wantedTitle,
                                std::string_view wantedArtist,
                                std::string_view candidateJson,
                                std::int64_t wantedDurationMs);

// 从 /me/storefront 响应读取账号曲库地区，如 tr、cn、us。
std::optional<std::string> parseAppleMusicAccountStorefront(std::string_view json);
// 地区检测失败时返回可在状态栏直接显示的原因，不包含凭证内容。
std::string appleMusicStorefrontFailure(const AppleMusicFetchResult& result);

std::int64_t appleMusicTokenExpiry(std::string_view token);

// 使用 Apple Music 官方 amp-api 获取逐字 TTML。catalogId 是精确身份；
// 普通 SMTC 才允许搜索，并且必须通过 title、artist、时长等校验。
// Apple Music 歌词跟随系统语言：简中 -> 台湾繁中 -> 香港繁中 -> 原文。
constexpr std::string_view kAppleMusicLyricsLanguages[] = {
    "zh-Hans-CN",
    "zh-Hant-TW",
    "zh-Hant-HK",
    ""
};

std::string appleMusicLyricQuery(std::string_view language);

// 从 Apple Music 歌词响应中选取指定语言的 TTML；language 为空时返回原文。
std::string appleMusicTtmlForLanguage(std::string_view responseJson, std::string_view requestedLanguage);

class AppleMusicLyricsClient {
public:
    explicit AppleMusicLyricsClient(http::HttpClient client = {});

    AppleMusicFetchResult fetch(std::string_view keywordUtf8,
                                const OnlineTrackVerifier& track,
                                const AppleMusicTokenSettings& settings) const;
    AppleMusicFetchResult fetch(std::string_view keywordUtf8,
                                const OnlineTrackVerifier& track,
                                const AppleMusicTokenSettings& settings,
                                std::string_view fixedLanguage) const;
    // 使用当前输入的授权凭证读取账号地区，不访问歌词或曲目接口。
    AppleMusicFetchResult detectStorefront(const AppleMusicTokenSettings& settings) const;
    // 校验手动填写地区格式；空值表示让系统自动读取。
    static bool validStorefront(std::string_view storefront);

private:
    http::HttpClient client_;
};

}
