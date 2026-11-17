#include "playback/PlaybackClock.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using smtc::playback::PlaybackClock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireNormalSample(PlaybackClock& clock, std::int64_t position,
                         std::int64_t time) {
    const auto before = clock.positionAt(time);
    clock.update(position, time, true);
    require(clock.positionAt(time) == before,
            "normal samples must leave the current displayed position continuous");
}

void requireNormalStep(std::int64_t previous, std::int64_t current,
                       std::int64_t elapsed) {
    const auto advance = current - previous;
    // Returning integer milliseconds introduces at most 1 ms of rounding error.
    require(advance >= 0, "normal playback must never move backwards");
    require(advance * 5 >= elapsed * 4 - 5,
            "negative phase correction must preserve at least 0.8x playback speed");
    require(advance * 5 <= elapsed * 6 + 5,
            "positive phase correction must not exceed 1.2x playback speed");
}

void testFirstSampleAndExtrapolation() {
    PlaybackClock clock;
    require(!clock.playing() && clock.positionAt(9000) == 0,
            "an uninitialized clock must be stopped at zero");
    clock.update(42'000, 7000, true);
    require(clock.playing() && clock.positionAt(7000) == 42'000,
            "the first valid sample must initialize the playing clock exactly");
    require(clock.positionAt(8250) == 43'250,
            "playback between samples must use the local elapsed time");
}

void testPersistentDrift() {
    for (const auto pollInterval : {500, 1000, 2000}) {
        for (const auto drift : {-200, 200}) {
            PlaybackClock clock;
            clock.synchronize(10'000, 0, true);
            auto previous = clock.positionAt(0);
            for (std::int64_t time = 10; time <= pollInterval + 6000; time += 10) {
                if (time % pollInterval == 0) {
                    requireNormalSample(clock, 10'000 + time + drift, time);
                }
                const auto current = clock.positionAt(time);
                requireNormalStep(previous, current, 10);
                previous = current;
                if (time == pollInterval + 2000) {
                    require(std::llabs(current - (10'000 + time + drift)) <= 20,
                            "a persistent 200 ms error must settle within 20 ms in two seconds");
                }
            }
            require(std::llabs(clock.positionAt(pollInterval + 6000) -
                               (10'000 + pollInterval + 6000 + drift)) <= 20,
                    "a corrected clock must not repeatedly accumulate the same phase error");
        }
    }
}

void testAlternatingSampleNoise() {
    for (const auto pollInterval : {500, 1000, 2000}) {
        for (const auto amplitude : {20, 50}) {
            PlaybackClock clock;
            clock.synchronize(10'000, 0, true);
            auto previous = clock.positionAt(0);
            for (std::int64_t time = 10; time <= 20'000; time += 10) {
                if (time % pollInterval == 0) {
                    const auto sign = (time / pollInterval) % 2 == 0 ? -1 : 1;
                    requireNormalSample(clock, 10'000 + time + sign * amplitude, time);
                }
                const auto current = clock.positionAt(time);
                requireNormalStep(previous, current, 10);
                require(std::llabs(current - (10'000 + time)) <= amplitude,
                        "sample jitter must not be amplified by the playback clock");
                if (amplitude == 20) {
                    require(current == 10'000 + time,
                            "20 ms sample jitter must stay inside the noise deadband");
                }
                previous = current;
            }
        }
    }
}

void testPauseAndResumeAtSamePosition() {
    PlaybackClock clock;
    clock.synchronize(10'000, 1000, true);
    requireNormalSample(clock, 11'200, 2000);
    clock.update(11'250, 2500, false);
    require(!clock.playing() && clock.positionAt(2500) == 11'250,
            "pause must replace an outstanding correction with the exact stopped position");
    constexpr std::int64_t resumeTime = 100'000'000;
    require(clock.positionAt(resumeTime) == 11'250,
            "a long pause must not advance the playback clock");
    clock.update(11'250, resumeTime, true);
    require(clock.playing() && clock.positionAt(resumeTime) == 11'250,
            "resuming at the same position must still establish a new time anchor");
    require(clock.positionAt(resumeTime + 1250) == 12'500,
            "resuming must not include any elapsed time from the pause");
    clock.update(12'510, resumeTime + 1500, false);
    clock.update(12'600, resumeTime + 2000, false);
    require(clock.positionAt(resumeTime + 10'000) == 12'600,
            "newer stopped samples must correct the paused position exactly");
}

void testBidirectionalSeeksAndThreshold() {
    PlaybackClock clock;
    clock.synchronize(10'000, 0, true);
    requireNormalSample(clock, 11'200, 1000);
    clock.update(25'000, 2000, true);
    require(clock.positionAt(2000) == 25'000 && clock.positionAt(2500) == 25'500,
            "a forward seek must immediately replace the position and phase anchors");
    clock.update(5000, 3000, true);
    require(clock.positionAt(3000) == 5000 && clock.positionAt(3500) == 5500,
            "a backward seek must immediately replace the position and phase anchors");

    for (const auto direction : {-1, 1}) {
        PlaybackClock threshold;
        threshold.synchronize(10'000, 0, true);
        requireNormalSample(threshold, 11'000 + direction * 1500, 1000);
        PlaybackClock beyond;
        beyond.synchronize(10'000, 0, true);
        const auto seekPosition = 11'000 + direction * 1501;
        beyond.update(seekPosition, 1000, true);
        require(beyond.positionAt(1000) == seekPosition,
                "errors greater than 1500 ms must synchronize in both directions");
    }
}

void testAccumulatedNormalDriftIsNotASeek() {
    PlaybackClock clock;
    clock.synchronize(10'000, 0, true);
    for (std::int64_t sample = 1; sample <= 30; ++sample) {
        const auto time = sample * 1000;
        const auto phase = std::min<std::int64_t>(sample * 100, 2000);
        requireNormalSample(clock, 10'000 + time + phase, time);
    }
    require(std::llabs(clock.positionAt(31'000) - 43'000) <= 20,
            "normal accumulated phase beyond 1500 ms must settle without a false seek");
}

void testResetAndTrackChange() {
    PlaybackClock clock;
    clock.synchronize(100'000, 0, true);
    requireNormalSample(clock, 101'200, 1000);
    clock.reset();
    require(!clock.playing() && clock.positionAt(100'000) == 0,
            "reset must discard the previous track and every pending correction");
    clock.update(500, 100'000, true);
    require(clock.positionAt(100'000) == 500 && clock.positionAt(101'000) == 1500,
            "the next track must start from its own first sample");
    clock.synchronize(750, 200'000, false);
    require(!clock.playing() && clock.positionAt(250'000) == 750,
            "explicit synchronization must also support a stopped new track");
}

void testOutOfOrderAndDuplicateSamples() {
    PlaybackClock clock;
    clock.synchronize(10'000, 1000, true);
    requireNormalSample(clock, 11'200, 2000);
    const auto reference = clock;
    clock.update(500, 1500, false);
    clock.update(1000, 2000, false);
    clock.update(25'000, 1999, true);
    require(clock.playing(), "old samples must not restore a stale paused status");
    for (std::int64_t time = 2000; time <= 6000; time += 50) {
        require(clock.positionAt(time) == reference.positionAt(time),
                "late or duplicate samples must not affect an existing phase trajectory");
    }

    PlaybackClock zeroTime;
    zeroTime.update(1000, 0, true);
    zeroTime.update(0, -1, false);
    require(zeroTime.playing() && zeroTime.positionAt(1000) == 2000,
            "initializing at timestamp zero must still reject an older response");
}

void testSingleBadSampleRecovers() {
    for (const auto pollInterval : {500, 1000, 2000}) {
        PlaybackClock clock;
        clock.synchronize(10'000, 0, true);
        auto previous = clock.positionAt(0);
        const auto endTime = pollInterval + 12'000;
        for (std::int64_t time = 10; time <= endTime; time += 10) {
            if (time % pollInterval == 0) {
                const auto error = time == pollInterval ? 1000 : 0;
                requireNormalSample(clock, 10'000 + time + error, time);
            }
            const auto current = clock.positionAt(time);
            requireNormalStep(previous, current, 10);
            previous = current;
        }
        require(std::llabs(clock.positionAt(endTime) - (10'000 + endTime)) <= 20,
                "a single anomalous sample must not leave a permanent phase error");
    }
}

void testLargeTimestampsAndFrameIndependence() {
    constexpr std::int64_t start = 8'000'000'000'000'000'000LL;
    PlaybackClock frequentlyRead;
    frequentlyRead.synchronize(10'000, start, true);
    require(frequentlyRead.positionAt(start + 1) == 10'001,
            "a large QPC timestamp must retain one-millisecond elapsed-time precision");
    auto sparselyRead = frequentlyRead;
    for (std::int64_t sample = 1; sample <= 12; ++sample) {
        for (std::int64_t frame = (sample - 1) * 1000; frame < sample * 1000; ++frame) {
            (void)frequentlyRead.positionAt(start + frame);
        }
        const auto time = start + sample * 1000;
        const auto position = 10'000 + sample * 1000 + 200;
        requireNormalSample(frequentlyRead, position, time);
        requireNormalSample(sparselyRead, position, time);
        for (const auto offset : {0, 1, 125, 500, 999}) {
            require(frequentlyRead.positionAt(time + offset) == sparselyRead.positionAt(time + offset),
                    "the number of rendered frames must not change playback-clock behavior");
        }
    }

    PlaybackClock extreme;
    extreme.synchronize(0, std::numeric_limits<std::int64_t>::min(), true);
    require(extreme.positionAt(std::numeric_limits<std::int64_t>::max()) ==
            std::numeric_limits<std::int64_t>::max(),
            "extreme elapsed times must saturate rather than overflow");
}

} // namespace

int main() {
    const std::array tests{
        testFirstSampleAndExtrapolation,
        testPersistentDrift,
        testAlternatingSampleNoise,
        testPauseAndResumeAtSamePosition,
        testBidirectionalSeeksAndThreshold,
        testAccumulatedNormalDriftIsNotASeek,
        testResetAndTrackChange,
        testOutOfOrderAndDuplicateSamples,
        testSingleBadSampleRecovers,
        testLargeTimestampsAndFrameIndependence
    };
    try {
        for (const auto test : tests) test();
        std::cout << "Passed " << tests.size() << " playback clock tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Playback clock test failed: " << error.what() << '\n';
        return 1;
    }
}
