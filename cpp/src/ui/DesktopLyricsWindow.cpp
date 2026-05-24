#include "ui/DesktopLyricsWindow.h"

#include <algorithm>
#include <array>
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

struct DisplayLines {
    std::array<std::wstring_view, 4> items{};
    std::size_t count = 0;
};

DisplayLines splitDisplayLines(std::wstring_view text) {
    DisplayLines lines;
    std::size_t start = 0;
    while (start <= text.size() && lines.count < lines.items.size()) {
        if (lines.count == lines.items.size() - 1) {
            lines.items[lines.count++] = text.substr(start);
            break;
        }
        const auto end = text.find(L'\n', start);
        if (end == std::wstring_view::npos) {
            lines.items[lines.count++] = text.substr(start);
            break;
        }
        lines.items[lines.count++] = text.substr(start, end - start);
        start = end + 1;
    }
    if (lines.count == 0) {
        lines.items[lines.count++] = text.substr(0, 0);
    }
    return lines;
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
        L"SMTC歌词 By:柱环",
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
    releaseBackBuffer();
}

void DesktopLyricsWindow::applyConfig(const config::AppConfig& config) {
    config_ = config;
    redraw();
}

void DesktopLyricsWindow::updateLyrics(std::wstring_view text, int highlightPercent, int highlightLine) {
    const auto clampedHighlight = std::clamp(highlightPercent, 0, 100);
    const auto clampedLine = std::max(0, highlightLine);
    if (text_ == text && highlightPercent_ == clampedHighlight && highlightLine_ == clampedLine) {
        return;
    }
    text_.assign(text.begin(), text.end());
    highlightPercent_ = clampedHighlight;
    highlightLine_ = clampedLine;
    redraw();
}

void DesktopLyricsWindow::setDraggable(bool draggable) {
    draggable_ = draggable;
}

void DesktopLyricsWindow::setGeometryChangedCallback(std::function<void(const config::WindowConfig&)> callback) {
    geometryChanged_ = std::move(callback);
}

void DesktopLyricsWindow::move(int left, int top, int width, int height) {
    if (!hwnd_) return;
    width_ = width;
    height_ = height;
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_, height_, SWP_NOACTIVATE);
    redraw();
    notifyGeometryChanged();
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
            notifyGeometryChanged();
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
    if (!screenDc) return;
    if (!ensureBackBuffer(screenDc)) {
        ReleaseDC(nullptr, screenDc);
        return;
    }

    if (backBufferBits_) {
        std::memset(backBufferBits_, 0, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    }

    {
        Gdiplus::Graphics graphics(memoryDc_);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (!text_.empty()) {
            auto* family = fontFamily();
            const auto style = fontStyleFromConfig(config_.font);
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            const auto lines = splitDisplayLines(text_);
            const int emHeight = family->GetEmHeight(style);
            const int lineSpacing = family->GetLineSpacing(style);
            const float fontSize = static_cast<float>(config_.font.size);
            const float lineHeight = emHeight > 0 ? fontSize * static_cast<float>(lineSpacing) / static_cast<float>(emHeight) : fontSize * 1.25f;
            const float totalHeight = lineHeight * static_cast<float>(lines.count);
            const float startY = (static_cast<float>(height_) - totalHeight) / 2.0f;
            const int activeLine = std::clamp(highlightLine_, 0, static_cast<int>(lines.count) - 1);

            for (std::size_t i = 0; i < lines.count; ++i) {
                const auto lineText = lines.items[i];
                const float y = startY + static_cast<float>(i) * lineHeight;
                Gdiplus::RectF layout(0.0f, y, static_cast<Gdiplus::REAL>(width_), lineHeight);

                // Determine which style to use for this line
                // In two-line mode: line 0 (active) uses highlight, line 1 (inactive) uses highlight2
                const bool isActiveLine = (static_cast<int>(i) == activeLine);
                const bool isSecondLine = (lines.count > 1 && i == 1 && activeLine == 0);
                const auto& lineNormalStyle = isSecondLine ? config_.highlight2 : config_.normal;
                const auto& lineBorderStyle = isSecondLine ? config_.highlight2.border : config_.normal.border;

                Gdiplus::GraphicsPath shadowPath;
                Gdiplus::RectF shadowLayout(2.0f, y + 2.0f, static_cast<Gdiplus::REAL>(width_), lineHeight);
                shadowPath.AddString(lineText.data(), static_cast<INT>(lineText.size()), family, style, fontSize, shadowLayout, &format);
                Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(150, 0, 0, 0));
                graphics.FillPath(&shadowBrush, &shadowPath);

                Gdiplus::GraphicsPath textPath;
                textPath.AddString(lineText.data(), static_cast<INT>(lineText.size()), family, style, fontSize, layout, &format);
                Gdiplus::RectF bounds;
                textPath.GetBounds(&bounds);
                Gdiplus::Pen outline(colorFromColorRef(lineBorderStyle), 1.0f);
                graphics.DrawPath(&outline, &textPath);
                auto brush = makeBrush(lineNormalStyle, bounds);
                graphics.FillPath(brush.get(), &textPath);

                if (highlightPercent_ > 0 && isActiveLine) {
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
    }

    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    POINT dst{rect.left, rect.top};
    POINT src{0, 0};
    SIZE size{width_, height_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screenDc, &dst, &size, memoryDc_, &src, 0, &blend, ULW_ALPHA);

    ReleaseDC(nullptr, screenDc);
}

bool DesktopLyricsWindow::ensureBackBuffer(HDC referenceDc) {
    if (memoryDc_ && backBufferBitmap_ && backBufferWidth_ == width_ && backBufferHeight_ == height_) {
        return true;
    }

    releaseBackBuffer();

    memoryDc_ = CreateCompatibleDC(referenceDc);
    if (!memoryDc_) return false;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width_;
    bitmapInfo.bmiHeader.biHeight = -height_;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    backBufferBitmap_ = CreateDIBSection(referenceDc, &bitmapInfo, DIB_RGB_COLORS, &backBufferBits_, nullptr, 0);
    if (!backBufferBitmap_) {
        releaseBackBuffer();
        return false;
    }

    oldBackBufferBitmap_ = SelectObject(memoryDc_, backBufferBitmap_);
    if (!oldBackBufferBitmap_) {
        releaseBackBuffer();
        return false;
    }

    backBufferWidth_ = width_;
    backBufferHeight_ = height_;
    return true;
}

void DesktopLyricsWindow::releaseBackBuffer() {
    if (memoryDc_ && oldBackBufferBitmap_) {
        SelectObject(memoryDc_, oldBackBufferBitmap_);
        oldBackBufferBitmap_ = nullptr;
    }
    if (backBufferBitmap_) {
        DeleteObject(backBufferBitmap_);
        backBufferBitmap_ = nullptr;
    }
    if (memoryDc_) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    backBufferBits_ = nullptr;
    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

Gdiplus::FontFamily* DesktopLyricsWindow::fontFamily() {
    if (!fontFamily_ || cachedFontRequest_ != config_.font.name) {
        cachedFontRequest_ = config_.font.name;
        auto candidate = std::make_unique<Gdiplus::FontFamily>(config_.font.name.c_str());
        if (candidate->GetLastStatus() != Gdiplus::Ok) {
            candidate = std::make_unique<Gdiplus::FontFamily>(L"Microsoft YaHei");
        }
        fontFamily_ = std::move(candidate);
    }
    return fontFamily_.get();
}

void DesktopLyricsWindow::notifyGeometryChanged() const {
    if (geometryChanged_) {
        geometryChanged_(geometry());
    }
}

Gdiplus::Color DesktopLyricsWindow::colorFromColorRef(COLORREF color, BYTE alpha) const {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

}
