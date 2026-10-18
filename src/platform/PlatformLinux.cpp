#include "Platform.h"

#include <alsa/asoundlib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace sim::platform
{
    namespace
    {
        void ignoreAlsaError(const char*, int, const char*, int, const char*, ...) {}

        class ProcLinkProbe final : public LinkProbe
        {
        public:
            explicit ProcLinkProbe(std::string card) : cardId(std::move(card)) {}
            LoopbackStatus query() override { return queryLoopbackStatus(cardId); }

        private:
            std::string cardId;
        };
    }

    void init() { snd_lib_error_set_handler(ignoreAlsaError); }

    std::string prepareLoopback(const SimConfig& config)
    {
        std::error_code ec;
        if (!std::filesystem::exists("/proc/asound/" + config.cardId, ec))
            return "ALSA card " + config.cardId + " is not loaded";
        return {};
    }

    std::unique_ptr<LinkProbe> makeLinkProbe(const SimConfig& config)
    {
        return std::make_unique<ProcLinkProbe>(config.cardId);
    }

    std::uint64_t residentMemoryBytes()
    {
        std::ifstream statm("/proc/self/statm");
        std::uint64_t size = 0, resident = 0;
        if (!(statm >> size >> resident))
            return 0;
        return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
}
