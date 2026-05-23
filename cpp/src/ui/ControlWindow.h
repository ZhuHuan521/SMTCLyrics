#pragma once

#include "config/Config.h"

#include <windows.h>

#include <array>
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
    std::function<std::array<bool, 4>()> checkLyricSources;
};

class ControlWindow {
public:
    ControlWindow();
    ~ControlWindow();

    bool create(const config::AppConfig& config, ControlWindowCallbacks callbacks);
    void show(int command = SW_SHOW);
    void setConfig(const config::AppConfig& config);
    void syncLyricGeometry(const config::WindowConfig& window);
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
    void saveLyricGeometry();
    void toggleLock();
    void startSourceCheck();
    void completeSourceCheck(const std::array<bool, 4>& status);
    void chooseColor(int id);
    void updateLockControls();
    void updateColorSwatches() const;
    bool drawColorButton(const DRAWITEMSTRUCT& item) const;
    config::AppConfig readConfigFromControls() const;

    HWND addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD exStyle = 0);
    HWND addLabel(const wchar_t* text, int x, int y, int width, int height, int id = 0);
    HWND addValueLabel(const wchar_t* text, int x, int y, int width, int height, int id = 0);
    HWND addEdit(const std::wstring& text, int x, int y, int width, int height, int id);
    HWND addButton(const wchar_t* text, int x, int y, int width, int height, int id);
    HWND addColorButton(int x, int y, int width, int height, int id);
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
    COLORREF colorForButton(int id) const;
    void setColorForButton(int id, COLORREF color);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    bool ownsFont_ = false;
    config::AppConfig config_;
    ControlWindowCallbacks callbacks_;
    COLORREF customColors_[16]{};
    bool lyricDraggable_ = false;
    bool sourceCheckRunning_ = false;
};

}
