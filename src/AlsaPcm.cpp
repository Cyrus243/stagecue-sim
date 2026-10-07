#include "AlsaPcm.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>

namespace sim
{
    namespace
    {
        std::string describeError(const std::string& what, int error)
        {
            return what + ": " + snd_strerror(error);
        }

        float clampSample(float value) noexcept { return std::clamp(value, -1.0f, 1.0f); }

        void ignoreAlsaError(const char*, int, const char*, int, const char*, ...) {}
    }

    void AlsaPcm::silenceLibraryErrors() noexcept { snd_lib_error_set_handler(ignoreAlsaError); }

    AlsaPcm::~AlsaPcm() { close(); }

    std::string AlsaPcm::open(const std::string& name, Direction dir, int numChannels,
                              unsigned rate, unsigned period, unsigned periods, bool exactRate)
    {
        close();
        direction = dir;

        const auto stream = dir == Direction::Playback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
        if (const int err = snd_pcm_open(&pcm, name.c_str(), stream, 0); err < 0)
        {
            pcm = nullptr;
            return describeError("cannot open " + name, err);
        }

        auto fail = [this, &name](const std::string& what, int err) {
            close();
            return describeError(name + ": " + what, err);
        };

        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);

        int err = snd_pcm_hw_params_any(pcm, hw);
        if (err < 0)
            return fail("no hw configuration", err);

        if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
            return fail("interleaved access unsupported", err);

        struct Candidate { snd_pcm_format_t alsa; Format format; const char* label; };
        constexpr Candidate candidates[] = {
            { SND_PCM_FORMAT_S32_LE, Format::S32, "S32_LE" },
            { SND_PCM_FORMAT_FLOAT_LE, Format::Float, "FLOAT_LE" },
            { SND_PCM_FORMAT_S16_LE, Format::S16, "S16_LE" },
        };

        bool formatSet = false;
        for (const auto& candidate : candidates)
        {
            if (snd_pcm_hw_params_test_format(pcm, hw, candidate.alsa) == 0
                && snd_pcm_hw_params_set_format(pcm, hw, candidate.alsa) == 0)
            {
                format = candidate.format;
                formatName = candidate.label;
                formatSet = true;
                break;
            }
        }
        if (!formatSet)
            return fail("no supported sample format (S32_LE/FLOAT_LE/S16_LE)", -EINVAL);

        if ((err = snd_pcm_hw_params_set_channels(pcm, hw, static_cast<unsigned>(numChannels))) < 0)
        {
            unsigned minCh = 0, maxCh = 0;
            snd_pcm_hw_params_get_channels_min(hw, &minCh);
            snd_pcm_hw_params_get_channels_max(hw, &maxCh);
            return fail("cannot use " + std::to_string(numChannels) + " channels (device allows "
                            + std::to_string(minCh) + ".." + std::to_string(maxCh) + ")",
                        err);
        }

        unsigned actualRate = rate;
        if (exactRate)
            err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
        else
            err = snd_pcm_hw_params_set_rate_near(pcm, hw, &actualRate, nullptr);
        if (err < 0)
            return fail("cannot use " + std::to_string(rate) + " Hz", err);

        snd_pcm_uframes_t periodSize = period;
        if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &periodSize, nullptr)) < 0)
            return fail("cannot set period size", err);

        unsigned numPeriods = periods;
        if ((err = snd_pcm_hw_params_set_periods_near(pcm, hw, &numPeriods, nullptr)) < 0)
            return fail("cannot set period count", err);

        if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
            return fail("hw params rejected", err);

        snd_pcm_hw_params_get_period_size(hw, &periodSize, nullptr);
        snd_pcm_uframes_t bufferSize = 0;
        snd_pcm_hw_params_get_buffer_size(hw, &bufferSize);

        snd_pcm_sw_params_t* sw = nullptr;
        snd_pcm_sw_params_alloca(&sw);
        snd_pcm_sw_params_current(pcm, sw);
        snd_pcm_sw_params_set_avail_min(pcm, sw, periodSize);
        // Playback starts once one period is queued; capture starts on first read.
        snd_pcm_sw_params_set_start_threshold(pcm, sw, dir == Direction::Playback ? periodSize : 1);
        if ((err = snd_pcm_sw_params(pcm, sw)) < 0)
            return fail("sw params rejected", err);

        channels = numChannels;
        sampleRate = actualRate;
        periodFrames = static_cast<unsigned>(periodSize);
        xruns = 0;

        const auto samples = static_cast<std::size_t>(bufferSize) * static_cast<std::size_t>(channels);
        scratch32.assign(samples, 0);
        scratchFloat.assign(samples, 0.0f);
        scratch16.assign(samples, 0);
        return {};
    }

    void AlsaPcm::close() noexcept
    {
        if (pcm != nullptr)
        {
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
    }

    int AlsaPcm::recover(int error) noexcept
    {
        if (error == -EPIPE || error == -ESTRPIPE || error == -EINTR)
        {
            if (snd_pcm_recover(pcm, error, 1) == 0)
            {
                if (error != -EINTR)
                    ++xruns;
                return 0;
            }
        }
        return error;
    }

    int AlsaPcm::transfer(void* interleaved, int numFrames) noexcept
    {
        const auto frameBytes = snd_pcm_frames_to_bytes(pcm, 1);
        auto* bytes = static_cast<char*>(interleaved);
        int done = 0;

        while (done < numFrames)
        {
            void* cursor = bytes + static_cast<std::ptrdiff_t>(done) * frameBytes;
            const auto remaining = static_cast<snd_pcm_uframes_t>(numFrames - done);
            const auto result = direction == Direction::Playback ? snd_pcm_writei(pcm, cursor, remaining)
                                                                 : snd_pcm_readi(pcm, cursor, remaining);
            if (result < 0)
            {
                const int recovered = recover(static_cast<int>(result));
                if (recovered < 0)
                    return recovered;
                if (result == -EINTR)
                    continue;
                return 0;
            }
            done += static_cast<int>(result);
        }
        return numFrames;
    }

    int AlsaPcm::read(float* const* channelData, int numFrames) noexcept
    {
        if (pcm == nullptr || numFrames <= 0)
            return -EBADFD;

        const auto total = static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(channels);
        void* interleaved = format == Format::S32   ? static_cast<void*>(scratch32.data())
                            : format == Format::Float ? static_cast<void*>(scratchFloat.data())
                                                      : static_cast<void*>(scratch16.data());
        if (total > scratch32.size())
            return -EINVAL;

        const int result = transfer(interleaved, numFrames);
        if (result <= 0)
        {
            for (int ch = 0; ch < channels; ++ch)
                std::fill_n(channelData[ch], numFrames, 0.0f);
            return result;
        }

        for (int i = 0; i < numFrames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto idx = static_cast<std::size_t>(i * channels + ch);
                float value = 0.0f;
                switch (format)
                {
                    case Format::S32: value = static_cast<float>(scratch32[idx] / 2147483648.0); break;
                    case Format::Float: value = scratchFloat[idx]; break;
                    case Format::S16: value = static_cast<float>(scratch16[idx]) / 32768.0f; break;
                }
                channelData[ch][i] = value;
            }
        }
        return numFrames;
    }

    int AlsaPcm::write(const float* const* channelData, int numFrames) noexcept
    {
        if (pcm == nullptr || numFrames <= 0)
            return -EBADFD;

        const auto total = static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(channels);
        if (total > scratch32.size())
            return -EINVAL;

        for (int i = 0; i < numFrames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto idx = static_cast<std::size_t>(i * channels + ch);
                const float value = channelData != nullptr ? clampSample(channelData[ch][i]) : 0.0f;
                switch (format)
                {
                    case Format::S32: scratch32[idx] = static_cast<std::int32_t>(value * 2147483647.0); break;
                    case Format::Float: scratchFloat[idx] = value; break;
                    case Format::S16: scratch16[idx] = static_cast<std::int16_t>(value * 32767.0f); break;
                }
            }
        }

        void* interleaved = format == Format::S32   ? static_cast<void*>(scratch32.data())
                            : format == Format::Float ? static_cast<void*>(scratchFloat.data())
                                                      : static_cast<void*>(scratch16.data());
        return transfer(interleaved, numFrames);
    }

    int AlsaPcm::writeSilence(int numFrames) noexcept
    {
        return write(nullptr, numFrames);
    }
}
