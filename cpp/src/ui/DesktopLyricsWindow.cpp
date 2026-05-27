#include "ui/DesktopLyricsWindow.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace smtc::ui {
namespace {

// 桌面歌词窗口类名，用于 RegisterClassExW/CreateWindowExW。
constexpr wchar_t kClassName[] = L"SMTCLyricsOverlayWindow";

// 把配置里的粗体/斜体/下划线转换成 GDI+ FontStyle 位标志。
Gdiplus::FontStyle fontStyleFromConfig(const config::FontConfig& font) {
    int style = Gdiplus::FontStyleRegular;
    if (font.bold) style |= Gdiplus::FontStyleBold;
    if (font.italic) style |= Gdiplus::FontStyleItalic;
    if (font.underline) style |= Gdiplus::FontStyleUnderline;
    return static_cast<Gdiplus::FontStyle>(style);
}

// COLORREF 是 0x00bbggrr，GDI+ Color 需要 ARGB 分量。
Gdiplus::Color toGdiColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// 根据渐变模式创建画刷；当前实现用垂直线性渐变覆盖两色模式。
std::unique_ptr<Gdiplus::Brush> makeBrush(const config::TextStyle& style, const Gdiplus::RectF& rect) {
    if (style.gradientMode <= 0) {
        return std::make_unique<Gdiplus::SolidBrush>(toGdiColor(style.color1));
    }
    Gdiplus::PointF p1(rect.X, rect.Y);
    Gdiplus::PointF p2(rect.X, rect.Y + std::max(1.0f, rect.Height));
    return std::make_unique<Gdiplus::LinearGradientBrush>(p1, p2, toGdiColor(style.color1), toGdiColor(style.color2));
}

// 最多显示四行，当前配置实际只会生成一行或两行。
struct DisplayLines {
    std::array<std::wstring_view, 4> items{};
    std::size_t count = 0;
};

// 把 LrcParser 输出的换行文本拆成可绘制的行视图。
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
    // GDI+ 初始化放在窗口对象生命周期内。
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplusToken_, &input, nullptr);
}

DesktopLyricsWindow::~DesktopLyricsWindow() {
    // 先销毁窗口/缓冲区，再关闭 GDI+。
    destroy();
    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
}

bool DesktopLyricsWindow::create(const config::WindowConfig& windowConfig) {
    if (hwnd_) return true;

    // 注册一个轻量弹出窗口类，用分层窗口实现透明歌词。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DesktopLyricsWindow::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // 没有保存位置时，默认横向铺满工作区并靠近屏幕底部。
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    width_ = windowConfig.hasPosition ? windowConfig.width : workArea.right - workArea.left;
    height_ = windowConfig.hasPosition ? windowConfig.height : 150;
    const int left = windowConfig.hasPosition ? windowConfig.left : workArea.left + ((workArea.right - workArea.left) - width_) / 2;
    const int top = windowConfig.hasPosition ? windowConfig.top : workArea.bottom - height_;

    hwnd_ = CreateWindowExW(
        // WS_EX_LAYERED 支持逐像素 alpha，TOPMOST 让歌词浮在普通窗口上方。
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
    // Win32 窗口和 DIB 后备缓冲都需要显式释放。
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    layoutCache_.clear();
    releaseBackBuffer();
}

void DesktopLyricsWindow::applyConfig(const config::AppConfig& config) {
    // 样式变化后立即重绘当前歌词文本。
    config_ = config;
    invalidateLayout();
    redraw();
}

void DesktopLyricsWindow::updateLyrics(std::wstring_view text, int highlightPercent, int highlightLine) {
    // 同一帧内容不重复绘制；高亮值也限制在合法范围。
    const auto clampedHighlight = std::clamp(highlightPercent, 0, 100);
    const auto clampedLine = std::max(0, highlightLine);
    if (text_ == text && highlightPercent_ == clampedHighlight && highlightLine_ == clampedLine) {
        return;
    }
    const bool textChanged = text_ != text;
    if (textChanged) {
        text_.assign(text.begin(), text.end());
    }
    highlightPercent_ = clampedHighlight;
    highlightLine_ = clampedLine;
    if (textChanged) {
        invalidateLayout();
    }
    redraw();
}

void DesktopLyricsWindow::setDraggable(bool draggable) {
    draggable_ = draggable;
}

void DesktopLyricsWindow::setGeometryChangedCallback(std::function<void(const config::WindowConfig&)> callback) {
    geometryChanged_ = std::move(callback);
}

void DesktopLyricsWindow::move(int left, int top, int width, int height) {
    // 控制窗口应用几何时走这里，同时通知配置层保存新位置。
    if (!hwnd_) return;
    const bool sizeChanged = width_ != width || height_ != height;
    width_ = width;
    height_ = height;
    if (sizeChanged) {
        invalidateLayout();
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_, height_, SWP_NOACTIVATE);
    redraw();
    notifyGeometryChanged();
}

config::WindowConfig DesktopLyricsWindow::geometry() const {
    // 从实际窗口矩形生成可持久化的 WindowConfig。
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
    // WM_NCCREATE 时把 this 保存进窗口 userdata，后续消息才能回到实例方法。
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
            // 通过伪装成标题栏拖动，让无边框窗口也能移动。
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            notifyGeometryChanged();
            return 0;
        }
        break;
    case WM_CLOSE:
        // 用户不能直接关闭歌词窗口，只能关闭控制窗口结束程序。
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
    // 所有绘制都先进入内存 DIB，再一次性 UpdateLayeredWindow 到屏幕。
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
            // 使用缓存好的文字路径绘制，高亮推进时避免反复 AddString。
            if (layoutDirty_) {
                rebuildLayoutCache();
            }
            if (!layoutCache_.empty()) {
                const int activeLine = std::clamp(highlightLine_, 0, static_cast<int>(layoutCache_.size()) - 1);
                Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(150, 0, 0, 0));

                for (std::size_t i = 0; i < layoutCache_.size(); ++i) {
                    const auto& line = layoutCache_[i];
                    // 两行模式下，非当前高亮行使用第二句样式。
                    const bool isActiveLine = (static_cast<int>(i) == activeLine);
                    const bool isSecondLine = (layoutCache_.size() > 1 && i == 1 && activeLine == 0);
                    const auto& lineNormalStyle = isSecondLine ? config_.highlight2 : config_.normal;
                    const auto& lineBorderStyle = isSecondLine ? config_.highlight2.border : config_.normal.border;

                    // 先画轻微阴影，再画描边和普通文字。
                    graphics.FillPath(&shadowBrush, line.shadowPath.get());
                    Gdiplus::Pen outline(colorFromColorRef(lineBorderStyle), 1.0f);
                    graphics.DrawPath(&outline, line.textPath.get());
                    auto brush = makeBrush(lineNormalStyle, line.bounds);
                    graphics.FillPath(brush.get(), line.textPath.get());

                    if (highlightPercent_ > 0 && isActiveLine) {
                        // 通过水平裁剪区域叠加高亮画刷，实现从左到右的扫光效果。
                        Gdiplus::GraphicsState state = graphics.Save();
                        Gdiplus::RectF clip(line.bounds.X, line.bounds.Y, line.bounds.Width * highlightPercent_ / 100.0f, line.bounds.Height);
                        graphics.SetClip(clip);
                        Gdiplus::Pen highlightOutline(colorFromColorRef(config_.highlight.border), 1.0f);
                        graphics.DrawPath(&highlightOutline, line.textPath.get());
                        auto highlightBrush = makeBrush(config_.highlight, line.bounds);
                        graphics.FillPath(highlightBrush.get(), line.textPath.get());
                        graphics.Restore(state);
                    }
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
    // 尺寸未变时复用已有 DIB，减少频繁分配。
    if (memoryDc_ && backBufferBitmap_ && backBufferWidth_ == width_ && backBufferHeight_ == height_) {
        return true;
    }

    releaseBackBuffer();

    memoryDc_ = CreateCompatibleDC(referenceDc);
    if (!memoryDc_) return false;

    // 使用 top-down 32 位 DIB，像素内存可直接清零成透明背景。
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
    // 按 SelectObject/CreateDIBSection/CreateCompatibleDC 的反向顺序释放。
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
    // 字体名没变时复用 FontFamily；创建失败时回退微软雅黑。
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

void DesktopLyricsWindow::invalidateLayout() {
    layoutDirty_ = true;
    layoutCache_.clear();
}

bool DesktopLyricsWindow::rebuildLayoutCache() {
    layoutCache_.clear();
    layoutDirty_ = false;
    if (text_.empty() || width_ <= 0 || height_ <= 0) return true;

    auto* family = fontFamily();
    if (!family) return false;

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

    layoutCache_.reserve(lines.count);
    for (std::size_t i = 0; i < lines.count; ++i) {
        const auto lineText = lines.items[i];
        const float y = startY + static_cast<float>(i) * lineHeight;
        Gdiplus::RectF layout(0.0f, y, static_cast<Gdiplus::REAL>(width_), lineHeight);
        Gdiplus::RectF shadowLayout(2.0f, y + 2.0f, static_cast<Gdiplus::REAL>(width_), lineHeight);

        CachedLine cached;
        cached.shadowPath = std::make_unique<Gdiplus::GraphicsPath>();
        cached.textPath = std::make_unique<Gdiplus::GraphicsPath>();
        cached.shadowPath->AddString(lineText.data(), static_cast<INT>(lineText.size()), family, style, fontSize, shadowLayout, &format);
        cached.textPath->AddString(lineText.data(), static_cast<INT>(lineText.size()), family, style, fontSize, layout, &format);
        cached.textPath->GetBounds(&cached.bounds);
        layoutCache_.push_back(std::move(cached));
    }
    return true;
}

void DesktopLyricsWindow::notifyGeometryChanged() const {
    // 位置变化回调由 Application 保存到 config.ini 并同步控制窗口。
    if (geometryChanged_) {
        geometryChanged_(geometry());
    }
}

Gdiplus::Color DesktopLyricsWindow::colorFromColorRef(COLORREF color, BYTE alpha) const {
    // 成员版本保留给需要访问对象状态的调用处，目前只是统一颜色转换。
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

}
