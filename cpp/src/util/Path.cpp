#include "util/Path.h"

#include <windows.h>

#include <fstream>
#include <vector>

namespace smtc::util {

std::filesystem::path executablePath() {
    // GetModuleFileNameW 的缓冲区可能不够，循环扩容直到完整取到路径。
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = 0;
    for (;;) {
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return {};
        if (size < buffer.size() - 1) break;
        buffer.resize(buffer.size() * 2);
    }
    buffer.resize(size);
    return buffer;
}

std::filesystem::path executableDirectory() {
    // 配置、缓存和 lyrics 默认都放在可执行文件目录旁。
    return executablePath().parent_path();
}

std::filesystem::path tempDirectory() {
    // 优先使用 Win32 临时目录，失败时退回标准库实现。
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (size == 0) return std::filesystem::temp_directory_path();
    buffer.resize(size);
    return buffer;
}

bool fileExists(const std::filesystem::path& path) {
    // 使用 error_code 避免权限或路径异常直接抛出。
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool directoryExists(const std::filesystem::path& path) {
    // 使用 error_code 避免权限或路径异常直接抛出。
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

bool copyFileIfDifferent(const std::filesystem::path& from, const std::filesystem::path& to) {
    // 只按文件大小判断是否需要复制，满足打包资源的轻量同步需求。
    std::error_code ec;
    if (!std::filesystem::exists(from, ec)) return false;
    if (std::filesystem::exists(to, ec)) {
        const auto fromSize = std::filesystem::file_size(from, ec);
        const auto toSize = std::filesystem::file_size(to, ec);
        if (!ec && fromSize == toSize) return true;
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

std::wstring quoteForCommandLine(const std::filesystem::path& path) {
    // ShellExecuteW 调用 notepad 时需要把带空格路径整体加引号。
    std::wstring text = path.wstring();
    std::wstring escaped;
    escaped.reserve(text.size() + 2);
    escaped.push_back(L'\"');
    for (wchar_t ch : text) {
        if (ch == L'\"') escaped.push_back(L'\\');
        escaped.push_back(ch);
    }
    escaped.push_back(L'\"');
    return escaped;
}

}
