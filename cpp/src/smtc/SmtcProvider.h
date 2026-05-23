#pragma once

#include <chrono>
#include <cstdint>
#include <string>

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
    std::wstring lastTitle_;
    std::int64_t lastRawPositionMs_ = -1;
    std::chrono::steady_clock::time_point lastObserved_{};
    bool apartmentInitialized_ = false;
};

}
