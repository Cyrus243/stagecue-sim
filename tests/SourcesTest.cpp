#include "Meters.h"
#include "Sources.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
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

    std::vector<float> ramp(std::size_t length)
    {
        std::vector<float> samples(length);
        for (std::size_t i = 0; i < length; ++i)
            samples[i] = static_cast<float>(i + 1) / static_cast<float>(length);
        return samples;
    }

    constexpr TransportFrame kStopped {};
}

TEST(SilenceSource, RendersZeros)
{
    SilenceSource source;
    std::vector<float> buffer(64, 1.0f);
    source.render(buffer.data(), 64, kStopped);
    EXPECT_EQ(peakOf(buffer), 0.0f);
}

TEST(SineSource, PeakMatchesRequestedLevel)
{
    SineSource source(48000.0, 1000.0f, -12.0f);
    std::vector<float> buffer(4800);
    source.render(buffer.data(), static_cast<int>(buffer.size()), kStopped);
    EXPECT_NEAR(gainToDb(peakOf(buffer)), -12.0f, 0.05f);

    source.setLevelDb(-6.0f);
    source.render(buffer.data(), static_cast<int>(buffer.size()), kStopped);
    EXPECT_NEAR(gainToDb(peakOf(buffer)), -6.0f, 0.05f);
}

TEST(SineSource, IsPhaseContinuousAcrossBlocks)
{
    SineSource chunked(48000.0, 997.0f, 0.0f);
    SineSource whole(48000.0, 997.0f, 0.0f);

    std::vector<float> a(1024), b(1024);
    chunked.render(a.data(), 300, kStopped);
    chunked.render(a.data() + 300, 724, kStopped);
    whole.render(b.data(), 1024, kStopped);
    for (std::size_t i = 0; i < a.size(); ++i)
        ASSERT_NEAR(a[i], b[i], 1e-4f) << "sample " << i;
}

TEST(FileSource, PlaysAtTheSongPositionAndWrapsAtTheLoopLength)
{
    const auto samples = ramp(1000);
    auto source = FileSource::fromSamples(samples, 0.0f);
    ASSERT_EQ(source->getLengthFrames(), 1000u);

    // A block straddling the loop point wraps back to the start.
    std::vector<float> out(200);
    source->render(out.data(), 200, { true, 900, 1000 });
    EXPECT_FLOAT_EQ(out[0], samples[900]);
    EXPECT_FLOAT_EQ(out[99], samples[999]);
    EXPECT_FLOAT_EQ(out[100], samples[0]);
    EXPECT_FLOAT_EQ(out[199], samples[99]);
}

TEST(FileSource, IsSilentWhileStoppedButSineKeepsRunning)
{
    auto file = FileSource::fromSamples(std::vector<float>(1000, 0.5f), 0.0f);
    SineSource sine(48000.0, 1000.0f, -12.0f);

    std::vector<float> out(480);
    file->render(out.data(), 480, { false, 100, 1000 });
    EXPECT_EQ(peakOf(out), 0.0f);

    sine.render(out.data(), 480, { false, 100, 1000 });
    EXPECT_GT(peakOf(out), 0.0f);
}

TEST(FileSource, FilesOfDifferentLengthsStayAlignedToOneSong)
{
    const auto longSamples = ramp(1000);
    const std::vector<float> shortSamples(longSamples.begin(), longSamples.begin() + 400);
    auto longFile = FileSource::fromSamples(longSamples, 0.0f);
    auto shortFile = FileSource::fromSamples(shortSamples, 0.0f);

    // The song loops at the longest file; render it in periods like the engine does.
    constexpr int period = 128;
    constexpr std::uint64_t loopLength = 1000;
    std::vector<float> a(period), b(period);
    for (std::uint64_t position = 0; position < 3 * loopLength; position += period)
    {
        const TransportFrame transport { true, position % loopLength, loopLength };
        longFile->render(a.data(), period, transport);
        shortFile->render(b.data(), period, transport);

        for (int i = 0; i < period; ++i)
        {
            const auto songFrame = (position + static_cast<std::uint64_t>(i)) % loopLength;
            ASSERT_FLOAT_EQ(a[i], longSamples[songFrame]) << "frame " << position + i;
            ASSERT_FLOAT_EQ(b[i], songFrame < shortSamples.size() ? shortSamples[songFrame] : 0.0f)
                << "frame " << position + i;
        }
    }
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
    // Resampled 44.1 -> 48 kHz while decoding (give or take the resampler's latency).
    EXPECT_NEAR(static_cast<double>(source->getLengthFrames()), 48000.0, 64.0);
    EXPECT_NE(source->describe().find("soundcard-sim-test.wav"), std::string::npos);

    std::vector<float> out(4800);
    source->render(out.data(), static_cast<int>(out.size()), { true, 4800, source->getLengthFrames() });
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

TEST(FileSource, StopsDecodingWhenCancelled)
{
    constexpr std::uint32_t rate = 48000;
    const auto path = writeWav("soundcard-sim-cancel.wav", { std::vector<float>(rate, 0.25f) }, rate);

    std::atomic<bool> cancelled { true };
    std::string error;
    EXPECT_EQ(FileSource::load(path.string(), 48000.0, 0.0f, error, &cancelled), nullptr);
    EXPECT_EQ(error, "cancelled");

    cancelled = false;
    error.clear();
    EXPECT_NE(FileSource::load(path.string(), 48000.0, 0.0f, error, &cancelled), nullptr) << error;
    std::filesystem::remove(path);
}
