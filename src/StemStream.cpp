#include "StemStream.h"

#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sim
{
    namespace
    {
        /** Best effort: keeps the callback's memory out of swap and the compressor. */
        void lockResident(std::vector<float>& data) noexcept
        {
            if (!data.empty())
                (void) ::mlock(data.data(), data.size() * sizeof(float));
        }

        void unlockResident(std::vector<float>& data) noexcept
        {
            if (!data.empty())
                (void) ::munlock(data.data(), data.size() * sizeof(float));
        }
    }

    // --------------------------------------------------------- MemoryFrameReader

    bool MemoryFrameReader::seek(std::uint64_t frame)
    {
        position = std::min<std::uint64_t>(frame, samples.size());
        return true;
    }

    std::uint64_t MemoryFrameReader::read(float* out, std::uint64_t numFrames)
    {
        const auto n = std::min<std::uint64_t>(numFrames, samples.size() - position);
        std::copy_n(samples.data() + position, n, out);
        position += n;
        return n;
    }

    // -------------------------------------------------------------- StreamLayout

    StreamLayout StreamLayout::forRate(double sampleRate)
    {
        StreamLayout layout;
        layout.headFrames = static_cast<std::size_t>(std::lround(2.0 * sampleRate));
        layout.jumpLeadFrames = static_cast<std::size_t>(std::lround(0.1 * sampleRate));
        return layout;
    }

    // ---------------------------------------------------------------- StemStream

    StemStream::StemStream(std::unique_ptr<IFrameReader> r, std::uint64_t lengthFrames, const StreamLayout& l)
        : reader(std::move(r)), length(lengthFrames), layout(l)
    {
        head.assign(static_cast<std::size_t>(std::min<std::uint64_t>(layout.headFrames, length)), 0.0f);
        reader->seek(0);
        (void) reader->read(head.data(), head.size());
        lockResident(head);

        ring.resize(layout.ringBlocks);
        for (auto& block : ring)
        {
            block.data.assign(layout.blockFrames, 0.0f);
            lockResident(block.data);
        }
        nextFrame = head.size();   // the reader is already there
    }

    StemStream::~StemStream()
    {
        unlockResident(head);
        for (auto& block : ring)
            unlockResident(block.data);
    }

    bool StemStream::fill()
    {
        const std::uint64_t streamStart = head.size();
        if (streamStart >= length || ring.empty())
            return false;

        bool wrote = false;
        while (true)
        {
            // Acquire: the target was stored before its generation.
            if (const auto g = seekGeneration.load(std::memory_order_acquire); g != producerGeneration)
            {
                producerGeneration = g;
                nextFrame = seekTarget.load(std::memory_order_relaxed);
                reader->seek(nextFrame);
            }

            const auto w = written.load(std::memory_order_relaxed);
            if (w - consumed.load(std::memory_order_acquire) >= ring.size())
                break;

            auto& block = ring[w % ring.size()];
            const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(layout.blockFrames, length - nextFrame));
            const auto got = static_cast<std::size_t>(reader->read(block.data.data(), frames));
            std::fill(block.data.begin() + static_cast<std::ptrdiff_t>(got),
                      block.data.begin() + static_cast<std::ptrdiff_t>(frames), 0.0f);   // file shorter than its length
            block.generation = producerGeneration;
            block.start = nextFrame;
            block.frames = frames;
            written.store(w + 1, std::memory_order_release);
            wrote = true;

            nextFrame += frames;
            if (nextFrame >= length)
            {
                // The song plays the head itself, so the ring wraps to just after it.
                nextFrame = streamStart;
                reader->seek(nextFrame);
            }
        }
        return wrote;
    }

    void StemStream::render(float* out, int numFrames, std::uint64_t position, std::uint64_t loopLength) noexcept
    {
        if (loopLength == 0)
        {
            std::fill_n(out, numFrames, 0.0f);
            return;
        }

        position %= loopLength;
        if (position != expectedPosition)
            jumpTo(position, loopLength);
        discardStale();

        for (int i = 0; i < numFrames; ++i)
        {
            const bool leading = leadRemaining > 0;
            if (leading)
                --leadRemaining;

            if (position < head.size())
                out[i] = head[position];
            else if (position >= length || leading)
                out[i] = 0.0f;
            else
                out[i] = streamedSample(position);

            if (++position == loopLength)
                position = 0;
        }
        expectedPosition = position;
    }

    float StemStream::streamedSample(std::uint64_t position) noexcept
    {
        const std::uint64_t span = length - head.size();   // the cycle the ring follows
        while (true)
        {
            const auto c = consumed.load(std::memory_order_relaxed);
            if (c == written.load(std::memory_order_acquire))
                break;

            const auto& block = ring[c % ring.size()];
            if (block.generation == generation)
            {
                if (position >= block.start && position < block.start + block.frames)
                {
                    const float sample = block.data[static_cast<std::size_t>(position - block.start)];
                    if (position + 1 == block.start + block.frames)
                        consumed.store(c + 1, std::memory_order_release);
                    return sample;
                }
                // Ahead (within half the cycle): the ring has not reached us; wait for it.
                if ((block.start + span - position) % span < span / 2)
                    break;
            }
            consumed.store(c + 1, std::memory_order_release);   // stale or already passed
        }
        underruns.fetch_add(1, std::memory_order_relaxed);
        return 0.0f;
    }

    void StemStream::jumpTo(std::uint64_t position, std::uint64_t loopLength) noexcept
    {
        const std::uint64_t streamStart = head.size();
        leadRemaining = 0;
        if (position >= streamStart && position < length)
            leadRemaining = layout.jumpLeadFrames;   // into the streamed part: let the producer get there first

        // The ring resumes where the song will be once the lead is over; from the head
        // or past the file's end, that is the head's end.
        auto target = (position + leadRemaining) % loopLength;
        if (target < streamStart || target >= length)
            target = streamStart;

        ++generation;
        seekTarget.store(target, std::memory_order_relaxed);
        seekGeneration.store(generation, std::memory_order_release);
    }

    void StemStream::discardStale() noexcept
    {
        auto c = consumed.load(std::memory_order_relaxed);
        const auto w = written.load(std::memory_order_acquire);
        while (c != w && ring[c % ring.size()].generation != generation)
            ++c;
        consumed.store(c, std::memory_order_release);
    }
}
