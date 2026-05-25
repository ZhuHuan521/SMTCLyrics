#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <winrt/Windows.Media.Control.h>

namespace smtc::smtc_provider {

// 从 Windows 全局媒体控制会话读到的一帧媒体状态。
struct MediaState {
    std::wstring artist;
    std::wstring title;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    bool valid = false;
    bool playing = false;
};

// 封装 Windows Runtime 的 SMTC 会话读取，并对位置更新做本地估算。
class SmtcProvider {
public:
    SmtcProvider();
    ~SmtcProvider();

    // mode=1 偏向使用系统 timeline 的 LastUpdatedTime；mode=2 偏向本地 steady_clock 插值。
    MediaState readState(int mode);
    // 主动释放 WinRT apartment 和缓存会话。
    void shutdown();

private:
    // 懒加载 GlobalSystemMediaTransportControlsSessionManager。
    bool ensureManager();
    // 当前会话失效或异常时清空状态，等待下一次重新读取。
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
