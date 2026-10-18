#pragma once

#include "LoopbackStatus.h"
#include "SimConfig.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sim::platform
{
    /** Process-wide setup, called once before any audio API is touched.
        macOS: lets the CoreAudio HAL deliver its notifications on its own thread
        (a CLI never runs the main CFRunLoop they would otherwise wait on).
        Linux: keeps alsa-lib's stderr diagnostics out of the terminal UI. */
    void init();

    /** Checks that the loopback exists and puts it in the state the simulator needs
        before the devices are opened. macOS: channel count and the devices' nominal
        sample rate. Linux: the snd-aloop card is loaded.
        Returns an empty string on success, otherwise a human-readable error. */
    [[nodiscard]] std::string prepareLoopback(const SimConfig& config);

    /** Reports what PluginHost does with its end of the loopback. Keeps whatever it
        can between calls (e.g. CoreAudio device IDs), so it is not thread-safe:
        one instance per polling thread. */
    class LinkProbe
    {
    public:
        virtual ~LinkProbe() = default;
        [[nodiscard]] virtual LoopbackStatus query() = 0;
    };

    [[nodiscard]] std::unique_ptr<LinkProbe> makeLinkProbe(const SimConfig& config);

    /** Resident memory of this process in bytes (0 when unknown). */
    [[nodiscard]] std::uint64_t residentMemoryBytes();
}
