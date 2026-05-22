#pragma once

#include "config/Config.h"

#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>

#include <string>

namespace smtc::ui {

class DesktopLyricsWindow {
public:
    DesktopLyricsWindow();
    ~DesktopLyricsWindow();

    bool create(const config::WindowConfig& windowConfig);
    void destroy();
    void applyConfig(const config::AppConfig& config);
    void updateLyrics(std::wstring text, int highlightPercent = 0);
    void setDraggable(bool draggable);
    void move(int left, int top, int width, int height);
    config::WindowConfig geometry() const;

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void redraw();
    Gdiplus::Color colorFromColorRef(COLORREF color, BYTE alpha = 255) const;

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool draggable_ = true;
    std::wstring text_;
    int highlightPercent_ = 0;
    config::AppConfig config_;
    ULONG_PTR gdiplusToken_ = 0;
};

}
