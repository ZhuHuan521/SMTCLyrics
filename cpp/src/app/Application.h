#pragma once

#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/LrcParser.h"
#include "lyrics/LyricRepository.h"
#include "smtc/SmtcProvider.h"
#include "ui/ControlWindow.h"
#include "ui/DesktopLyricsWindow.h"

#include <filesystem>
#include <string>

namespace smtc::app {

class Application {
public:
    Application();
    int run();

private:
    void initialize();
    void tick();
    void applyConfig(const config::AppConfig& config);
    void loadLyricsForCurrentTrack(const smtc_provider::MediaState& state, bool ignoreCache = false, const config::AppConfig* configOverride = nullptr);
    void reloadLyrics(bool ignoreCache);
    void switchLyricsSource();
    void clearLyricCache();
    void openLocalLyric();
    void showTextOnce(const std::wstring& text);

    std::filesystem::path exeDir_;
    std::filesystem::path lyricsDir_;
    config::ConfigStore configStore_;
    config::AppConfig config_;
    cache::LyricCache cache_;
    lyrics::LyricRepository repository_;
    smtc_provider::SmtcProvider smtc_;
    ui::DesktopLyricsWindow window_;
    ui::ControlWindow controlWindow_;
    lyrics::LrcParser parser_;
    lyrics::LyricSource currentSource_ = lyrics::LyricSource::Local;
    std::wstring currentKeyword_;
    std::wstring lastShownText_;
};

}
