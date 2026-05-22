#include "ui/DesktopLyricsWindow.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace smtc::ui {
namespace {

constexpr wchar_t kClassName[] = L"SMTCLyricsOverlayWindow";

Gdiplus::FontStyle fontStyleFromConfig(const config::FontConfig& font) {
    int style = Gdiplus::FontStyleRegular;
    if (font.bold) style |= Gdiplus::FontStyleBold;
    if (font.italic) style |= Gdiplus::FontStyleItalic;
    if (font.underline) style |= Gdiplus::FontStyleUnderline;
    return static_cast<Gdiplus::FontStyle>(style);
}

Gdiplus::Color toGdiColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

std::unique_ptr<Gdiplus::Brush> makeBrush(const config::TextStyle& style, const Gdiplus::RectF& rect) {
    if (style.gradientMode <= 0) {
        return std::make_unique<Gdiplus::SolidBrush>(toGdiColor(style.color1));
    }
    Gdiplus::PointF p1(rect.X, rect.Y);
    Gdiplus::PointF p2(rect.X, rect.Y + std::max(1.0f, rect.Height));
    return std::make_unique<Gdiplus::LinearGradientBrush>(p1, p2, toGdiColor(style.color1), toGdiColor(style.color2));
}

}

DesktopLyricsWindow::DesktopLyricsWindow() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplusToken_, &input, nullptr);
}

DesktopLyricsWindow::~DesktopLyricsWindow() {
    destroy();
    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
}

bool DesktopLyricsWindow::create(const config::WindowConfig& windowConfig) {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DesktopLyricsWindow::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    width_ = windowConfig.hasPosition ? windowConfig.width : workArea.right - workArea.left;
    height_ = windowConfig.hasPosition ? windowConfig.height : 150;
    const int left = windowConfig.hasPosition ? windowConfig.left : workArea.left + ((workArea.right - workArea.left) - width_) / 2;
    const int top = windowConfig.hasPosition ? windowConfig.top : workArea.bottom - height_;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName,
        kClassName,
        WS_POPUP | WS_VISIBLE,
        left,
        top,
        width_,
        height_,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) return false;
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    redraw();
    return true;
}

void DesktopLyricsWindow::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void DesktopLyricsWindow::applyConfig(const config::AppConfig& config) {
    config_ = config;
    redraw();
}

void DesktopLyricsWindow::updateLyrics(std::wstring text, int highlightPercent) {
    text_ = std::move(text);
    highlightPercent_ = std::clamp(highlightPercent, 0, 100);
    redraw();
}

void DesktopLyricsWindow::setDraggable(bool draggable) {
    draggable_ = draggable;
}

void DesktopLyricsWindow::move(int left, int top, int width, int height) {
    if (!hwnd_) return;
    width_ = width;
    height_ = height;
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_, height_, SWP_NOACTIVATE);
    redraw();
}

config::WindowConfig DesktopLyricsWindow::geometry() const {
    config::WindowConfig result;
    if (!hwnd_) return result;
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    result.left = rect.left;
    result.top = rect.top;
    result.width = rect.right - rect.left;
    result.height = rect.bottom - rect.top;
    result.hasPosition = true;
    return result;
}

LRESULT CALLBACK DesktopLyricsWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<DesktopLyricsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DesktopLyricsWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (self) {
        return self->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DesktopLyricsWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN:
        if (draggable_) {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        break;
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void DesktopLyricsWindow::redraw() {
    if (!hwnd_ || width_ <= 0 || height_ <= 0) return;

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width_;
    bitmapInfo.bmiHeader.biHeight = -height_;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    if (bits) {
        std::memset(bits, 0, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    }

    {
        Gdiplus::Graphics graphics(memoryDc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (!text_.empty()) {
            auto family = std::make_unique<Gdiplus::FontFamily>(config_.font.name.c_str());
            if (family->GetLastStatus() != Gdiplus::Ok) {
                family = std::make_unique<Gdiplus::FontFamily>(L"Microsoft YaHei");
            }
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            Gdiplus::RectF layout(0.0f, 0.0f, static_cast<Gdiplus::REAL>(width_), static_cast<Gdiplus::REAL>(height_));
            Gdiplus::GraphicsPath shadowPath;
            Gdiplus::RectF shadowLayout(2.0f, 2.0f, static_cast<Gdiplus::REAL>(width_), static_cast<Gdiplus::REAL>(height_));
            shadowPath.AddString(text_.c_str(), -1, family.get(), fontStyleFromConfig(config_.font), static_cast<Gdiplus::REAL>(config_.font.size), shadowLayout, &format);
            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(150, 0, 0, 0));
            graphics.FillPath(&shadowBrush, &shadowPath);

            Gdiplus::GraphicsPath textPath;
            textPath.AddString(text_.c_str(), -1, family.get(), fontStyleFromConfig(config_.font), static_cast<Gdiplus::REAL>(config_.font.size), layout, &format);
            Gdiplus::RectF bounds;
            textPath.GetBounds(&bounds);
            Gdiplus::Pen outline(colorFromColorRef(config_.normal.border), 1.0f);
            graphics.DrawPath(&outline, &textPath);
            auto brush = makeBrush(config_.normal, bounds);
            graphics.FillPath(brush.get(), &textPath);

            if (highlightPercent_ > 0) {
                Gdiplus::GraphicsState state = graphics.Save();
                Gdiplus::RectF clip(bounds.X, bounds.Y, bounds.Width * highlightPercent_ / 100.0f, bounds.Height);
                graphics.SetClip(clip);
                Gdiplus::Pen highlightOutline(colorFromColorRef(config_.highlight.border), 1.0f);
                graphics.DrawPath(&highlightOutline, &textPath);
                auto highlightBrush = makeBrush(config_.highlight, bounds);
                graphics.FillPath(highlightBrush.get(), &textPath);
                graphics.Restore(state);
            }
        }
    }

    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    POINT dst{rect.left, rect.top};
    POINT src{0, 0};
    SIZE size{width_, height_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screenDc, &dst, &size, memoryDc, &src, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}

Gdiplus::Color DesktopLyricsWindow::colorFromColorRef(COLORREF color, BYTE alpha) const {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

}
