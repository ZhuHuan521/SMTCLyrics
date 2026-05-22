#pragma once

#include "config/Config.h"

#include <windows.h>

#include <functional>
#include <string>

namespace smtc::ui {

struct ControlWindowCallbacks {
    std::function<void(const config::AppConfig&)> applyConfig;
    std::function<config::WindowConfig()> getLyricGeometry;
    std::function<void(const config::WindowConfig&)> moveLyricWindow;
    std::function<void(bool)> setLyricDraggable;
    std::function<void()> reloadLyrics;
    std::function<void()> switchSource;
    std::function<void()> clearCache;
    std::function<void()> openLocalLyric;
};

class ControlWindow {
public:
    ControlWindow();
    ~ControlWindow();

    bool create(const config::AppConfig& config, ControlWindowCallbacks callbacks);
    void show(int command = SW_SHOW);
    void setConfig(const config::AppConfig& config);
    void setStatusText(std::wstring text);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void createControls();
    void populateControls();
    void applyFontAndLyricsSettings();
    void applyColorSettings();
    void applySmtcSettings();
    void applyDisplaySettings();
    void applySourceSettings();
    void readLyricGeometry();
    void saveLyricGeometry();
    void toggleLock();
    config::AppConfig readConfigFromControls() const;

    HWND addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD exStyle = 0);
    HWND addLabel(const wchar_t* text, int x, int y, int width, int height, int id = 0);
    HWND addEdit(const std::wstring& text, int x, int y, int width, int height, int id);
    HWND addButton(const wchar_t* text, int x, int y, int width, int height, int id);
    HWND addCheckBox(const wchar_t* text, int x, int y, int width, int height, int id);
    HWND addRadio(const wchar_t* text, int x, int y, int width, int height, int id, bool firstInGroup = false);
    HWND addCombo(int x, int y, int width, int height, int id);
    void setText(int id, const std::wstring& text) const;
    std::wstring getText(int id) const;
    int getInt(int id, int fallback) const;
    void setChecked(int id, bool checked) const;
    bool isChecked(int id) const;
    int comboSelection(int id) const;
    void setComboSelection(int id, int index) const;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    config::AppConfig config_;
    ControlWindowCallbacks callbacks_;
    bool lyricDraggable_ = true;
};

}
