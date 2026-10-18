#include "SimConfig.h"

#include <gtest/gtest.h>

#include <vector>

using namespace sim;

namespace
{
    ParseResult parse(std::vector<const char*> args)
    {
        args.insert(args.begin(), "StageCueSoundcardSim");
        return parseArgs(static_cast<int>(args.size()), args.data());
    }
}

TEST(SimConfig, DefaultsMatchPluginHostDeviceDefaults)
{
    const auto result = parse({});
    ASSERT_TRUE(result.config);
    EXPECT_EQ(result.config->numInputs, 8);
    EXPECT_EQ(result.config->numOutputs, 8);
    EXPECT_EQ(result.config->sampleRate, 48000u);
    EXPECT_EQ(result.config->periodFrames, 512u);
    EXPECT_EQ(simPcmName(*result.config), "hw:CARD=StageCueSim,DEV=0");
    EXPECT_EQ(appPcmName(*result.config), "hw:CARD=StageCueSim,DEV=1");
}

#if defined(__APPLE__)
TEST(SimConfig, ParsesIoCountsAndDevices)
{
    const auto result = parse({ "--inputs", "4", "--outputs", "2", "--inputs-device", "BlackHole 16ch",
                                "--outputs-device", "BlackHole 2ch", "--host-device", "BlackHole 64ch",
                                "--monitor", "MacBook Pro Speakers" });
    ASSERT_TRUE(result.config);
    EXPECT_EQ(result.config->numInputs, 4);
    EXPECT_EQ(result.config->numOutputs, 2);
    EXPECT_EQ(result.config->inputsDevice, "BlackHole 16ch");
    EXPECT_EQ(result.config->outputsDevice, "BlackHole 2ch");
    EXPECT_EQ(result.config->hostDevice, "BlackHole 64ch");
    EXPECT_EQ(result.config->monitorDevice, "MacBook Pro Speakers");
}

TEST(SimConfig, DefaultsToTheStageCueLoopbackDevices)
{
    const auto result = parse({});
    ASSERT_TRUE(result.config);
    // One crossed loopback driver: the sim uses its "sim side" device for both directions,
    // PluginHost a single device for input and output.
    EXPECT_EQ(result.config->inputsDevice, "StageCue Soundcard Sim (sim side)");
    EXPECT_EQ(result.config->outputsDevice, "StageCue Soundcard Sim (sim side)");
    EXPECT_EQ(result.config->hostDevice, "StageCue Soundcard Sim");
    EXPECT_FALSE(parse({ "--card", "Foo" }).config);
}
#else
TEST(SimConfig, ParsesIoCountsAndDevices)
{
    const auto result = parse({ "--inputs", "4", "--outputs", "2", "--card", "Foo", "--monitor", "pipewire" });
    ASSERT_TRUE(result.config);
    EXPECT_EQ(result.config->numInputs, 4);
    EXPECT_EQ(result.config->numOutputs, 2);
    EXPECT_EQ(result.config->cardId, "Foo");
    EXPECT_EQ(result.config->monitorDevice, "pipewire");
    EXPECT_EQ(simPcmName(*result.config), "hw:CARD=Foo,DEV=0");
}
#endif

TEST(SimConfig, TransportStartsStoppedUnlessPlayIsGiven)
{
    EXPECT_FALSE(parse({}).config->startPlaying);

    const auto result = parse({ "--play", "--inputs", "2" });
    ASSERT_TRUE(result.config);
    EXPECT_TRUE(result.config->startPlaying);
    EXPECT_EQ(result.config->numInputs, 2);
}

TEST(SimConfig, HeadlessIsOptIn)
{
    EXPECT_FALSE(parse({}).config->headless);
    EXPECT_TRUE(parse({ "--headless" }).config->headless);
}

TEST(SimConfig, RejectsOutOfRangeChannelCounts)
{
    EXPECT_FALSE(parse({ "--inputs", "0" }).config);
    EXPECT_FALSE(parse({ "--outputs", "33" }).config);
    EXPECT_FALSE(parse({ "--inputs", "abc" }).config);
    EXPECT_EQ(parse({ "--inputs", "0" }).exitCode, 2);
}

TEST(SimConfig, HelpExitsCleanly)
{
    const auto result = parse({ "--help" });
    EXPECT_FALSE(result.config);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.message.find("Usage"), std::string::npos);
}

TEST(SimConfig, ParsesSineSourceSpec)
{
    std::string error;
    const auto spec = parseSourceSpec("2=sine:440:-6", error);
    ASSERT_TRUE(spec) << error;
    EXPECT_EQ(spec->input, 1);
    EXPECT_EQ(spec->kind, SourceSpec::Kind::Sine);
    EXPECT_FLOAT_EQ(spec->frequencyHz, 440.0f);
    EXPECT_FLOAT_EQ(spec->levelDb, -6.0f);

    const auto defaults = parseSourceSpec("1=sine", error);
    ASSERT_TRUE(defaults) << error;
    EXPECT_FLOAT_EQ(defaults->frequencyHz, 1000.0f);
    EXPECT_FLOAT_EQ(defaults->levelDb, -12.0f);
}

TEST(SimConfig, ParsesFileSourceSpecWithOptionalLevel)
{
    std::string error;
    const auto withLevel = parseSourceSpec("3=file:/tmp/a:b.wav:-3", error);
    ASSERT_TRUE(withLevel) << error;
    EXPECT_EQ(withLevel->filePath, "/tmp/a:b.wav");
    EXPECT_FLOAT_EQ(withLevel->levelDb, -3.0f);

    const auto withoutLevel = parseSourceSpec("3=file:/tmp/kick.wav", error);
    ASSERT_TRUE(withoutLevel) << error;
    EXPECT_EQ(withoutLevel->filePath, "/tmp/kick.wav");
}

TEST(SimConfig, RejectsBadSourceSpecs)
{
    std::string error;
    EXPECT_FALSE(parseSourceSpec("sine", error));
    EXPECT_FALSE(parseSourceSpec("0=sine", error));
    EXPECT_FALSE(parseSourceSpec("1=sine:abc", error));
    EXPECT_FALSE(parseSourceSpec("1=file", error));
    EXPECT_FALSE(parseSourceSpec("1=noise", error));
}

TEST(SimConfig, RejectsSourceForMissingInput)
{
    EXPECT_FALSE(parse({ "--inputs", "2", "--source", "3=sine" }).config);
    EXPECT_TRUE(parse({ "--inputs", "3", "--source", "3=sine" }).config);
}

TEST(SimConfig, FormatsSourceSpecsThatParseBack)
{
    std::string error;
    for (const char* text : { "1=silence", "2=sine:440:-6", "3=file:/tmp/My Stems/a:b ü.wav:-12", "4=sine:1234.5:-3.25" })
    {
        const auto spec = parseSourceSpec(text, error);
        ASSERT_TRUE(spec) << error;
        EXPECT_EQ(formatSourceSpec(*spec), text);
    }
}

TEST(SimConfig, FormattedFileSpecAlwaysCarriesALevel)
{
    // A path ending in ":12" would otherwise read back as a level.
    SourceSpec spec;
    spec.kind = SourceSpec::Kind::File;
    spec.filePath = "/tmp/take:12";
    spec.levelDb = -12.0f;
    std::string error;
    const auto parsed = parseSourceSpec(formatSourceSpec(spec), error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->filePath, "/tmp/take:12");
    EXPECT_FLOAT_EQ(parsed->levelDb, -12.0f);
}
