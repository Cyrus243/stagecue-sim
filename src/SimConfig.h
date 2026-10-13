#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sim
{
    // snd-aloop supports 1..32 channels per stream.
    constexpr int kMaxChannels = 32;

    /** A source requested on the command line, e.g. `--source 1=sine:1000:-12`. */
    struct SourceSpec
    {
        enum class Kind { Silence, Sine, File };

        int input = 0;              // 0-based input index
        Kind kind = Kind::Silence;
        float frequencyHz = 1000.0f;
        float levelDb = -12.0f;
        std::string filePath;
    };

    struct SimConfig
    {
        int numInputs = 8;
        int numOutputs = 8;
        unsigned sampleRate = 48000;     // PluginHost's DeviceManager default
        unsigned periodFrames = 512;     // PluginHost's DeviceManager default buffer size
        unsigned periods = 4;
        std::string cardId = "StageCueSim";
        std::string monitorDevice = "default";
        bool startPlaying = false;       // --play: start the song transport right away
        std::vector<SourceSpec> sources;
    };

    /** ALSA name of the simulator's end of the loopback cable (device 0). */
    [[nodiscard]] std::string simPcmName(const SimConfig& config);
    /** ALSA name PluginHost should open (device 1). */
    [[nodiscard]] std::string appPcmName(const SimConfig& config);

    /** Parses `index=kind[:args]` (1-based index), e.g. `2=file:/tmp/kick.wav:-6`. */
    [[nodiscard]] std::optional<SourceSpec> parseSourceSpec(const std::string& text, std::string& error);

    struct ParseResult
    {
        std::optional<SimConfig> config;  // empty when we should exit
        std::string message;              // usage text or error
        int exitCode = 0;
    };

    [[nodiscard]] ParseResult parseArgs(int argc, const char* const* argv);
}
