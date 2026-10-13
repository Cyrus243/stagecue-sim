#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim
{
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

    /** A mono (mixed-down) audio file, resampled once to the device rate. It has no
        playhead of its own: it plays at the transport's song position, and is
        silent while stopped or past its end (until the song loops). */
    class FileSource final : public ISource
    {
    public:
        /** Decodes WAV/AIFF/FLAC (dr_libs). Returns nullptr and sets `error` on failure. */
        static std::shared_ptr<FileSource> load(const std::string& path, double targetRate,
                                                float levelDb, std::string& error);

        /** Builds a source from already-decoded mono samples (resampling if needed). */
        static std::shared_ptr<FileSource> fromSamples(std::vector<float> mono, double sourceRate,
                                                       double targetRate, float levelDb,
                                                       std::string name = "buffer");

        void render(float* out, int numFrames, const TransportFrame& transport) noexcept override;
        [[nodiscard]] std::string describe() const override;

        void setLevelDb(float db) noexcept { levelDb.store(db); }
        [[nodiscard]] float getLevelDb() const noexcept { return levelDb.load(); }
        [[nodiscard]] std::size_t getLengthFrames() const noexcept override { return samples.size(); }

    private:
        FileSource(std::vector<float> resampled, float levelDb, std::string name);

        std::vector<float> samples;
        std::atomic<float> levelDb;
        std::string name;
    };
}
