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

constexpr auto kManagerRetryInterval = std::chrono::seconds(5);
constexpr auto kMediaPropertiesRefreshInterval = std::chrono::seconds(1);
constexpr auto kPrecisePositionMaxAge = std::chrono::milliseconds(3000);

std::int64_t millisecondsFromTimeSpan(winrt::Windows::Foundation::TimeSpan value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

std::int64_t millisecondsSinceDateTime(winrt::Windows::Foundation::DateTime value) {
    if (value.time_since_epoch().count() <= 0) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(winrt::clock::now() - value).count();
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
        if (!ensureManager()) return state;

        const auto session = manager_.GetCurrentSession();
        if (!session) {
            clearCachedSession();
            return state;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto sessionId = hstringToWide(session.SourceAppUserModelId());
        const auto timeline = session.GetTimelineProperties();
        const auto playback = session.GetPlaybackInfo();
        const auto rawPositionMs = millisecondsFromTimeSpan(timeline.Position());
        const auto timelineUpdatedAgeMs = millisecondsSinceDateTime(timeline.LastUpdatedTime());
        const bool timelineUpdatedRecently = timelineUpdatedAgeMs >= 0 &&
            timelineUpdatedAgeMs <= kPrecisePositionMaxAge.count();

        const bool sessionChanged = !session_ || sessionId != lastSessionId_;
        const bool positionRestarted = lastRawPositionMs_ > 3000 && rawPositionMs < 1000;
        const bool propertiesExpired = lastPropertiesRead_ == std::chrono::steady_clock::time_point{} ||
            now - lastPropertiesRead_ >= kMediaPropertiesRefreshInterval;

        if (sessionChanged) {
            session_ = session;
            lastSessionId_ = sessionId;
            lastArtist_.clear();
            lastTitle_.clear();
            lastObserved_ = {};
            lastPropertiesRead_ = {};
        }

        bool mediaPropertiesChanged = false;
        if (sessionChanged || positionRestarted || lastTitle_.empty() || propertiesExpired) {
            const auto previousTitle = lastTitle_;
            const auto mediaProperties = session.TryGetMediaPropertiesAsync().get();
            lastArtist_ = hstringToWide(mediaProperties.AlbumArtist());
            if (lastArtist_.empty()) {
                lastArtist_ = hstringToWide(mediaProperties.Artist());
            }
            lastTitle_ = hstringToWide(mediaProperties.Title());
            lastPropertiesRead_ = now;
            mediaPropertiesChanged = lastTitle_ != previousTitle;
        }

        state.artist = lastArtist_;
        state.title = lastTitle_;
        state.durationMs = millisecondsFromTimeSpan(timeline.MaxSeekTime());
        state.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

        if (lastObserved_ == std::chrono::steady_clock::time_point{} || mediaPropertiesChanged || rawPositionMs != lastRawPositionMs_) {
            lastObserved_ = now;
            lastRawPositionMs_ = rawPositionMs;
        }

        if (mode == 1 && state.playing && timelineUpdatedRecently) {
            state.positionMs = rawPositionMs + timelineUpdatedAgeMs;
        } else if (mode == 2 && state.playing) {
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
        clearCachedSession();
        manager_ = nullptr;
        state = {};
    }

    return state;
}

void SmtcProvider::shutdown() {
    if (apartmentInitialized_) {
        clearCachedSession();
        manager_ = nullptr;
        winrt::uninit_apartment();
        apartmentInitialized_ = false;
    }
}

bool SmtcProvider::ensureManager() {
    if (manager_) return true;

    const auto now = std::chrono::steady_clock::now();
    if (lastManagerRequest_ != std::chrono::steady_clock::time_point{} &&
        now - lastManagerRequest_ < kManagerRetryInterval) {
        return false;
    }

    lastManagerRequest_ = now;
    manager_ = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    return manager_ != nullptr;
}

void SmtcProvider::clearCachedSession() {
    session_ = nullptr;
    lastSessionId_.clear();
    lastArtist_.clear();
    lastTitle_.clear();
    lastRawPositionMs_ = -1;
    lastObserved_ = {};
    lastPropertiesRead_ = {};
}

}
