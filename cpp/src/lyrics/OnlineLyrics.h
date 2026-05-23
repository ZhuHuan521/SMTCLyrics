#pragma once

#include "http/HttpClient.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::lyrics {

enum class LyricSource {
    Local = 0,
    QQ = 1,
    Kugou = 2,
    Kuwo = 3,
    Netease = 4,
};

std::string sourceName(LyricSource source);
std::optional<LyricSource> sourceFromIndex(int index);

class OnlineLyrics {
public:
    OnlineLyrics() = default;
    explicit OnlineLyrics(http::HttpClient client);

    std::vector<std::uint8_t> fetch(LyricSource source, std::string_view keywordUtf8) const;

private:
    std::vector<std::uint8_t> fetchQQ(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchKugou(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchKuwo(std::string_view keywordUtf8) const;
    std::vector<std::uint8_t> fetchNetease(std::string_view keywordUtf8) const;

    http::HttpClient client_;
};

}
