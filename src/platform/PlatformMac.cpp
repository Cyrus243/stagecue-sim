#include "Platform.h"

#include "CoreAudioDevices.h"

#include <mach/mach.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace sim::platform
{
    namespace
    {
        using coreaudio::Scope;

        template <typename T>
        std::vector<T> arrayProperty(AudioObjectID object, AudioObjectPropertySelector selector,
                                     AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
        {
            const AudioObjectPropertyAddress addr { selector, scope, kAudioObjectPropertyElementMain };
            UInt32 size = 0;
            if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &size) != noErr || size == 0)
                return {};
            std::vector<T> values(size / sizeof(T));
            if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, values.data()) != noErr)
                return {};
            values.resize(size / sizeof(T));
            return values;
        }

        template <typename T>
        T scalarProperty(AudioObjectID object, AudioObjectPropertySelector selector, T fallback)
        {
            const AudioObjectPropertyAddress addr { selector, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain };
            T value = fallback;
            UInt32 size = sizeof(value);
            return AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) == noErr ? value : fallback;
        }

        std::string prepareDevice(const std::string& name, Scope scope, int channels, unsigned rate)
        {
            const auto device = coreaudio::findDevice(name, scope);
            if (!device)
                return "no CoreAudio device named \"" + name + "\" (available: " + coreaudio::describeDevices(scope) + ")";

            if (const int available = coreaudio::channelCount(*device, scope); available < channels)
                return name + " has " + std::to_string(available) + " channels, need " + std::to_string(channels);

            if (const auto actual = coreaudio::setNominalSampleRate(*device, rate); std::abs(actual - rate) >= 0.5)
                return name + " runs at " + std::to_string(static_cast<int>(actual)) + " Hz and cannot switch to "
                       + std::to_string(rate) + " Hz";
            return {};
        }

        /** Finds whether another process (PluginHost) runs input and/or output on the
            device it is meant to open (macOS 14+). Device IDs are looked up by name once
            and kept while they stay valid, so a poll costs one process-list read plus a
            few property reads. */
        class HalLinkProbe final : public LinkProbe
        {
        public:
            explicit HalLinkProbe(const SimConfig& config)
                : simInputsName(config.inputsDevice), simOutputsName(config.outputsDevice),
                  hostName(config.hostDevice)
            {
            }

            LoopbackStatus query() override
            {
                refresh(simInputs, simInputsName, Scope::Output);
                refresh(simOutputs, simOutputsName, Scope::Input);
                refresh(host, hostName, Scope::Input);

                LoopbackStatus result;
                result.cardPresent = simInputs && simOutputs && host;
                if (!result.cardPresent)
                    return result;

                std::vector<AudioObjectID> capturing, playing;
                if (__builtin_available(macOS 14.0, *))
                {
                    const auto self = static_cast<pid_t>(getpid());
                    for (const auto process : arrayProperty<AudioObjectID>(kAudioObjectSystemObject,
                                                                           kAudioHardwarePropertyProcessObjectList))
                    {
                        const bool runsInput = scalarProperty<UInt32>(process, kAudioProcessPropertyIsRunningInput, 0) != 0;
                        const bool runsOutput = scalarProperty<UInt32>(process, kAudioProcessPropertyIsRunningOutput, 0) != 0;
                        if ((!runsInput && !runsOutput) || scalarProperty<pid_t>(process, kAudioProcessPropertyPID, self) == self)
                            continue;

                        if (runsInput)
                            for (const auto device : arrayProperty<AudioObjectID>(process, kAudioProcessPropertyDevices,
                                                                                  kAudioObjectPropertyScopeInput))
                                capturing.push_back(device);
                        if (runsOutput)
                            for (const auto device : arrayProperty<AudioObjectID>(process, kAudioProcessPropertyDevices,
                                                                                  kAudioObjectPropertyScopeOutput))
                                playing.push_back(device);
                    }
                }

                result.appCapture = substream(*host, capturing);
                result.appPlayback = substream(*host, playing);
                return result;
            }

        private:
            static void refresh(std::optional<AudioObjectID>& cached, const std::string& name, Scope scope)
            {
                if (cached && coreaudio::deviceName(*cached) == name)
                    return;
                cached = coreaudio::findDevice(name, scope);
            }

            static SubstreamStatus substream(AudioObjectID device, const std::vector<AudioObjectID>& users)
            {
                SubstreamStatus status;
                status.open = std::find(users.begin(), users.end(), device) != users.end();
                if (status.open)
                {
                    status.state = "RUNNING";
                    status.rate = static_cast<unsigned>(coreaudio::nominalSampleRate(device));
                }
                return status;
            }

            std::string simInputsName;
            std::string simOutputsName;
            std::string hostName;
            std::optional<AudioObjectID> simInputs;
            std::optional<AudioObjectID> simOutputs;
            std::optional<AudioObjectID> host;
        };
    }

    void init()
    {
        // Without this the HAL queues notifications (device alive, rate changes...) for
        // the main run loop, which a terminal app never runs.
        const AudioObjectPropertyAddress addr { kAudioHardwarePropertyRunLoop, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
        CFRunLoopRef runLoop = nullptr;
        AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, sizeof(runLoop), &runLoop);
    }

    std::string prepareLoopback(const SimConfig& config)
    {
        if (auto error = prepareDevice(config.inputsDevice, Scope::Output, config.numInputs, config.sampleRate);
            !error.empty())
            return error;
        if (auto error = prepareDevice(config.outputsDevice, Scope::Input, config.numOutputs, config.sampleRate);
            !error.empty())
            return error;
        // PluginHost's end: has to exist and run at the same rate (the patched driver
        // shares one rate between its two devices, so this is normally already true).
        if (auto error = prepareDevice(config.hostDevice, Scope::Input, config.numInputs, config.sampleRate);
            !error.empty())
            return error;
        return prepareDevice(config.hostDevice, Scope::Output, config.numOutputs, config.sampleRate);
    }

    std::unique_ptr<LinkProbe> makeLinkProbe(const SimConfig& config) { return std::make_unique<HalLinkProbe>(config); }

    std::uint64_t residentMemoryBytes()
    {
        mach_task_basic_info info {};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return 0;
        return info.resident_size;
    }
}
