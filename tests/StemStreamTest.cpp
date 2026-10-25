#include "StemStream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace sim;

namespace
{
    std::vector<float> ramp(std::size_t length)
    {
        std::vector<float> samples(length);
        for (std::size_t i = 0; i < length; ++i)
            samples[i] = static_cast<float>(i + 1) / static_cast<float>(length);
        return samples;
    }

    // 1000-frame file: 100 resident, 4 blocks of 50 read ahead, and a jump lead of two
    // periods (the producer only runs between periods here).
    constexpr StreamLayout kLayout { 100, 50, 4, 64 };
    constexpr int kPeriod = 32;

    std::unique_ptr<StemStream> makeStream(const std::vector<float>& samples)
    {
        return std::make_unique<StemStream>(std::make_unique<MemoryFrameReader>(samples), samples.size(), kLayout);
    }

    /** Renders [from, from + frames) in periods like the device does, letting the
        producer catch up between periods; returns the song position it stopped at. */
    std::uint64_t play(StemStream& stream, std::uint64_t from, std::uint64_t frames, std::uint64_t loopLength,
                       std::vector<float>& rendered)
    {
        rendered.clear();
        auto position = from;
        std::vector<float> period(kPeriod);
        for (std::uint64_t done = 0; done < frames; done += kPeriod)
        {
            stream.fill();
            stream.render(period.data(), kPeriod, position, loopLength);
            rendered.insert(rendered.end(), period.begin(), period.end());
            position = (position + kPeriod) % loopLength;
        }
        return position;
    }
}

TEST(MemoryFrameReader, ReadsFromWhereItSeeked)
{
    MemoryFrameReader reader({ 1.0f, 2.0f, 3.0f, 4.0f });
    float out[4] {};
    EXPECT_EQ(reader.read(out, 2), 2u);
    EXPECT_EQ(out[1], 2.0f);
    EXPECT_TRUE(reader.seek(3));
    EXPECT_EQ(reader.read(out, 4), 1u);
    EXPECT_EQ(out[0], 4.0f);
}

TEST(StemStream, PlaysTheWholeFileAcrossLoops)
{
    const auto samples = ramp(1000);
    auto stream = makeStream(samples);

    std::vector<float> rendered;
    play(*stream, 0, 3 * 1024, 1024, rendered);
    for (std::size_t i = 0; i < rendered.size(); ++i)
    {
        const auto songFrame = i % 1024;
        ASSERT_FLOAT_EQ(rendered[i], songFrame < samples.size() ? samples[songFrame] : 0.0f) << "frame " << i;
    }
    EXPECT_EQ(stream->getUnderruns(), 0u);
}

TEST(StemStream, RestartPlaysFromTheHeadWithoutAGap)
{
    const auto samples = ramp(1000);
    auto stream = makeStream(samples);

    std::vector<float> rendered;
    play(*stream, 0, 640, 1000, rendered);

    // Rewind to 0: the head covers the first 100 frames while the ring refills.
    play(*stream, 0, 320, 1000, rendered);
    for (std::size_t i = 0; i < rendered.size(); ++i)
        ASSERT_FLOAT_EQ(rendered[i], samples[i]) << "frame " << i;
    EXPECT_EQ(stream->getUnderruns(), 0u);
}

TEST(StemStream, JumpIntoTheStreamedPartIsSilentForTheLeadThenAligned)
{
    const auto samples = ramp(1000);
    auto stream = makeStream(samples);

    std::vector<float> rendered;
    play(*stream, 0, 128, 1000, rendered);
    play(*stream, 500, 128, 1000, rendered);
    for (std::size_t i = 0; i < rendered.size(); ++i)
    {
        const float expected = i < kLayout.jumpLeadFrames ? 0.0f : samples[500 + i];
        ASSERT_FLOAT_EQ(rendered[i], expected) << "frame " << i;
    }
    EXPECT_EQ(stream->getUnderruns(), 0u);
}

TEST(StemStream, FallingBehindIsSilentButStaysAligned)
{
    const auto samples = ramp(1000);
    auto stream = makeStream(samples);

    // No fill() for longer than the read-ahead: the ring runs dry.
    std::vector<float> period(kPeriod);
    std::uint64_t position = 0;
    for (int i = 0; i < 12; ++i, position += kPeriod)
        stream->render(period.data(), kPeriod, position, 1000);
    EXPECT_GT(stream->getUnderruns(), 0u);
    EXPECT_EQ(period[0], 0.0f);

    // Once the producer catches up, frames are the song's again.
    std::vector<float> rendered;
    play(*stream, position, 256, 1000, rendered);
    const auto tail = rendered.size() - kPeriod;
    for (std::size_t i = tail; i < rendered.size(); ++i)
        ASSERT_FLOAT_EQ(rendered[i], samples[position + i]) << "frame " << position + i;
}

TEST(StreamLayout, SizesHeadReadAheadAndLeadForTheRate)
{
    const auto layout = StreamLayout::forRate(48000.0);
    EXPECT_EQ(layout.headFrames, 96000u);
    EXPECT_EQ(layout.blockFrames * layout.ringBlocks, 262144u);
    EXPECT_EQ(layout.jumpLeadFrames, 4800u);
}

TEST(StemStream, JumpNearTheEndPlaysTheHeadAndResumesAfterIt)
{
    const auto samples = ramp(1000);
    auto stream = makeStream(samples);

    std::vector<float> rendered;
    play(*stream, 0, 128, 1000, rendered);
    play(*stream, 960, 320, 1000, rendered);   // lead runs past the loop point
    for (std::size_t i = 0; i < rendered.size(); ++i)
    {
        const auto songFrame = (960 + i) % 1000;
        const float expected = songFrame >= 960 ? 0.0f : samples[songFrame];
        ASSERT_FLOAT_EQ(rendered[i], expected) << "frame " << i;
    }
    EXPECT_EQ(stream->getUnderruns(), 0u);
}
