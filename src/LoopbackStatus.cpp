#include "LoopbackStatus.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace sim
{
    namespace
    {
        std::string readFile(const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
                return {};
            std::ostringstream contents;
            contents << stream.rdbuf();
            return contents.str();
        }

        /** Returns the value after "key:" on the first matching line, trimmed. */
        std::string valueFor(const std::string& text, const std::string& key)
        {
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                if (line.rfind(key + ":", 0) != 0)
                    continue;
                auto value = line.substr(key.size() + 1);
                const auto first = value.find_first_not_of(" \t");
                return first == std::string::npos ? std::string{} : value.substr(first);
            }
            return {};
        }

        template <typename T>
        T leadingNumber(const std::string& text)
        {
            T value{};
            std::istringstream(text) >> value;
            return value;
        }
    }

    SubstreamStatus parseSubstreamStatus(const std::string& statusText, const std::string& hwParamsText)
    {
        SubstreamStatus status;
        if (statusText.empty() || statusText.rfind("closed", 0) == 0)
            return status;

        status.open = true;
        status.state = valueFor(statusText, "state");

        if (!hwParamsText.empty() && hwParamsText.rfind("closed", 0) != 0)
        {
            status.channels = leadingNumber<int>(valueFor(hwParamsText, "channels"));
            status.rate = leadingNumber<unsigned>(valueFor(hwParamsText, "rate"));
            status.format = valueFor(hwParamsText, "format");
        }
        return status;
    }

    LoopbackStatus queryLoopbackStatus(const std::string& cardId, const std::string& procRoot)
    {
        LoopbackStatus result;
        const std::filesystem::path card = std::filesystem::path(procRoot) / cardId;

        std::error_code ec;
        result.cardPresent = std::filesystem::exists(card, ec);
        if (!result.cardPresent)
            return result;

        auto query = [&card](const char* substream) {
            const auto dir = card / substream / "sub0";
            return parseSubstreamStatus(readFile(dir / "status"), readFile(dir / "hw_params"));
        };

        result.appCapture = query("pcm1c");
        result.appPlayback = query("pcm1p");
        return result;
    }
}
