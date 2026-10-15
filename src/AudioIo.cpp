#include "AudioIo.h"

#include "MiniaudioConfig.h"
#include "Mixer.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace sim
{
    namespace
    {
        // Callbacks are processed in chunks of at most this many frames, so the scratch
        // buffers are sized once whatever block size the backend hands us.
        constexpr std::uint32_t kChunkFrames = 1024;
        // Monitor ring: room for this many periods; the reader drops anything past half
        // of it, so drift between the loopback and speaker clocks can't build up latency.
        constexpr std::uint32_t kMonitorRingPeriods = 8;

        std::string describe(ma_result result) { return ma_result_description(result); }

        float clampSample(float value) noexcept { return std::clamp(value, -1.0f, 1.0f); }
    }

    struct AudioIo::Context
    {
        ma_context context {};
        ma_pcm_rb monitorRing {};
        bool ringReady = false;
    };

    struct AudioIo::Device
    {
        ma_device device {};
        ma_format format = ma_format_f32;
        int channels = 0;
    };

    // ------------------------------------------------------------------ trampolines

    namespace
    {
        AudioIo& owner(ma_device* device) { return *static_cast<AudioIo*>(device->pUserData); }

        void inputsTrampoline(ma_device* device, void* output, const void*, ma_uint32 numFrames)
        {
            owner(device).inputsCallback(output, numFrames);
        }

        void outputsTrampoline(ma_device* device, void*, const void* input, ma_uint32 numFrames)
        {
            owner(device).outputsCallback(input, numFrames);
        }

        void monitorTrampoline(ma_device* device, void* output, const void*, ma_uint32 numFrames)
        {
            owner(device).monitorCallback(static_cast<float*>(output), numFrames);
        }

        void notificationTrampoline(const ma_device_notification* notification)
        {
            if (notification->type == ma_device_notification_type_stopped)
                owner(notification->pDevice).deviceStopped(notification->pDevice);
        }

        /** miniaudio device id for a loopback/monitor endpoint. */
        std::optional<ma_device_id> findDeviceId(ma_context& context, const std::string& name, bool playback,
                                                 std::string& error)
        {
            ma_device_id id {};
#if defined(__APPLE__)
            ma_device_info* playbackInfos = nullptr;
            ma_device_info* captureInfos = nullptr;
            ma_uint32 playbackCount = 0, captureCount = 0;
            if (const auto result = ma_context_get_devices(&context, &playbackInfos, &playbackCount,
                                                           &captureInfos, &captureCount);
                result != MA_SUCCESS)
            {
                error = "cannot list audio devices: " + describe(result);
                return std::nullopt;
            }

            const auto* infos = playback ? playbackInfos : captureInfos;
            const auto count = playback ? playbackCount : captureCount;
            std::string available;
            for (ma_uint32 i = 0; i < count; ++i)
            {
                if (name == infos[i].name)
                    return infos[i].id;
                available += (available.empty() ? "" : ", ") + std::string(infos[i].name);
            }
            error = "no " + std::string(playback ? "output" : "input") + " device named \"" + name
                    + "\" (available: " + (available.empty() ? "none" : available) + ")";
            return std::nullopt;
#else
            (void) context;
            (void) playback;
            (void) error;
            std::strncpy(id.alsa, name.c_str(), sizeof(id.alsa) - 1);
            return id;
#endif
        }
    }

    // ---------------------------------------------------------------- CallbackClock

    void AudioIo::CallbackClock::tick(std::uint32_t numFrames, unsigned rate) noexcept
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (count.load(std::memory_order_relaxed) > 0 && now - last > allowed)
            late.fetch_add(1, std::memory_order_relaxed);

        // Next callback is due one block from now; allow twice that plus scheduling slack.
        const auto block = duration<double>(static_cast<double>(numFrames) / rate);
        allowed = duration_cast<steady_clock::duration>(2 * block) + milliseconds(10);
        last = now;
        count.fetch_add(1, std::memory_order_relaxed);
    }

    void AudioIo::CallbackClock::reset() noexcept
    {
        count = 0;
        late = 0;
    }

    // ----------------------------------------------------------------- lifecycle

    AudioIo::AudioIo() : context(std::make_unique<Context>())
    {
        // One context for the process: creating it enumerates the backend, which is slow.
        if (ma_context_init(nullptr, 0, nullptr, &context->context) != MA_SUCCESS)
            context.reset();
    }

    AudioIo::~AudioIo()
    {
        close();
        if (context)
            ma_context_uninit(&context->context);
    }

    std::string AudioIo::open(const SimConfig& config, Mixer& mixerToUse)
    {
        close();
        if (!context)
            return "cannot initialise the audio backend";

        mixer = &mixerToUse;
        numInputs = config.numInputs;
        numOutputs = config.numOutputs;
        sampleRate = config.sampleRate;

        inputsPlanar.assign(std::size_t { kChunkFrames } * static_cast<std::size_t>(numInputs), 0.0f);
        inputsInterleaved.assign(inputsPlanar.size(), 0.0f);
        outputsPlanar.assign(std::size_t { kChunkFrames } * static_cast<std::size_t>(numOutputs), 0.0f);
        outputsInterleaved.assign(outputsPlanar.size(), 0.0f);
        stereo.assign(std::size_t { kChunkFrames } * 2, 0.0f);

        closing = false;
        loopbackLost = false;
        monitorLost = false;
        monitorPrimed = false;
        monitorDropouts = 0;
        inputsClock.reset();
        outputsClock.reset();
        monitorClock.reset();

        if (auto error = openLoopback(config); !error.empty())
        {
            close();
            return error;
        }
        openMonitor(config);

        // Start everything only once all devices exist, monitor first so it is ready for data.
        if (monitorOpened && ma_device_start(&monitor->device) != MA_SUCCESS)
        {
            monitorError = "cannot start " + config.monitorDevice;
            ma_device_uninit(&monitor->device);
            monitor.reset();
            monitorOpened = false;
        }
        for (auto* device : { inputs.get(), outputs.get() })
        {
            if (const auto result = ma_device_start(&device->device); result != MA_SUCCESS)
            {
                close();
                return "cannot start the loopback: " + describe(result);
            }
        }

        opened = true;
        return {};
    }

    void AudioIo::close() noexcept
    {
        closing = true;
        // ma_device_uninit stops the device and waits for its callback to return.
        for (auto* device : { &inputs, &outputs, &monitor })
        {
            if (*device)
                ma_device_uninit(&(*device)->device);
            device->reset();
        }
        if (context && context->ringReady)
        {
            ma_pcm_rb_uninit(&context->monitorRing);
            context->ringReady = false;
        }
        opened = false;
        monitorOpened = false;
    }

    std::string AudioIo::openLoopback(const SimConfig& config)
    {
        if (auto error = platform::prepareLoopback(config); !error.empty())
            return error + "\nIs the loopback set up (./setup-loopback.sh)?";

#if defined(__APPLE__)
        const std::string inputsName = config.inputsDevice;
        const std::string outputsName = config.outputsDevice;
        // CoreAudio is float-native; anything else would only add a conversion.
        const ma_format formats[] = { ma_format_f32 };
        const auto shareMode = ma_share_mode_shared;   // CoreAudio has no exclusive mode
#else
        const std::string inputsName = simPcmName(config);
        const std::string outputsName = inputsName;
        // snd-aloop: the side that opens first fixes the format, so offer what PluginHost
        // can match, best first (S32_LE is what JUCE picks on its own).
        const ma_format formats[] = { ma_format_s32, ma_format_f32, ma_format_s16 };
        const auto shareMode = ma_share_mode_exclusive;   // the hw device itself, never dmix/dsnoop
#endif

        auto openOne = [&](std::unique_ptr<Device>& slot, bool playback, const std::string& name,
                           int channels) -> std::string {
            std::string error;
            const auto id = findDeviceId(context->context, name, playback, error);
            if (!id)
                return error;

            ma_result firstError = MA_SUCCESS;
            for (const auto format : formats)
            {
                auto device = std::make_unique<Device>();
                auto cfg = ma_device_config_init(playback ? ma_device_type_playback : ma_device_type_capture);
                cfg.sampleRate = config.sampleRate;
                cfg.periodSizeInFrames = config.periodFrames;
                cfg.periods = config.periods;
                cfg.noPreSilencedOutputBuffer = MA_TRUE;   // inputsCallback writes every sample
                cfg.noClip = MA_TRUE;                      // ... already clamped
                cfg.pUserData = this;
                cfg.notificationCallback = notificationTrampoline;
                // The loopback must run exactly as configured: no plugin conversions.
                cfg.alsa.noMMap = MA_TRUE;
                cfg.alsa.noAutoFormat = MA_TRUE;
                cfg.alsa.noAutoChannels = MA_TRUE;
                cfg.alsa.noAutoResample = MA_TRUE;
                if (playback)
                {
                    cfg.dataCallback = inputsTrampoline;
                    cfg.playback.pDeviceID = &*id;
                    cfg.playback.format = format;
                    cfg.playback.channels = static_cast<ma_uint32>(channels);
                    cfg.playback.shareMode = shareMode;
                    cfg.playback.channelMixMode = ma_channel_mix_mode_simple;   // first N device channels
                }
                else
                {
                    cfg.dataCallback = outputsTrampoline;
                    cfg.capture.pDeviceID = &*id;
                    cfg.capture.format = format;
                    cfg.capture.channels = static_cast<ma_uint32>(channels);
                    cfg.capture.shareMode = shareMode;
                    cfg.capture.channelMixMode = ma_channel_mix_mode_simple;
                }

                const auto result = ma_device_init(&context->context, &cfg, &device->device);
                if (result == MA_SUCCESS)
                {
                    device->format = format;
                    device->channels = channels;
                    slot = std::move(device);
                    return {};
                }
                if (firstError == MA_SUCCESS)
                    firstError = result;   // the preferred format's error says the most
            }
            return name + ": " + describe(firstError);
        };

        if (auto error = openOne(inputs, true, inputsName, config.numInputs); !error.empty())
            return "inputs: " + error + "\nIf PluginHost already has it open with other settings, deselect "
                                        "the device there and Apply again.";
        if (auto error = openOne(outputs, false, outputsName, config.numOutputs); !error.empty())
            return "outputs: " + error + "\nIf PluginHost already has it open with other settings, deselect "
                                         "the device there and Apply again.";

        formatName = ma_get_format_name(inputs->format);
        return {};
    }

    void AudioIo::openMonitor(const SimConfig& config)
    {
        monitorError.clear();
        monitorOpened = false;

        const auto ringFrames = std::max(kMonitorRingPeriods * config.periodFrames, 4 * kChunkFrames);
        if (ma_pcm_rb_init(ma_format_f32, 2, ringFrames, nullptr, nullptr, &context->monitorRing) != MA_SUCCESS)
        {
            monitorError = "out of memory";
            return;
        }
        context->ringReady = true;

        std::optional<ma_device_id> id;
        if (config.monitorDevice != "default")
        {
            id = findDeviceId(context->context, config.monitorDevice, true, monitorError);
            if (!id)
                return;
        }

        auto device = std::make_unique<Device>();
        auto cfg = ma_device_config_init(ma_device_type_playback);
        cfg.sampleRate = config.sampleRate;   // miniaudio resamples to the speakers' own rate
        cfg.periodSizeInFrames = config.periodFrames;
        cfg.noPreSilencedOutputBuffer = MA_TRUE;
        cfg.pUserData = this;
        cfg.dataCallback = monitorTrampoline;
        cfg.notificationCallback = notificationTrampoline;
        cfg.playback.pDeviceID = id ? &*id : nullptr;
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.playback.shareMode = ma_share_mode_shared;

        if (const auto result = ma_device_init(&context->context, &cfg, &device->device); result != MA_SUCCESS)
        {
            monitorError = config.monitorDevice + ": " + describe(result);
            return;
        }
        device->format = ma_format_f32;
        device->channels = 2;
        monitor = std::move(device);
        monitorOpened = true;
    }

    // ------------------------------------------------------------------ callbacks

    void AudioIo::inputsCallback(void* output, std::uint32_t numFrames) noexcept
    {
        inputsClock.tick(numFrames, sampleRate);

        const auto channels = static_cast<std::size_t>(numInputs);
        const auto bytesPerFrame = ma_get_bytes_per_frame(inputs->format, static_cast<ma_uint32>(numInputs));
        auto* out = static_cast<std::uint8_t*>(output);

        float* planar[kMaxChannels];
        for (std::size_t ch = 0; ch < channels; ++ch)
            planar[ch] = inputsPlanar.data() + ch * kChunkFrames;

        for (std::uint32_t done = 0; done < numFrames;)
        {
            const auto n = std::min(kChunkFrames, numFrames - done);
            mixer->renderInputs(planar, numInputs, static_cast<int>(n));

            float* interleaved = inputsInterleaved.data();
            for (std::size_t ch = 0; ch < channels; ++ch)
                for (std::uint32_t i = 0; i < n; ++i)
                    interleaved[i * channels + ch] = clampSample(planar[ch][i]);

            auto* destination = out + std::size_t { done } * bytesPerFrame;
            if (inputs->format == ma_format_f32)
                std::memcpy(destination, interleaved, std::size_t { n } * bytesPerFrame);
            else
                ma_pcm_convert(destination, inputs->format, interleaved, ma_format_f32, n * channels, ma_dither_mode_none);
            done += n;
        }
    }

    void AudioIo::outputsCallback(const void* input, std::uint32_t numFrames) noexcept
    {
        outputsClock.tick(numFrames, sampleRate);

        const auto channels = static_cast<std::size_t>(numOutputs);
        const auto bytesPerFrame = ma_get_bytes_per_frame(outputs->format, static_cast<ma_uint32>(numOutputs));
        const auto* in = static_cast<const std::uint8_t*>(input);

        float* planar[kMaxChannels];
        for (std::size_t ch = 0; ch < channels; ++ch)
            planar[ch] = outputsPlanar.data() + ch * kChunkFrames;

        for (std::uint32_t done = 0; done < numFrames;)
        {
            const auto n = std::min(kChunkFrames, numFrames - done);
            const auto* source = in + std::size_t { done } * bytesPerFrame;

            const float* interleaved = reinterpret_cast<const float*>(source);
            if (outputs->format != ma_format_f32)
            {
                ma_pcm_convert(outputsInterleaved.data(), ma_format_f32, source, outputs->format, n * channels,
                               ma_dither_mode_none);
                interleaved = outputsInterleaved.data();
            }

            for (std::size_t ch = 0; ch < channels; ++ch)
                for (std::uint32_t i = 0; i < n; ++i)
                    planar[ch][i] = interleaved[i * channels + ch];

            mixer->processOutputs(planar, numOutputs, static_cast<int>(n), stereo.data());
            if (monitorOpened && !writeMonitor(stereo.data(), n))
                monitorDropouts.fetch_add(1, std::memory_order_relaxed);
            done += n;
        }
    }

    bool AudioIo::writeMonitor(const float* data, std::uint32_t numFrames) noexcept
    {
        auto* ring = &context->monitorRing;
        if (ma_pcm_rb_available_write(ring) < numFrames)
            return false;   // the speakers are behind: drop this block rather than queue latency

        while (numFrames > 0)
        {
            ma_uint32 frames = numFrames;
            void* buffer = nullptr;
            if (ma_pcm_rb_acquire_write(ring, &frames, &buffer) != MA_SUCCESS || frames == 0)
                return false;
            std::memcpy(buffer, data, std::size_t { frames } * 2 * sizeof(float));
            ma_pcm_rb_commit_write(ring, frames);
            data += std::size_t { frames } * 2;
            numFrames -= frames;
        }
        monitorPrimed.store(true, std::memory_order_relaxed);
        return true;
    }

    void AudioIo::monitorCallback(float* output, std::uint32_t numFrames) noexcept
    {
        monitorClock.tick(numFrames, sampleRate);
        auto* ring = &context->monitorRing;

        // Keep at most half the ring queued: drop the oldest audio when the loopback
        // clock runs ahead of the speakers', so latency stays bounded.
        const auto highWater = ma_pcm_rb_get_subbuffer_size(ring) / 2;
        if (const auto queued = ma_pcm_rb_available_read(ring); queued > highWater + numFrames)
        {
            ma_pcm_rb_seek_read(ring, queued - highWater);
            monitorDropouts.fetch_add(1, std::memory_order_relaxed);
        }

        std::uint32_t done = 0;
        while (done < numFrames)
        {
            ma_uint32 frames = numFrames - done;
            void* buffer = nullptr;
            if (ma_pcm_rb_acquire_read(ring, &frames, &buffer) != MA_SUCCESS || frames == 0)
                break;
            std::memcpy(output + std::size_t { done } * 2, buffer, std::size_t { frames } * 2 * sizeof(float));
            ma_pcm_rb_commit_read(ring, frames);
            done += frames;
        }

        if (done < numFrames)
        {
            std::fill(output + std::size_t { done } * 2, output + std::size_t { numFrames } * 2, 0.0f);
            if (monitorPrimed.load(std::memory_order_relaxed))
                monitorDropouts.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void AudioIo::deviceStopped(const void* device) noexcept
    {
        if (closing.load())
            return;
        if (monitor && device == &monitor->device)
            monitorLost = true;
        else
            loopbackLost = true;
    }
}
