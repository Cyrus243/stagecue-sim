#pragma once

#include "StemStream.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sim
{
    /** Expands a leading "~/" and makes the path absolute (left as is if that fails). */
    [[nodiscard]] std::filesystem::path resolvePath(const std::string& path);

    /** The shared song position for one period. Every input renders the same
        period with the same frame, which keeps file sources sample-aligned. */
    struct TransportFrame
    {
        bool playing = false;
        std::uint64_t position = 0;     // song position of the period's first frame
        std::uint64_t loopLength = 0;   // longest loaded file; the song loops there
    };

    /** Generates the signal fed into one simulated input. render() runs on the
        audio thread; setters are called from the UI thread. */
    class ISource
    {
    public:
        virtual ~ISource() = default;
        virtual void render(float* out, int numFrames, const TransportFrame& transport) noexcept = 0;
        [[nodiscard]] virtual std::string describe() const = 0;
        /** Frames of song material (files only); 0 for generated signals. */
        [[nodiscard]] virtual std::size_t getLengthFrames() const noexcept { return 0; }
    };

    class SilenceSource final : public ISource
    {
    public:
        void render(float* out, int numFrames, const TransportFrame& transport) noexcept override;
        [[nodiscard]] std::string describe() const override { return "silence"; }
    };

    class SineSource final : public ISource
    {
    public:
        SineSource(double sampleRate, float frequencyHz, float levelDb);

        void render(float* out, int numFrames, const TransportFrame& transport) noexcept override;
        [[nodiscard]] std::string describe() const override;

        void setFrequency(float hz) noexcept { frequency.store(hz); }
        void setLevelDb(float db) noexcept { levelDb.store(db); }
        [[nodiscard]] float getFrequency() const noexcept { return frequency.load(); }
        [[nodiscard]] float getLevelDb() const noexcept { return levelDb.load(); }

    private:
        double sampleRate;
        double phase = 0.0;
        std::atomic<float> frequency;
        std::atomic<float> levelDb;
    };

    /** A mono (mixed-down) audio file at the device rate. It has no playhead of its
        own: it plays at the transport's song position, and is silent while stopped or
        past its end (until the song loops). Short files are held in memory; long ones
        stream from disk through a StemStream (a set of stems decoded whole is gigabytes,
        and once the OS pages them out the audio callback stalls on page faults). */
    class FileSource final : public ISource
    {
    public:
        /** How a file is kept: in memory up to residentMaxFrames, streamed beyond. */
        struct Storage
        {
            std::uint64_t residentMaxFrames = 0;
            StreamLayout layout;

            /** Up to 30 s in memory; longer files stream with StreamLayout::forRate. */
            [[nodiscard]] static Storage forRate(double sampleRate);
        };

        /** Opens WAV/FLAC/MP3 (miniaudio), mixed to mono and resampled to targetRate.
            Returns nullptr and sets `error` on failure. Reads short files whole and the
            head of long ones, so it can take a while: call it off the UI thread. Setting
            `cancel` stops it (error "cancelled"). */
        static std::shared_ptr<FileSource> load(const std::string& path, double targetRate,
                                                float levelDb, std::string& error,
                                                const std::atomic<bool>* cancel = nullptr,
                                                std::optional<Storage> storage = std::nullopt);

        /** Builds an in-memory source from mono samples already at the device rate. */
        static std::shared_ptr<FileSource> fromSamples(std::vector<float> mono, float levelDb,
                                                       std::string name = "buffer");

        ~FileSource() override;
        FileSource(const FileSource&) = delete;
        FileSource& operator=(const FileSource&) = delete;

        void render(float* out, int numFrames, const TransportFrame& transport) noexcept override;
        [[nodiscard]] std::string describe() const override;

        void setLevelDb(float db) noexcept { levelDb.store(db); }
        [[nodiscard]] float getLevelDb() const noexcept { return levelDb.load(); }
        [[nodiscard]] std::size_t getLengthFrames() const noexcept override;

        [[nodiscard]] bool isStreamed() const noexcept { return stream != nullptr; }
        /** Frames a streamed file played silent because its read-ahead ran dry. */
        [[nodiscard]] std::uint64_t getUnderruns() const noexcept { return stream ? stream->getUnderruns() : 0; }

    private:
        FileSource(std::vector<float> resampled, float levelDb, std::string name);
        FileSource(std::unique_ptr<StemStream> stream, float levelDb, std::string name);

        std::vector<float> samples;          // in memory, or
        std::unique_ptr<StemStream> stream;  // streamed (fed by `producer`)
        std::thread producer;
        std::atomic<bool> stopProducer { false };
        std::atomic<float> levelDb;
        std::string name;
    };
}
