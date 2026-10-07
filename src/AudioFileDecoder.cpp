#include "AudioFileDecoder.h"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

#include <cstdint>

namespace sim
{
    namespace
    {
        // Caps memory use: 10 minutes of 48 kHz stereo float is ~230 MB.
        constexpr std::uint64_t kMaxFrames = 48000ULL * 60 * 10;

        template <typename Free>
        std::optional<DecodedAudio> adopt(float* samples, unsigned channels, unsigned rate,
                                          std::uint64_t frames, Free&& freeSamples, std::string& error)
        {
            if (frames == 0 || channels == 0)
            {
                freeSamples(samples);
                error = "audio file is empty";
                return std::nullopt;
            }
            if (frames > kMaxFrames)
            {
                freeSamples(samples);
                error = "audio file is longer than 10 minutes";
                return std::nullopt;
            }

            DecodedAudio audio;
            audio.channels = channels;
            audio.sampleRate = rate;
            audio.interleaved.assign(samples, samples + frames * channels);
            freeSamples(samples);
            return audio;
        }
    }

    std::optional<DecodedAudio> decodeAudioFile(const std::string& path, std::string& error)
    {
        unsigned channels = 0;
        unsigned rate = 0;

        drwav_uint64 wavFrames = 0;
        if (float* samples = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &wavFrames, nullptr))
            return adopt(samples, channels, rate, wavFrames, [](float* p) { drwav_free(p, nullptr); }, error);

        drflac_uint64 flacFrames = 0;
        if (float* samples = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &flacFrames, nullptr))
            return adopt(samples, channels, rate, flacFrames, [](float* p) { drflac_free(p, nullptr); }, error);

        error = "unsupported or unreadable audio file (expected WAV, AIFF or FLAC)";
        return std::nullopt;
    }
}
