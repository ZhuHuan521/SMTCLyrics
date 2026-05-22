#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace smtc::util {

std::wstring utf8ToWide(std::string_view text);
std::string wideToUtf8(std::wstring_view text);
std::wstring ansiToWide(std::string_view text, unsigned codePage = 936);
std::string wideToAnsi(std::wstring_view text, unsigned codePage = 936);
std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);
bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
std::wstring readTextAuto(const std::filesystem::path& path);
std::string readUtf8Auto(const std::filesystem::path& path);
std::string urlEncode(std::string_view utf8);
std::string htmlDecodeUtf8(std::string_view utf8);
std::wstring trim(std::wstring_view text);
std::string trimUtf8(std::string_view text);
std::wstring replaceAll(std::wstring text, std::wstring_view from, std::wstring_view to);
std::string replaceAll(std::string text, std::string_view from, std::string_view to);

}
