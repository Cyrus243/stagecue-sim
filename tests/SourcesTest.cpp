#include "Meters.h"
#include "Sources.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

using namespace sim;

namespace
{
    float peakOf(const std::vector<float>& data)
    {
        float peak = 0.0f;
        for (float v : data)
            peak = std::max(peak, std::abs(v));
        return peak;
    }

    template <typename T>
    void writeLE(std::ofstream& out, T value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    /** Writes a minimal 16-bit PCM WAV so the test does not depend on JUCE headers. */
    std::filesystem::path writeWav(const std::string& name, const std::vector<std::vector<float>>& channels,
                                   std::uint32_t rate)
    {
        const auto path = std::filesystem::temp_directory_path() / name;
        const auto numChannels = static_cast<std::uint16_t>(channels.size());
        const auto frames = static_cast<std::uint32_t>(channels[0].size());
        const std::uint32_t dataBytes = frames * numChannels * 2;

        std::ofstream out(path, std::ios::binary);
        out.write("RIFF", 4); writeLE<std::uint32_t>(out, 36 + dataBytes); out.write("WAVE", 4);
        out.write("fmt ", 4); writeLE<std::uint32_t>(out, 16); writeLE<std::uint16_t>(out, 1);
        writeLE<std::uint16_t>(out, numChannels); writeLE<std::uint32_t>(out, rate);
        writeLE<std::uint32_t>(out, rate * numChannels * 2); writeLE<std::uint16_t>(out, static_cast<std::uint16_t>(numChannels * 2));
        writeLE<std::uint16_t>(out, 16);
        out.write("data", 4); writeLE<std::uint32_t>(out, dataBytes);
        for (std::uint32_t i = 0; i < frames; ++i)
            for (const auto& channel : channels)
                writeLE<std::int16_t>(out, static_cast<std::int16_t>(std::lround(channel[i] * 32767.0f)));
        return path;
    }
}

TEST(SilenceSource, RendersZeros)
{
    SilenceSource source;
    std::vector<float> buffer(64, 1.0f);
    source.render(buffer.data(), 64);
    EXPECT_EQ(peakOf(buffer), 0.0f);
}

TEST(SineSource, PeakMatchesRequestedLevel)
{
    SineSource source(48000.0, 1000.0f, -12.0f);
    std::vector<float> buffer(4800);
    source.render(buffer.data(), static_cast<int>(buffer.size()));
    EXPECT_NEAR(gainToDb(peakOf(buffer)), -12.0f, 0.05f);

    source.setLevelDb(-6.0f);
    source.render(buffer.data(), static_cast<int>(buffer.size()));
    EXPECT_NEAR(gainToDb(peakOf(buffer)), -6.0f, 0.05f);
}

TEST(SineSource, IsPhaseContinuousAcrossBlocks)
{
    SineSource chunked(48000.0, 997.0f, 0.0f);
    SineSource whole(48000.0, 997.0f, 0.0f);

    std::vector<float> a(1024), b(1024);
    chunked.render(a.data(), 300);
    chunked.render(a.data() + 300, 724);
    whole.render(b.data(), 1024);
    for (std::size_t i = 0; i < a.size(); ++i)
        ASSERT_NEAR(a[i], b[i], 1e-4f) << "sample " << i;
}

TEST(FileSource, LoopsSeamlesslyAtDeviceRate)
{
    std::vector<float> ramp(1000);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 1000.0f;

    auto source = FileSource::fromSamples(ramp, 48000.0, 48000.0, 0.0f);
    ASSERT_EQ(source->getLengthFrames(), 1000u);

    std::vector<float> out(2500);
    source->render(out.data(), 2500);
    EXPECT_FLOAT_EQ(out[999], ramp[999]);
    EXPECT_FLOAT_EQ(out[1000], ramp[0]);
    EXPECT_FLOAT_EQ(out[2499], ramp[499]);
}

TEST(FileSource, ResamplesToDeviceRate)
{
    std::vector<float> oneSecond(44100, 0.25f);
    auto source = FileSource::fromSamples(oneSecond, 44100.0, 48000.0, 0.0f);
    EXPECT_EQ(source->getLengthFrames(), 48000u);
}

TEST(FileSource, LoadsWavMixesToMonoAndAppliesLevel)
{
    constexpr std::uint32_t rate = 44100;
    std::vector<float> left(rate), right(rate);
    for (std::uint32_t i = 0; i < rate; ++i)
    {
        left[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * i / rate));
        right[i] = left[i];
    }
    const auto path = writeWav("soundcard-sim-test.wav", { left, right }, rate);

    std::string error;
    auto source = FileSource::load(path.string(), 48000.0, -6.0f, error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->getLengthFrames(), 48000u);
    EXPECT_NE(source->describe().find("soundcard-sim-test.wav"), std::string::npos);

    std::vector<float> out(4800);
    source->render(out.data(), static_cast<int>(out.size()));
    EXPECT_NEAR(gainToDb(peakOf(out)), gainToDb(0.5f) - 6.0f, 0.2f);

    std::filesystem::remove(path);
}

TEST(FileSource, ReportsMissingAndInvalidFiles)
{
    std::string error;
    EXPECT_EQ(FileSource::load("/nonexistent/file.wav", 48000.0, 0.0f, error), nullptr);
    EXPECT_NE(error.find("not found"), std::string::npos);

    const auto bogus = std::filesystem::temp_directory_path() / "soundcard-sim-bogus.wav";
    std::ofstream(bogus) << "definitely not audio";
    error.clear();
    EXPECT_EQ(FileSource::load(bogus.string(), 48000.0, 0.0f, error), nullptr);
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(bogus);
}
