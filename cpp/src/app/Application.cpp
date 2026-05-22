#include "app/Application.h"

#include "util/Encoding.h"
#include "util/Path.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace smtc::app {
namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 800;

std::filesystem::path chooseLyricsDirectory(const std::filesystem::path& exeDir) {
    auto path = exeDir / L"lyrics";
    if (std::filesystem::is_directory(path)) return path;
    path = std::filesystem::current_path() / L"lyrics";
    if (std::filesystem::is_directory(path)) return path;
    std::filesystem::create_directories(exeDir / L"lyrics");
    return exeDir / L"lyrics";
}

std::vector<int> normalizedSourcePriority(const std::vector<int>& priority) {
    std::vector<int> result;
    for (int source : priority) {
        if (source >= 1 && source <= 4 && std::find(result.begin(), result.end(), source) == result.end()) {
            result.push_back(source);
        }
    }
    for (int source = 1; source <= 4; ++source) {
        if (std::find(result.begin(), result.end(), source) == result.end()) {
            result.push_back(source);
        }
    }
    return result;
}

std::vector<int> sourcePriorityAfter(const std::vector<int>& priority, lyrics::LyricSource currentSource) {
    auto result = normalizedSourcePriority(priority);
    const int current = static_cast<int>(currentSource);
    if (current <= 0) return result;
    const auto currentIt = std::find(result.begin(), result.end(), current);
    if (currentIt != result.end()) {
        std::rotate(result.begin(), std::next(currentIt), result.end());
    }
    return result;
}

}

Application::Application()
    : exeDir_(util::executableDirectory()),
      lyricsDir_(chooseLyricsDirectory(exeDir_)),
      configStore_(exeDir_ / L"config.ini"),
      cache_(exeDir_ / L"cache.json"),
      repository_(lyricsDir_, cache_) {}

int Application::run() {
    initialize();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER && message.hwnd == window_.hwnd() && message.wParam == kTimerId) {
            tick();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void Application::initialize() {
    cache_.ensureExists();
    cache_.load();
    config_ = configStore_.load();
    if (!window_.create(config_.window)) {
        MessageBoxW(nullptr, L"歌词窗口创建失败", L"SMTCLyrics", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return;
    }
    window_.applyConfig(config_);
    window_.setDraggable(true);

    ui::ControlWindowCallbacks callbacks;
    callbacks.applyConfig = [this](const config::AppConfig& config) { applyConfig(config); };
    callbacks.getLyricGeometry = [this] { return window_.geometry(); };
    callbacks.moveLyricWindow = [this](const config::WindowConfig& window) {
        if (!window.hasPosition) return;
        config_.window = window;
        window_.move(window.left, window.top, window.width, window.height);
        configStore_.saveWindow(window);
        controlWindow_.setConfig(config_);
    };
    callbacks.setLyricDraggable = [this](bool draggable) { window_.setDraggable(draggable); };
    callbacks.reloadLyrics = [this] { reloadLyrics(true); };
    callbacks.switchSource = [this] { switchLyricsSource(); };
    callbacks.clearCache = [this] { clearLyricCache(); };
    callbacks.openLocalLyric = [this] { openLocalLyric(); };
    if (!controlWindow_.create(config_, std::move(callbacks))) {
        MessageBoxW(nullptr, L"控制窗口创建失败", L"SMTCLyrics", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return;
    }
    controlWindow_.show();

    showTextOnce(L"等待 SMTC 媒体会话...");
    SetTimer(window_.hwnd(), kTimerId, kTimerMs, nullptr);
}

void Application::tick() {
    const auto state = smtc_.readState(config_.smtcMode);
    if (!state.valid) {
        currentKeyword_.clear();
        currentSource_ = lyrics::LyricSource::Local;
        parser_ = lyrics::LrcParser{};
        showTextOnce(L"未检测到正在播放的 SMTC 媒体");
        return;
    }

    const auto keyword = lyrics::makeKeyword(state.artist, state.title);
    if (keyword.empty()) {
        return;
    }
    if (keyword != currentKeyword_) {
        currentKeyword_ = keyword;
        loadLyricsForCurrentTrack(state);
    }

    if (!parser_.empty()) {
        const auto frame = parser_.frameAt(static_cast<std::int64_t>(state.positionSeconds) * 1000, config_.displayMode, config_.lyricOffsetSeconds);
        if (!frame.text.empty() && frame.text != lastShownText_) {
            window_.updateLyrics(frame.text, frame.highlightPercent);
            lastShownText_ = frame.text;
        }
    }
}

void Application::applyConfig(const config::AppConfig& config) {
    config_ = config;
    configStore_.save(config_);
    window_.applyConfig(config_);
    lastShownText_.clear();
    controlWindow_.setConfig(config_);
}

void Application::loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache, const config::AppConfig* configOverride) {
    lastShownText_.clear();
    parser_ = lyrics::LrcParser{};
    currentSource_ = lyrics::LyricSource::Local;
    const auto keyword = lyrics::makeKeyword(state.artist, state.title);
    currentKeyword_ = keyword;
    const auto& activeConfig = configOverride ? *configOverride : config_;
    const auto result = repository_.loadForKeyword(keyword, activeConfig, ignoreCache);
    if (result.lrcBytes.empty() || !parser_.parseBytes(result.lrcBytes)) {
        showTextOnce(L"未找到歌词：" + keyword);
        controlWindow_.setStatusText(L"未找到歌词");
        return;
    }
    currentSource_ = result.source;
    showTextOnce(L"已载入歌词：" + keyword);
    controlWindow_.setStatusText(L"已载入歌词");
}

void Application::reloadLyrics(bool ignoreCache) {
    const auto state = smtc_.readState(config_.smtcMode);
    if (!state.valid || lyrics::makeKeyword(state.artist, state.title).empty()) {
        controlWindow_.setStatusText(L"未检测到正在播放的 SMTC 媒体");
        return;
    }
    loadLyricsForCurrentTrack(state, ignoreCache);
}

void Application::switchLyricsSource() {
    const auto state = smtc_.readState(config_.smtcMode);
    if (!state.valid || lyrics::makeKeyword(state.artist, state.title).empty()) {
        controlWindow_.setStatusText(L"未检测到正在播放的 SMTC 媒体");
        return;
    }
    auto config = config_;
    config.sourcePriority = sourcePriorityAfter(config_.sourcePriority, currentSource_);
    loadLyricsForCurrentTrack(state, true, &config);
    controlWindow_.setStatusText(L"已尝试切换歌词源");
}

void Application::clearLyricCache() {
    cache_.clear();
    cache_.save();
    controlWindow_.setStatusText(L"歌词缓存已清除");
}

void Application::openLocalLyric() {
    auto keyword = currentKeyword_;
    if (keyword.empty()) {
        const auto state = smtc_.readState(config_.smtcMode);
        if (state.valid) keyword = lyrics::makeKeyword(state.artist, state.title);
    }
    if (keyword.empty()) {
        controlWindow_.setStatusText(L"未检测到正在播放的 SMTC 媒体");
        return;
    }

    std::filesystem::create_directories(lyricsDir_);
    const auto path = lyricsDir_ / (keyword + L".lrc");
    if (!std::filesystem::exists(path)) {
        util::writeFileBytes(path, {});
    }
    const auto quotedPath = util::quoteForCommandLine(path);
    const auto result = ShellExecuteW(nullptr, L"open", L"notepad.exe", quotedPath.c_str(), nullptr, SW_SHOWNORMAL);
    controlWindow_.setStatusText(reinterpret_cast<INT_PTR>(result) > 32 ? L"已打开本地歌词" : L"打开本地歌词失败");
}

void Application::showTextOnce(const std::wstring& text) {
    if (text != lastShownText_) {
        window_.updateLyrics(text, 0);
        lastShownText_ = text;
    }
}

}
