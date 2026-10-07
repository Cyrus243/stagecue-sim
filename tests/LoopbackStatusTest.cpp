#include "LoopbackStatus.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace sim;

TEST(LoopbackStatus, ClosedSubstreamIsNotOpen)
{
    EXPECT_FALSE(parseSubstreamStatus("closed\n", "closed\n").open);
    EXPECT_FALSE(parseSubstreamStatus("", "").open);
}

TEST(LoopbackStatus, ParsesRunningSubstream)
{
    const auto status = parseSubstreamStatus(
        "state: RUNNING\nowner_pid   : 1234\ntrigger_time: 1.0\n",
        "access: RW_INTERLEAVED\nformat: S32_LE\nsubformat: STD\nchannels: 4\nrate: 48000 (48000/1)\nperiod_size: 512\n");
    EXPECT_TRUE(status.open);
    EXPECT_EQ(status.state, "RUNNING");
    EXPECT_EQ(status.channels, 4);
    EXPECT_EQ(status.rate, 48000u);
    EXPECT_EQ(status.format, "S32_LE");
}

TEST(LoopbackStatus, ReadsProcTree)
{
    const auto root = std::filesystem::temp_directory_path() / "soundcard-sim-proc-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Sim" / "pcm1c" / "sub0");
    std::filesystem::create_directories(root / "Sim" / "pcm1p" / "sub0");
    std::ofstream(root / "Sim" / "pcm1c" / "sub0" / "status") << "state: RUNNING\n";
    std::ofstream(root / "Sim" / "pcm1c" / "sub0" / "hw_params") << "channels: 2\nrate: 48000 (48000/1)\n";
    std::ofstream(root / "Sim" / "pcm1p" / "sub0" / "status") << "closed\n";

    const auto status = queryLoopbackStatus("Sim", root.string());
    EXPECT_TRUE(status.cardPresent);
    EXPECT_TRUE(status.appCapture.open);
    EXPECT_EQ(status.appCapture.channels, 2);
    EXPECT_FALSE(status.appPlayback.open);

    EXPECT_FALSE(queryLoopbackStatus("Missing", root.string()).cardPresent);
    std::filesystem::remove_all(root);
}
