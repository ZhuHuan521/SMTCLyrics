#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <winrt/Windows.Media.Control.h>

namespace smtc::smtc_provider {

struct MediaState {
    std::wstring artist;
    std::wstring title;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    bool valid = false;
    bool playing = false;
};

class SmtcProvider {
public:
    SmtcProvider();
    ~SmtcProvider();

    MediaState readState(int mode);
    void shutdown();

private:
    bool ensureManager();
    void clearCachedSession();

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session_{nullptr};
    std::wstring lastSessionId_;
    std::wstring lastArtist_;
    std::wstring lastTitle_;
    std::int64_t lastRawPositionMs_ = -1;
    std::chrono::steady_clock::time_point lastObserved_{};
    std::chrono::steady_clock::time_point lastManagerRequest_{};
    std::chrono::steady_clock::time_point lastPropertiesRead_{};
    bool apartmentInitialized_ = false;
};

}
