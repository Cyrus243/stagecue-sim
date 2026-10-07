#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sim
{
    struct DecodedAudio
    {
        std::vector<float> interleaved;
        unsigned channels = 0;
        unsigned sampleRate = 0;

        [[nodiscard]] std::size_t numFrames() const noexcept { return channels == 0 ? 0 : interleaved.size() / channels; }
    };

    /** Decodes a WAV / AIFF / RF64 / W64 (dr_wav) or FLAC (dr_flac) file to float. */
    [[nodiscard]] std::optional<DecodedAudio> decodeAudioFile(const std::string& path, std::string& error);
}
