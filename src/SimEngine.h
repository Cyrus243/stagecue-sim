#pragma once

#include "AlsaPcm.h"
#include "Meters.h"
#include "SimConfig.h"
#include "Sources.h"
#include "SpscRing.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sim
{
    /** Owns the loopback streams and the audio threads.
        - loop thread: sources -> loopback playback (the app's inputs) and
          loopback capture (the app's outputs) -> meters + monitor ring.
        - monitor thread: monitor ring -> main out (e.g. "default"). */
    class SimEngine
    {
    public:
        explicit SimEngine(SimConfig config);
        ~SimEngine();

        /** Opens the loopback (fatal on failure) and the monitor (non-fatal). */
        [[nodiscard]] std::string start();
        void stop();

        /** Restarts with new IO counts; on failure restores the previous counts. */
        [[nodiscard]] std::string reconfigure(int numInputs, int numOutputs);

        [[nodiscard]] bool isRunning() const noexcept { return running.load(); }
        [[nodiscard]] const SimConfig& getConfig() const noexcept { return config; }
        [[nodiscard]] int getNumInputs() const noexcept { return config.numInputs; }
        [[nodiscard]] int getNumOutputs() const noexcept { return config.numOutputs; }

        void setInputSource(int input, std::shared_ptr<ISource> source);
        [[nodiscard]] std::shared_ptr<ISource> getInputSource(int input) const;

        [[nodiscard]] float consumeInputPeak(int input) noexcept { return inputMeters[clamp(input)].consume(); }
        [[nodiscard]] float consumeOutputPeak(int output) noexcept { return outputMeters[clamp(output)].consume(); }

        /** Song transport shared by every file input. Stopped files are silent;
            restart() rewinds the song to 0 at the next period. */
        void setPlaying(bool shouldPlay) noexcept { transportPlaying = shouldPlay; }
        [[nodiscard]] bool isPlaying() const noexcept { return transportPlaying.load(); }
        void restart() noexcept { ++restartRequests; }
        [[nodiscard]] std::uint64_t getSongPositionFrames() const noexcept { return songPosition.load(); }
        [[nodiscard]] std::uint64_t getSongLengthFrames() const noexcept { return songLength.load(); }

        void setMonitorEnabled(int output, bool enabled) noexcept { monitorEnabled[clamp(output)] = enabled; }
        [[nodiscard]] bool isMonitorEnabled(int output) const noexcept { return monitorEnabled[clamp(output)]; }
        void setMonitorMuted(bool muted) noexcept { monitorMuted = muted; }
        [[nodiscard]] bool isMonitorMuted() const noexcept { return monitorMuted; }

        [[nodiscard]] std::uint64_t getLoopbackXruns() const noexcept;
        [[nodiscard]] std::uint64_t getMonitorXruns() const noexcept;
        [[nodiscard]] std::uint64_t getMonitorDropouts() const noexcept { return monitorDropouts.load(); }
        [[nodiscard]] bool isMonitorOpen() const noexcept { return monitor.isOpen(); }
        [[nodiscard]] std::string getLastError() const;
        [[nodiscard]] std::string getMonitorError() const;
        [[nodiscard]] std::string getLoopbackFormat() const { return loopbackOut.getFormatName(); }

    private:
        static int clamp(int index) noexcept { return index < 0 ? 0 : (index >= kMaxChannels ? kMaxChannels - 1 : index); }

        void loopThreadMain();
        void monitorThreadMain();
        void setError(std::string message);

        SimConfig config;

        AlsaPcm loopbackOut;   // sim inputs  -> app (playback on dev 0)
        AlsaPcm loopbackIn;    // app outputs -> sim (capture on dev 0)
        AlsaPcm monitor;       // main out

        std::array<std::atomic<std::shared_ptr<ISource>>, kMaxChannels> sources;
        std::array<PeakMeter, kMaxChannels> inputMeters;
        std::array<PeakMeter, kMaxChannels> outputMeters;
        std::array<std::atomic<bool>, kMaxChannels> monitorEnabled {};
        std::atomic<bool> monitorMuted { false };

        std::atomic<bool> transportPlaying { false };
        std::atomic<std::uint64_t> restartRequests { 0 };
        std::atomic<std::uint64_t> songPosition { 0 };   // survives stream restarts
        std::atomic<std::uint64_t> songLength { 0 };

        std::unique_ptr<SpscRing> monitorRing;
        std::atomic<std::uint64_t> monitorDropouts { 0 };

        std::atomic<bool> running { false };
        std::thread loopThread;
        std::thread monitorThread;

        mutable std::mutex errorMutex;
        std::string lastError;
        std::string monitorError;
    };
}
