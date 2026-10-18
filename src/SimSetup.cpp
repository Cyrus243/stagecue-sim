#include "SimSetup.h"
#include "Sources.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace sim
{
    namespace
    {
        bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
        char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

        /** -1, 0 or 1, like strcmp. */
        int compareNatural(const std::string& a, const std::string& b)
        {
            std::size_t i = 0, j = 0;
            while (i < a.size() && j < b.size())
            {
                if (isDigit(a[i]) && isDigit(b[j]))
                {
                    auto runEnd = [](const std::string& s, std::size_t from) {
                        while (from < s.size() && isDigit(s[from]))
                            ++from;
                        return from;
                    };
                    const auto endA = runEnd(a, i), endB = runEnd(b, j);
                    auto firstSignificant = [](const std::string& s, std::size_t from, std::size_t end) {
                        while (from + 1 < end && s[from] == '0')
                            ++from;
                        return from;
                    };
                    const auto startA = firstSignificant(a, i, endA), startB = firstSignificant(b, j, endB);
                    const auto lengthA = endA - startA, lengthB = endB - startB;
                    if (lengthA != lengthB)
                        return lengthA < lengthB ? -1 : 1;
                    if (const int c = a.compare(startA, lengthA, b, startB, lengthB); c != 0)
                        return c < 0 ? -1 : 1;
                    if (endA - i != endB - j)   // same value: fewer leading zeros first
                        return endA - i < endB - j ? -1 : 1;
                    i = endA;
                    j = endB;
                    continue;
                }
                if (lower(a[i]) != lower(b[j]))
                    return lower(a[i]) < lower(b[j]) ? -1 : 1;
                ++i;
                ++j;
            }
            if (i < a.size() || j < b.size())
                return i < a.size() ? 1 : -1;
            return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);   // total order: case breaks ties
        }

        bool isAudioFile(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), lower);
            return extension == ".wav" || extension == ".flac" || extension == ".mp3";
        }

        constexpr const char* kSetupExtension = ".setup";
        constexpr std::size_t kMaxSetupName = 64;

        std::string trim(const std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }
    }

    bool naturalLess(const std::string& a, const std::string& b) { return compareNatural(a, b) < 0; }

    std::vector<std::filesystem::path> listAudioFiles(const std::string& folder, std::string& error)
    {
        const auto dir = resolvePath(folder);
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
        {
            error = "no such folder: " + dir.string();
            return {};
        }

        std::vector<std::filesystem::path> files;
        std::filesystem::directory_iterator it(dir, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            const auto& path = it->path();
            const auto name = path.filename().string();
            // Hidden files include the macOS "._name" resource forks next to every stem on a FAT drive.
            std::error_code entryError;   // a broken link skips that entry, not the whole folder
            if (name.empty() || name.front() == '.' || !isAudioFile(path) || !it->is_regular_file(entryError))
                continue;
            files.push_back(path);
        }
        if (ec)
        {
            error = dir.string() + ": " + ec.message();
            return {};
        }

        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return naturalLess(a.filename().string(), b.filename().string());
        });
        return files;
    }

    std::string formatSetup(const InputSetup& setup)
    {
        auto sources = setup.sources;
        std::stable_sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.input < b.input; });

        std::string text = "# StageCue soundcard sim setup: one input per line (N=silence | N=sine:HZ:DB | N=file:PATH:DB)\n";
        for (const auto& spec : sources)
            text += formatSourceSpec(spec) + "\n";
        return text;
    }

    std::optional<InputSetup> parseSetup(const std::string& text, std::string& error)
    {
        InputSetup setup;
        std::set<int> seen;
        std::istringstream stream(text);
        std::string line;
        for (int number = 1; std::getline(stream, line); ++number)
        {
            line = trim(line);
            if (line.empty() || line.front() == '#')
                continue;

            std::string specError;
            auto spec = parseSourceSpec(line, specError);
            if (!spec)
            {
                error = "line " + std::to_string(number) + ": " + specError;
                return std::nullopt;
            }
            if (!seen.insert(spec->input).second)
            {
                error = "line " + std::to_string(number) + ": input " + std::to_string(spec->input + 1) + " appears twice";
                return std::nullopt;
            }
            setup.sources.push_back(std::move(*spec));
        }
        return setup;
    }

    bool isValidSetupName(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxSetupName || name.front() == ' ' || name.back() == ' ')
            return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_';
        });
    }

    std::filesystem::path setupsDirectory()
    {
        std::filesystem::path base;
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            base = xdg;
        else if (const char* home = std::getenv("HOME"))
            base = std::filesystem::path(home) / ".config";
        return base / "stagecue-sim" / "setups";
    }

    std::vector<std::string> listSetups(const std::filesystem::path& dir)
    {
        std::vector<std::string> names;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            const auto& path = it->path();
            std::error_code entryError;
            if (path.extension() == kSetupExtension && it->is_regular_file(entryError)
                && isValidSetupName(path.stem().string()))
                names.push_back(path.stem().string());
        }
        std::sort(names.begin(), names.end(), naturalLess);
        return names;
    }

    bool saveSetup(const std::filesystem::path& dir, const std::string& name, const InputSetup& setup, std::string& error)
    {
        if (!isValidSetupName(name))
        {
            error = "invalid setup name '" + name + "' (letters, digits, space, - and _)";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            error = dir.string() + ": " + ec.message();
            return false;
        }

        // Write next to the target and rename over it, so a crash never leaves half a setup.
        const auto target = dir / (name + kSetupExtension);
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << formatSetup(setup);
            if (!out.flush())
            {
                error = "could not write " + temporary.string();
                return false;
            }
        }
        std::filesystem::rename(temporary, target, ec);
        if (ec)
        {
            error = target.string() + ": " + ec.message();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        return true;
    }

    std::optional<InputSetup> loadSetup(const std::filesystem::path& dir, const std::string& name, std::string& error)
    {
        const auto path = dir / (name + kSetupExtension);
        std::ifstream in(path, std::ios::binary);
        if (!isValidSetupName(name) || !in)
        {
            error = "no setup named '" + name + "' in " + dir.string();
            return std::nullopt;
        }

        std::ostringstream text;
        text << in.rdbuf();
        std::string parseError;
        auto setup = parseSetup(text.str(), parseError);
        if (!setup)
            error = path.filename().string() + " " + parseError;
        return setup;
    }
}
