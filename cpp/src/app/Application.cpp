#include "app/Application.h"

#include "lyrics/LyricRepository.h"
#include "resource.h"
#include "util/Encoding.h"
#include "util/Path.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace smtc::app {
namespace {

constexpr int kMinSmtcPollIntervalMs = 500;
constexpr int kMaxSmtcPollIntervalMs = 2000;
constexpr long long kSmtc1TrackChangeCalibrationDelayMs = 10000;
constexpr UINT kLyricsLoadedMessage = WM_APP + 101;

struct AsyncLyricLoadResult {
    std::uint64_t requestId = 0;
    std::wstring keyword;
    lyrics::LyricSource source = lyrics::LyricSource::Local;
    lyrics::LrcParser parser;
    bool success = false;
};

UINT smtcPollIntervalMs(const config::AppConfig& config) {
    return static_cast<UINT>(std::clamp(config.smtcPollIntervalMs, kMinSmtcPollIntervalMs, kMaxSmtcPollIntervalMs));
}

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
      cache_(exeDir_ / L"cache.json") {}

int Application::run() {
    initialize();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kLyricsLoadedMessage) {
            handleLyricsLoadedMessage(message.lParam);
            continue;
        }
        if (message.message == WM_TIMER) {
            if (message.wParam == kSmtcTimerId) {
                smtcTick();
                continue;
            }
            if (message.wParam == kRenderTimerId) {
                renderTick();
                continue;
            }
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
    configStore_.save(config_);
    if (!window_.create(config_.window)) {
        MessageBoxW(nullptr, L"歌词窗口创建失败", L"SMTCLyrics", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return;
    }
    window_.applyConfig(config_);
    window_.setDraggable(false);
    window_.setGeometryChangedCallback([this](const config::WindowConfig& window) { rememberLyricWindow(window); });
    rememberLyricWindow(window_.geometry());

    ui::ControlWindowCallbacks callbacks;
    callbacks.applyConfig = [this](const config::AppConfig& config) { applyConfig(config); };
    callbacks.getLyricGeometry = [this] { return window_.geometry(); };
    callbacks.moveLyricWindow = [this](const config::WindowConfig& window) {
        if (!window.hasPosition) return;
        window_.move(window.left, window.top, window.width, window.height);
    };
    callbacks.setLyricDraggable = [this](bool draggable) { window_.setDraggable(draggable); };
    callbacks.reloadLyrics = [this] { reloadLyrics(true); };
    callbacks.switchSource = [this] { switchLyricsSource(); };
    callbacks.clearCache = [this] { clearLyricCache(); };
    callbacks.openLocalLyric = [this] { openLocalLyric(); };
    callbacks.checkLyricSources = [] { return checkLyricSources(); };
    callbacks.saveSongOffset = [this](int ms) { saveSongOffset(ms); };
    if (!controlWindow_.create(config_, std::move(callbacks))) {
        MessageBoxW(nullptr, L"控制窗口创建失败", L"SMTCLyrics", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return;
    }
    controlWindow_.show();

    // Set window icon from embedded resource
    if (auto hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON))) {
        SendMessageW(window_.hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(controlWindow_.hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
    }

    showTextOnce(L"等待 SMTC 媒体会话...");
    restartTimers();
}

void Application::smtcTick() {
    try {
        const auto state = smtc_.readState(config_.smtcMode);
        if (!state.valid) {
            ++lyricLoadRequestId_;
            currentKeyword_.clear();
            currentSource_ = lyrics::LyricSource::Local;
            parser_ = lyrics::LrcParser{};
            smtc1TrackChangeCalibrationAtMs_ = 0;
            isPlaying_ = false;
            showTextOnce(L"未检测到正在播放的 SMTC 媒体");
            return;
        }

        const auto keyword = lyrics::makeKeyword(state.artist, state.title);
        if (keyword.empty()) {
            return;
        }

        const long long now = currentTimeMs();
        const auto syncToSmtcPosition = [&] {
            lastSmtcPositionMs_ = state.positionMs;
            lastAcceptedPositionMs_ = state.positionMs + totalOffsetMs();
            lastSmtcTimestampMs_ = now;
        };

        if (keyword != currentKeyword_) {
            currentKeyword_ = keyword;
            syncToSmtcPosition();
            smtc1TrackChangeCalibrationAtMs_ = config_.smtcMode == 1 ? now + kSmtc1TrackChangeCalibrationDelayMs : 0;
            loadLyricsForCurrentTrack(state);
        }

        isPlaying_ = state.playing;

        if (config_.smtcMode == 1) {
            // SMTC1: updates ~1000ms, use local extrapolation + ±1500ms calibration
            // Use SMTC position for delta calculation; large deltas are treated as seek/jump.
            const long long localEstimate = lastSmtcPositionMs_ + (now - lastSmtcTimestampMs_);
            const long long delta = state.positionMs - localEstimate;
            const bool trackChangeCalibrationDue = smtc1TrackChangeCalibrationAtMs_ > 0 && now >= smtc1TrackChangeCalibrationAtMs_;

            if (state.playing) {
                if (trackChangeCalibrationDue) {
                    syncToSmtcPosition();
                } else if (std::abs(delta) > 1500) {
                    syncToSmtcPosition();
                }
            } else {
                syncToSmtcPosition();
            }
            if (trackChangeCalibrationDue) {
                smtc1TrackChangeCalibrationAtMs_ = 0;
            }
        } else {
            smtc1TrackChangeCalibrationAtMs_ = 0;
            // SMTC2: only updates on play/pause/seek, no updates during playback
            // Calibrate when raw SMTC position changes (seek detected)
            if (state.positionMs != lastSmtcPositionMs_) {
                lastSmtcPositionMs_ = state.positionMs;
                lastAcceptedPositionMs_ = state.positionMs + totalOffsetMs();
                lastSmtcTimestampMs_ = now;
            }
        }

        // Render
        if (!parser_.empty()) {
            const long long now = currentTimeMs();
            const long long renderPos = estimatedPositionMs(now);
            const auto frame = parser_.frameAt(renderPos, config_.displayMode);
            if (!frame.text.empty() && (frame.text != lastShownText_ || frame.highlightPercent != lastHighlightPercent_ || frame.highlightLine != lastHighlightLine_)) {
                window_.updateLyrics(frame.text, frame.highlightPercent, frame.highlightLine);
                lastShownText_ = frame.text;
                lastHighlightPercent_ = frame.highlightPercent;
                lastHighlightLine_ = frame.highlightLine;
            }
        }
    } catch (...) {
    }
}

void Application::renderTick() {
    // Both modes need render tick for smooth extrapolation when playing
    if (!isPlaying_ || parser_.empty()) return;

    try {
        const long long renderPos = estimatedPositionMs(currentTimeMs());

        const auto frame = parser_.frameAt(renderPos, config_.displayMode);
        if (!frame.text.empty() && (frame.text != lastShownText_ || frame.highlightPercent != lastHighlightPercent_ || frame.highlightLine != lastHighlightLine_)) {
            window_.updateLyrics(frame.text, frame.highlightPercent, frame.highlightLine);
            lastShownText_ = frame.text;
            lastHighlightPercent_ = frame.highlightPercent;
            lastHighlightLine_ = frame.highlightLine;
        }
    } catch (...) {
    }
}

void Application::applyConfig(const config::AppConfig& config) {
    config_ = config;
    configStore_.save(config_);
    if (config_.smtcMode != 1) {
        smtc1TrackChangeCalibrationAtMs_ = 0;
    }
    // Recalculate render position with new offset
    lastAcceptedPositionMs_ = lastSmtcPositionMs_ + totalOffsetMs();
    window_.applyConfig(config_);
    restartTimers();
    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;
    controlWindow_.setConfig(config_);
}

void Application::restartTimers() {
    if (!window_.hwnd()) return;
    KillTimer(window_.hwnd(), kSmtcTimerId);
    KillTimer(window_.hwnd(), kRenderTimerId);
    // SMTC polling at lower frequency to save CPU
    SetTimer(window_.hwnd(), kSmtcTimerId, smtcPollIntervalMs(config_), nullptr);
    // Render timer at ~60fps for smooth animation
    SetTimer(window_.hwnd(), kRenderTimerId, kRenderIntervalMs, nullptr);
}

void Application::loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache, const config::AppConfig* configOverride) {
    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;
    parser_ = lyrics::LrcParser{};
    currentSource_ = lyrics::LyricSource::Local;
    const auto keyword = lyrics::makeKeyword(state.artist, state.title);
    currentKeyword_ = keyword;
    const auto keywordUtf8 = util::wideToUtf8(keyword);
    currentSongOffsetMs_ = cache_.offsetFor(keywordUtf8).value_or(0);
    controlWindow_.setSongOffset(currentSongOffsetMs_);
    showTextOnce(L"正在解析歌词...");
    controlWindow_.setStatusText(L"正在解析歌词...");

    const auto asyncConfig = configOverride ? *configOverride : config_;
    const auto requestId = ++lyricLoadRequestId_;
    const auto hwnd = window_.hwnd();
    const auto exeDir = exeDir_;
    const auto lyricsDir = lyricsDir_;

    std::thread([requestId, hwnd, exeDir, lyricsDir, keyword, asyncConfig, ignoreCache] {
        auto asyncResult = std::make_unique<AsyncLyricLoadResult>();
        asyncResult->requestId = requestId;
        asyncResult->keyword = keyword;

        try {
            cache::LyricCache cache(exeDir / L"cache.json");
            cache.load();
            lyrics::LyricRepository repository(lyricsDir, cache);
            const auto result = repository.loadForKeyword(keyword, asyncConfig, ignoreCache);
            asyncResult->source = result.source;
            asyncResult->success = !result.lrcBytes.empty() && asyncResult->parser.parseBytes(result.lrcBytes);
        } catch (...) {
            asyncResult->success = false;
        }

        if (PostMessageW(hwnd, kLyricsLoadedMessage, 0, reinterpret_cast<LPARAM>(asyncResult.get()))) {
            asyncResult.release();
        }
    }).detach();
    return;
}

void Application::handleLyricsLoadedMessage(LPARAM lParam) {
    std::unique_ptr<AsyncLyricLoadResult> result(reinterpret_cast<AsyncLyricLoadResult*>(lParam));
    if (!result) return;
    if (result->requestId != lyricLoadRequestId_ || result->keyword != currentKeyword_) {
        return;
    }

    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;

    if (!result->success) {
        parser_ = lyrics::LrcParser{};
        currentSource_ = lyrics::LyricSource::Local;
        showTextOnce(L"未找到歌词：" + result->keyword);
        controlWindow_.setStatusText(L"未找到歌词");
        return;
    }

    parser_ = std::move(result->parser);
    currentSource_ = result->source;
    if (currentSource_ != lyrics::LyricSource::Local) {
        const auto keywordUtf8 = util::wideToUtf8(result->keyword);
        cache_.setSource(keywordUtf8, static_cast<int>(currentSource_));
        cache_.save();
    }

    showTextOnce(L"已载入歌词：" + result->keyword);
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
}

void Application::clearLyricCache() {
    cache_.clear();
    cache_.save();
    controlWindow_.setStatusText(L"歌词源和微调记忆已清除");
}

void Application::saveSongOffset(int offsetMs) {
    currentSongOffsetMs_ = offsetMs;
    if (!currentKeyword_.empty()) {
        const auto keywordUtf8 = util::wideToUtf8(currentKeyword_);
        cache_.setOffset(keywordUtf8, offsetMs);
        cache_.save();
    }
    // Recalculate render position with new total offset
    lastAcceptedPositionMs_ = lastSmtcPositionMs_ + totalOffsetMs();
    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;
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

void Application::rememberLyricWindow(const config::WindowConfig& window) {
    if (!window.hasPosition) return;
    config_.window = window;
    configStore_.saveWindow(window);
    controlWindow_.syncLyricGeometry(window);
}

std::array<bool, 4> Application::checkLyricSources() {
    std::array<bool, 4> result{};
    const auto keywordUtf8 = util::wideToUtf8(L"关键词");
    lyrics::OnlineLyrics online;

    for (int sourceIndex = 1; sourceIndex <= 4; ++sourceIndex) {
        try {
            const auto source = lyrics::sourceFromIndex(sourceIndex);
            if (!source) continue;
            const auto bytes = online.fetch(*source, keywordUtf8);
            lyrics::LrcParser parser;
            result[static_cast<std::size_t>(sourceIndex - 1)] = bytes.size() >= 10 && parser.parseBytes(bytes);
        } catch (...) {
            result[static_cast<std::size_t>(sourceIndex - 1)] = false;
        }
    }

    return result;
}

void Application::showTextOnce(const std::wstring& text) {
    if (text != lastShownText_) {
        window_.updateLyrics(text, 0);
        lastShownText_ = text;
        lastHighlightPercent_ = 0;
        lastHighlightLine_ = 0;
    }
}

long long Application::estimatedPositionMs(long long now) const {
    return isPlaying_ ? lastAcceptedPositionMs_ + (now - lastSmtcTimestampMs_) : lastAcceptedPositionMs_;
}

long long Application::currentTimeMs() const {
    static const long long frequency = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000) / frequency;
}

}
