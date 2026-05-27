#pragma once

#include "config/Config.h"

#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::ui {

// 透明置顶桌面歌词窗口，使用 GDI+ 绘制描边、渐变和逐字高亮。
class DesktopLyricsWindow {
public:
    DesktopLyricsWindow();
    ~DesktopLyricsWindow();

    // 创建/销毁分层窗口。
    bool create(const config::WindowConfig& windowConfig);
    void destroy();
    // 应用样式配置并刷新绘制。
    void applyConfig(const config::AppConfig& config);
    // 更新歌词文本与高亮状态。
    void updateLyrics(std::wstring_view text, int highlightPercent = 0, int highlightLine = 0);
    // 控制是否允许鼠标拖动歌词窗口。
    void setDraggable(bool draggable);
    void setGeometryChangedCallback(std::function<void(const config::WindowConfig&)> callback);
    void move(int left, int top, int width, int height);
    config::WindowConfig geometry() const;

    HWND hwnd() const { return hwnd_; }

private:
    // Win32 消息分发。
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    // GDI+ 双缓冲绘制和透明窗口更新。
    void redraw();
    bool ensureBackBuffer(HDC referenceDc);
    void releaseBackBuffer();
    Gdiplus::FontFamily* fontFamily();
    void invalidateLayout();
    bool rebuildLayoutCache();
    void notifyGeometryChanged() const;
    Gdiplus::Color colorFromColorRef(COLORREF color, BYTE alpha = 255) const;

    // 窗口几何、当前歌词帧和 GDI/GDI+ 资源。
    struct CachedLine {
        std::unique_ptr<Gdiplus::GraphicsPath> shadowPath;
        std::unique_ptr<Gdiplus::GraphicsPath> textPath;
        Gdiplus::RectF bounds;
    };

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool draggable_ = true;
    std::wstring text_;
    int highlightPercent_ = 0;
    int highlightLine_ = 0;
    config::AppConfig config_;
    std::function<void(const config::WindowConfig&)> geometryChanged_;
    ULONG_PTR gdiplusToken_ = 0;
    HDC memoryDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HGDIOBJ oldBackBufferBitmap_ = nullptr;
    void* backBufferBits_ = nullptr;
    int backBufferWidth_ = 0;
    int backBufferHeight_ = 0;
    std::unique_ptr<Gdiplus::FontFamily> fontFamily_;
    std::wstring cachedFontRequest_;
    std::vector<CachedLine> layoutCache_;
    bool layoutDirty_ = true;
};

}
