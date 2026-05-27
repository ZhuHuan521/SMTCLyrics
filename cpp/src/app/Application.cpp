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

// SMTC 轮询不能过快，否则 WinRT 调用和窗口刷新会占用不必要的 CPU。
constexpr int kMinSmtcPollIntervalMs = 500;
constexpr int kMaxSmtcPollIntervalMs = 2000;
// SMTC1 在换歌初期的位置偶尔会滞后，延迟一段时间后再校准一次。
constexpr long long kSmtc1TrackChangeCalibrationDelayMs = 10000;
// 后台歌词加载线程通过自定义窗口消息把结果交回 UI 线程。
constexpr UINT kLyricsLoadedMessage = WM_APP + 101;

// 跨线程传递的歌词加载结果；由主线程在 handleLyricsLoadedMessage 中接管释放。
struct AsyncLyricLoadResult {
    std::uint64_t requestId = 0;
    std::wstring keyword;
    lyrics::LyricSource source = lyrics::LyricSource::Local;
    lyrics::LrcParser parser;
    bool success = false;
};

// 控制窗口允许配置轮询间隔，这里做最终边界保护。
UINT smtcPollIntervalMs(const config::AppConfig& config) {
    return static_cast<UINT>(std::clamp(config.smtcPollIntervalMs, kMinSmtcPollIntervalMs, kMaxSmtcPollIntervalMs));
}

// 优先使用可执行文件旁的 lyrics 目录，开发运行时也兼容当前工作目录下的 lyrics。
std::filesystem::path chooseLyricsDirectory(const std::filesystem::path& exeDir) {
    auto path = exeDir / L"lyrics";
    if (std::filesystem::is_directory(path)) return path;
    path = std::filesystem::current_path() / L"lyrics";
    if (std::filesystem::is_directory(path)) return path;
    std::filesystem::create_directories(exeDir / L"lyrics");
    return exeDir / L"lyrics";
}

// 配置文件中的歌词源优先级可能缺项或重复；这里归一成 1..4 的完整排列。
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

// “换源”时把当前源移动到队尾，从下一个源开始重新搜索。
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

// 构造阶段只准备路径和轻量对象，真正的窗口/文件初始化放在 initialize。
Application::Application()
    : exeDir_(util::executableDirectory()),
      lyricsDir_(chooseLyricsDirectory(exeDir_)),
      configStore_(exeDir_ / L"config.ini"),
      cache_(exeDir_ / L"cache.json") {}

int Application::run() {
    initialize();

    // 主线程消息循环同时处理窗口消息、定时器和后台歌词加载完成消息。
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
    // 缓存和配置先落地，避免后续 UI 读到缺省文件。
    cache_.ensureExists();
    cache_.load();
    config_ = configStore_.load();
    configStore_.save(config_);
    if (!window_.create(config_.window)) {
        MessageBoxW(nullptr, L"歌词窗口创建失败", L"SMTCLyrics", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return;
    }
    // 桌面歌词窗口只负责显示，位置变化通过回调写回配置。
    window_.applyConfig(config_);
    window_.setDraggable(false);
    window_.setGeometryChangedCallback([this](const config::WindowConfig& window) { rememberLyricWindow(window); });
    rememberLyricWindow(window_.geometry());

    ui::ControlWindowCallbacks callbacks;
    // 控制窗口不直接操作业务对象，而是通过回调把意图交给 Application。
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

    // 从资源文件设置两个窗口的图标。
    if (auto hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON))) {
        SendMessageW(window_.hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(controlWindow_.hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
    }

    showTextOnce(L"等待 SMTC 媒体会话...");
    restartTimers();
}

void Application::smtcTick() {
    try {
        // 每次轮询读取当前媒体会话，若没有有效会话则清空当前歌词状态。
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
        // 接受 SMTC 当前位置，并把总歌词偏移合并到渲染位置基线里。
        const auto syncToSmtcPosition = [&] {
            lastSmtcPositionMs_ = state.positionMs;
            lastAcceptedPositionMs_ = state.positionMs + totalOffsetMs();
            lastSmtcTimestampMs_ = now;
        };

        if (keyword != currentKeyword_) {
            // 曲目变化时重置进度基线并异步加载新歌词。
            currentKeyword_ = keyword;
            syncToSmtcPosition();
            smtc1TrackChangeCalibrationAtMs_ = config_.smtcMode == 1 ? now + kSmtc1TrackChangeCalibrationDelayMs : 0;
            loadLyricsForCurrentTrack(state);
        }

        isPlaying_ = state.playing;

        if (config_.smtcMode == 1) {
            // SMTC1 通常约 1 秒更新一次：平时本地外推，偏差过大时认为发生跳转并重新同步。
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
            // SMTC2 在播放中不持续更新位置，只有原始位置变化时才认为用户进行了 seek。
            if (state.positionMs != lastSmtcPositionMs_) {
                lastSmtcPositionMs_ = state.positionMs;
                lastAcceptedPositionMs_ = state.positionMs + totalOffsetMs();
                lastSmtcTimestampMs_ = now;
            }
        }

        // 轮询 tick 也会渲染一次，保证暂停/跳转后的歌词能立即更新。
        if (!parser_.empty()) {
            const long long now = currentTimeMs();
            const long long renderPos = estimatedPositionMs(now);
            parser_.frameAt(renderPos, config_.displayMode, scratchFrame_);
            showFrameIfChanged(scratchFrame_);
        }
    } catch (...) {
        // SMTC/WinRT 偶发异常不应终止整个 UI 消息循环。
    }
}

void Application::renderTick() {
    // 播放时用高频 render tick 推进高亮动画，避免只跟随 SMTC 低频轮询。
    if (!isPlaying_ || parser_.empty()) return;

    try {
        const long long renderPos = estimatedPositionMs(currentTimeMs());

        parser_.frameAt(renderPos, config_.displayMode, scratchFrame_);
        showFrameIfChanged(scratchFrame_);
    } catch (...) {
        // 绘制过程失败时跳过当前帧，下一帧继续尝试。
    }
}

void Application::applyConfig(const config::AppConfig& config) {
    // 配置变更会影响歌词偏移、样式和轮询周期，因此需要同时更新持久化和运行态。
    config_ = config;
    configStore_.save(config_);
    if (config_.smtcMode != 1) {
        smtc1TrackChangeCalibrationAtMs_ = 0;
    }
    // 歌词偏移变化后重新计算渲染基线。
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
    // SMTC 轮询低频执行，节省 CPU 和 WinRT 调用开销。
    SetTimer(window_.hwnd(), kSmtcTimerId, smtcPollIntervalMs(config_), nullptr);
    // 渲染定时器约 60fps，只在播放且已有歌词时真正刷新。
    SetTimer(window_.hwnd(), kRenderTimerId, kRenderIntervalMs, nullptr);
}

void Application::loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache, const config::AppConfig* configOverride) {
    // 先清空旧帧，避免后台加载期间继续显示上一首歌的歌词进度。
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

    // 在线请求可能阻塞，所以放到后台线程；结果只通过 PostMessage 回到主线程。
    std::thread([requestId, hwnd, exeDir, lyricsDir, keyword, asyncConfig, ignoreCache] {
        auto asyncResult = std::make_unique<AsyncLyricLoadResult>();
        asyncResult->requestId = requestId;
        asyncResult->keyword = keyword;

        try {
            // 后台线程使用自己的 cache/repository 实例，避免跨线程共享可变对象。
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
            // 消息投递成功后所有权交给主线程。
            asyncResult.release();
        }
    }).detach();
    return;
}

void Application::handleLyricsLoadedMessage(LPARAM lParam) {
    // 重新取得后台线程 release 出来的所有权。
    std::unique_ptr<AsyncLyricLoadResult> result(reinterpret_cast<AsyncLyricLoadResult*>(lParam));
    if (!result) return;
    // 防止慢请求覆盖新歌或新一轮手动刷新。
    if (result->requestId != lyricLoadRequestId_ || result->keyword != currentKeyword_) {
        return;
    }

    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;

    if (!result->success) {
        // 加载失败时保持空解析器，窗口显示明确的失败提示。
        parser_ = lyrics::LrcParser{};
        currentSource_ = lyrics::LyricSource::Local;
        showTextOnce(L"未找到歌词：" + result->keyword);
        controlWindow_.setStatusText(L"未找到歌词");
        return;
    }

    parser_ = std::move(result->parser);
    currentSource_ = result->source;
    if (currentSource_ != lyrics::LyricSource::Local) {
        // 记住成功的在线源，下次同一首歌可优先走缓存源。
        const auto keywordUtf8 = util::wideToUtf8(result->keyword);
        cache_.setSource(keywordUtf8, static_cast<int>(currentSource_));
        cache_.save();
    }

    showTextOnce(L"已载入歌词：" + result->keyword);
    controlWindow_.setStatusText(L"已载入歌词");
}

void Application::reloadLyrics(bool ignoreCache) {
    // 重新读取一次 SMTC，避免手动刷新时仍使用过期的曲目信息。
    const auto state = smtc_.readState(config_.smtcMode);
    if (!state.valid || lyrics::makeKeyword(state.artist, state.title).empty()) {
        controlWindow_.setStatusText(L"未检测到正在播放的 SMTC 媒体");
        return;
    }
    loadLyricsForCurrentTrack(state, ignoreCache);
}

void Application::switchLyricsSource() {
    // 换源不改写当前配置，只用临时优先级重新加载当前歌曲。
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
    // 清空内存并立即写回，保证下一次加载不再命中旧源/旧偏移。
    cache_.clear();
    cache_.save();
    controlWindow_.setStatusText(L"歌词源和微调记忆已清除");
}

void Application::saveSongOffset(int offsetMs) {
    // 单曲微调保存到 cache.json，和全局 lyricOffsetMs 叠加生效。
    currentSongOffsetMs_ = offsetMs;
    if (!currentKeyword_.empty()) {
        const auto keywordUtf8 = util::wideToUtf8(currentKeyword_);
        cache_.setOffset(keywordUtf8, offsetMs);
        cache_.save();
    }
    // 偏移变化后重新计算渲染基线。
    lastAcceptedPositionMs_ = lastSmtcPositionMs_ + totalOffsetMs();
    lastShownText_.clear();
    lastHighlightPercent_ = -1;
    lastHighlightLine_ = -1;
}

void Application::openLocalLyric() {
    // 尽量使用当前关键字；没有缓存时临时从 SMTC 读一次。
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
        // 本地歌词不存在时创建空文件，方便用户直接编辑。
        util::writeFileBytes(path, {});
    }
    const auto quotedPath = util::quoteForCommandLine(path);
    const auto result = ShellExecuteW(nullptr, L"open", L"notepad.exe", quotedPath.c_str(), nullptr, SW_SHOWNORMAL);
    controlWindow_.setStatusText(reinterpret_cast<INT_PTR>(result) > 32 ? L"已打开本地歌词" : L"打开本地歌词失败");
}

void Application::rememberLyricWindow(const config::WindowConfig& window) {
    // 拖动或手动输入位置都会走这里，把歌词窗口几何同步到配置文件和控制窗口。
    if (!window.hasPosition) return;
    config_.window = window;
    configStore_.saveWindow(window);
    controlWindow_.syncLyricGeometry(window);
}

std::array<bool, 4> Application::checkLyricSources() {
    // 用一个固定中文关键字分别测试四个在线源，结果显示在控制窗口。
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
    // 状态提示不需要重复刷同一段文本。
    if (text != lastShownText_) {
        window_.updateLyrics(text, 0);
        lastShownText_ = text;
        lastHighlightPercent_ = 0;
        lastHighlightLine_ = 0;
    }
}

void Application::showFrameIfChanged(const lyrics::LyricFrame& frame) {
    if (frame.text.empty()) return;
    if (frame.text == lastShownText_ && frame.highlightPercent == lastHighlightPercent_ && frame.highlightLine == lastHighlightLine_) {
        return;
    }

    window_.updateLyrics(frame.text, frame.highlightPercent, frame.highlightLine);
    if (frame.text != lastShownText_) {
        lastShownText_ = frame.text;
    }
    lastHighlightPercent_ = frame.highlightPercent;
    lastHighlightLine_ = frame.highlightLine;
}

long long Application::estimatedPositionMs(long long now) const {
    // 暂停时固定在最后接受的位置；播放时按本地时间差外推。
    return isPlaying_ ? lastAcceptedPositionMs_ + (now - lastSmtcTimestampMs_) : lastAcceptedPositionMs_;
}

long long Application::currentTimeMs() const {
    // QueryPerformanceCounter 是单调高精度时钟，适合做歌词动画插值。
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
