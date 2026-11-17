#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace smtc::playback {

// A local clock between playback samples. Normal corrections change its phase
// gradually; only playback transitions and seeks replace the position anchor.
class PlaybackClock {
public:
    static constexpr std::int64_t kSeekThresholdMs = 1500;

    void reset() noexcept {
        *this = PlaybackClock{};
    }

    void synchronize(std::int64_t positionMs, std::int64_t sampleTimeMs,
                     bool playing) noexcept {
        initialized_ = true;
        playing_ = playing;
        anchorPositionMs_ = std::max<std::int64_t>(0, positionMs);
        anchorTimeMs_ = sampleTimeMs;
        lastSampleTimeMs_ = sampleTimeMs;
        filteredPhaseMs_ = 0.0;
        startPhaseMs_ = 0.0;
        targetPhaseMs_ = 0.0;
        transitionTimeMs_ = sampleTimeMs;
        transitionDurationMs_ = 0.0;
    }

    void update(std::int64_t positionMs, std::int64_t sampleTimeMs,
                bool playing) noexcept {
        if (!initialized_) {
            synchronize(positionMs, sampleTimeMs, playing);
            return;
        }
        // An older response must not rewind the clock or restore stale status.
        if (sampleTimeMs <= lastSampleTimeMs_) return;

        if (!playing || playing != playing_) {
            synchronize(positionMs, sampleTimeMs, playing);
            return;
        }

        const double position = static_cast<double>(std::max<std::int64_t>(0, positionMs));
        const double baseline = baselineAt(sampleTimeMs);
        const double currentPhase = phaseAt(sampleTimeMs);
        // Compare with the corrected clock: accumulated normal drift relative
        // to the original anchor is not itself a new seek.
        if (std::abs(position - (baseline + currentPhase)) > kSeekThresholdMs) {
            synchronize(positionMs, sampleTimeMs, playing);
            return;
        }

        const double sampleInterval = differenceMs(sampleTimeMs, lastSampleTimeMs_);
        lastSampleTimeMs_ = sampleTimeMs;
        // Filter absolute phase relative to the unchanged anchor. Adding the
        // residual error each time would apply an outstanding correction twice.
        const double observedPhase = position - baseline;
        if (std::abs(observedPhase - filteredPhaseMs_) <= kNoiseDeadbandMs) return;

        const double alpha = 1.0 - std::exp(-sampleInterval / kFilterTimeConstantMs);
        filteredPhaseMs_ += alpha * (observedPhase - filteredPhaseMs_);
        if (std::abs(filteredPhaseMs_ - targetPhaseMs_) <= kTargetDeadbandMs) return;

        startPhaseMs_ = currentPhase;
        targetPhaseMs_ = filteredPhaseMs_;
        transitionTimeMs_ = sampleTimeMs;
        // smoothstep's largest derivative is 1.5. This duration therefore
        // bounds correction speed to +/-0.2 ms per elapsed millisecond.
        transitionDurationMs_ = std::max(kMinimumTransitionMs,
            1.5 * std::abs(targetPhaseMs_ - startPhaseMs_) / kMaximumCorrectionRate);
    }

    std::int64_t positionAt(std::int64_t nowMs) const noexcept {
        if (!initialized_) return 0;
        const double position = baselineAt(nowMs) + phaseAt(nowMs);
        if (position <= 0.0) return 0;
        if (position >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(std::llround(position));
    }

    bool playing() const noexcept { return playing_; }

private:
    static double differenceMs(std::int64_t left, std::int64_t right) noexcept {
        // Subtract before converting to floating point, even for a large QPC
        // timestamp. Unsigned subtraction also avoids signed overflow.
        if (left >= right) {
            return static_cast<double>(static_cast<std::uint64_t>(left) -
                                       static_cast<std::uint64_t>(right));
        }
        return -static_cast<double>(static_cast<std::uint64_t>(right) -
                                    static_cast<std::uint64_t>(left));
    }

    double baselineAt(std::int64_t nowMs) const noexcept {
        return static_cast<double>(anchorPositionMs_) +
               (playing_ ? differenceMs(nowMs, anchorTimeMs_) : 0.0);
    }

    double phaseAt(std::int64_t nowMs) const noexcept {
        if (transitionDurationMs_ <= 0.0) return targetPhaseMs_;
        const double progress = std::clamp(
            differenceMs(nowMs, transitionTimeMs_) / transitionDurationMs_, 0.0, 1.0);
        const double eased = progress * progress * (3.0 - 2.0 * progress);
        return startPhaseMs_ + (targetPhaseMs_ - startPhaseMs_) * eased;
    }

    static constexpr double kFilterTimeConstantMs = 600.0;
    static constexpr double kMinimumTransitionMs = 600.0;
    static constexpr double kMaximumCorrectionRate = 0.2;
    static constexpr double kNoiseDeadbandMs = 20.0;
    static constexpr double kTargetDeadbandMs = 5.0;

    bool initialized_ = false;
    bool playing_ = false;
    std::int64_t anchorPositionMs_ = 0;
    std::int64_t anchorTimeMs_ = 0;
    std::int64_t lastSampleTimeMs_ = 0;
    double filteredPhaseMs_ = 0.0;
    double startPhaseMs_ = 0.0;
    double targetPhaseMs_ = 0.0;
    std::int64_t transitionTimeMs_ = 0;
    double transitionDurationMs_ = 0.0;
};

} // namespace smtc::playback
