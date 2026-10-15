#include "CoreAudioDevices.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace sim::coreaudio
{
    namespace
    {
        AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
        {
            return { selector, scope, kAudioObjectPropertyElementMain };
        }

        AudioObjectPropertyScope toScope(Scope scope)
        {
            return scope == Scope::Input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
        }
    }

    std::vector<AudioObjectID> allDevices()
    {
        const auto addr = address(kAudioHardwarePropertyDevices);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
            return {};

        std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data()) != noErr)
            return {};
        devices.resize(size / sizeof(AudioObjectID));
        return devices;
    }

    std::string deviceName(AudioObjectID device)
    {
        const auto addr = address(kAudioObjectPropertyName);
        CFStringRef name = nullptr;
        UInt32 size = sizeof(name);
        if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &name) != noErr || name == nullptr)
            return {};

        std::string result;
        const auto length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name), kCFStringEncodingUTF8) + 1;
        result.resize(static_cast<std::size_t>(length));
        if (CFStringGetCString(name, result.data(), length, kCFStringEncodingUTF8))
            result.resize(std::char_traits<char>::length(result.c_str()));
        else
            result.clear();
        CFRelease(name);
        return result;
    }

    std::optional<AudioObjectID> findDevice(const std::string& name, Scope scope)
    {
        if (name == "default")
        {
            const auto addr = address(scope == Scope::Input ? kAudioHardwarePropertyDefaultInputDevice
                                                            : kAudioHardwarePropertyDefaultOutputDevice);
            AudioObjectID device = kAudioObjectUnknown;
            UInt32 size = sizeof(device);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device) != noErr
                || device == kAudioObjectUnknown)
                return std::nullopt;
            return device;
        }

        for (const auto device : allDevices())
            if (deviceName(device) == name)
                return device;
        return std::nullopt;
    }

    int channelCount(AudioObjectID device, Scope scope)
    {
        const auto addr = address(kAudioDevicePropertyStreamConfiguration, toScope(scope));
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0)
            return 0;

        auto* list = static_cast<AudioBufferList*>(std::malloc(size));
        int channels = 0;
        if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) == noErr)
            for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
                channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
        std::free(list);
        return channels;
    }

    double nominalSampleRate(AudioObjectID device)
    {
        const auto addr = address(kAudioDevicePropertyNominalSampleRate);
        Float64 rate = 0.0;
        UInt32 size = sizeof(rate);
        return AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &rate) == noErr ? rate : 0.0;
    }

    double setNominalSampleRate(AudioObjectID device, double rate)
    {
        if (std::abs(nominalSampleRate(device) - rate) < 0.5)
            return rate;

        const auto addr = address(kAudioDevicePropertyNominalSampleRate);
        const Float64 wanted = rate;
        if (AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(wanted), &wanted) != noErr)
            return nominalSampleRate(device);

        // The HAL applies the change asynchronously.
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + seconds(1);
        while (std::abs(nominalSampleRate(device) - rate) >= 0.5 && steady_clock::now() < deadline)
            std::this_thread::sleep_for(milliseconds(10));
        return nominalSampleRate(device);
    }

    std::string describeDevices(Scope scope)
    {
        std::string names;
        for (const auto device : allDevices())
        {
            if (channelCount(device, scope) == 0)
                continue;
            names += (names.empty() ? "" : ", ") + deviceName(device);
        }
        return names.empty() ? "(none)" : names;
    }
}
