#include "Sources.h"
#include "AudioFileDecoder.h"
#include "Meters.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <sstream>

namespace sim
{
    namespace
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

        /** Catmull-Rom resampling of a looped signal (indices wrap around). */
        std::vector<float> resampleLooped(const std::vector<float>& input, double ratio)
        {
            const auto inLength = input.size();
            const auto outLength = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::floor(static_cast<double>(inLength) / ratio)));

            auto at = [&input, inLength](std::ptrdiff_t index) {
                const auto n = static_cast<std::ptrdiff_t>(inLength);
                return input[static_cast<std::size_t>(((index % n) + n) % n)];
            };

            std::vector<float> output(outLength);
            for (std::size_t i = 0; i < outLength; ++i)
            {
                const double position = static_cast<double>(i) * ratio;
                const auto base = static_cast<std::ptrdiff_t>(std::floor(position));
                const auto t = static_cast<float>(position - static_cast<double>(base));

                const float y0 = at(base - 1), y1 = at(base), y2 = at(base + 1), y3 = at(base + 2);
                output[i] = y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3
                                                            + t * (3.0f * (y1 - y2) + y3 - y0)));
            }
            return output;
        }

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

    void SilenceSource::render(float* out, int numFrames) noexcept
    {
        std::fill_n(out, numFrames, 0.0f);
    }

    // ------------------------------------------------------------------- sine

    SineSource::SineSource(double rate, float frequencyHz, float level)
        : sampleRate(rate), frequency(frequencyHz), levelDb(level)
    {
    }

    void SineSource::render(float* out, int numFrames) noexcept
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
                                                 float levelDb, std::string& error)
    {
        const auto file = resolvePath(path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
        {
            error = "file not found: " + file.string();
            return nullptr;
        }

        std::string decodeError;
        auto decoded = decodeAudioFile(file.string(), decodeError);
        if (!decoded)
        {
            error = file.filename().string() + ": " + decodeError;
            return nullptr;
        }

        const auto numFrames = decoded->numFrames();
        const auto numChannels = decoded->channels;
        const float channelGain = 1.0f / static_cast<float>(numChannels);

        std::vector<float> mono(numFrames, 0.0f);
        for (std::size_t i = 0; i < numFrames; ++i)
            for (unsigned ch = 0; ch < numChannels; ++ch)
                mono[i] += decoded->interleaved[i * numChannels + ch] * channelGain;

        return fromSamples(std::move(mono), decoded->sampleRate, targetRate, levelDb, file.filename().string());
    }

    std::shared_ptr<FileSource> FileSource::fromSamples(std::vector<float> mono, double sourceRate,
                                                        double targetRate, float levelDb,
                                                        std::string name)
    {
        if (mono.empty())
            mono.push_back(0.0f);

        const double ratio = sourceRate / targetRate;
        if (std::abs(ratio - 1.0) > 1.0e-9)
            mono = resampleLooped(mono, ratio);

        return std::shared_ptr<FileSource>(new FileSource(std::move(mono), levelDb, std::move(name)));
    }

    void FileSource::render(float* out, int numFrames) noexcept
    {
        const float gain = dbToGain(levelDb.load());
        for (int i = 0; i < numFrames; ++i)
        {
            out[i] = gain * samples[position];
            if (++position == samples.size())
                position = 0;
        }
    }

    std::string FileSource::describe() const
    {
        return "file " + name + " @ " + formatNumber(getLevelDb(), 1) + " dBFS";
    }
}
