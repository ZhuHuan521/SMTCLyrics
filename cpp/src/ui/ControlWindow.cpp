#include "ui/ControlWindow.h"

#include "util/Encoding.h"

#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace smtc::ui {
namespace {

constexpr wchar_t kClassName[] = L"SMTCLyricsControlWindow";
constexpr UINT kSourceCheckCompleteMessage = WM_APP + 1;

enum ControlId {
    IdFontName = 101,
    IdFontSize,
    IdFontBold,
    IdFontItalic,
    IdFontUnderline,
    IdOffset,
    IdApplyFont,

    IdNormalColor1 = 201,
    IdNormalColor2,
    IdNormalBorder,
    IdNormalGradient,
    IdHighlightColor1,
    IdHighlightColor2,
    IdHighlightBorder,
    IdHighlightGradient,
    IdApplyColor,

    IdSmtc1 = 301,
    IdSmtc2,
    IdSmtcInterval,
    IdSaveSmtc,

    IdDisplay1 = 401,
    IdDisplay2,
    IdDisplay3,
    IdSaveDisplay,

    IdWinLeft = 501,
    IdWinTop,
    IdWinWidth,
    IdWinHeight,
    IdSaveWinPos,

    IdSource1 = 601,
    IdSource2,
    IdSource3,
    IdSource4,
    IdSaveSource,

    IdLock = 701,
    IdReload,
    IdLocalLyric,
    IdCheckSources,
    IdSwitchSource,
    IdClearCache,

    IdQqStatus = 801,
    IdKgStatus,
    IdKuwoStatus,
    IdWyStatus,
    IdLockStatus,
    IdStatusText
};

std::wstring intText(int value) {
    return std::to_wstring(value);
}

int sourceToCombo(int source) {
    if (source < 1 || source > 4) return 0;
    return source - 1;
}

int comboToSource(int selection) {
    if (selection < 0 || selection > 3) return 1;
    return selection + 1;
}

int clampSmtcPollIntervalMs(int value) {
    return std::clamp(value, 30, 2000);
}

const wchar_t* sourceLabel(int index) {
    switch (index) {
    case 0: return L"QQ 音乐";
    case 1: return L"酷狗";
    case 2: return L"酷我";
    case 3: return L"网易云";
    default: return L"QQ 音乐";
    }
}

const wchar_t* gradientLabel(int index) {
    switch (index) {
    case 0: return L"无渐变";
    case 1: return L"两色渐变";
    case 2: return L"三色渐变(兼容)";
    default: return L"两色渐变";
    }
}

bool isColorButtonId(int id) {
    return id == IdNormalColor1 || id == IdNormalColor2 || id == IdNormalBorder ||
           id == IdHighlightColor1 || id == IdHighlightColor2 || id == IdHighlightBorder;
}

int sourceStatusId(int index) {
    switch (index) {
    case 0: return IdQqStatus;
    case 1: return IdKgStatus;
    case 2: return IdKuwoStatus;
    case 3: return IdWyStatus;
    default: return IdQqStatus;
    }
}

}

ControlWindow::ControlWindow() {
    font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    ownsFont_ = font_ != nullptr;
    if (!font_) {
        font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

ControlWindow::~ControlWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
    if (ownsFont_ && font_) {
        DeleteObject(font_);
    }
}

bool ControlWindow::create(const config::AppConfig& config, ControlWindowCallbacks callbacks) {
    config_ = config;
    callbacks_ = std::move(callbacks);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ControlWindow::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"桌面歌词 with SMTC",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        780,
        690,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) return false;
    createControls();
    populateControls();
    updateLockControls();
    setStatusText(L"就绪");
    return true;
}

void ControlWindow::show(int command) {
    if (hwnd_) {
        ShowWindow(hwnd_, command);
        UpdateWindow(hwnd_);
    }
}

void ControlWindow::setConfig(const config::AppConfig& config) {
    config_ = config;
    if (hwnd_) populateControls();
}

void ControlWindow::syncLyricGeometry(const config::WindowConfig& window) {
    if (!window.hasPosition) return;
    config_.window = window;
    if (!hwnd_) return;
    setText(IdWinLeft, intText(window.left));
    setText(IdWinTop, intText(window.top));
    setText(IdWinWidth, intText(window.width));
    setText(IdWinHeight, intText(window.height));
}

void ControlWindow::setStatusText(std::wstring text) {
    setText(IdStatusText, text);
}

LRESULT CALLBACK ControlWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ControlWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (self) return self->handleMessage(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ControlWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdApplyFont: applyFontAndLyricsSettings(); return 0;
        case IdApplyColor: applyColorSettings(); return 0;
        case IdSaveSmtc: applySmtcSettings(); return 0;
        case IdSaveDisplay: applyDisplaySettings(); return 0;
        case IdSaveSource: applySourceSettings(); return 0;
        case IdSaveWinPos: saveLyricGeometry(); return 0;
        case IdLock: toggleLock(); return 0;
        case IdReload: if (callbacks_.reloadLyrics) callbacks_.reloadLyrics(); return 0;
        case IdSwitchSource: if (callbacks_.switchSource) callbacks_.switchSource(); return 0;
        case IdClearCache: if (callbacks_.clearCache) callbacks_.clearCache(); return 0;
        case IdLocalLyric: if (callbacks_.openLocalLyric) callbacks_.openLocalLyric(); return 0;
        case IdCheckSources: startSourceCheck(); return 0;
        default:
            if (isColorButtonId(LOWORD(wParam))) {
                chooseColor(LOWORD(wParam));
                return 0;
            }
            break;
        }
        break;
    case WM_DRAWITEM:
        if (drawColorButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case kSourceCheckCompleteMessage: {
        std::unique_ptr<std::array<bool, 4>> status(reinterpret_cast<std::array<bool, 4>*>(lParam));
        completeSourceCheck(*status);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ControlWindow::createControls() {
    addControl(L"BUTTON", L"基础设置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 12, 360, 220, 0);
    addLabel(L"字体", 30, 44, 54, 24);
    addEdit(L"", 86, 38, 164, 28, IdFontName);
    addLabel(L"大小", 262, 44, 42, 24);
    addEdit(L"", 306, 38, 44, 28, IdFontSize);
    addCheckBox(L"加粗", 86, 78, 70, 26, IdFontBold);
    addCheckBox(L"倾斜", 160, 78, 70, 26, IdFontItalic);
    addCheckBox(L"下划线", 234, 78, 88, 26, IdFontUnderline);
    addLabel(L"歌词微调", 30, 120, 72, 24);
    addEdit(L"", 110, 114, 64, 28, IdOffset);
    addButton(L"保存基础设置", 210, 112, 124, 34, IdApplyFont);
    addLabel(L"监视", 30, 164, 54, 24);
    addRadio(L"SMTC1", 86, 160, 60, 26, IdSmtc1, true);
    addRadio(L"SMTC2", 146, 160, 60, 26, IdSmtc2);
    addLabel(L"轮询(ms)", 210, 164, 64, 24);
    addEdit(L"", 274, 158, 44, 28, IdSmtcInterval);
    addButton(L"保存", 324, 156, 48, 32, IdSaveSmtc);
    addLabel(L"显示", 30, 196, 54, 24);
    addRadio(L"一句", 86, 192, 64, 26, IdDisplay1, true);
    addRadio(L"两句", 150, 192, 64, 26, IdDisplay2);
    addRadio(L"两句向前", 214, 192, 92, 26, IdDisplay3);
    addButton(L"保存", 306, 190, 48, 30, IdSaveDisplay);

    addControl(L"BUTTON", L"歌词窗口", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 390, 12, 360, 220, 0);
    addLabel(L"当前状态", 408, 44, 72, 24);
    addValueLabel(L"", 486, 40, 96, 28, IdLockStatus);
    addButton(L"解锁歌词窗口", 600, 36, 126, 34, IdLock);
    addLabel(L"左边", 408, 94, 44, 24);
    addEdit(L"", 454, 88, 72, 28, IdWinLeft);
    addLabel(L"顶边", 548, 94, 44, 24);
    addEdit(L"", 594, 88, 72, 28, IdWinTop);
    addLabel(L"宽度", 408, 134, 44, 24);
    addEdit(L"", 454, 128, 72, 28, IdWinWidth);
    addLabel(L"高度", 548, 134, 44, 24);
    addEdit(L"", 594, 128, 72, 28, IdWinHeight);
    addButton(L"应用位置", 600, 174, 126, 34, IdSaveWinPos);

    addControl(L"BUTTON", L"颜色", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 246, 736, 156, 0);
    addLabel(L"普通歌词", 30, 282, 76, 24);
    addLabel(L"起始", 118, 282, 42, 24);
    addColorButton(160, 278, 34, 28, IdNormalColor1);
    addLabel(L"结束", 206, 282, 42, 24);
    addColorButton(248, 278, 34, 28, IdNormalColor2);
    addLabel(L"描边", 294, 282, 42, 24);
    addColorButton(336, 278, 34, 28, IdNormalBorder);
    addLabel(L"渐变", 390, 282, 42, 24);
    addCombo(438, 276, 150, 160, IdNormalGradient);

    addLabel(L"高亮歌词", 30, 334, 76, 24);
    addLabel(L"起始", 118, 334, 42, 24);
    addColorButton(160, 330, 34, 28, IdHighlightColor1);
    addLabel(L"结束", 206, 334, 42, 24);
    addColorButton(248, 330, 34, 28, IdHighlightColor2);
    addLabel(L"描边", 294, 334, 42, 24);
    addColorButton(336, 330, 34, 28, IdHighlightBorder);
    addLabel(L"渐变", 390, 334, 42, 24);
    addCombo(438, 328, 150, 160, IdHighlightGradient);
    addButton(L"保存颜色", 618, 304, 104, 36, IdApplyColor);

    addControl(L"BUTTON", L"歌词源优先级", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 416, 360, 188, 0);
    addLabel(L"优先级 1", 30, 454, 70, 24);
    addCombo(112, 448, 132, 150, IdSource1);
    addLabel(L"优先级 2", 30, 494, 70, 24);
    addCombo(112, 488, 132, 150, IdSource2);
    addLabel(L"优先级 3", 30, 534, 70, 24);
    addCombo(112, 528, 132, 150, IdSource3);
    addLabel(L"优先级 4", 30, 574, 70, 24);
    addCombo(112, 568, 132, 150, IdSource4);
    addButton(L"保存优先级", 260, 526, 96, 36, IdSaveSource);

    addControl(L"BUTTON", L"操作与检测", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 390, 416, 360, 188, 0);
    addLabel(L"QQ 音乐", 408, 454, 64, 24);
    addValueLabel(L"待检测", 474, 450, 70, 28, IdQqStatus);
    addLabel(L"酷狗", 558, 454, 46, 24);
    addValueLabel(L"待检测", 606, 450, 70, 28, IdKgStatus);
    addLabel(L"酷我", 408, 492, 46, 24);
    addValueLabel(L"待检测", 474, 488, 70, 28, IdKuwoStatus);
    addLabel(L"网易云", 558, 492, 56, 24);
    addValueLabel(L"待检测", 606, 488, 70, 28, IdWyStatus);
    addButton(L"检测歌词源", 408, 532, 104, 34, IdCheckSources);
    addButton(L"重新获取", 522, 532, 92, 34, IdReload);
    addButton(L"换源", 624, 532, 70, 34, IdSwitchSource);
    addButton(L"本地歌词", 408, 572, 104, 34, IdLocalLyric);
    addButton(L"清除缓存", 522, 572, 92, 34, IdClearCache);

    addValueLabel(L"", 14, 620, 736, 28, IdStatusText);

    for (int comboId : {IdNormalGradient, IdHighlightGradient}) {
        for (int i = 0; i < 3; ++i) SendMessageW(GetDlgItem(hwnd_, comboId), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(gradientLabel(i)));
    }
    for (int comboId : {IdSource1, IdSource2, IdSource3, IdSource4}) {
        for (int i = 0; i < 4; ++i) SendMessageW(GetDlgItem(hwnd_, comboId), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sourceLabel(i)));
    }
}

void ControlWindow::populateControls() {
    setText(IdFontName, config_.font.name);
    setText(IdFontSize, intText(config_.font.size));
    setChecked(IdFontBold, config_.font.bold);
    setChecked(IdFontItalic, config_.font.italic);
    setChecked(IdFontUnderline, config_.font.underline);
    setText(IdOffset, intText(config_.lyricOffsetSeconds));
    setComboSelection(IdNormalGradient, config_.normal.gradientMode);
    setComboSelection(IdHighlightGradient, config_.highlight.gradientMode);
    CheckRadioButton(hwnd_, IdSmtc1, IdSmtc2, config_.smtcMode == 2 ? IdSmtc2 : IdSmtc1);
    setText(IdSmtcInterval, intText(config_.smtcPollIntervalMs));
    CheckRadioButton(hwnd_, IdDisplay1, IdDisplay3, IdDisplay1 + std::clamp(config_.displayMode, 1, 3) - 1);
    syncLyricGeometry(config_.window);
    for (std::size_t i = 0; i < config_.sourcePriority.size() && i < 4; ++i) {
        setComboSelection(IdSource1 + static_cast<int>(i), sourceToCombo(config_.sourcePriority[i]));
    }
    updateColorSwatches();
    updateLockControls();
}

void ControlWindow::applyFontAndLyricsSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"基础设置已保存");
}

void ControlWindow::applyColorSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    updateColorSwatches();
    setStatusText(L"颜色设置已保存");
}

void ControlWindow::applySmtcSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"SMTC 设置已保存");
}

void ControlWindow::applyDisplaySettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"显示方式已保存");
}

void ControlWindow::applySourceSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"歌词源优先级已保存");
}

void ControlWindow::saveLyricGeometry() {
    config_.window.left = getInt(IdWinLeft, config_.window.left);
    config_.window.top = getInt(IdWinTop, config_.window.top);
    config_.window.width = getInt(IdWinWidth, config_.window.width);
    config_.window.height = getInt(IdWinHeight, config_.window.height);
    config_.window.hasPosition = config_.window.width > 0 && config_.window.height > 0;
    if (callbacks_.moveLyricWindow) callbacks_.moveLyricWindow(config_.window);
    setStatusText(L"歌词窗口位置已应用");
}

void ControlWindow::toggleLock() {
    lyricDraggable_ = !lyricDraggable_;
    if (callbacks_.setLyricDraggable) callbacks_.setLyricDraggable(lyricDraggable_);
    updateLockControls();
    setStatusText(lyricDraggable_ ? L"歌词窗口已解除锁定" : L"歌词窗口已锁定");
}

void ControlWindow::startSourceCheck() {
    if (sourceCheckRunning_) return;
    sourceCheckRunning_ = true;
    for (int i = 0; i < 4; ++i) setText(sourceStatusId(i), L"检测中");
    setStatusText(L"正在搜索《关键词》检测歌词源...");
    EnableWindow(GetDlgItem(hwnd_, IdCheckSources), FALSE);

    HWND target = hwnd_;
    auto check = callbacks_.checkLyricSources;
    std::thread([target, check = std::move(check)]() mutable {
        auto result = std::make_unique<std::array<bool, 4>>();
        result->fill(false);
        if (check) {
            *result = check();
        }
        if (!PostMessageW(target, kSourceCheckCompleteMessage, 0, reinterpret_cast<LPARAM>(result.get()))) {
            return;
        }
        result.release();
    }).detach();
}

void ControlWindow::completeSourceCheck(const std::array<bool, 4>& status) {
    sourceCheckRunning_ = false;
    EnableWindow(GetDlgItem(hwnd_, IdCheckSources), TRUE);
    for (int i = 0; i < 4; ++i) {
        setText(sourceStatusId(i), status[static_cast<std::size_t>(i)] ? L"可用" : L"不可用");
    }
    setStatusText(L"歌词源检测完成");
}

void ControlWindow::chooseColor(int id) {
    CHOOSECOLORW color{};
    color.lStructSize = sizeof(color);
    color.hwndOwner = hwnd_;
    color.rgbResult = colorForButton(id);
    color.lpCustColors = customColors_;
    color.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&color)) return;

    setColorForButton(id, color.rgbResult);
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    updateColorSwatches();
    setStatusText(L"颜色已更新");
}

void ControlWindow::updateLockControls() {
    setText(IdLockStatus, lyricDraggable_ ? L"已解锁" : L"已锁定");
    setText(IdLock, lyricDraggable_ ? L"锁定歌词窗口" : L"解锁歌词窗口");
}

void ControlWindow::updateColorSwatches() const {
    for (int id : {IdNormalColor1, IdNormalColor2, IdNormalBorder, IdHighlightColor1, IdHighlightColor2, IdHighlightBorder}) {
        if (HWND child = GetDlgItem(hwnd_, id)) InvalidateRect(child, nullptr, TRUE);
    }
}

bool ControlWindow::drawColorButton(const DRAWITEMSTRUCT& item) const {
    const int id = static_cast<int>(item.CtlID);
    if (!isColorButtonId(id)) return false;

    RECT rect = item.rcItem;
    FillRect(item.hDC, &rect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    InflateRect(&rect, -3, -3);
    HBRUSH brush = CreateSolidBrush(colorForButton(id));
    FillRect(item.hDC, &rect, brush);
    DeleteObject(brush);
    FrameRect(item.hDC, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -1, -1);
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

config::AppConfig ControlWindow::readConfigFromControls() const {
    auto config = config_;
    config.font.name = getText(IdFontName);
    config.font.size = getInt(IdFontSize, config.font.size);
    config.font.bold = isChecked(IdFontBold);
    config.font.italic = isChecked(IdFontItalic);
    config.font.underline = isChecked(IdFontUnderline);
    config.lyricOffsetSeconds = getInt(IdOffset, config.lyricOffsetSeconds);
    config.normal.gradientMode = comboSelection(IdNormalGradient);
    config.highlight.gradientMode = comboSelection(IdHighlightGradient);
    config.smtcMode = isChecked(IdSmtc2) ? 2 : 1;
    config.smtcPollIntervalMs = clampSmtcPollIntervalMs(getInt(IdSmtcInterval, config.smtcPollIntervalMs));
    if (isChecked(IdDisplay2)) config.displayMode = 2;
    else if (isChecked(IdDisplay3)) config.displayMode = 3;
    else config.displayMode = 1;
    config.sourcePriority = {
        comboToSource(comboSelection(IdSource1)),
        comboToSource(comboSelection(IdSource2)),
        comboToSource(comboSelection(IdSource3)),
        comboToSource(comboSelection(IdSource4))
    };
    config.window.left = getInt(IdWinLeft, config.window.left);
    config.window.top = getInt(IdWinTop, config.window.top);
    config.window.width = getInt(IdWinWidth, config.window.width);
    config.window.height = getInt(IdWinHeight, config.window.height);
    config.window.hasPosition = config.window.width > 0 && config.window.height > 0;
    return config;
}

HWND ControlWindow::addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD exStyle) {
    HWND child = CreateWindowExW(exStyle, className, text, style, x, y, width, height, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (child && font_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return child;
}

HWND ControlWindow::addLabel(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, x, y, width, height, id);
}

HWND ControlWindow::addValueLabel(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, x, y, width, height, id, WS_EX_STATICEDGE);
}

HWND ControlWindow::addEdit(const std::wstring& text, int x, int y, int width, int height, int id) {
    return addControl(L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width, height, id, WS_EX_CLIENTEDGE);
}

HWND ControlWindow::addButton(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, width, height, id);
}

HWND ControlWindow::addColorButton(int x, int y, int width, int height, int id) {
    return addControl(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, width, height, id);
}

HWND ControlWindow::addCheckBox(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, width, height, id);
}

HWND ControlWindow::addRadio(const wchar_t* text, int x, int y, int width, int height, int id, bool firstInGroup) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
    if (firstInGroup) style |= WS_GROUP;
    return addControl(L"BUTTON", text, style, x, y, width, height, id);
}

HWND ControlWindow::addCombo(int x, int y, int width, int height, int id) {
    return addControl(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, width, height, id);
}

void ControlWindow::setText(int id, const std::wstring& text) const {
    if (auto* child = GetDlgItem(hwnd_, id)) SetWindowTextW(child, text.c_str());
}

std::wstring ControlWindow::getText(int id) const {
    HWND child = GetDlgItem(hwnd_, id);
    if (!child) return {};
    const int length = GetWindowTextLengthW(child);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(child, text.data(), length + 1);
    return text;
}

int ControlWindow::getInt(int id, int fallback) const {
    const auto text = util::trim(getText(id));
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 10);
    return end == text.c_str() ? fallback : static_cast<int>(value);
}

void ControlWindow::setChecked(int id, bool checked) const {
    SendMessageW(GetDlgItem(hwnd_, id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool ControlWindow::isChecked(int id) const {
    return SendMessageW(GetDlgItem(hwnd_, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int ControlWindow::comboSelection(int id) const {
    return static_cast<int>(SendMessageW(GetDlgItem(hwnd_, id), CB_GETCURSEL, 0, 0));
}

void ControlWindow::setComboSelection(int id, int index) const {
    SendMessageW(GetDlgItem(hwnd_, id), CB_SETCURSEL, std::max(0, index), 0);
}

COLORREF ControlWindow::colorForButton(int id) const {
    switch (id) {
    case IdNormalColor1: return config_.normal.color1;
    case IdNormalColor2: return config_.normal.color2;
    case IdNormalBorder: return config_.normal.border;
    case IdHighlightColor1: return config_.highlight.color1;
    case IdHighlightColor2: return config_.highlight.color2;
    case IdHighlightBorder: return config_.highlight.border;
    default: return RGB(0, 0, 0);
    }
}

void ControlWindow::setColorForButton(int id, COLORREF color) {
    switch (id) {
    case IdNormalColor1: config_.normal.color1 = color; break;
    case IdNormalColor2: config_.normal.color2 = color; break;
    case IdNormalBorder: config_.normal.border = color; break;
    case IdHighlightColor1: config_.highlight.color1 = color; break;
    case IdHighlightColor2: config_.highlight.color2 = color; break;
    case IdHighlightBorder: config_.highlight.border = color; break;
    default: break;
    }
}

}
