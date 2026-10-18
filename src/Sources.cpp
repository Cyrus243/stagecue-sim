#include "Sources.h"
#include "Meters.h"
#include "MiniaudioConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <sstream>

namespace sim
{
    std::filesystem::path resolvePath(const std::string& path)
    {
        std::string expanded = path;
        if (expanded.rfind("~/", 0) == 0)
            if (const char* home = std::getenv("HOME"))
                expanded = std::string(home) + expanded.substr(1);

        std::error_code ec;
        const auto absolute = std::filesystem::absolute(expanded, ec);
        return ec ? std::filesystem::path(expanded) : absolute;
    }

    namespace
    {
        std::string formatNumber(float value, int precision)
        {
            std::ostringstream stream;
            stream.setf(std::ios::fixed);
            stream.precision(precision);
            stream << value;
            return stream.str();
        }
    }

    // ---------------------------------------------------------------- silence

    void SilenceSource::render(float* out, int numFrames, const TransportFrame&) noexcept
    {
        std::fill_n(out, numFrames, 0.0f);
    }

    // ------------------------------------------------------------------- sine

    SineSource::SineSource(double rate, float frequencyHz, float level)
        : sampleRate(rate), frequency(frequencyHz), levelDb(level)
    {
    }

    void SineSource::render(float* out, int numFrames, const TransportFrame&) noexcept
    {
        const double increment = 2.0 * std::numbers::pi * frequency.load() / sampleRate;
        const float gain = dbToGain(levelDb.load());

        for (int i = 0; i < numFrames; ++i)
        {
            out[i] = gain * static_cast<float>(std::sin(phase));
            phase += increment;
        }
        phase = std::fmod(phase, 2.0 * std::numbers::pi);
    }

    std::string SineSource::describe() const
    {
        return "sine " + formatNumber(getFrequency(), 0) + " Hz @ " + formatNumber(getLevelDb(), 1) + " dBFS";
    }

    // ------------------------------------------------------------------- file

    FileSource::FileSource(std::vector<float> resampled, float level, std::string displayName)
        : samples(std::move(resampled)), levelDb(level), name(std::move(displayName))
    {
    }

    std::shared_ptr<FileSource> FileSource::load(const std::string& path, double targetRate,
                                                 float levelDb, std::string& error,
                                                 const std::atomic<bool>* cancel)
    {
        const auto file = resolvePath(path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
        {
            error = "file not found: " + file.string();
            return nullptr;
        }

        // The decoder mixes down to mono and resamples to the device rate as it reads.
        const auto rate = static_cast<ma_uint32>(targetRate);
        const auto decoderConfig = ma_decoder_config_init(ma_format_f32, 1, rate);
        ma_decoder decoder;
        if (const auto result = ma_decoder_init_file(file.c_str(), &decoderConfig, &decoder); result != MA_SUCCESS)
        {
            error = file.filename().string() + ": cannot decode (" + ma_result_description(result)
                    + "; WAV, FLAC and MP3 are supported)";
            return nullptr;
        }

        const auto maxFrames = static_cast<std::uint64_t>(kMaxFileSeconds * targetRate);
        std::vector<float> mono;
        if (ma_uint64 length = 0; ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0)
            mono.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(length, maxFrames) + 1));

        constexpr ma_uint64 kChunk = 16384;
        bool tooLong = false;
        bool cancelled = false;
        while (true)
        {
            if (cancel && cancel->load(std::memory_order_relaxed))
            {
                cancelled = true;
                break;
            }
            const auto offset = mono.size();
            mono.resize(offset + kChunk);
            ma_uint64 read = 0;
            const auto result = ma_decoder_read_pcm_frames(&decoder, mono.data() + offset, kChunk, &read);
            mono.resize(offset + static_cast<std::size_t>(read));
            if (mono.size() > maxFrames)
            {
                tooLong = true;
                break;
            }
            if (result != MA_SUCCESS || read < kChunk)
                break;
        }
        ma_decoder_uninit(&decoder);

        if (cancelled)
        {
            error = "cancelled";
            return nullptr;
        }
        if (tooLong)
        {
            error = file.filename().string() + ": longer than " + std::to_string(static_cast<int>(kMaxFileSeconds / 60))
                    + " minutes (files are held in memory, about 11.5 MB per minute)";
            return nullptr;
        }
        if (mono.empty())
        {
            error = file.filename().string() + ": no audio";
            return nullptr;
        }

        mono.shrink_to_fit();
        return fromSamples(std::move(mono), levelDb, file.filename().string());
    }

    std::shared_ptr<FileSource> FileSource::fromSamples(std::vector<float> mono, float levelDb, std::string name)
    {
        if (mono.empty())
            mono.push_back(0.0f);
        return std::shared_ptr<FileSource>(new FileSource(std::move(mono), levelDb, std::move(name)));
    }

    void FileSource::render(float* out, int numFrames, const TransportFrame& transport) noexcept
    {
        if (!transport.playing || transport.loopLength == 0)
        {
            std::fill_n(out, numFrames, 0.0f);
            return;
        }

        const float gain = dbToGain(levelDb.load());
        auto position = transport.position % transport.loopLength;
        for (int i = 0; i < numFrames; ++i)
        {
            out[i] = position < samples.size() ? gain * samples[position] : 0.0f;
            if (++position == transport.loopLength)
                position = 0;
        }
    }

    std::string FileSource::describe() const
    {
        return "file " + name + " @ " + formatNumber(getLevelDb(), 1) + " dBFS";
    }
}
