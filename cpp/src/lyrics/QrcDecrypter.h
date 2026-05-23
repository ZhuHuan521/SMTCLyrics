#pragma once

#include <string>
#include <string_view>

namespace smtc::lyrics {

std::string decryptQrc(std::string_view encryptedHex);

}
