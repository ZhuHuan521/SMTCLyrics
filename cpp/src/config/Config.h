#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace smtc::config {

// 字体配置，对应歌词窗口绘制时创建的 GDI+ 字体。
struct FontConfig {
    std::wstring name = L"楷体";
    int size = 35;
    bool bold = true;
    bool italic = false;
    bool underline = false;
};

// 一组文字颜色：起始色、结束色、描边色和渐变模式。
struct TextStyle {
    COLORREF color1 = RGB(255, 0, 0);
    COLORREF color2 = RGB(255, 255, 0);
    COLORREF border = RGB(0, 0, 0);
    int gradientMode = 1;
};

// 歌词窗口几何信息；hasPosition=false 时由程序按工作区自动摆放。
struct WindowConfig {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 150;
    bool hasPosition = false;
};

// 应用完整配置，会被控制窗口编辑并写入 config.ini。
struct AppConfig {
    FontConfig font;
    TextStyle normal;
    TextStyle highlight{RGB(0, 128, 255), RGB(0, 255, 255), RGB(0, 0, 0), 1};
    TextStyle highlight2{RGB(128, 128, 128), RGB(192, 192, 192), RGB(0, 0, 0), 1};  // 两句显示时第二行的颜色。
    WindowConfig window;
    int lyricOffsetMs = 0;
    std::vector<int> sourcePriority{1, 2, 3, 4};
    int smtcMode = 1;
    int smtcPollIntervalMs = 1000;
    int displayMode = 1;
};

// INI 配置读写器，同时兼容旧版中文 section/key 和新版英文 section/key。
class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path);

    // 读取、完整保存，以及针对高频变更字段的局部保存。
    AppConfig load() const;
    void save(const AppConfig& config) const;
    void saveWindow(const WindowConfig& window) const;
    void saveDisplayMode(int mode) const;
    void saveSmtcMode(int mode) const;

    const std::filesystem::path& path() const { return path_; }

private:
    // 下面的 read*Any 会先读新版键名，缺失时再读旧版键名。
    std::wstring readString(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const;
    std::wstring readStringAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, std::wstring_view fallback) const;
    COLORREF readColorAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, COLORREF fallback) const;
    int readSourcePriority(std::wstring_view key, std::wstring_view legacyKey, int fallback) const;
    int readInt(std::wstring_view section, std::wstring_view key, int fallback) const;
    int readIntAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, int fallback) const;
    bool readBool(std::wstring_view section, std::wstring_view key, bool fallback) const;
    bool readBoolAny(std::wstring_view section, std::wstring_view key, std::wstring_view legacySection, std::wstring_view legacyKey, bool fallback) const;
    void writeString(std::wstring_view section, std::wstring_view key, std::wstring_view value) const;
    void deleteKey(std::wstring_view section, std::wstring_view key) const;
    void deleteSection(std::wstring_view section) const;

    std::filesystem::path path_;
};

}
