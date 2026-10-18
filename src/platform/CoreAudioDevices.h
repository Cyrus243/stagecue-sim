#pragma once

#include <CoreAudio/CoreAudio.h>

#include <optional>
#include <string>
#include <vector>

namespace sim::coreaudio
{
    enum class Scope { Input, Output };

    [[nodiscard]] std::vector<AudioObjectID> allDevices();
    [[nodiscard]] std::string deviceName(AudioObjectID device);

    /** Finds a device by its exact name; "default" is the system default device for `scope`. */
    [[nodiscard]] std::optional<AudioObjectID> findDevice(const std::string& name, Scope scope);

    /** Number of channels the device has in `scope` (0 when it has none). */
    [[nodiscard]] int channelCount(AudioObjectID device, Scope scope);

    [[nodiscard]] double nominalSampleRate(AudioObjectID device);

    /** Asks the device to run at `rate` and waits (up to 1 s) for the change to land.
        Returns the rate the device ended up at. */
    double setNominalSampleRate(AudioObjectID device, double rate);

    /** Comma-separated device names, for error messages. */
    [[nodiscard]] std::string describeDevices(Scope scope);
}
