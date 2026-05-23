#pragma once

#include <cstdint>
#include <vector>

namespace smtc::util {

std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& bytes);

}
