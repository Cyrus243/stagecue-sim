#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace sim
{
    /** Lock-free single-producer / single-consumer ring of floats. */
    class SpscRing
    {
    public:
        explicit SpscRing(std::size_t capacity) : buffer(capacity + 1) {}

        [[nodiscard]] std::size_t size() const noexcept
        {
            const auto w = writePos.load(std::memory_order_acquire);
            const auto r = readPos.load(std::memory_order_acquire);
            return w >= r ? w - r : buffer.size() - r + w;
        }

        [[nodiscard]] std::size_t freeSpace() const noexcept { return buffer.size() - 1 - size(); }

        /** Pushes all samples or none; returns false (dropping them) when full. */
        bool push(const float* data, std::size_t count) noexcept
        {
            if (count > freeSpace())
                return false;

            auto w = writePos.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < count; ++i)
            {
                buffer[w] = data[i];
                w = (w + 1) % buffer.size();
            }
            writePos.store(w, std::memory_order_release);
            return true;
        }

        /** Pops up to `count` samples, returns how many were popped. */
        std::size_t pop(float* data, std::size_t count) noexcept
        {
            const auto n = std::min(count, size());
            auto r = readPos.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < n; ++i)
            {
                data[i] = buffer[r];
                r = (r + 1) % buffer.size();
            }
            readPos.store(r, std::memory_order_release);
            return n;
        }

    private:
        std::vector<float> buffer;
        std::atomic<std::size_t> writePos { 0 };
        std::atomic<std::size_t> readPos { 0 };
    };
}
