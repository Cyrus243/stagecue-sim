#include "SimEngine.h"

#include "platform/Platform.h"

#include <algorithm>

namespace sim
{
    namespace
    {
        constexpr auto kStatusInterval = std::chrono::seconds(1);
        constexpr auto kStallTimeout = std::chrono::seconds(1);

        StatusPoller::Query makeQuery(const SimConfig& config)
        {
            // shared_ptr: std::function needs a copyable callable.
            std::shared_ptr<platform::LinkProbe> probe = platform::makeLinkProbe(config);
            return [probe] { return probe->query(); };
        }
    }

    SimEngine::SimEngine(SimConfig cfg)
        : config(std::move(cfg)), poller(makeQuery(config), kStatusInterval)
    {
    }

    SimEngine::~SimEngine() { stop(); }

    std::string SimEngine::start()
    {
        if (isRunning())
            return {};

        lastError = io.open(config, mixer);
        stalled = false;
        lastCallbacks = 0;
        lastProgress = Clock::now();
        return lastError;
    }

    void SimEngine::stop()
    {
        io.close();
        slots.collect(true);
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
            error += "\n(restoring the previous configuration also failed)";
        else
            error += "\n(kept the previous configuration)";
        lastError = error;
        return error;
    }

    void SimEngine::housekeeping()
    {
        slots.collect(!io.isOpen());

        if (io.isOpen() && io.isLost() && lastError.empty())
            lastError = "the loopback device stopped (removed, or taken over by another app) - press Apply to reopen it";

        const auto now = Clock::now();
        if (const auto callbacks = io.getLoopbackCallbacks(); callbacks != lastCallbacks || !isRunning())
        {
            lastCallbacks = callbacks;
            lastProgress = now;
        }
        stalled = isRunning() && now - lastProgress > kStallTimeout;
    }

    std::string SimEngine::getMonitorError() const
    {
        if (io.isMonitorLost())
            return "the main out device stopped - press Apply to reopen it";
        return io.getMonitorError();
    }
}
