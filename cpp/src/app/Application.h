#pragma once

#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/LrcParser.h"
#include "lyrics/OnlineLyrics.h"
#include "smtc/SmtcProvider.h"
#include "ui/ControlWindow.h"
#include "ui/DesktopLyricsWindow.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace smtc::app {

// 应用总控类：串联配置、SMTC 媒体状态、歌词加载/解析、控制窗口和桌面歌词窗口。
class Application {
public:
    Application();
    // 进入 Win32 消息循环；返回值作为进程退出码。
    int run();

private:
    // 初始化持久化文件、窗口、回调和定时器。
    void initialize();
    // 低频读取 SMTC 状态，检测换歌/暂停/跳转，并触发歌词加载。
    void smtcTick();
    // 高频渲染插值后的歌词进度，让高亮动画更顺滑。
    void renderTick();
    // 接收控制窗口的新配置，并同步到存储、窗口和计时状态。
    void applyConfig(const config::AppConfig& config);
    // 根据配置重启 SMTC 轮询定时器和渲染定时器。
    void restartTimers();
    // 为当前曲目异步加载歌词；可选择绕过缓存或使用临时配置。
    void loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache = false, const config::AppConfig* configOverride = nullptr);
    // 接收后台歌词加载线程发回主线程的结果。
    void handleLyricsLoadedMessage(LPARAM lParam);
    // 手动重新获取当前曲目的歌词。
    void reloadLyrics(bool ignoreCache);
    // 从当前歌词源之后开始轮转优先级，用于“换源”。
    void switchLyricsSource();
    // 清空歌词源缓存和单曲微调缓存。
    void clearLyricCache();
    // 创建/打开当前歌曲对应的本地 .lrc 文件。
    void openLocalLyric();
    // 保存当前歌曲独立的歌词时间偏移。
    void saveSongOffset(int offsetMs);
    // 保存歌词窗口的位置和尺寸，供下次启动恢复。
    void rememberLyricWindow(const config::WindowConfig& window);
    // 用固定关键字探测四个在线歌词源是否还能返回可解析歌词。
    static std::array<bool, 4> checkLyricSources();
    // 只在文本变化时刷新歌词窗口，减少无意义重绘。
    void showTextOnce(const std::wstring& text);
    void showFrameIfChanged(const lyrics::LyricFrame& frame);
    // 用最近一次 SMTC 位置和本地高精度时钟估算当前播放进度。
    long long estimatedPositionMs(long long now) const;
    // 高精度单调时钟，单位毫秒。
    long long currentTimeMs() const;
    // 全局歌词偏移 + 单曲微调偏移。
    int totalOffsetMs() const { return config_.lyricOffsetMs + currentSongOffsetMs_; }

    // 程序目录、歌词目录以及持久化存储。
    std::filesystem::path exeDir_;
    std::filesystem::path lyricsDir_;
    config::ConfigStore configStore_;
    config::AppConfig config_;
    cache::LyricCache cache_;
    // 核心协作者：媒体状态读取、两个窗口和当前歌词解析器。
    smtc_provider::SmtcProvider smtc_;
    ui::DesktopLyricsWindow window_;
    ui::ControlWindow controlWindow_;
    lyrics::LrcParser parser_;
    // 当前歌曲、歌词源和渲染去重状态。
    lyrics::LyricSource currentSource_ = lyrics::LyricSource::Local;
    std::wstring currentKeyword_;
    int currentSongOffsetMs_ = 0;
    lyrics::LyricFrame scratchFrame_;
    std::wstring lastShownText_;
    int lastHighlightPercent_ = -1;
    int lastHighlightLine_ = -1;
    std::uint64_t lyricLoadRequestId_ = 0;

    // SMTC 轮询之间的插值状态，用于让歌词进度平滑前进。
    long long lastSmtcPositionMs_ = 0;
    long long lastSmtcTimestampMs_ = 0;
    long long lastAcceptedPositionMs_ = 0;  // 最近一次接受的播放位置，避免轻微回跳导致歌词抖动。
    long long smtc1TrackChangeCalibrationAtMs_ = 0;
    bool isPlaying_ = false;

    // 两个 Win32 定时器：一个轮询 SMTC，一个驱动歌词高亮动画。
    static constexpr UINT_PTR kSmtcTimerId = 1;
    static constexpr UINT_PTR kRenderTimerId = 2;
    static constexpr int kRenderIntervalMs = 16;  // 约 60fps。
    static constexpr long long kMaxPositionJumpMs = 500;  // 预留的最大回跳阈值。
};

}
