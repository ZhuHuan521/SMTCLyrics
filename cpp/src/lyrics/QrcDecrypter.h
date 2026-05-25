#pragma once

#include <string>
#include <string_view>

namespace smtc::lyrics {

// 解密 QQ 音乐 QRC：输入为十六进制密文，输出为解压后的 XML/歌词文本。
std::string decryptQrc(std::string_view encryptedHex);

}
