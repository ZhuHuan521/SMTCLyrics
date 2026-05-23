#include "smtc/SmtcProvider.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace smtc::smtc_provider {
namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::int64_t millisecondsFromTimeSpan(winrt::Windows::Foundation::TimeSpan value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

std::wstring hstringToWide(const winrt::hstring& text) {
    return std::wstring(text.c_str(), text.size());
}

}

SmtcProvider::SmtcProvider() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized_ = true;
    } catch (...) {
        apartmentInitialized_ = false;
    }
}

SmtcProvider::~SmtcProvider() {
    shutdown();
}

MediaState SmtcProvider::readState(int mode) {
    MediaState state;
    if (!apartmentInitialized_) return state;

    try {
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const auto session = manager.GetCurrentSession();
        if (!session) return state;

        const auto mediaProperties = session.TryGetMediaPropertiesAsync().get();
        const auto timeline = session.GetTimelineProperties();
        const auto playback = session.GetPlaybackInfo();

        state.artist = hstringToWide(mediaProperties.AlbumArtist());
        if (state.artist.empty()) {
            state.artist = hstringToWide(mediaProperties.Artist());
        }
        state.title = hstringToWide(mediaProperties.Title());
        state.durationMs = millisecondsFromTimeSpan(timeline.MaxSeekTime());
        const auto rawPositionMs = millisecondsFromTimeSpan(timeline.Position());
        state.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

        const auto now = std::chrono::steady_clock::now();
        if (lastObserved_ == std::chrono::steady_clock::time_point{} || state.title != lastTitle_ || rawPositionMs != lastRawPositionMs_) {
            lastObserved_ = now;
            lastTitle_ = state.title;
            lastRawPositionMs_ = rawPositionMs;
        }

        if (mode == 2 && state.playing) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastObserved_).count();
            state.positionMs = rawPositionMs + elapsed;
        } else {
            state.positionMs = rawPositionMs;
        }

        if (!state.playing) {
            lastObserved_ = now;
            lastRawPositionMs_ = rawPositionMs;
        }

        if (state.durationMs > 0) {
            state.positionMs = std::clamp(state.positionMs, std::int64_t{0}, state.durationMs);
        }
        state.valid = !state.title.empty();
    } catch (...) {
        state = {};
    }

    return state;
}

void SmtcProvider::shutdown() {
    if (apartmentInitialized_) {
        winrt::uninit_apartment();
        apartmentInitialized_ = false;
    }
}

}
