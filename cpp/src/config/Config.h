#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace smtc::config {

struct FontConfig {
    std::wstring name = L"楷体";
    int size = 35;
    bool bold = true;
    bool italic = false;
    bool underline = false;
};

struct TextStyle {
    COLORREF color1 = RGB(255, 0, 0);
    COLORREF color2 = RGB(255, 255, 0);
    COLORREF border = RGB(0, 0, 0);
    int gradientMode = 1;
};

struct WindowConfig {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 150;
    bool hasPosition = false;
};

struct AppConfig {
    FontConfig font;
    TextStyle normal;
    TextStyle highlight{RGB(0, 128, 255), RGB(0, 255, 255), RGB(0, 0, 0), 1};
    WindowConfig window;
    std::wstring cookie;
    int lyricOffsetSeconds = 0;
    std::vector<int> sourcePriority{1, 2, 3, 4};
    int smtcMode = 1;
    int displayMode = 1;
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path);

    AppConfig load() const;
    void save(const AppConfig& config) const;
    void saveWindow(const WindowConfig& window) const;
    void saveDisplayMode(int mode) const;
    void saveSmtcMode(int mode) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::wstring readString(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const;
    int readInt(std::wstring_view section, std::wstring_view key, int fallback) const;
    bool readBool(std::wstring_view section, std::wstring_view key, bool fallback) const;
    void writeString(std::wstring_view section, std::wstring_view key, std::wstring_view value) const;

    std::filesystem::path path_;
};

}
