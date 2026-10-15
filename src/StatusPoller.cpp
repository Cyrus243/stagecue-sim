#include "StatusPoller.h"

namespace sim
{
    StatusPoller::StatusPoller(Query queryToRun, std::chrono::milliseconds pollInterval)
        : query(std::move(queryToRun)), interval(pollInterval), thread([this] { run(); })
    {
    }

    StatusPoller::~StatusPoller()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        thread.join();
    }

    LoopbackStatus StatusPoller::latest() const
    {
        const std::lock_guard lock(mutex);
        return status;
    }

    void StatusPoller::run()
    {
        std::unique_lock lock(mutex);
        while (!stopping)
        {
            // The query may block on IPC: run it without holding the lock.
            lock.unlock();
            auto next = query();
            lock.lock();

            status = std::move(next);
            wake.wait_for(lock, interval, [this] { return stopping; });
        }
    }
}
