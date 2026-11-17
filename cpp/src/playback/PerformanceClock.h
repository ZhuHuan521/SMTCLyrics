#pragma once

#include <windows.h>

#include <cstdint>

namespace smtc::playback {

// Divide first: multiplying a long-running QPC counter by 1000 can overflow.
constexpr std::int64_t qpcMilliseconds(std::int64_t ticks, std::int64_t frequency) noexcept {
    if (ticks <= 0 || frequency <= 0) return 0;
    return (ticks / frequency) * 1000 + ((ticks % frequency) * 1000) / frequency;
}

struct PerformanceCounter {
    std::int64_t ticks = 0;
    std::int64_t frequency = 0;

    std::int64_t milliseconds() const noexcept {
        return qpcMilliseconds(ticks, frequency);
    }
};

inline PerformanceCounter readPerformanceCounter() noexcept {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return {value.QuadPart, frequency};
}

}
