#pragma once

#include "Meters.h"
#include "SimConfig.h"
#include "Sources.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sim
{
    /** All of the simulator's signal processing, with no device I/O, so it can be unit-tested.
        The audio callbacks call renderInputs() / processOutputs(), and the UI thread
        uses the rest. Nothing on the audio side locks, allocates or frees.

        Sources are handed over as raw pointers. The UI thread keeps them alive
        (see SourceSlots) until getEpoch() shows that every callback that could
        still be using an old pointer has returned. */
    class Mixer
    {
    public:
        Mixer();

        // ------------------------------------------------------------ audio side

        /** Renders every input's source at the shared song position into planar
            buffers, meters them and advances the transport. Bumps the epoch at the end. */
        void renderInputs(float* const* channels, int numChannels, int numFrames) noexcept;

        /** Meters PluginHost's outputs and mixes the monitored ones into an interleaved
            stereo buffer (odd outputs -> L, even -> R; silence while muted). */
        void processOutputs(const float* const* channels, int numChannels, int numFrames,
                            float* stereoOut) noexcept;

        // -------------------------------------------------------------- UI side

        /** Publishes a source for an input (nullptr = silence). The caller keeps
            the old one alive until the epoch has moved past the value it read
            after this call. */
        void setSource(int input, ISource* source) noexcept;

        /** Number of renderInputs() calls that have returned. */
        [[nodiscard]] std::uint64_t getEpoch() const noexcept { return epoch.load(); }

        [[nodiscard]] float consumeInputPeak(int input) noexcept { return inputMeters[clamp(input)].consume(); }
        [[nodiscard]] float consumeOutputPeak(int output) noexcept { return outputMeters[clamp(output)].consume(); }

        void setPlaying(bool shouldPlay) noexcept { playing = shouldPlay; }
        [[nodiscard]] bool isPlaying() const noexcept { return playing.load(); }
        /** Rewinds the song to 0 at the next block. */
        void restart() noexcept { ++restartRequests; }
        [[nodiscard]] std::uint64_t getSongPositionFrames() const noexcept { return songPosition.load(); }
        [[nodiscard]] std::uint64_t getSongLengthFrames() const noexcept { return songLength.load(); }

        void setMonitorEnabled(int output, bool enabled) noexcept { monitorEnabled[clamp(output)] = enabled; }
        [[nodiscard]] bool isMonitorEnabled(int output) const noexcept { return monitorEnabled[clamp(output)]; }
        void setMonitorMuted(bool muted) noexcept { monitorMuted = muted; }
        [[nodiscard]] bool isMonitorMuted() const noexcept { return monitorMuted; }

        static int clamp(int index) noexcept { return index < 0 ? 0 : (index >= kMaxChannels ? kMaxChannels - 1 : index); }

    private:
        std::array<std::atomic<ISource*>, kMaxChannels> sources {};
        std::atomic<std::uint64_t> epoch { 0 };

        std::array<PeakMeter, kMaxChannels> inputMeters;
        std::array<PeakMeter, kMaxChannels> outputMeters;
        std::array<std::atomic<bool>, kMaxChannels> monitorEnabled {};
        std::atomic<bool> monitorMuted { false };

        std::atomic<bool> playing { false };
        std::atomic<std::uint64_t> restartRequests { 0 };
        std::atomic<std::uint64_t> songPosition { 0 };   // published copy of `position`
        std::atomic<std::uint64_t> songLength { 0 };

        // Audio side only (one inputs callback at a time).
        std::uint64_t position = 0;
        std::uint64_t seenRestarts = 0;
    };
}
