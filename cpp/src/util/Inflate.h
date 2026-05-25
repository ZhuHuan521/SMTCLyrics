#pragma once

#include <cstdint>
#include <vector>

namespace smtc::util {

// 解压 zlib 包装的 Deflate 数据；失败时返回空数组。
std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& bytes);

}
