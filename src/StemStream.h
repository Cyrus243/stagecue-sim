#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sim
{
    /** Where a stream reads its file from: miniaudio's decoder, or memory in tests.
        Producer thread only. */
    class IFrameReader
    {
    public:
        virtual ~IFrameReader() = default;
        /** Moves to `frame` (device-rate frames from the start). */
        virtual bool seek(std::uint64_t frame) = 0;
        /** Reads up to `numFrames` mono frames; returns how many it read. */
        virtual std::uint64_t read(float* out, std::uint64_t numFrames) = 0;
    };

    /** Mono frames held in memory (tests, and fromSamples). */
    class MemoryFrameReader final : public IFrameReader
    {
    public:
        explicit MemoryFrameReader(std::vector<float> samples) : samples(std::move(samples)) {}
        bool seek(std::uint64_t frame) override;
        std::uint64_t read(float* out, std::uint64_t numFrames) override;

    private:
        std::vector<float> samples;
        std::uint64_t position = 0;
    };

    /** Sizes of a stream, in frames. */
    struct StreamLayout
    {
        std::size_t headFrames = 0;    // kept resident: a restart plays from here without a gap
        std::size_t blockFrames = 4096;
        std::size_t ringBlocks = 64;   // read-ahead past the song position
        std::size_t jumpLeadFrames = 0;   // silence after a jump into the streamed part

        /** About 2 s of head, 5.5 s of read-ahead and a 100 ms jump lead at `sampleRate`. */
        [[nodiscard]] static StreamLayout forRate(double sampleRate);
    };

    /** Plays one long file from a small ring instead of holding it all in memory
        (a set of stems can be gigabytes, and paged-out samples stall the callback).

        The first headFrames stay resident. The rest, [head, length), is decoded block by
        block into the ring by fill() on a producer thread, cycling back to the head's end
        when it reaches the file's end. render() runs on the audio thread and only reads
        the ring. Every block carries the song position it holds, so a stream that falls
        behind or jumps plays silence rather than drifting out of sync with the others. */
    class StemStream
    {
    public:
        StemStream(std::unique_ptr<IFrameReader> reader, std::uint64_t lengthFrames, const StreamLayout& layout);
        ~StemStream();
        StemStream(const StemStream&) = delete;
        StemStream& operator=(const StemStream&) = delete;

        /** Producer: follows the latest jump and decodes until the ring is full.
            Returns false when there was nothing to do. */
        bool fill();

        /** Audio thread: unscaled samples for song positions position, position + 1, ...
            wrapping at loopLength (>= the file's length). Frames past the file's end are
            silent; so are frames the ring does not hold yet. */
        void render(float* out, int numFrames, std::uint64_t position, std::uint64_t loopLength) noexcept;

        [[nodiscard]] std::uint64_t getLengthFrames() const noexcept { return length; }
        /** Frames rendered silent because the ring had not caught up (not jump leads). */
        [[nodiscard]] std::uint64_t getUnderruns() const noexcept { return underruns.load(); }

    private:
        struct Block
        {
            std::vector<float> data;
            std::uint64_t generation = 0;
            std::uint64_t start = 0;   // song position of data[0]
            std::size_t frames = 0;
        };

        [[nodiscard]] float streamedSample(std::uint64_t position) noexcept;
        void jumpTo(std::uint64_t position, std::uint64_t loopLength) noexcept;
        void discardStale() noexcept;

        std::unique_ptr<IFrameReader> reader;
        const std::uint64_t length;
        const StreamLayout layout;
        std::vector<float> head;
        std::vector<Block> ring;

        // Single producer, single consumer: the producer owns `written` and the block
        // it is filling, the consumer owns `consumed`.
        std::atomic<std::uint64_t> written { 0 };
        std::atomic<std::uint64_t> consumed { 0 };
        // A jump: the consumer stores the target, then the generation.
        std::atomic<std::uint64_t> seekGeneration { 0 };
        std::atomic<std::uint64_t> seekTarget { 0 };
        std::atomic<std::uint64_t> underruns { 0 };

        // Producer state.
        std::uint64_t producerGeneration = 0;
        std::uint64_t nextFrame = 0;

        // Consumer state.
        std::uint64_t generation = 0;
        std::uint64_t expectedPosition = 0;
        std::uint64_t leadRemaining = 0;   // song frames still silent after a jump
    };
}
