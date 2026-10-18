#include "Mixer.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace sim;

namespace
{
    /** Planar buffers for `channels` x `frames`, with the pointer array the Mixer takes. */
    struct Planar
    {
        Planar(int channels, int frames)
            : data(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames)))
        {
            for (auto& channel : data)
                pointers.push_back(channel.data());
        }

        std::vector<std::vector<float>> data;
        std::vector<float*> pointers;
    };

    std::vector<float> ramp(std::size_t length)
    {
        std::vector<float> samples(length);
        for (std::size_t i = 0; i < length; ++i)
            samples[i] = static_cast<float>(i + 1) / static_cast<float>(length);
        return samples;
    }
}

TEST(Mixer, NullSourcesRenderSilenceAndBumpTheEpoch)
{
    Mixer mixer;
    Planar out(2, 64);
    out.data[0].assign(64, 1.0f);

    mixer.renderInputs(out.pointers.data(), 2, 64);
    EXPECT_EQ(out.data[0][0], 0.0f);
    EXPECT_EQ(mixer.getEpoch(), 1u);
}

TEST(Mixer, FilesStaySampleAlignedAndLoopAtTheLongest)
{
    Mixer mixer;
    const auto longSamples = ramp(1000);
    const std::vector<float> shortSamples(longSamples.begin(), longSamples.begin() + 300);
    auto longFile = FileSource::fromSamples(longSamples, 0.0f);
    auto shortFile = FileSource::fromSamples(shortSamples, 0.0f);
    mixer.setSource(0, longFile.get());
    mixer.setSource(1, shortFile.get());
    mixer.setPlaying(true);

    constexpr int block = 128;
    Planar out(2, block);
    for (std::uint64_t song = 0; song < 3000; song += block)
    {
        mixer.renderInputs(out.pointers.data(), 2, block);
        for (int i = 0; i < block; ++i)
        {
            const auto frame = (song + static_cast<std::uint64_t>(i)) % 1000;
            ASSERT_FLOAT_EQ(out.data[0][i], longSamples[frame]) << "frame " << song + i;
            ASSERT_FLOAT_EQ(out.data[1][i], frame < 300 ? shortSamples[frame] : 0.0f) << "frame " << song + i;
        }
    }
    EXPECT_EQ(mixer.getSongLengthFrames(), 1000u);
}

TEST(Mixer, StoppedTransportHoldsPositionAndRestartRewinds)
{
    Mixer mixer;
    auto file = FileSource::fromSamples(ramp(1000), 0.0f);
    mixer.setSource(0, file.get());
    Planar out(1, 100);

    mixer.setPlaying(true);
    mixer.renderInputs(out.pointers.data(), 1, 100);
    mixer.renderInputs(out.pointers.data(), 1, 100);
    EXPECT_EQ(mixer.getSongPositionFrames(), 200u);

    mixer.setPlaying(false);
    mixer.renderInputs(out.pointers.data(), 1, 100);
    EXPECT_EQ(mixer.getSongPositionFrames(), 200u);
    EXPECT_EQ(out.data[0][0], 0.0f);

    mixer.restart();
    mixer.setPlaying(true);
    mixer.renderInputs(out.pointers.data(), 1, 100);
    EXPECT_FLOAT_EQ(out.data[0][0], 1.0f / 1000.0f);   // first sample of the ramp
    EXPECT_EQ(mixer.getSongPositionFrames(), 100u);
}

TEST(Mixer, MetersInputsAndOutputs)
{
    Mixer mixer;
    auto file = FileSource::fromSamples(std::vector<float>(100, 0.5f), 0.0f);
    mixer.setSource(0, file.get());
    mixer.setPlaying(true);

    Planar in(1, 100);
    mixer.renderInputs(in.pointers.data(), 1, 100);
    EXPECT_FLOAT_EQ(mixer.consumeInputPeak(0), 0.5f);
    EXPECT_EQ(mixer.consumeInputPeak(0), 0.0f);   // consume-and-reset

    Planar out(3, 16);
    out.data[2].assign(16, -0.25f);
    std::vector<float> stereo(32);
    mixer.processOutputs(out.pointers.data(), 3, 16, stereo.data());
    EXPECT_FLOAT_EQ(mixer.consumeOutputPeak(2), 0.25f);
}

TEST(Mixer, MonitorMixesOddOutputsLeftEvenRight)
{
    Mixer mixer;
    mixer.setMonitorEnabled(2, true);   // out 3 -> L, with out 1
    Planar out(4, 8);
    out.data[0].assign(8, 0.1f);
    out.data[1].assign(8, 0.2f);
    out.data[2].assign(8, 0.3f);
    out.data[3].assign(8, 0.4f);   // not monitored by default

    std::vector<float> stereo(16, 9.0f);
    mixer.processOutputs(out.pointers.data(), 4, 8, stereo.data());
    EXPECT_FLOAT_EQ(stereo[0], 0.4f);
    EXPECT_FLOAT_EQ(stereo[1], 0.2f);

    mixer.setMonitorMuted(true);
    mixer.processOutputs(out.pointers.data(), 4, 8, stereo.data());
    EXPECT_EQ(stereo[0], 0.0f);
    EXPECT_EQ(stereo[1], 0.0f);
}
