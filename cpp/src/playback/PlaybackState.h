#pragma once

#include <cstdint>
#include <string>

namespace smtc::playback {

struct MediaState {
    std::wstring artist;
    std::wstring title;
    std::wstring album;
    std::wstring appleMusicCatalogId;
    std::wstring isrc;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    bool valid = false;
    bool playing = false;
};

// 管道不存在说明当前 Apple Music 进程没有 bridge；只有这个 7002 状态需要加载。
constexpr bool bridgePipeMissing(std::uint32_t win32Error) {
    return win32Error == 2;
}

}
