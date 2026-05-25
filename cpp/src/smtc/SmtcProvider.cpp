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

// Manager 创建失败时隔一段时间再重试，避免每个 tick 都阻塞。
constexpr auto kManagerRetryInterval = std::chrono::seconds(5);
// 媒体属性读取较慢，缓存歌手/标题并按固定间隔刷新。
constexpr auto kMediaPropertiesRefreshInterval = std::chrono::seconds(1);
// SMTC timeline 的 LastUpdatedTime 过旧时，不再用它修正播放位置。
constexpr auto kPrecisePositionMaxAge = std::chrono::milliseconds(3000);

// WinRT TimeSpan/DateTime 转成本项目统一使用的毫秒。
std::int64_t millisecondsFromTimeSpan(winrt::Windows::Foundation::TimeSpan value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

std::int64_t millisecondsSinceDateTime(winrt::Windows::Foundation::DateTime value) {
    if (value.time_since_epoch().count() <= 0) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(winrt::clock::now() - value).count();
}

// hstring 的数据在 WinRT 对象里，拷贝成 std::wstring 便于缓存。
std::wstring hstringToWide(const winrt::hstring& text) {
    return std::wstring(text.c_str(), text.size());
}

}

SmtcProvider::SmtcProvider() {
    try {
        // SMTC API 是 WinRT API，需要在线程上初始化 apartment。
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
        // 懒加载 manager，再读取当前全局媒体会话。
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
            // 当前播放应用变化时，清空上一会话缓存的标题和位置基线。
            session_ = session;
            lastSessionId_ = sessionId;
            lastArtist_.clear();
            lastTitle_.clear();
            lastObserved_ = {};
            lastPropertiesRead_ = {};
        }

        bool mediaPropertiesChanged = false;
        if (sessionChanged || positionRestarted || lastTitle_.empty() || propertiesExpired) {
            // 读取媒体属性可能触发异步调用，因此只在必要时刷新。
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
            // 记录最近一次系统位置变化，为 mode=2 的本地插值提供基线。
            lastObserved_ = now;
            lastRawPositionMs_ = rawPositionMs;
        }

        if (mode == 1 && state.playing && timelineUpdatedRecently) {
            // mode=1 使用 SMTC 自带更新时间修正位置。
            state.positionMs = rawPositionMs + timelineUpdatedAgeMs;
        } else if (mode == 2 && state.playing) {
            // mode=2 在原始位置不更新时，用 steady_clock 推进播放进度。
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastObserved_).count();
            state.positionMs = rawPositionMs + elapsed;
        } else {
            state.positionMs = rawPositionMs;
        }

        if (!state.playing) {
            // 暂停时固定基线，防止暂停状态下继续外推。
            lastObserved_ = now;
            lastRawPositionMs_ = rawPositionMs;
        }

        if (state.durationMs > 0) {
            state.positionMs = std::clamp(state.positionMs, std::int64_t{0}, state.durationMs);
        }
        state.valid = !state.title.empty();
    } catch (...) {
        // WinRT 调用异常时丢弃当前 manager/session，下次重新建立。
        clearCachedSession();
        manager_ = nullptr;
        state = {};
    }

    return state;
}

void SmtcProvider::shutdown() {
    // 析构或主动关闭时释放 WinRT 对象，并反初始化 apartment。
    if (apartmentInitialized_) {
        clearCachedSession();
        manager_ = nullptr;
        winrt::uninit_apartment();
        apartmentInitialized_ = false;
    }
}

bool SmtcProvider::ensureManager() {
    if (manager_) return true;

    // 上一次创建失败后，在重试窗口内直接返回，避免 UI 卡顿。
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
    // 清理所有与当前播放会话相关的缓存字段。
    session_ = nullptr;
    lastSessionId_.clear();
    lastArtist_.clear();
    lastTitle_.clear();
    lastRawPositionMs_ = -1;
    lastObserved_ = {};
    lastPropertiesRead_ = {};
}

}
