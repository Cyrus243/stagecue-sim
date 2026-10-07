#pragma once

#include <atomic>
#include <cmath>

namespace sim
{
    constexpr float kMeterFloorDb = -60.0f;
    constexpr double kMeterReleaseSeconds = 0.3;

    [[nodiscard]] inline float dbToGain(float db) noexcept
    {
        return db <= kMeterFloorDb - 40.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
    }

    [[nodiscard]] inline float gainToDb(float gain) noexcept
    {
        return gain > 0.0f ? std::fmax(20.0f * std::log10(gain), kMeterFloorDb) : kMeterFloorDb;
    }

    /** Audio thread pushes peaks, UI thread consumes-and-resets
        (same contract as domain::ChannelStripProcessor's meters). */
    class PeakMeter
    {
    public:
        void push(float peak) noexcept
        {
            float current = value.load(std::memory_order_relaxed);
            while (peak > current
                   && !value.compare_exchange_weak(current, peak, std::memory_order_relaxed))
            {
            }
        }

        [[nodiscard]] float consume() noexcept { return value.exchange(0.0f, std::memory_order_relaxed); }

    private:
        std::atomic<float> value { 0.0f };
    };

    /** UI-side meter ballistics: instant attack, exponential release, floor at -60 dB
        (mirrors ChannelStripProcessor::consumeSmoothedLevelDb). */
    class MeterBallistics
    {
    public:
        float update(float peak, double intervalSeconds) noexcept
        {
            if (peak >= smoothed)
                smoothed = peak;
            else
                smoothed *= static_cast<float>(std::exp(-intervalSeconds / kMeterReleaseSeconds));

            if (smoothed < dbToGain(kMeterFloorDb))
                smoothed = 0.0f;

            return gainToDb(smoothed);
        }

        void reset() noexcept { smoothed = 0.0f; }

    private:
        float smoothed = 0.0f;
    };
}
