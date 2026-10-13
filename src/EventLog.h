#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <deque>
#include <string>

namespace sim
{
    /** Bounded, timestamped list of things that happened, shown in the UI's activity log.
        UI thread only. */
    class EventLog
    {
    public:
        enum class Severity { Info, Ok, Warn, Error };

        struct Entry
        {
            std::string time;   // HH:MM:SS
            std::string text;
            Severity severity = Severity::Info;
        };

        explicit EventLog(std::size_t capacityToUse = 200) : capacity(capacityToUse) {}

        void add(Severity severity, std::string text) { add(severity, std::move(text), clockTime()); }

        void add(Severity severity, std::string text, std::string time)
        {
            entries.push_back({ std::move(time), std::move(text), severity });
            while (entries.size() > capacity)
                entries.pop_front();
        }

        /** Oldest first. */
        [[nodiscard]] const std::deque<Entry>& getEntries() const noexcept { return entries; }

        /** Local wall-clock time as HH:MM:SS. */
        [[nodiscard]] static std::string clockTime()
        {
            const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local {};
            localtime_r(&now, &local);
            char buffer[16];
            std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
            return buffer;
        }

    private:
        std::size_t capacity;
        std::deque<Entry> entries;
    };
}
