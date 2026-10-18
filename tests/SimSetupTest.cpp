#include "SimSetup.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace sim;

namespace
{
    /** A fresh directory per test, removed afterwards. */
    class TempDirTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::random_device random;
            dir = std::filesystem::temp_directory_path() / ("sim-setup-test-" + std::to_string(random()));
            std::filesystem::create_directories(dir);
        }

        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        static void touch(const std::filesystem::path& path) { std::ofstream(path) << "x"; }

        std::filesystem::path dir;
    };
}

TEST(SimSetup, NaturalOrderComparesNumbersByValueAndIgnoresCase)
{
    EXPECT_TRUE(naturalLess("2 Snare.wav", "10 Tom.wav"));
    EXPECT_TRUE(naturalLess("01 kick.wav", "02 Snare.wav"));
    EXPECT_TRUE(naturalLess("bass.wav", "Drums.wav"));
    EXPECT_FALSE(naturalLess("10 Tom.wav", "2 Snare.wav"));
    EXPECT_FALSE(naturalLess("a.wav", "a.wav"));
}

TEST_F(TempDirTest, ListsOnlyAudioFilesInNaturalOrder)
{
    for (const char* name : { "10 Tom.WAV", "2 Snare.flac", "1 Kick.wav", "3 Gtr.mp3",
                              "._1 Kick.wav", ".DS_Store", "notes.txt" })
        touch(dir / name);
    std::filesystem::create_directory(dir / "old.wav");

    std::string error;
    const auto files = listAudioFiles(dir.string(), error);
    EXPECT_TRUE(error.empty()) << error;
    std::vector<std::string> names;
    for (const auto& file : files)
        names.push_back(file.filename().string());
    EXPECT_EQ(names, (std::vector<std::string> { "1 Kick.wav", "2 Snare.flac", "3 Gtr.mp3", "10 Tom.WAV" }));
    ASSERT_FALSE(files.empty());
    EXPECT_TRUE(files.front().is_absolute());
}

TEST(SimSetup, MissingFolderIsAnError)
{
    std::string error;
    EXPECT_TRUE(listAudioFiles("/definitely/not/here", error).empty());
    EXPECT_FALSE(error.empty());
}

TEST(SimSetup, FormatAndParseRoundTrip)
{
    std::string error;
    InputSetup setup;
    for (const char* text : { "1=file:/tmp/My Stems/a:b ü.wav:-6", "2=sine:1000:-12", "3=silence" })
        setup.sources.push_back(*parseSourceSpec(text, error));

    const auto parsed = parseSetup(formatSetup(setup), error);
    ASSERT_TRUE(parsed) << error;
    ASSERT_EQ(parsed->sources.size(), 3u);
    EXPECT_EQ(parsed->sources[0].filePath, "/tmp/My Stems/a:b ü.wav");
    EXPECT_FLOAT_EQ(parsed->sources[0].levelDb, -6.0f);
    EXPECT_FLOAT_EQ(parsed->sources[1].frequencyHz, 1000.0f);
    EXPECT_EQ(parsed->sources[2].kind, SourceSpec::Kind::Silence);
}

TEST(SimSetup, ParseSkipsCommentsAndBlankLinesAndNamesBadLines)
{
    std::string error;
    const auto ok = parseSetup("# hi\n\n  \n2=sine\r\n", error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(ok->sources.size(), 1u);

    EXPECT_FALSE(parseSetup("# hi\n1=sine\n1=noise\n", error));
    EXPECT_NE(error.find("line 3"), std::string::npos) << error;

    EXPECT_FALSE(parseSetup("1=sine\n1=silence\n", error));
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
    EXPECT_NE(error.find("twice"), std::string::npos) << error;
}

TEST(SimSetup, ValidatesNames)
{
    EXPECT_TRUE(isValidSetupName("band"));
    EXPECT_TRUE(isValidSetupName("Live Set_2-b"));
    for (const char* bad : { "", " band", "band ", "../x", "a/b", "a.setup", "x:y" })
        EXPECT_FALSE(isValidSetupName(bad)) << bad;
    EXPECT_FALSE(isValidSetupName(std::string(65, 'a')));
}

TEST_F(TempDirTest, SaveListLoadAndOverwrite)
{
    const auto setups = dir / "nested" / "setups";
    EXPECT_TRUE(listSetups(setups).empty());   // first run: no folder yet

    std::string error;
    InputSetup a;
    a.sources.push_back(*parseSourceSpec("1=sine:440:-6", error));
    InputSetup b;
    b.sources.push_back(*parseSourceSpec("1=silence", error));
    ASSERT_TRUE(saveSetup(setups, "band", a, error)) << error;
    ASSERT_TRUE(saveSetup(setups, "10 songs", a, error)) << error;
    ASSERT_TRUE(saveSetup(setups, "2 songs", a, error)) << error;
    EXPECT_EQ(listSetups(setups), (std::vector<std::string> { "2 songs", "10 songs", "band" }));

    ASSERT_TRUE(saveSetup(setups, "band", b, error)) << error;   // overwrite
    const auto loaded = loadSetup(setups, "band", error);
    ASSERT_TRUE(loaded) << error;
    ASSERT_EQ(loaded->sources.size(), 1u);
    EXPECT_EQ(loaded->sources[0].kind, SourceSpec::Kind::Silence);

    EXPECT_FALSE(saveSetup(setups, "../evil", a, error));
    EXPECT_FALSE(loadSetup(setups, "missing", error));
    EXPECT_FALSE(error.empty());
}

TEST_F(TempDirTest, SetupsDirectoryFollowsXdgConfigHome)
{
    const char* previous = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> saved = previous ? std::optional<std::string>(previous) : std::nullopt;

    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    EXPECT_EQ(setupsDirectory(), dir / "stagecue-sim" / "setups");
    ::setenv("XDG_CONFIG_HOME", "", 1);
    EXPECT_EQ(setupsDirectory(), std::filesystem::path(std::getenv("HOME")) / ".config" / "stagecue-sim" / "setups");

    if (saved)
        ::setenv("XDG_CONFIG_HOME", saved->c_str(), 1);
    else
        ::unsetenv("XDG_CONFIG_HOME");
}
