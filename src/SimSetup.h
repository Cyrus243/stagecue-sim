#pragma once

#include "SimConfig.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sim
{
    /** Case-insensitive order that compares digit runs by value, so "2 Snare" sorts before "10 Tom". */
    [[nodiscard]] bool naturalLess(const std::string& a, const std::string& b);

    /** The WAV/FLAC/MP3 files directly in `folder` (hidden files skipped), as absolute paths in
        natural name order. A missing or unreadable folder returns nothing and sets `error`. */
    [[nodiscard]] std::vector<std::filesystem::path> listAudioFiles(const std::string& folder, std::string& error);

    /** What every input plays, saved under a name. On disk: one `--source` spec per line. */
    struct InputSetup
    {
        std::vector<SourceSpec> sources;
    };

    [[nodiscard]] std::string formatSetup(const InputSetup& setup);
    /** Skips blank and `#` lines; errors name the line ("line 3: ..."), including a repeated input. */
    [[nodiscard]] std::optional<InputSetup> parseSetup(const std::string& text, std::string& error);

    /** 1-64 characters of letters, digits, space, '-' and '_', not starting or ending with a space. */
    [[nodiscard]] bool isValidSetupName(const std::string& name);

    /** $XDG_CONFIG_HOME/stagecue-sim/setups, or ~/.config/stagecue-sim/setups. */
    [[nodiscard]] std::filesystem::path setupsDirectory();

    /** Saved setup names in natural order; nothing if the folder doesn't exist yet. */
    [[nodiscard]] std::vector<std::string> listSetups(const std::filesystem::path& dir);
    /** Writes `<dir>/<name>.setup` (replacing it), creating `dir` if needed. */
    [[nodiscard]] bool saveSetup(const std::filesystem::path& dir, const std::string& name,
                                 const InputSetup& setup, std::string& error);
    [[nodiscard]] std::optional<InputSetup> loadSetup(const std::filesystem::path& dir, const std::string& name,
                                                      std::string& error);
}
