#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smtc::util {

std::string base64Encode(std::string_view text);
std::string base64Encode(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> base64Decode(std::string_view text);
std::string base64DecodeToString(std::string_view text);

}
