#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

typedef struct _snd_pcm snd_pcm_t;

namespace sim
{
    /** Minimal blocking ALSA PCM wrapper: interleaved hardware I/O, planar float API. */
    class AlsaPcm
    {
    public:
        enum class Direction { Playback, Capture };

        /** Routes alsa-lib's own stderr diagnostics to nowhere (they would corrupt the TUI);
            errors are still reported through return values. */
        static void silenceLibraryErrors() noexcept;

        AlsaPcm() = default;
        ~AlsaPcm();
        AlsaPcm(const AlsaPcm&) = delete;
        AlsaPcm& operator=(const AlsaPcm&) = delete;

        /** Opens and configures the device. Tries S32_LE, FLOAT_LE then S16_LE.
            Returns an empty string on success, otherwise a human-readable error. */
        [[nodiscard]] std::string open(const std::string& name, Direction direction, int channels,
                                       unsigned sampleRate, unsigned periodFrames, unsigned periods,
                                       bool exactRate);
        void close() noexcept;

        [[nodiscard]] bool isOpen() const noexcept { return pcm != nullptr; }

        /** Reads exactly numFrames into planar buffers. Returns numFrames, 0 after a
            recovered xrun (buffers zeroed), or a negative errno on a fatal error. */
        int read(float* const* channelData, int numFrames) noexcept;

        /** Writes exactly numFrames from planar buffers. Same return contract as read(). */
        int write(const float* const* channelData, int numFrames) noexcept;

        /** Queues numFrames of silence (used to prefill playback). */
        int writeSilence(int numFrames) noexcept;

        [[nodiscard]] int getChannels() const noexcept { return channels; }
        [[nodiscard]] unsigned getSampleRate() const noexcept { return sampleRate; }
        [[nodiscard]] unsigned getPeriodFrames() const noexcept { return periodFrames; }
        [[nodiscard]] std::uint64_t getXrunCount() const noexcept { return xruns.load(); }
        [[nodiscard]] const std::string& getFormatName() const noexcept { return formatName; }

    private:
        enum class Format { S32, Float, S16 };

        int transfer(void* interleaved, int numFrames) noexcept;
        int recover(int error) noexcept;

        snd_pcm_t* pcm = nullptr;
        Direction direction = Direction::Playback;
        Format format = Format::S32;
        std::string formatName;
        int channels = 0;
        unsigned sampleRate = 0;
        unsigned periodFrames = 0;
        std::atomic<std::uint64_t> xruns { 0 };
        std::vector<std::int32_t> scratch32;
        std::vector<float> scratchFloat;
        std::vector<std::int16_t> scratch16;
    };
}
