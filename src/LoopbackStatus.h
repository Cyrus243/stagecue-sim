#pragma once

#include <string>

namespace sim
{
    /** State of one ALSA substream as reported by /proc/asound. */
    struct SubstreamStatus
    {
        bool open = false;       // false when "closed" or missing
        std::string state;       // e.g. RUNNING, PREPARED, SETUP, XRUN
        int channels = 0;        // from hw_params, 0 when unknown
        unsigned rate = 0;
        std::string format;
    };

    /** What PluginHost is doing with its end (device 1) of the loopback cable. */
    struct LoopbackStatus
    {
        bool cardPresent = false;
        SubstreamStatus appCapture;   // PluginHost reading the sim's inputs
        SubstreamStatus appPlayback;  // PluginHost writing the sim's outputs
    };

    [[nodiscard]] SubstreamStatus parseSubstreamStatus(const std::string& statusText,
                                                       const std::string& hwParamsText);

    /** Reads /proc/asound/<cardId>/pcm1{c,p}/sub0/{status,hw_params}. */
    [[nodiscard]] LoopbackStatus queryLoopbackStatus(const std::string& cardId,
                                                     const std::string& procRoot = "/proc/asound");
}
