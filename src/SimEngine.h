#pragma once

#include "AudioIo.h"
#include "LoopbackStatus.h"
#include "Mixer.h"
#include "SimConfig.h"
#include "SourceSlots.h"
#include "Sources.h"
#include "StatusPoller.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace sim
{
    /** What the UI (or headless mode) talks to: the audio devices, the signal
        processing, source ownership and the PluginHost link status, behind one API.
        UI thread only; the audio runs in AudioIo's device callbacks. */
    class SimEngine
    {
    public:
        explicit SimEngine(SimConfig config);
        ~SimEngine();

        /** Opens the loopback (fatal on failure) and the monitor (not fatal). */
        [[nodiscard]] std::string start();
        void stop();

        /** Restarts with new IO counts; on failure restores the previous counts. */
        [[nodiscard]] std::string reconfigure(int numInputs, int numOutputs);

        /** Call once per UI frame: frees replaced sources once the audio thread is done
            with them and notices a lost or stalled loopback. */
        void housekeeping();

        [[nodiscard]] bool isRunning() const noexcept { return io.isOpen() && !io.isLost(); }
        /** Running, but no loopback callback for over a second. */
        [[nodiscard]] bool isStalled() const noexcept { return stalled; }
        [[nodiscard]] const SimConfig& getConfig() const noexcept { return config; }
        [[nodiscard]] int getNumInputs() const noexcept { return config.numInputs; }
        [[nodiscard]] int getNumOutputs() const noexcept { return config.numOutputs; }

        void setInputSource(int input, std::shared_ptr<ISource> source) { slots.set(input, std::move(source)); }
        [[nodiscard]] std::shared_ptr<ISource> getInputSource(int input) const { return slots.get(input); }

        [[nodiscard]] float consumeInputPeak(int input) noexcept { return mixer.consumeInputPeak(input); }
        [[nodiscard]] float consumeOutputPeak(int output) noexcept { return mixer.consumeOutputPeak(output); }

        /** Song transport shared by every file input (see Mixer). */
        void setPlaying(bool shouldPlay) noexcept { mixer.setPlaying(shouldPlay); }
        [[nodiscard]] bool isPlaying() const noexcept { return mixer.isPlaying(); }
        void restart() noexcept { mixer.restart(); }
        [[nodiscard]] std::uint64_t getSongPositionFrames() const noexcept { return mixer.getSongPositionFrames(); }
        [[nodiscard]] std::uint64_t getSongLengthFrames() const noexcept { return mixer.getSongLengthFrames(); }

        void setMonitorEnabled(int output, bool enabled) noexcept { mixer.setMonitorEnabled(output, enabled); }
        [[nodiscard]] bool isMonitorEnabled(int output) const noexcept { return mixer.isMonitorEnabled(output); }
        void setMonitorMuted(bool muted) noexcept { mixer.setMonitorMuted(muted); }
        [[nodiscard]] bool isMonitorMuted() const noexcept { return mixer.isMonitorMuted(); }

        [[nodiscard]] std::uint64_t getLoopbackXruns() const noexcept { return io.getLoopbackXruns(); }
        [[nodiscard]] std::uint64_t getMonitorXruns() const noexcept { return io.getMonitorXruns(); }
        [[nodiscard]] std::uint64_t getMonitorDropouts() const noexcept { return io.getMonitorDropouts(); }
        [[nodiscard]] bool isMonitorOpen() const noexcept { return io.isMonitorOpen() && !io.isMonitorLost(); }
        [[nodiscard]] std::string getLastError() const { return lastError; }
        [[nodiscard]] std::string getMonitorError() const;
        [[nodiscard]] std::string getLoopbackFormat() const { return io.getFormatName(); }

        /** Latest PluginHost link status, polled in the background once a second. */
        [[nodiscard]] LoopbackStatus getLinkStatus() const { return poller.latest(); }

    private:
        using Clock = std::chrono::steady_clock;

        SimConfig config;
        Mixer mixer;
        SourceSlots slots { mixer };
        AudioIo io;
        StatusPoller poller;

        std::string lastError;
        bool stalled = false;
        std::uint64_t lastCallbacks = 0;
        Clock::time_point lastProgress {};
    };
}
