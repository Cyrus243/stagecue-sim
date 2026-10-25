#include "Sources.h"
#include "Meters.h"
#include "MiniaudioConfig.h"

#include <algorithm>
#include <chrono>
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

    namespace
    {
        /** miniaudio's decoder as a stream's reader: mono at the device rate. */
        class DecoderReader final : public IFrameReader
        {
        public:
            explicit DecoderReader(std::unique_ptr<ma_decoder> d) : decoder(std::move(d)) {}
            ~DecoderReader() override { ma_decoder_uninit(decoder.get()); }

            bool seek(std::uint64_t frame) override
            {
                return ma_decoder_seek_to_pcm_frame(decoder.get(), frame) == MA_SUCCESS;
            }

            [[nodiscard]] ma_decoder& get() noexcept { return *decoder; }

            std::uint64_t read(float* out, std::uint64_t numFrames) override
            {
                ma_uint64 read = 0;
                (void) ma_decoder_read_pcm_frames(decoder.get(), out, numFrames, &read);
                return read;
            }

        private:
            std::unique_ptr<ma_decoder> decoder;
        };

        bool isCancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

        /** Decodes the rest of the file into `mono`; false if cancelled. */
        bool decodeAll(ma_decoder& decoder, std::vector<float>& mono, const std::atomic<bool>* cancel)
        {
            constexpr ma_uint64 kChunk = 16384;
            while (true)
            {
                if (isCancelled(cancel))
                    return false;
                const auto offset = mono.size();
                mono.resize(offset + kChunk);
                ma_uint64 read = 0;
                const auto result = ma_decoder_read_pcm_frames(&decoder, mono.data() + offset, kChunk, &read);
                mono.resize(offset + static_cast<std::size_t>(read));
                if (result != MA_SUCCESS || read < kChunk)
                    return true;
            }
        }

        /** Frames the decoder will give at the device rate; decodes through when the
            format does not say (some MP3s). 0 if cancelled or empty. */
        std::uint64_t lengthOf(ma_decoder& decoder, const std::atomic<bool>* cancel)
        {
            if (ma_uint64 length = 0; ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0)
                return length;

            std::uint64_t length = 0;
            std::vector<float> scratch(16384);
            while (!isCancelled(cancel))
            {
                ma_uint64 read = 0;
                const auto result = ma_decoder_read_pcm_frames(&decoder, scratch.data(), scratch.size(), &read);
                length += read;
                if (result != MA_SUCCESS || read < scratch.size())
                    break;
            }
            ma_decoder_seek_to_pcm_frame(&decoder, 0);
            return isCancelled(cancel) ? 0 : length;
        }
    }

    FileSource::Storage FileSource::Storage::forRate(double sampleRate)
    {
        return { static_cast<std::uint64_t>(30.0 * sampleRate), StreamLayout::forRate(sampleRate) };
    }

    FileSource::FileSource(std::vector<float> resampled, float level, std::string displayName)
        : samples(std::move(resampled)), levelDb(level), name(std::move(displayName))
    {
    }

    FileSource::FileSource(std::unique_ptr<StemStream> s, float level, std::string displayName)
        : stream(std::move(s)), levelDb(level), name(std::move(displayName))
    {
        stream->fill();   // primed before the first callback can reach it
        producer = std::thread([this] {
            while (!stopProducer.load(std::memory_order_relaxed))
            {
                stream->fill();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the ring holds seconds
            }
        });
    }

    FileSource::~FileSource()
    {
        stopProducer = true;
        if (producer.joinable())
            producer.join();
    }

    std::size_t FileSource::getLengthFrames() const noexcept
    {
        return stream ? static_cast<std::size_t>(stream->getLengthFrames()) : samples.size();
    }

    std::shared_ptr<FileSource> FileSource::load(const std::string& path, double targetRate,
                                                 float levelDb, std::string& error,
                                                 const std::atomic<bool>* cancel, std::optional<Storage> storage)
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
        auto decoder = std::make_unique<ma_decoder>();
        if (const auto result = ma_decoder_init_file(file.c_str(), &decoderConfig, decoder.get()); result != MA_SUCCESS)
        {
            error = file.filename().string() + ": cannot decode (" + ma_result_description(result)
                    + "; WAV, FLAC and MP3 are supported)";
            return nullptr;
        }
        auto reader = std::make_unique<DecoderReader>(std::move(decoder));   // uninits it on every return below
        auto& raw = reader->get();

        const auto length = lengthOf(raw, cancel);
        if (isCancelled(cancel))
        {
            error = "cancelled";
            return nullptr;
        }
        if (length == 0)
        {
            error = file.filename().string() + ": no audio";
            return nullptr;
        }

        const auto layout = storage.value_or(Storage::forRate(targetRate));
        if (length > layout.residentMaxFrames)
        {
            auto stream = std::make_unique<StemStream>(std::move(reader), length, layout.layout);
            return std::shared_ptr<FileSource>(new FileSource(std::move(stream), levelDb, file.filename().string()));
        }

        std::vector<float> mono;
        mono.reserve(static_cast<std::size_t>(length + 1));
        if (!decodeAll(raw, mono, cancel))
        {
            error = "cancelled";
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
        if (stream)
        {
            stream->render(out, numFrames, transport.position, transport.loopLength);
            for (int i = 0; i < numFrames; ++i)
                out[i] *= gain;
            return;
        }

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
