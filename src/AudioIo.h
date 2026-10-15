#pragma once

#include "SimConfig.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim
{
    class Mixer;

    /** The three audio devices, all driven by their own device callbacks (no extra
        threads, no polling):
        - inputs  (playback): Mixer::renderInputs -> loopback -> PluginHost's inputs
        - outputs (capture):  PluginHost's outputs -> loopback -> Mixer::processOutputs -> monitor ring
        - monitor (playback): monitor ring -> main out (resampled to the speakers' rate)
        Linux opens both directions of snd-aloop device 0 with exact parameters;
        macOS opens the two BlackHole-based devices by name. UI thread only, apart
        from the counters. */
    class AudioIo
    {
    public:
        AudioIo();
        ~AudioIo();
        AudioIo(const AudioIo&) = delete;
        AudioIo& operator=(const AudioIo&) = delete;

        /** Opens and starts the loopback (fatal on failure) and the monitor (not fatal:
            see getMonitorError()). Returns an empty string on success. */
        [[nodiscard]] std::string open(const SimConfig& config, Mixer& mixer);

        /** Stops every device; returns once no callback is running. */
        void close() noexcept;

        [[nodiscard]] bool isOpen() const noexcept { return opened; }
        [[nodiscard]] bool isMonitorOpen() const noexcept { return monitorOpened; }

        /** The loopback stopped on its own (device removed, driver or ALSA error). */
        [[nodiscard]] bool isLost() const noexcept { return loopbackLost.load(); }
        [[nodiscard]] bool isMonitorLost() const noexcept { return monitorLost.load(); }

        /** Callbacks that came much later than the block length allows: the
            simulator's view of an xrun (miniaudio recovers from real ones itself). */
        [[nodiscard]] std::uint64_t getLoopbackXruns() const noexcept { return inputsClock.late + outputsClock.late; }
        [[nodiscard]] std::uint64_t getMonitorXruns() const noexcept { return monitorClock.late.load(); }
        /** Blocks the monitor path dropped or padded with silence (clock drift). */
        [[nodiscard]] std::uint64_t getMonitorDropouts() const noexcept { return monitorDropouts.load(); }

        /** Total loopback callbacks so far: stops moving when the loopback stalls. */
        [[nodiscard]] std::uint64_t getLoopbackCallbacks() const noexcept
        {
            return inputsClock.count + outputsClock.count;
        }

        [[nodiscard]] const std::string& getFormatName() const noexcept { return formatName; }
        [[nodiscard]] const std::string& getMonitorError() const noexcept { return monitorError; }

        /** Device callbacks; public only for the C trampolines. */
        void inputsCallback(void* output, std::uint32_t numFrames) noexcept;
        void outputsCallback(const void* input, std::uint32_t numFrames) noexcept;
        void monitorCallback(float* output, std::uint32_t numFrames) noexcept;
        void deviceStopped(const void* device) noexcept;

    private:
        struct Device;

        /** Spots callbacks that arrive far later than the previous block's length. */
        struct CallbackClock
        {
            std::atomic<std::uint64_t> count { 0 };
            std::atomic<std::uint64_t> late { 0 };
            std::chrono::steady_clock::time_point last {};
            std::chrono::steady_clock::duration allowed {};

            void tick(std::uint32_t numFrames, unsigned sampleRate) noexcept;
            void reset() noexcept;
        };

        [[nodiscard]] std::string openLoopback(const SimConfig& config);
        void openMonitor(const SimConfig& config);
        [[nodiscard]] bool writeMonitor(const float* stereo, std::uint32_t numFrames) noexcept;

        struct Context;
        std::unique_ptr<Context> context;
        std::unique_ptr<Device> inputs;
        std::unique_ptr<Device> outputs;
        std::unique_ptr<Device> monitor;

        Mixer* mixer = nullptr;
        int numInputs = 0;
        int numOutputs = 0;
        unsigned sampleRate = 0;
        bool opened = false;
        bool monitorOpened = false;
        std::string formatName;
        std::string monitorError;

        // Preallocated scratch for the callbacks: planar float per channel,
        // interleaved float, and the monitor's stereo mix.
        std::vector<float> inputsPlanar, inputsInterleaved;
        std::vector<float> outputsPlanar, outputsInterleaved, stereo;

        std::atomic<bool> closing { false };
        std::atomic<bool> loopbackLost { false };
        std::atomic<bool> monitorLost { false };
        std::atomic<bool> monitorPrimed { false };   // first block reached the ring
        std::atomic<std::uint64_t> monitorDropouts { 0 };
        CallbackClock inputsClock, outputsClock, monitorClock;
    };
}
