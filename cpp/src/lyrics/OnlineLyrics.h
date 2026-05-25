#pragma once

#include "http/HttpClient.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::lyrics {

// 歌词来源枚举：数值与配置中的优先级索引保持一致。
enum class LyricSource {
    Local = 0,
    QQ = 1,
    Kugou = 2,
    Kuwo = 3,
    Netease = 4,
};

// 枚举与配置/缓存中保存的字符串或数字之间的转换。
std::string sourceName(LyricSource source);
std::optional<LyricSource> sourceFromIndex(int index);

// 在线歌词抓取器：封装 QQ、酷狗、酷我、网易云各自的接口和解密/转换逻辑。
class OnlineLyrics {
public:
    OnlineLyrics() = default;
    explicit OnlineLyrics(http::HttpClient client);

    // 统一入口，返回可交给 LrcParser 解析的 UTF-8 歌词字节。
    std::vector<std::uint8_t> fetch(LyricSource source, std::string_view keywordUtf8) const;

private:
    std::vector<std::uint8_t> fetchQQ(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchKugou(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchKuwo(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchNetease(std::string_view keywordUtf8) const;

    http::HttpClient client_;
};

}
