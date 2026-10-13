#include "SimEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace sim
{
    namespace
    {
        constexpr unsigned kPrefillPeriods = 2;
        constexpr std::size_t kMonitorRingPeriods = 8;

        float peakOf(const float* data, int numFrames) noexcept
        {
            float peak = 0.0f;
            for (int i = 0; i < numFrames; ++i)
                peak = std::max(peak, std::abs(data[i]));
            return peak;
        }

        std::vector<float*> pointersTo(std::vector<std::vector<float>>& buffers)
        {
            std::vector<float*> pointers;
            pointers.reserve(buffers.size());
            for (auto& buffer : buffers)
                pointers.push_back(buffer.data());
            return pointers;
        }
    }

    SimEngine::SimEngine(SimConfig cfg) : config(std::move(cfg))
    {
        for (auto& source : sources)
            source.store(std::make_shared<SilenceSource>());

        // Default monitoring: the first two outputs (L/R) go to the main out.
        for (int i = 0; i < kMaxChannels; ++i)
            monitorEnabled[i] = i < 2;
    }

    SimEngine::~SimEngine() { stop(); }

    std::string SimEngine::start()
    {
        if (running.load())
            return {};

        // Joins threads that exited on their own after a fatal stream error.
        stop();

        {
            const std::lock_guard lock(errorMutex);
            lastError.clear();
            monitorError.clear();
        }

        const auto pcmName = simPcmName(config);
        const auto hint = std::string("\nIs the loopback card loaded (./setup-loopback.sh)? If PluginHost already "
                                      "has it open with other settings, deselect it in the app and Apply again.");

        auto error = loopbackOut.open(pcmName, AlsaPcm::Direction::Playback, config.numInputs,
                                      config.sampleRate, config.periodFrames, config.periods, true);
        if (!error.empty())
        {
            setError("inputs: " + error + hint);
            return getLastError();
        }

        error = loopbackIn.open(pcmName, AlsaPcm::Direction::Capture, config.numOutputs,
                                config.sampleRate, config.periodFrames, config.periods, true);
        if (!error.empty())
        {
            loopbackOut.close();
            setError("outputs: " + error + hint);
            return getLastError();
        }

        auto monitorOpenError = monitor.open(config.monitorDevice, AlsaPcm::Direction::Playback, 2,
                                             config.sampleRate, config.periodFrames, config.periods, false);
        if (monitorOpenError.empty() && monitor.getSampleRate() != config.sampleRate)
        {
            monitorOpenError = config.monitorDevice + " runs at " + std::to_string(monitor.getSampleRate())
                               + " Hz instead of " + std::to_string(config.sampleRate) + " Hz";
            monitor.close();
        }
        {
            const std::lock_guard lock(errorMutex);
            monitorError = monitorOpenError;
        }

        monitorRing = std::make_unique<SpscRing>(
            static_cast<std::size_t>(loopbackIn.getPeriodFrames()) * 2 * kMonitorRingPeriods);
        monitorDropouts = 0;

        running = true;
        loopThread = std::thread([this] { loopThreadMain(); });
        if (monitor.isOpen())
            monitorThread = std::thread([this] { monitorThreadMain(); });

        return {};
    }

    void SimEngine::stop()
    {
        running = false;
        if (loopThread.joinable())
            loopThread.join();
        if (monitorThread.joinable())
            monitorThread.join();

        loopbackOut.close();
        loopbackIn.close();
        monitor.close();
    }

    std::string SimEngine::reconfigure(int numInputs, int numOutputs)
    {
        const int previousInputs = config.numInputs;
        const int previousOutputs = config.numOutputs;

        stop();
        config.numInputs = std::clamp(numInputs, 1, kMaxChannels);
        config.numOutputs = std::clamp(numOutputs, 1, kMaxChannels);

        auto error = start();
        if (error.empty())
            return {};

        config.numInputs = previousInputs;
        config.numOutputs = previousOutputs;
        if (const auto restoreError = start(); !restoreError.empty())
            return error + "\n(restoring the previous configuration also failed)";

        return error + "\n(kept the previous configuration)";
    }

    void SimEngine::setInputSource(int input, std::shared_ptr<ISource> source)
    {
        if (source == nullptr)
            source = std::make_shared<SilenceSource>();
        sources[clamp(input)].store(std::move(source));
    }

    std::shared_ptr<ISource> SimEngine::getInputSource(int input) const
    {
        return sources[clamp(input)].load();
    }

    std::uint64_t SimEngine::getLoopbackXruns() const noexcept
    {
        return loopbackOut.getXrunCount() + loopbackIn.getXrunCount();
    }

    std::uint64_t SimEngine::getMonitorXruns() const noexcept { return monitor.getXrunCount(); }

    std::string SimEngine::getLastError() const
    {
        const std::lock_guard lock(errorMutex);
        return lastError;
    }

    std::string SimEngine::getMonitorError() const
    {
        const std::lock_guard lock(errorMutex);
        return monitorError;
    }

    void SimEngine::setError(std::string message)
    {
        const std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    void SimEngine::loopThreadMain()
    {
        const int numFrames = static_cast<int>(loopbackIn.getPeriodFrames());
        const int numIn = config.numInputs;
        const int numOut = config.numOutputs;

        std::vector<std::vector<float>> inputBuffers(static_cast<std::size_t>(numIn), std::vector<float>(static_cast<std::size_t>(numFrames)));
        std::vector<std::vector<float>> outputBuffers(static_cast<std::size_t>(numOut), std::vector<float>(static_cast<std::size_t>(numFrames)));
        auto inputPointers = pointersTo(inputBuffers);
        auto outputPointers = pointersTo(outputBuffers);
        std::vector<float> stereo(static_cast<std::size_t>(numFrames) * 2);
        std::vector<std::shared_ptr<ISource>> currentSources(static_cast<std::size_t>(numIn));

        std::uint64_t position = songPosition.load();
        std::uint64_t seenRestarts = restartRequests.load();

        for (unsigned i = 0; i < kPrefillPeriods; ++i)
            if (loopbackOut.writeSilence(static_cast<int>(loopbackOut.getPeriodFrames())) < 0)
                break;

        while (running.load())
        {
            // App outputs -> meters + monitor mix.
            const int got = loopbackIn.read(outputPointers.data(), numFrames);
            if (got < 0)
            {
                setError(std::string("loopback capture failed: ") + std::strerror(-got));
                break;
            }

            std::fill(stereo.begin(), stereo.end(), 0.0f);
            const bool muted = monitorMuted.load();
            for (int ch = 0; ch < numOut; ++ch)
            {
                const float* data = outputPointers[static_cast<std::size_t>(ch)];
                outputMeters[ch].push(peakOf(data, numFrames));

                if (muted || !monitorEnabled[ch].load())
                    continue;

                const std::size_t side = ch % 2 == 0 ? 0 : 1;
                for (int i = 0; i < numFrames; ++i)
                    stereo[static_cast<std::size_t>(i) * 2 + side] += data[i];
            }
            if (monitor.isOpen() && !monitorRing->push(stereo.data(), stereo.size()))
                ++monitorDropouts;

            // Sources -> app inputs, all rendered from the same song position.
            std::uint64_t loopLength = 0;
            for (int ch = 0; ch < numIn; ++ch)
            {
                auto& source = currentSources[static_cast<std::size_t>(ch)];
                source = sources[ch].load();
                if (source)
                    loopLength = std::max<std::uint64_t>(loopLength, source->getLengthFrames());
            }

            if (const auto restarts = restartRequests.load(); restarts != seenRestarts)
            {
                seenRestarts = restarts;
                position = 0;
            }
            position = loopLength > 0 ? position % loopLength : 0;

            const TransportFrame transport { transportPlaying.load(), position, loopLength };
            for (int ch = 0; ch < numIn; ++ch)
            {
                float* data = inputPointers[static_cast<std::size_t>(ch)];
                if (const auto& source = currentSources[static_cast<std::size_t>(ch)])
                    source->render(data, numFrames, transport);
                else
                    std::fill_n(data, numFrames, 0.0f);
                inputMeters[ch].push(peakOf(data, numFrames));
            }

            if (transport.playing && loopLength > 0)
                position = (position + static_cast<std::uint64_t>(numFrames)) % loopLength;
            songPosition = position;
            songLength = loopLength;

            if (const int written = loopbackOut.write(inputPointers.data(), numFrames); written < 0)
            {
                setError(std::string("loopback playback failed: ") + std::strerror(-written));
                break;
            }
        }

        running = false;
    }

    void SimEngine::monitorThreadMain()
    {
        using namespace std::chrono;

        const int numFrames = static_cast<int>(monitor.getPeriodFrames());
        const auto needed = static_cast<std::size_t>(numFrames) * 2;
        const auto waitBudget = duration<double>(static_cast<double>(numFrames) / monitor.getSampleRate());

        std::vector<float> stereo(needed);
        std::vector<float> left(static_cast<std::size_t>(numFrames));
        std::vector<float> right(static_cast<std::size_t>(numFrames));
        const float* planar[] = { left.data(), right.data() };

        while (running.load())
        {
            // Give the loop thread up to one period to deliver; past that, pad
            // with silence so the main out never starves (absorbs clock drift).
            const auto deadline = steady_clock::now() + duration_cast<steady_clock::duration>(waitBudget);
            while (running.load() && monitorRing->size() < needed && steady_clock::now() < deadline)
                std::this_thread::sleep_for(milliseconds(1));

            const auto got = monitorRing->pop(stereo.data(), needed);
            if (got < needed)
            {
                std::fill(stereo.begin() + static_cast<std::ptrdiff_t>(got), stereo.end(), 0.0f);
                ++monitorDropouts;
            }

            for (int i = 0; i < numFrames; ++i)
            {
                left[static_cast<std::size_t>(i)] = stereo[static_cast<std::size_t>(i) * 2];
                right[static_cast<std::size_t>(i)] = stereo[static_cast<std::size_t>(i) * 2 + 1];
            }

            if (const int written = monitor.write(planar, numFrames); written < 0)
            {
                const std::lock_guard lock(errorMutex);
                monitorError = std::string("main out failed: ") + std::strerror(-written);
                return;
            }
        }
    }
}
