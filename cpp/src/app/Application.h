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

class Application {
public:
    Application();
    int run();

private:
    void initialize();
    void smtcTick();
    void renderTick();
    void applyConfig(const config::AppConfig& config);
    void restartTimers();
    void loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache = false, const config::AppConfig* configOverride = nullptr);
    void handleLyricsLoadedMessage(LPARAM lParam);
    void reloadLyrics(bool ignoreCache);
    void switchLyricsSource();
    void clearLyricCache();
    void openLocalLyric();
    void saveSongOffset(int offsetMs);
    void rememberLyricWindow(const config::WindowConfig& window);
    static std::array<bool, 4> checkLyricSources();
    void showTextOnce(const std::wstring& text);
    long long estimatedPositionMs(long long now) const;
    long long currentTimeMs() const;
    int totalOffsetMs() const { return config_.lyricOffsetMs + currentSongOffsetMs_; }

    std::filesystem::path exeDir_;
    std::filesystem::path lyricsDir_;
    config::ConfigStore configStore_;
    config::AppConfig config_;
    cache::LyricCache cache_;
    smtc_provider::SmtcProvider smtc_;
    ui::DesktopLyricsWindow window_;
    ui::ControlWindow controlWindow_;
    lyrics::LrcParser parser_;
    lyrics::LyricSource currentSource_ = lyrics::LyricSource::Local;
    std::wstring currentKeyword_;
    int currentSongOffsetMs_ = 0;
    std::wstring lastShownText_;
    int lastHighlightPercent_ = -1;
    int lastHighlightLine_ = -1;
    std::uint64_t lyricLoadRequestId_ = 0;

    // Interpolation state for smooth rendering between SMTC polls
    long long lastSmtcPositionMs_ = 0;
    long long lastSmtcTimestampMs_ = 0;
    long long lastAcceptedPositionMs_ = 0;  // last position we accepted (no going back)
    long long smtc1TrackChangeCalibrationAtMs_ = 0;
    bool isPlaying_ = false;

    static constexpr UINT_PTR kSmtcTimerId = 1;
    static constexpr UINT_PTR kRenderTimerId = 2;
    static constexpr int kRenderIntervalMs = 16;  // ~60fps for animation
    static constexpr long long kMaxPositionJumpMs = 500;  // max allowed backward jump
};

}
