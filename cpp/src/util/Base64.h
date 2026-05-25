#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::util {

// 标准 Base64 编码/解码，用于缓存 key 和部分歌词接口返回体。
std::string base64Encode(std::string_view text);
std::string base64Encode(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> base64Decode(std::string_view text);
std::string base64DecodeToString(std::string_view text);

}
