#include "Mixer.h"

#include <algorithm>
#include <cmath>

namespace sim
{
    namespace
    {
        float peakOf(const float* data, int numFrames) noexcept
        {
            float peak = 0.0f;
            for (int i = 0; i < numFrames; ++i)
                peak = std::max(peak, std::abs(data[i]));
            return peak;
        }
    }

    Mixer::Mixer()
    {
        // Default monitoring: the first two outputs (L/R) go to the main out.
        for (int i = 0; i < kMaxChannels; ++i)
            monitorEnabled[i] = i < 2;
    }

    void Mixer::setSource(int input, ISource* source) noexcept { sources[clamp(input)].store(source); }

    void Mixer::renderInputs(float* const* channels, int numChannels, int numFrames) noexcept
    {
        numChannels = std::min(numChannels, kMaxChannels);

        // Load every pointer once, so all inputs render this block from the same sources.
        std::array<ISource*, kMaxChannels> current {};
        std::uint64_t loopLength = 0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            current[ch] = sources[ch].load();
            if (current[ch] != nullptr)
                loopLength = std::max<std::uint64_t>(loopLength, current[ch]->getLengthFrames());
        }

        if (const auto restarts = restartRequests.load(); restarts != seenRestarts)
        {
            seenRestarts = restarts;
            position = 0;
        }
        position = loopLength > 0 ? position % loopLength : 0;

        const TransportFrame transport { playing.load(), position, loopLength };
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (current[ch] != nullptr)
                current[ch]->render(channels[ch], numFrames, transport);
            else
                std::fill_n(channels[ch], numFrames, 0.0f);
            inputMeters[ch].push(peakOf(channels[ch], numFrames));
        }

        if (transport.playing && loopLength > 0)
            position = (position + static_cast<std::uint64_t>(numFrames)) % loopLength;
        songPosition = position;
        songLength = loopLength;

        // Last: from here on this block no longer touches any source.
        ++epoch;
    }

    void Mixer::processOutputs(const float* const* channels, int numChannels, int numFrames,
                               float* stereoOut) noexcept
    {
        numChannels = std::min(numChannels, kMaxChannels);
        std::fill_n(stereoOut, static_cast<std::size_t>(numFrames) * 2, 0.0f);

        const bool muted = monitorMuted.load();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = channels[ch];
            outputMeters[ch].push(peakOf(data, numFrames));

            if (muted || !monitorEnabled[ch].load())
                continue;

            float* out = stereoOut + (ch % 2);
            for (int i = 0; i < numFrames; ++i)
                out[i * 2] += data[i];
        }
    }
}
