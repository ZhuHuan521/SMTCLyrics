#include "lyrics/OnlineLyrics.h"

#include "json.hpp"
#include "util/Base64.h"
#include "util/Encoding.h"
#include "util/Inflate.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace smtc::lyrics {
namespace {

std::vector<http::HttpClient::Header> browserHeaders() {
    return {
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"},
    };
}

std::vector<http::HttpClient::Header> qqHeaders() {
    auto headers = browserHeaders();
    headers.emplace_back("Referer", "https://y.qq.com/");
    return headers;
}

nlohmann::json parseJson(const std::vector<std::uint8_t>& body) {
    try {
        return nlohmann::json::parse(std::string(body.begin(), body.end()));
    } catch (...) {
        return {};
    }
}

std::string jsonString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) return std::to_string(value.get<double>());
    return {};
}

std::vector<std::uint8_t> toBytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> decryptKrc(const std::vector<std::uint8_t>& bytes) {
    constexpr std::uint8_t kHeader[] = {'k', 'r', 'c', '1'};
    constexpr std::uint8_t kKey[] = {64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105};
    if (bytes.size() <= sizeof(kHeader) || std::memcmp(bytes.data(), kHeader, sizeof(kHeader)) != 0) {
        return {};
    }

    std::vector<std::uint8_t> zlibBytes(bytes.begin() + sizeof(kHeader), bytes.end());
    for (std::size_t i = 0; i < zlibBytes.size(); ++i) {
        zlibBytes[i] ^= kKey[i % std::size(kKey)];
    }

    return util::inflateZlib(zlibBytes);
}

std::string formatLrcTimestamp(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto totalMs = static_cast<long long>(seconds * 1000.0 + 0.5);
    const auto minutes = totalMs / 60000;
    const auto sec = (totalMs % 60000) / 1000;
    const auto ms = totalMs % 1000;
    std::ostringstream out;
    out << '[' << std::setw(2) << std::setfill('0') << minutes << ':'
        << std::setw(2) << std::setfill('0') << sec << '.'
        << std::setw(3) << std::setfill('0') << ms << ']';
    return out.str();
}

std::string makeUuidV4() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);
    unsigned char bytes[16]{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(dis(gen));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

}

std::string sourceName(LyricSource source) {
    switch (source) {
    case LyricSource::Local: return "local";
    case LyricSource::QQ: return "qq";
    case LyricSource::Kugou: return "kg";
    case LyricSource::Kuwo: return "kuwo";
    case LyricSource::Netease: return "wy";
    }
    return "unknown";
}

std::optional<LyricSource> sourceFromIndex(int index) {
    switch (index) {
    case 0: return LyricSource::Local;
    case 1: return LyricSource::QQ;
    case 2: return LyricSource::Kugou;
    case 3: return LyricSource::Kuwo;
    case 4: return LyricSource::Netease;
    default: return std::nullopt;
    }
}

OnlineLyrics::OnlineLyrics(http::HttpClient client) : client_(std::move(client)) {}

std::vector<std::uint8_t> OnlineLyrics::fetch(LyricSource source, std::string_view keywordUtf8) const {
    switch (source) {
    case LyricSource::QQ: return fetchQQ(keywordUtf8);
    case LyricSource::Kugou: return fetchKugou(keywordUtf8);
    case LyricSource::Kuwo: return fetchKuwo(keywordUtf8);
    case LyricSource::Netease: return fetchNetease(keywordUtf8);
    case LyricSource::Local: break;
    }
    return {};
}

std::vector<std::uint8_t> OnlineLyrics::fetchQQ(std::string_view keywordUtf8) const {
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto searchUrl = "https://shc.y.qq.com/soso/fcgi-bin/search_for_qq_cp?_=1657641526460&g_tk=1037878909&format=json&inCharset=utf-8&outCharset=utf-8&notice=0&platform=h5&needNewCode=1&w=" + encoded + "&zhidaqu=1&catZhida=1&t=0&flag=1&ie=utf-8&sem=1&aggr=0&perpage=20&n=20&p=1&remoteplace=txt.mqq.all";
    const auto search = parseJson(client_.get(searchUrl, qqHeaders()).body);
    if (!search.contains("data")) return {};
    const auto list = search["data"]["song"]["list"];
    if (!list.is_array() || list.empty()) return {};
    auto songmid = jsonString(list[0]["songmid"]);
    if (songmid.empty() || songmid == "0") {
        songmid = jsonString(list[0]["mid"]);
    }
    if (songmid.empty() || songmid == "0") return {};

    std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string loginUin;
    for (int i = 0; i < 10; ++i) loginUin += static_cast<char>('0' + digit(rd));

    const auto lyricUrl = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&loginUin=" + loginUin + "&songmid=" + songmid;
    const auto lyricJson = parseJson(client_.get(lyricUrl, qqHeaders()).body);
    const auto lyricBase64 = jsonString(lyricJson["lyric"]);
    if (lyricBase64.empty()) return {};
    auto decoded = util::base64DecodeToString(lyricBase64);
    decoded = util::htmlDecodeUtf8(decoded);
    return toBytes(decoded);
}

std::vector<std::uint8_t> OnlineLyrics::fetchKugou(std::string_view keywordUtf8) const {
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto headers = browserHeaders();
    const auto searchUrl = "http://ioscdn.kugou.com/api/v3/search/song?keyword=" + encoded + "&page=1&pagesize=40&showtype=10&plat=2&version=7910&tag=1&correct=1&privilege=1&sver=5";
    const auto search = parseJson(client_.get(searchUrl, headers).body);
    const auto info = search["data"]["info"];
    if (!info.is_array() || info.empty()) return {};
    const auto hash = jsonString(info[0]["hash"]);
    std::string albumAudioId = jsonString(info[0]["album_audio_id"]);
    if (albumAudioId.empty() && info[0].contains("group") && info[0]["group"].is_array() && !info[0]["group"].empty()) {
        albumAudioId = jsonString(info[0]["group"][0]["album_audio_id"]);
    }
    if (hash.empty()) return {};

    const auto candidateUrl = "http://krcs.kugou.com/search?ver=1&man=no&client=pc&keyword=" + encoded + "&duration=139039&hash=" + hash + "&album_audio_id=" + albumAudioId + "&lrctxt=1";
    const auto candidates = parseJson(client_.get(candidateUrl, headers).body);
    const auto list = candidates["candidates"];
    if (!list.is_array() || list.empty()) return {};
    const auto id = jsonString(list[0]["id"]);
    const auto accessKey = jsonString(list[0]["accesskey"]);
    if (id.empty() || accessKey.empty()) return {};

    const auto krcUrl = "http://lyrics2.kugou.com/download?accesskey=" + accessKey + "&charset=utf8&client=pc&fmt=krc&id=" + id + "&ver=1";
    const auto krcDownload = parseJson(client_.get(krcUrl, headers).body);
    const auto krcContent = jsonString(krcDownload["content"]);
    if (!krcContent.empty()) {
        const auto decoded = util::base64Decode(krcContent);
        if (auto decrypted = decryptKrc(decoded); !decrypted.empty()) {
            return decrypted;
        }
    }

    const auto lrcUrl = "http://lyrics2.kugou.com/download?accesskey=" + accessKey + "&charset=utf8&client=pc&fmt=lrc&id=" + id + "&ver=1";
    const auto lrcDownload = parseJson(client_.get(lrcUrl, headers).body);
    const auto lrcContent = jsonString(lrcDownload["content"]);
    return lrcContent.empty() ? std::vector<std::uint8_t>{} : util::base64Decode(lrcContent);
}

std::vector<std::uint8_t> OnlineLyrics::fetchKuwo(std::string_view keywordUtf8) const {
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto headers = browserHeaders();
    const auto searchUrl = "https://kuwo.cn/search/searchMusicBykeyWord?vipver=1&client=kt&ft=music&cluster=0&strategy=2012&encoding=utf8&rformat=json&mobi=1&issubtitle=1&show_copyright_off=1&pn=0&rn=20&all=" + encoded;
    const auto search = parseJson(client_.get(searchUrl, headers).body);
    const auto list = search["abslist"];
    if (!list.is_array() || list.empty()) return {};
    const auto musicId = jsonString(list[0]["DC_TARGETID"]);
    if (musicId.empty()) return {};

    const auto lyricUrl = "https://kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + musicId + "&httpsStatus=1&reqId=" + makeUuidV4() + "&plat=web_www&from=";
    const auto lyric = parseJson(client_.get(lyricUrl, headers).body);
    const auto lrcList = lyric["data"]["lrclist"];
    if (!lrcList.is_array() || lrcList.empty()) return {};

    std::string out;
    for (const auto& item : lrcList) {
        const auto line = jsonString(item["lineLyric"]);
        const auto timeText = jsonString(item["time"]);
        if (timeText.empty()) continue;
        try {
            out += formatLrcTimestamp(std::stod(timeText));
            out += line;
            out += "\n";
        } catch (...) {
        }
    }
    return toBytes(out);
}

std::vector<std::uint8_t> OnlineLyrics::fetchNetease(std::string_view keywordUtf8) const {
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto headers = browserHeaders();
    const auto searchUrl = "https://music.163.com/api/search/get/web?csrf_token=&hlpretag=&hlposttag=&s=" + encoded + "&type=1&offset=0&total=true&limit=10";
    const auto search = parseJson(client_.get(searchUrl, headers).body);
    const auto songs = search["result"]["songs"];
    if (!songs.is_array() || songs.empty()) return {};
    const auto id = jsonString(songs[0]["id"]);
    if (id.empty()) return {};

    const auto lyricUrl = "https://music.163.com/api/song/lyric?id=" + id + "&lv=-1&kv=-1&tv=-1";
    const auto lyric = parseJson(client_.get(lyricUrl, headers).body);
    const auto lrc = jsonString(lyric["lrc"]["lyric"]);
    return lrc.empty() ? std::vector<std::uint8_t>{} : toBytes(lrc);
}

}
