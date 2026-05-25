#pragma once

#include <filesystem>
#include <string>

namespace smtc::util {

// 可执行文件、临时目录、文件存在性和命令行引用相关工具。
std::filesystem::path executablePath();
std::filesystem::path executableDirectory();
std::filesystem::path tempDirectory();
bool fileExists(const std::filesystem::path& path);
bool directoryExists(const std::filesystem::path& path);
bool copyFileIfDifferent(const std::filesystem::path& from, const std::filesystem::path& to);
std::wstring quoteForCommandLine(const std::filesystem::path& path);

}
