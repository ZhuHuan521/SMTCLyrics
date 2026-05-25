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

// 控制窗口类名和“歌词源检测完成”自定义消息。
constexpr wchar_t kClassName[] = L"SMTCLyricsControlWindow";
constexpr UINT kSourceCheckCompleteMessage = WM_APP + 1;

// 所有子控件的 ID。不同百位表示不同设置分组，便于阅读 WM_COMMAND 分发。
enum ControlId {
    IdFontName = 101,
    IdFontSize,
    IdFontBold,
    IdFontItalic,
    IdFontUnderline,
    IdOffset,
    IdSongOffset,
    IdSaveSongOffset,
    IdApplyFont,

    IdNormalColor1 = 201,
    IdNormalColor2,
    IdNormalBorder,
    IdNormalGradient,
    IdHighlightColor1,
    IdHighlightColor2,
    IdHighlightBorder,
    IdHighlightGradient,
    IdHighlight2Color1,
    IdHighlight2Color2,
    IdHighlight2Border,
    IdHighlight2Gradient,
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
    // Win32 编辑框只接受文本，数值统一在这里转宽字符串。
    return std::to_wstring(value);
}

int sourceToCombo(int source) {
    // 配置中的歌词源是 1..4，下拉框索引是 0..3。
    if (source < 1 || source > 4) return 0;
    return source - 1;
}

int comboToSource(int selection) {
    // 下拉框未选择时回到 QQ 音乐。
    if (selection < 0 || selection > 3) return 1;
    return selection + 1;
}

int clampSmtcPollIntervalMs(int value) {
    // 与配置层保持同样的轮询范围。
    return std::clamp(value, 500, 2000);
}

const wchar_t* sourceLabel(int index) {
    // 歌词源下拉框显示文本。
    switch (index) {
    case 0: return L"QQ 音乐";
    case 1: return L"酷狗";
    case 2: return L"酷我";
    case 3: return L"网易云";
    default: return L"QQ 音乐";
    }
}

const wchar_t* gradientLabel(int index) {
    // 渐变模式下拉框显示文本。
    switch (index) {
    case 0: return L"无渐变";
    case 1: return L"两色渐变";
    case 2: return L"三色渐变(兼容)";
    default: return L"两色渐变";
    }
}

bool isColorButtonId(int id) {
    // 颜色按钮统一走 WM_DRAWITEM 和颜色选择器。
    return id == IdNormalColor1 || id == IdNormalColor2 || id == IdNormalBorder ||
           id == IdHighlightColor1 || id == IdHighlightColor2 || id == IdHighlightBorder ||
           id == IdHighlight2Color1 || id == IdHighlight2Color2 || id == IdHighlight2Border;
}

int sourceStatusId(int index) {
    // 歌词源检测结果标签按源索引映射到控件 ID。
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
    // 控制窗口使用固定 UI 字体，创建失败时退回系统默认 GUI 字体。
    font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    ownsFont_ = font_ != nullptr;
    if (!font_) {
        font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

ControlWindow::~ControlWindow() {
    // 释放窗口和本类创建的字体资源。
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
    if (ownsFont_ && font_) {
        DeleteObject(font_);
    }
}

bool ControlWindow::create(const config::AppConfig& config, ControlWindowCallbacks callbacks) {
    // 保存初始配置和回调后注册/创建 Win32 窗口。
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
        L"SMTC歌词 By:柱环",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        820,
        790,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) return false;
    SetWindowTextW(hwnd_, L"SMTC歌词 By:柱环");
    createControls();
    populateControls();
    updateLockControls();
    setStatusText(L"就绪");
    return true;
}

void ControlWindow::show(int command) {
    // 外层 Application 控制窗口显示时机。
    if (hwnd_) {
        ShowWindow(hwnd_, command);
        UpdateWindow(hwnd_);
    }
}

void ControlWindow::setConfig(const config::AppConfig& config) {
    // 外部配置变化后重新填充控件，保持 UI 与运行态一致。
    config_ = config;
    if (hwnd_) populateControls();
}

void ControlWindow::syncLyricGeometry(const config::WindowConfig& window) {
    // 歌词窗口被拖动时，控制面板里的几何输入框也要同步。
    if (!window.hasPosition) return;
    config_.window = window;
    if (!hwnd_) return;
    setText(IdWinLeft, intText(window.left));
    setText(IdWinTop, intText(window.top));
    setText(IdWinWidth, intText(window.width));
    setText(IdWinHeight, intText(window.height));
}

void ControlWindow::setStatusText(std::wstring text) {
    // 底部状态栏文本更新。
    setText(IdStatusText, text);
}

LRESULT CALLBACK ControlWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // 把 Win32 静态回调转发到 ControlWindow 实例。
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
        // 按按钮 ID 分发到对应设置保存或业务操作。
        switch (LOWORD(wParam)) {
        case IdApplyFont: applyFontAndLyricsSettings(); return 0;
        case IdSaveSongOffset: applySongOffset(); return 0;
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
        // 颜色按钮是 owner-draw，需要自己画色块。
        if (drawColorButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case kSourceCheckCompleteMessage: {
        // 后台检测线程完成后把结果数组交回 UI 线程。
        std::unique_ptr<std::array<bool, 4>> status(reinterpret_cast<std::array<bool, 4>*>(lParam));
        completeSourceCheck(*status);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        // 控制窗口关闭即退出整个程序。
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ControlWindow::createControls() {
    // 这里使用固定坐标创建传统 Win32 控件，分组与 AppConfig 字段基本对应。
    // 基础设置
    addControl(L"BUTTON", L"基础设置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 12, 390, 252, 0);
    addLabel(L"字体", 30, 44, 54, 24);
    addEdit(L"", 86, 38, 180, 28, IdFontName);
    addLabel(L"大小", 278, 44, 42, 24);
    addEdit(L"", 322, 38, 44, 28, IdFontSize);
    addCheckBox(L"加粗", 86, 78, 70, 26, IdFontBold);
    addCheckBox(L"倾斜", 160, 78, 70, 26, IdFontItalic);
    addCheckBox(L"下划线", 234, 78, 88, 26, IdFontUnderline);
    addLabel(L"微调(ms)", 30, 120, 72, 24);
    addEdit(L"", 110, 114, 64, 28, IdOffset);
    addButton(L"保存基础设置", 210, 112, 124, 34, IdApplyFont);
    addLabel(L"歌曲微调(ms)", 30, 154, 96, 24);
    addEdit(L"", 130, 148, 64, 28, IdSongOffset);
    addButton(L"保存歌曲微调", 210, 146, 124, 34, IdSaveSongOffset);
    addLabel(L"监视", 30, 194, 54, 24);
    addRadio(L"SMTC1", 86, 190, 70, 26, IdSmtc1, true);
    addRadio(L"SMTC2", 160, 190, 70, 26, IdSmtc2);
    addLabel(L"轮询(ms)", 240, 194, 64, 24);
    addEdit(L"", 306, 188, 50, 28, IdSmtcInterval);
    addButton(L"保存", 360, 186, 48, 32, IdSaveSmtc);
    addLabel(L"显示", 30, 226, 54, 24);
    addRadio(L"一句", 86, 222, 64, 26, IdDisplay1, true);
    addRadio(L"两句", 154, 222, 64, 26, IdDisplay2);
    addRadio(L"两句向前", 222, 222, 92, 26, IdDisplay3);
    addButton(L"保存", 318, 220, 48, 30, IdSaveDisplay);

    // 歌词窗口
    addControl(L"BUTTON", L"歌词窗口", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 420, 12, 380, 252, 0);
    addLabel(L"当前状态", 438, 44, 72, 24);
    addValueLabel(L"", 516, 40, 96, 28, IdLockStatus);
    addButton(L"解锁歌词窗口", 630, 36, 140, 34, IdLock);
    addLabel(L"左边", 438, 94, 44, 24);
    addEdit(L"", 484, 88, 80, 28, IdWinLeft);
    addLabel(L"顶边", 580, 94, 44, 24);
    addEdit(L"", 626, 88, 80, 28, IdWinTop);
    addLabel(L"宽度", 438, 134, 44, 24);
    addEdit(L"", 484, 128, 80, 28, IdWinWidth);
    addLabel(L"高度", 580, 134, 44, 24);
    addEdit(L"", 626, 128, 80, 28, IdWinHeight);
    addButton(L"应用位置", 630, 174, 140, 34, IdSaveWinPos);

    // 颜色
    addControl(L"BUTTON", L"颜色", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 278, 786, 200, 0);
    addLabel(L"普通歌词", 30, 306, 76, 24);
    addLabel(L"起始", 118, 306, 42, 24);
    addColorButton(160, 302, 34, 28, IdNormalColor1);
    addLabel(L"结束", 206, 306, 42, 24);
    addColorButton(248, 302, 34, 28, IdNormalColor2);
    addLabel(L"描边", 294, 306, 42, 24);
    addColorButton(336, 302, 34, 28, IdNormalBorder);
    addLabel(L"渐变", 390, 306, 42, 24);
    addCombo(438, 300, 160, 160, IdNormalGradient);

    addLabel(L"高亮歌词", 30, 346, 76, 24);
    addLabel(L"起始", 118, 346, 42, 24);
    addColorButton(160, 342, 34, 28, IdHighlightColor1);
    addLabel(L"结束", 206, 346, 42, 24);
    addColorButton(248, 342, 34, 28, IdHighlightColor2);
    addLabel(L"描边", 294, 346, 42, 24);
    addColorButton(336, 342, 34, 28, IdHighlightBorder);
    addLabel(L"渐变", 390, 346, 42, 24);
    addCombo(438, 340, 160, 160, IdHighlightGradient);

    addLabel(L"第二句", 30, 386, 76, 24);
    addLabel(L"起始", 118, 386, 42, 24);
    addColorButton(160, 382, 34, 28, IdHighlight2Color1);
    addLabel(L"结束", 206, 386, 42, 24);
    addColorButton(248, 382, 34, 28, IdHighlight2Color2);
    addLabel(L"描边", 294, 386, 42, 24);
    addColorButton(336, 382, 34, 28, IdHighlight2Border);
    addLabel(L"渐变", 390, 386, 42, 24);
    addCombo(438, 380, 160, 160, IdHighlight2Gradient);
    addButton(L"保存颜色", 660, 342, 104, 36, IdApplyColor);

    // 歌词源优先级
    addControl(L"BUTTON", L"歌词源优先级", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 14, 492, 390, 188, 0);
    addLabel(L"优先级 1", 30, 530, 70, 24);
    addCombo(112, 524, 140, 150, IdSource1);
    addLabel(L"优先级 2", 30, 570, 70, 24);
    addCombo(112, 564, 140, 150, IdSource2);
    addLabel(L"优先级 3", 30, 610, 70, 24);
    addCombo(112, 604, 140, 150, IdSource3);
    addLabel(L"优先级 4", 30, 650, 70, 24);
    addCombo(112, 644, 140, 150, IdSource4);
    addButton(L"保存优先级", 270, 602, 100, 36, IdSaveSource);

    // 操作与检测
    addControl(L"BUTTON", L"操作与检测", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 420, 492, 380, 188, 0);
    addLabel(L"QQ 音乐", 438, 530, 64, 24);
    addValueLabel(L"待检测", 504, 526, 70, 28, IdQqStatus);
    addLabel(L"酷狗", 590, 530, 46, 24);
    addValueLabel(L"待检测", 638, 526, 70, 28, IdKgStatus);
    addLabel(L"酷我", 438, 568, 46, 24);
    addValueLabel(L"待检测", 504, 564, 70, 28, IdKuwoStatus);
    addLabel(L"网易云", 590, 568, 56, 24);
    addValueLabel(L"待检测", 638, 564, 70, 28, IdWyStatus);
    addButton(L"检测歌词源", 438, 608, 110, 34, IdCheckSources);
    addButton(L"重新获取", 558, 608, 92, 34, IdReload);
    addButton(L"换源", 660, 608, 70, 34, IdSwitchSource);
    addButton(L"本地歌词", 438, 648, 110, 34, IdLocalLyric);
    addButton(L"清除缓存", 558, 648, 92, 34, IdClearCache);

    // 状态栏
    addValueLabel(L"", 14, 696, 786, 28, IdStatusText);

    for (int comboId : {IdNormalGradient, IdHighlightGradient, IdHighlight2Gradient}) {
        // 初始化渐变模式下拉框。
        for (int i = 0; i < 3; ++i) SendMessageW(GetDlgItem(hwnd_, comboId), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(gradientLabel(i)));
    }
    for (int comboId : {IdSource1, IdSource2, IdSource3, IdSource4}) {
        // 初始化歌词源优先级下拉框。
        for (int i = 0; i < 4; ++i) SendMessageW(GetDlgItem(hwnd_, comboId), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sourceLabel(i)));
    }
}

void ControlWindow::populateControls() {
    // 把当前 config_ 写入所有输入框、复选框、单选框和下拉框。
    setText(IdFontName, config_.font.name);
    setText(IdFontSize, intText(config_.font.size));
    setChecked(IdFontBold, config_.font.bold);
    setChecked(IdFontItalic, config_.font.italic);
    setChecked(IdFontUnderline, config_.font.underline);
    setText(IdOffset, intText(config_.lyricOffsetMs));
    setComboSelection(IdNormalGradient, config_.normal.gradientMode);
    setComboSelection(IdHighlightGradient, config_.highlight.gradientMode);
    setComboSelection(IdHighlight2Gradient, config_.highlight2.gradientMode);
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
    // 基础设置包括字体、全局偏移、SMTC 和显示方式等可从控件读出的字段。
    config_ = readConfigFromControls();
    if (callbacks_.applyConfig) callbacks_.applyConfig(config_);
    setStatusText(L"基础设置已保存");
}

void ControlWindow::applySongOffset() {
    // 歌曲微调是当前歌曲独立偏移，不直接写入 AppConfig。
    const int offset = getInt(IdSongOffset, 0);
    if (callbacks_.saveSongOffset) callbacks_.saveSongOffset(offset);
    setStatusText(L"歌曲微调已保存");
}

void ControlWindow::setSongOffset(int offsetMs) {
    // Application 在换歌时会把缓存里的单曲偏移同步到这里。
    setText(IdSongOffset, intText(offsetMs));
}

void ControlWindow::applyColorSettings() {
    // 颜色按钮已经直接写 config_，这里再读取下拉框等控件并应用。
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
    // 用户手动输入的几何值会移动歌词窗口，再由回调保存位置。
    config_.window.left = getInt(IdWinLeft, config_.window.left);
    config_.window.top = getInt(IdWinTop, config_.window.top);
    config_.window.width = getInt(IdWinWidth, config_.window.width);
    config_.window.height = getInt(IdWinHeight, config_.window.height);
    config_.window.hasPosition = config_.window.width > 0 && config_.window.height > 0;
    if (callbacks_.moveLyricWindow) callbacks_.moveLyricWindow(config_.window);
    setStatusText(L"歌词窗口位置已应用");
}

void ControlWindow::toggleLock() {
    // lyricDraggable_ 为 true 表示当前已解锁、允许拖动。
    lyricDraggable_ = !lyricDraggable_;
    if (callbacks_.setLyricDraggable) callbacks_.setLyricDraggable(lyricDraggable_);
    updateLockControls();
    setStatusText(lyricDraggable_ ? L"歌词窗口已解除锁定" : L"歌词窗口已锁定");
}

void ControlWindow::startSourceCheck() {
    // 歌词源检测会访问网络，放到后台线程避免卡住窗口。
    if (sourceCheckRunning_) return;
    sourceCheckRunning_ = true;
    for (int i = 0; i < 4; ++i) setText(sourceStatusId(i), L"检测中");
    setStatusText(L"正在搜索《关键词》检测歌词源...");
    EnableWindow(GetDlgItem(hwnd_, IdCheckSources), FALSE);

    HWND target = hwnd_;
    auto check = callbacks_.checkLyricSources;
    std::thread([target, check = std::move(check)]() mutable {
        // 结果数组通过 PostMessage 交回主线程；投递失败时 unique_ptr 自动释放。
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
    // 把四个源的可用性写回对应状态标签。
    sourceCheckRunning_ = false;
    EnableWindow(GetDlgItem(hwnd_, IdCheckSources), TRUE);
    for (int i = 0; i < 4; ++i) {
        setText(sourceStatusId(i), status[static_cast<std::size_t>(i)] ? L"可用" : L"不可用");
    }
    setStatusText(L"歌词源检测完成");
}

void ControlWindow::chooseColor(int id) {
    // 使用系统颜色选择器，选中后立即应用到歌词窗口。
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
    // 锁定状态和按钮文案互为反向动作。
    setText(IdLockStatus, lyricDraggable_ ? L"已解锁" : L"已锁定");
    setText(IdLock, lyricDraggable_ ? L"锁定歌词窗口" : L"解锁歌词窗口");
}

void ControlWindow::updateColorSwatches() const {
    // 颜色值变化后让 owner-draw 按钮重绘色块。
    for (int id : {IdNormalColor1, IdNormalColor2, IdNormalBorder, IdHighlightColor1, IdHighlightColor2, IdHighlightBorder,
                   IdHighlight2Color1, IdHighlight2Color2, IdHighlight2Border}) {
        if (HWND child = GetDlgItem(hwnd_, id)) InvalidateRect(child, nullptr, TRUE);
    }
}

bool ControlWindow::drawColorButton(const DRAWITEMSTRUCT& item) const {
    // owner-draw 颜色按钮：背景、色块、边框和焦点框。
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
    // 从控件读取完整配置；无效数值回退到当前 config_。
    auto config = config_;
    config.font.name = getText(IdFontName);
    config.font.size = getInt(IdFontSize, config.font.size);
    config.font.bold = isChecked(IdFontBold);
    config.font.italic = isChecked(IdFontItalic);
    config.font.underline = isChecked(IdFontUnderline);
    config.lyricOffsetMs = getInt(IdOffset, config.lyricOffsetMs);
    config.normal.gradientMode = comboSelection(IdNormalGradient);
    config.highlight.gradientMode = comboSelection(IdHighlightGradient);
    config.highlight2.gradientMode = comboSelection(IdHighlight2Gradient);
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
    // 所有子控件都走同一工厂，统一设置父窗口、ID 和字体。
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
    // 先询问长度再读取，避免固定缓冲区截断。
    HWND child = GetDlgItem(hwnd_, id);
    if (!child) return {};
    const int length = GetWindowTextLengthW(child);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(child, text.data(), length + 1);
    return text;
}

int ControlWindow::getInt(int id, int fallback) const {
    // 编辑框解析失败时返回调用者给的默认值。
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
    // 按按钮 ID 映射到 config_ 中对应的颜色字段。
    switch (id) {
    case IdNormalColor1: return config_.normal.color1;
    case IdNormalColor2: return config_.normal.color2;
    case IdNormalBorder: return config_.normal.border;
    case IdHighlightColor1: return config_.highlight.color1;
    case IdHighlightColor2: return config_.highlight.color2;
    case IdHighlightBorder: return config_.highlight.border;
    case IdHighlight2Color1: return config_.highlight2.color1;
    case IdHighlight2Color2: return config_.highlight2.color2;
    case IdHighlight2Border: return config_.highlight2.border;
    default: return RGB(0, 0, 0);
    }
}

void ControlWindow::setColorForButton(int id, COLORREF color) {
    // 系统颜色选择器返回 COLORREF，直接写入对应配置字段。
    switch (id) {
    case IdNormalColor1: config_.normal.color1 = color; break;
    case IdNormalColor2: config_.normal.color2 = color; break;
    case IdNormalBorder: config_.normal.border = color; break;
    case IdHighlightColor1: config_.highlight.color1 = color; break;
    case IdHighlightColor2: config_.highlight.color2 = color; break;
    case IdHighlightBorder: config_.highlight.border = color; break;
    case IdHighlight2Color1: config_.highlight2.color1 = color; break;
    case IdHighlight2Color2: config_.highlight2.color2 = color; break;
    case IdHighlight2Border: config_.highlight2.border = color; break;
    default: break;
    }
}

}
