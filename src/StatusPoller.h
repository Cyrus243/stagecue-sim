#pragma once

#include "LoopbackStatus.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace sim
{
    /** Runs a status query on its own thread at a fixed interval and keeps the latest
        result, so the UI never waits on /proc or the CoreAudio HAL. The thread sleeps
        on a condition variable and wakes at once for shutdown. */
    class StatusPoller
    {
    public:
        using Query = std::function<LoopbackStatus()>;

        StatusPoller(Query queryToRun, std::chrono::milliseconds interval);
        ~StatusPoller();
        StatusPoller(const StatusPoller&) = delete;
        StatusPoller& operator=(const StatusPoller&) = delete;

        /** The latest result (default-constructed until the first query returns). */
        [[nodiscard]] LoopbackStatus latest() const;

    private:
        void run();

        Query query;
        std::chrono::milliseconds interval;

        mutable std::mutex mutex;
        std::condition_variable wake;
        bool stopping = false;
        LoopbackStatus status;
        std::thread thread;   // last: starts once everything above is constructed
    };
}
