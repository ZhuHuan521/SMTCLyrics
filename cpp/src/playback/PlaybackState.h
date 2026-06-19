#pragma once

#include <cstdint>
#include <string>

namespace smtc::playback {

struct MediaState {
    std::wstring artist;
    std::wstring title;
    std::wstring album;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    bool valid = false;
    bool playing = false;
};

}
