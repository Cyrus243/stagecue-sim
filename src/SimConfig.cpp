#include "SimConfig.h"

#include <charconv>
#include <sstream>

namespace sim
{
    namespace
    {
        constexpr const char* kUsage =
            "StageCueSoundcardSim - ALSA loopback soundcard simulator for StageCue\n"
            "\n"
            "Usage: StageCueSoundcardSim [options]\n"
            "  --inputs N          simulated inputs (sim -> PluginHost), 1..32   [8]\n"
            "  --outputs N         simulated outputs (PluginHost -> sim), 1..32  [8]\n"
            "  --rate HZ           sample rate                                   [48000]\n"
            "  --period FRAMES     period size; PluginHost's buffer size follows [512]\n"
            "  --card ID           snd-aloop card id                             [StageCueSim]\n"
            "  --monitor PCM       ALSA device for the main out                  [default]\n"
            "  --source SPEC       preset an input source (repeatable):\n"
            "                        N=silence\n"
            "                        N=sine[:FREQ_HZ[:LEVEL_DB]]\n"
            "                        N=file:PATH[:LEVEL_DB]\n"
            "  --play              start the song transport playing (files in sync)\n"
            "  -h, --help          show this help\n"
            "\n"
            "Load the loopback card first: ./setup-loopback.sh\n";

        template <typename T>
        bool parseNumber(const std::string& text, T& out)
        {
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(first, last, out);
            return ec == std::errc{} && ptr == last;
        }

        std::vector<std::string> split(const std::string& text, char sep)
        {
            std::vector<std::string> parts;
            std::string part;
            std::istringstream stream(text);
            while (std::getline(stream, part, sep))
                parts.push_back(part);
            return parts;
        }
    }

    std::string simPcmName(const SimConfig& config) { return "hw:CARD=" + config.cardId + ",DEV=0"; }
    std::string appPcmName(const SimConfig& config) { return "hw:CARD=" + config.cardId + ",DEV=1"; }

    std::optional<SourceSpec> parseSourceSpec(const std::string& text, std::string& error)
    {
        const auto eq = text.find('=');
        if (eq == std::string::npos)
        {
            error = "expected N=kind[:args], got '" + text + "'";
            return std::nullopt;
        }

        SourceSpec spec;
        int oneBased = 0;
        if (!parseNumber(text.substr(0, eq), oneBased) || oneBased < 1 || oneBased > kMaxChannels)
        {
            error = "invalid input index in '" + text + "'";
            return std::nullopt;
        }
        spec.input = oneBased - 1;

        const auto rest = text.substr(eq + 1);
        const auto colon = rest.find(':');
        const auto kind = rest.substr(0, colon);
        const auto args = colon == std::string::npos ? std::string{} : rest.substr(colon + 1);

        if (kind == "silence")
        {
            spec.kind = SourceSpec::Kind::Silence;
            return spec;
        }

        if (kind == "sine")
        {
            spec.kind = SourceSpec::Kind::Sine;
            const auto parts = args.empty() ? std::vector<std::string>{} : split(args, ':');
            if (parts.size() > 2
                || (parts.size() > 0 && !parseNumber(parts[0], spec.frequencyHz))
                || (parts.size() > 1 && !parseNumber(parts[1], spec.levelDb)))
            {
                error = "expected N=sine[:FREQ_HZ[:LEVEL_DB]], got '" + text + "'";
                return std::nullopt;
            }
            return spec;
        }

        if (kind == "file")
        {
            spec.kind = SourceSpec::Kind::File;
            // The level is optional and the path may itself contain ':', so only
            // treat the last segment as a level when it parses as a number.
            spec.filePath = args;
            if (const auto last = args.rfind(':'); last != std::string::npos)
            {
                float level = 0.0f;
                if (parseNumber(args.substr(last + 1), level))
                {
                    spec.levelDb = level;
                    spec.filePath = args.substr(0, last);
                }
            }
            if (spec.filePath.empty())
            {
                error = "expected N=file:PATH[:LEVEL_DB], got '" + text + "'";
                return std::nullopt;
            }
            return spec;
        }

        error = "unknown source kind '" + kind + "' (silence|sine|file)";
        return std::nullopt;
    }

    ParseResult parseArgs(int argc, const char* const* argv)
    {
        SimConfig config;

        auto fail = [](const std::string& message) {
            return ParseResult{ std::nullopt, message + "\n\n" + kUsage, 2 };
        };

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];

            if (arg == "-h" || arg == "--help")
                return { std::nullopt, kUsage, 0 };

            if (arg == "--play")
            {
                config.startPlaying = true;
                continue;
            }

            if (i + 1 >= argc)
                return fail("missing value for " + arg);

            const std::string value = argv[++i];

            if (arg == "--inputs" || arg == "--outputs")
            {
                int n = 0;
                if (!parseNumber(value, n) || n < 1 || n > kMaxChannels)
                    return fail(arg + " must be between 1 and " + std::to_string(kMaxChannels));
                (arg == "--inputs" ? config.numInputs : config.numOutputs) = n;
            }
            else if (arg == "--rate")
            {
                if (!parseNumber(value, config.sampleRate) || config.sampleRate < 8000)
                    return fail("invalid --rate " + value);
            }
            else if (arg == "--period")
            {
                if (!parseNumber(value, config.periodFrames) || config.periodFrames < 16)
                    return fail("invalid --period " + value);
            }
            else if (arg == "--card")
            {
                config.cardId = value;
            }
            else if (arg == "--monitor")
            {
                config.monitorDevice = value;
            }
            else if (arg == "--source")
            {
                std::string error;
                auto spec = parseSourceSpec(value, error);
                if (!spec)
                    return fail(error);
                config.sources.push_back(*spec);
            }
            else
            {
                return fail("unknown option " + arg);
            }
        }

        for (const auto& spec : config.sources)
            if (spec.input >= config.numInputs)
                return fail("--source targets input " + std::to_string(spec.input + 1)
                            + " but only " + std::to_string(config.numInputs) + " inputs exist");

        return { config, {}, 0 };
    }
}
