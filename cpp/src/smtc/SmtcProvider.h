#pragma once

#include <chrono>
#include <string>

namespace smtc::smtc_provider {

struct MediaState {
    std::wstring artist;
    std::wstring title;
    int positionSeconds = 0;
    int durationSeconds = 0;
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
    int lastRawPositionSeconds_ = -1;
    std::chrono::steady_clock::time_point lastObserved_{};
    bool apartmentInitialized_ = false;
};

}
