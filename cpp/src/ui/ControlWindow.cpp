#include "ui/ControlWindow.h"

#include "util/Encoding.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <utility>

namespace smtc::ui {
namespace {

constexpr wchar_t kClassName[] = L"SMTCLyricsControlWindow";

enum ControlId {
    IdFontName = 101,
    IdFontSize,
    IdFontBold,
    IdFontItalic,
    IdFontUnderline,
    IdCookie,
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
    IdSaveSmtc,
    IdDisplay1 = 401,
    IdDisplay2,
    IdDisplay3,
    IdSaveDisplay,
    IdWinTop = 501,
    IdWinHeight,
    IdWinWidth,
    IdWinLeft,
    IdReadWinPos,
    IdSaveWinPos,
    IdSource1 = 601,
    IdSource2,
    IdSource3,
    IdSource4,
    IdSaveSource,
    IdLock = 701,
    IdReload,
    IdLocalLyric,
    IdRefreshStatus,
    IdSwitchSource,
    IdClearCache,
    IdQqStatus = 801,
    IdKgStatus,
    IdWyStatus,
    IdKuwoStatus,
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

const wchar_t* sourceLabel(int index) {
    switch (index) {
    case 0: return L"企鹅";
    case 1: return L"狗狗";
    case 2: return L"酷我";
    case 3: return L"网易";
    default: return L"企鹅";
    }
}

const wchar_t* gradientLabel(int index) {
    switch (index) {
    case 0: return L"无渐变";
    case 1: return L"两色渐变";
    case 2: return L"三色渐变";
    default: return L"两色渐变";
    }
}

}

ControlWindow::ControlWindow() {
    font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

ControlWindow::~ControlWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"桌面歌词 with SMTC By:柱环",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        668,
        850,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) return false;
    SetWindowTextW(hwnd_, L"桌面歌词 with SMTC By:柱环");
    createControls();
    populateControls();
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
        case IdReadWinPos: readLyricGeometry(); return 0;
        case IdSaveWinPos: saveLyricGeometry(); return 0;
        case IdLock: toggleLock(); return 0;
        case IdReload: if (callbacks_.reloadLyrics) callbacks_.reloadLyrics(); return 0;
        case IdSwitchSource: if (callbacks_.switchSource) callbacks_.switchSource(); return 0;
        case IdClearCache: if (callbacks_.clearCache) callbacks_.clearCache(); return 0;
        case IdLocalLyric: if (callbacks_.openLocalLyric) callbacks_.openLocalLyric(); return 0;
        case IdRefreshStatus:
            setText(IdQqStatus, L"待检测");
            setText(IdKgStatus, L"待检测");
            setText(IdKuwoStatus, L"待检测");
            setText(IdWyStatus, L"待检测");
            return 0;
        default:
            break;
        }
        break;
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
    addControl(L"BUTTON", L"歌词字体", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 10, 340, 330, 0);
    addLabel(L"字体名称:", 26, 54, 90, 24);
    addEdit(L"", 122, 48, 190, 28, IdFontName);
    addLabel(L"字体大小:", 26, 92, 90, 24);
    addEdit(L"", 122, 86, 190, 28, IdFontSize);
    addLabel(L"字体样式:", 26, 130, 90, 24);
    addCheckBox(L"字体加粗", 122, 124, 130, 26, IdFontBold);
    addCheckBox(L"字体倾斜", 122, 160, 130, 26, IdFontItalic);
    addCheckBox(L"字体下划线", 122, 196, 140, 26, IdFontUnderline);
    addLabel(L"cookie", 26, 236, 90, 24);
    addEdit(L"", 122, 230, 190, 28, IdCookie);
    addLabel(L"微调", 26, 274, 90, 24);
    addEdit(L"", 122, 268, 118, 28, IdOffset);
    addButton(L"设置", 122, 304, 104, 34, IdApplyFont);

    addControl(L"BUTTON", L"歌词颜色", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 364, 10, 290, 330, 0);
    addLabel(L"文字颜色:", 376, 54, 90, 24);
    addEdit(L"", 468, 48, 70, 28, IdNormalColor1);
    addEdit(L"", 550, 48, 70, 28, IdNormalColor2);
    addLabel(L"文字边框:", 376, 92, 90, 24);
    addEdit(L"", 468, 86, 70, 28, IdNormalBorder);
    addLabel(L"文字渐变:", 376, 130, 90, 24);
    addCombo(468, 124, 154, 200, IdNormalGradient);
    addLabel(L"高亮颜色:", 376, 168, 90, 24);
    addEdit(L"", 468, 162, 70, 28, IdHighlightColor1);
    addEdit(L"", 550, 162, 70, 28, IdHighlightColor2);
    addLabel(L"高亮边框:", 376, 206, 90, 24);
    addEdit(L"", 468, 200, 70, 28, IdHighlightBorder);
    addLabel(L"高亮渐变:", 376, 244, 90, 24);
    addCombo(468, 238, 154, 200, IdHighlightGradient);
    addButton(L"设置", 468, 282, 104, 34, IdApplyColor);

    addControl(L"BUTTON", L"监视方法", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 348, 146, 166, 0);
    addRadio(L"SMTC1", 28, 388, 104, 26, IdSmtc1, true);
    addRadio(L"SMTC2", 28, 426, 104, 26, IdSmtc2);
    addButton(L"保存", 52, 480, 58, 32, IdSaveSmtc);

    addControl(L"BUTTON", L"显示方式", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 166, 348, 160, 166, 0);
    addRadio(L"一句", 184, 386, 120, 26, IdDisplay1, true);
    addRadio(L"两句", 184, 424, 120, 26, IdDisplay2);
    addRadio(L"两句_向前", 184, 462, 130, 26, IdDisplay3);
    addButton(L"保存", 210, 480, 58, 32, IdSaveDisplay);

    addControl(L"BUTTON", L"歌词窗口", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 338, 348, 316, 166, 0);
    addLabel(L"顶边", 350, 386, 44, 24);
    addEdit(L"", 398, 380, 106, 28, IdWinTop);
    addLabel(L"高度", 350, 418, 44, 24);
    addEdit(L"", 398, 412, 106, 28, IdWinHeight);
    addLabel(L"宽度", 350, 450, 44, 24);
    addEdit(L"", 398, 444, 106, 28, IdWinWidth);
    addLabel(L"左边", 350, 482, 44, 24);
    addEdit(L"", 398, 476, 106, 28, IdWinLeft);
    addButton(L"获取当前位置", 516, 390, 122, 42, IdReadWinPos);
    addButton(L"保存", 516, 454, 122, 42, IdSaveWinPos);

    addControl(L"BUTTON", L"歌词源", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 526, 290, 292, 0);
    addLabel(L"优先级1", 26, 568, 86, 24);
    addCombo(146, 560, 150, 200, IdSource1);
    addLabel(L"优先级2", 26, 608, 86, 24);
    addCombo(146, 600, 150, 200, IdSource2);
    addLabel(L"优先级3", 26, 648, 86, 24);
    addCombo(146, 640, 150, 200, IdSource3);
    addLabel(L"优先级4", 26, 688, 86, 24);
    addCombo(146, 680, 150, 200, IdSource4);
    addButton(L"保存", 100, 762, 94, 36, IdSaveSource);

    addLabel(L"企鹅状态:", 318, 560, 84, 24);
    addLabel(L"待检测", 400, 560, 70, 24, IdQqStatus);
    addLabel(L"哭我状态:", 486, 560, 84, 24);
    addLabel(L"待检测", 568, 560, 70, 24, IdKuwoStatus);
    addLabel(L"狗狗状态:", 318, 600, 84, 24);
    addLabel(L"待检测", 400, 600, 70, 24, IdKgStatus);
    addLabel(L"网易状态:", 486, 600, 84, 24);
    addLabel(L"待检测", 568, 600, 70, 24, IdWyStatus);

    addButton(L"锁定/解除锁定", 316, 642, 128, 42, IdLock);
    addButton(L"刷新监视", 484, 642, 92, 42, IdRefreshStatus);
    addButton(L"重新联网获取", 316, 702, 128, 42, IdReload);
    addButton(L"换源", 484, 702, 92, 42, IdSwitchSource);
    addButton(L"本地写入歌词", 316, 762, 128, 42, IdLocalLyric);
    addButton(L"清除缓存", 484, 762, 92, 42, IdClearCache);
    addLabel(L"", 318, 812, 320, 24, IdStatusText);

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
    setText(IdCookie, config_.cookie);
    setText(IdOffset, intText(config_.lyricOffsetSeconds));
    setText(IdNormalColor1, intText(config_.normal.color1));
    setText(IdNormalColor2, intText(config_.normal.color2));
    setText(IdNormalBorder, intText(config_.normal.border));
    setComboSelection(IdNormalGradient, config_.normal.gradientMode);
    setText(IdHighlightColor1, intText(config_.highlight.color1));
    setText(IdHighlightColor2, intText(config_.highlight.color2));
    setText(IdHighlightBorder, intText(config_.highlight.border));
    setComboSelection(IdHighlightGradient, config_.highlight.gradientMode);
    CheckRadioButton(hwnd_, IdSmtc1, IdSmtc2, config_.smtcMode == 2 ? IdSmtc2 : IdSmtc1);
    CheckRadioButton(hwnd_, IdDisplay1, IdDisplay3, IdDisplay1 + std::clamp(config_.displayMode, 1, 3) - 1);
    setText(IdWinTop, intText(config_.window.top));
    setText(IdWinHeight, intText(config_.window.height));
    setText(IdWinWidth, intText(config_.window.width));
    setText(IdWinLeft, intText(config_.window.left));
    for (std::size_t i = 0; i < config_.sourcePriority.size() && i < 4; ++i) {
        setComboSelection(IdSource1 + static_cast<int>(i), sourceToCombo(config_.sourcePriority[i]));
    }
    setStatusText(L"就绪");
}

void ControlWindow::applyFontAndLyricsSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"字体与歌词设置已保存");
}

void ControlWindow::applyColorSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"歌词颜色已保存");
}

void ControlWindow::applySmtcSettings() {
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"SMTC 监视方式已保存");
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

void ControlWindow::readLyricGeometry() {
    if (!callbacks_.getLyricGeometry) return;
    const auto geometry = callbacks_.getLyricGeometry();
    setText(IdWinTop, intText(geometry.top));
    setText(IdWinHeight, intText(geometry.height));
    setText(IdWinWidth, intText(geometry.width));
    setText(IdWinLeft, intText(geometry.left));
    setStatusText(L"已读取歌词窗口位置");
}

void ControlWindow::saveLyricGeometry() {
    config_.window.top = getInt(IdWinTop, config_.window.top);
    config_.window.height = getInt(IdWinHeight, config_.window.height);
    config_.window.width = getInt(IdWinWidth, config_.window.width);
    config_.window.left = getInt(IdWinLeft, config_.window.left);
    config_.window.hasPosition = config_.window.width > 0 && config_.window.height > 0;
    if (callbacks_.moveLyricWindow) callbacks_.moveLyricWindow(config_.window);
    setStatusText(L"歌词窗口位置已保存");
}

void ControlWindow::toggleLock() {
    lyricDraggable_ = !lyricDraggable_;
    if (callbacks_.setLyricDraggable) callbacks_.setLyricDraggable(lyricDraggable_);
    setStatusText(lyricDraggable_ ? L"歌词窗口已解除锁定" : L"歌词窗口已锁定");
}

config::AppConfig ControlWindow::readConfigFromControls() const {
    auto config = config_;
    config.font.name = getText(IdFontName);
    config.font.size = getInt(IdFontSize, config.font.size);
    config.font.bold = isChecked(IdFontBold);
    config.font.italic = isChecked(IdFontItalic);
    config.font.underline = isChecked(IdFontUnderline);
    config.cookie = getText(IdCookie);
    config.lyricOffsetSeconds = getInt(IdOffset, config.lyricOffsetSeconds);
    config.normal.color1 = static_cast<COLORREF>(getInt(IdNormalColor1, config.normal.color1));
    config.normal.color2 = static_cast<COLORREF>(getInt(IdNormalColor2, config.normal.color2));
    config.normal.border = static_cast<COLORREF>(getInt(IdNormalBorder, config.normal.border));
    config.normal.gradientMode = comboSelection(IdNormalGradient);
    config.highlight.color1 = static_cast<COLORREF>(getInt(IdHighlightColor1, config.highlight.color1));
    config.highlight.color2 = static_cast<COLORREF>(getInt(IdHighlightColor2, config.highlight.color2));
    config.highlight.border = static_cast<COLORREF>(getInt(IdHighlightBorder, config.highlight.border));
    config.highlight.gradientMode = comboSelection(IdHighlightGradient);
    config.smtcMode = isChecked(IdSmtc2) ? 2 : 1;
    if (isChecked(IdDisplay2)) config.displayMode = 2;
    else if (isChecked(IdDisplay3)) config.displayMode = 3;
    else config.displayMode = 1;
    config.sourcePriority = {
        comboToSource(comboSelection(IdSource1)),
        comboToSource(comboSelection(IdSource2)),
        comboToSource(comboSelection(IdSource3)),
        comboToSource(comboSelection(IdSource4))
    };
    config.window.top = getInt(IdWinTop, config.window.top);
    config.window.height = getInt(IdWinHeight, config.window.height);
    config.window.width = getInt(IdWinWidth, config.window.width);
    config.window.left = getInt(IdWinLeft, config.window.left);
    config.window.hasPosition = config.window.width > 0 && config.window.height > 0;
    return config;
}

HWND ControlWindow::addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD exStyle) {
    HWND child = CreateWindowExW(exStyle, className, text, style, x, y, width, height, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (child && font_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return child;
}

HWND ControlWindow::addLabel(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, width, height, id);
}

HWND ControlWindow::addEdit(const std::wstring& text, int x, int y, int width, int height, int id) {
    return addControl(L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width, height, id, WS_EX_CLIENTEDGE);
}

HWND ControlWindow::addButton(const wchar_t* text, int x, int y, int width, int height, int id) {
    return addControl(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, width, height, id);
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

}
