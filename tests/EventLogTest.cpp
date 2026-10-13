#include "EventLog.h"

#include <gtest/gtest.h>

using namespace sim;

TEST(EventLog, KeepsEntriesOldestFirst)
{
    EventLog log;
    log.add(EventLog::Severity::Ok, "first", "10:00:00");
    log.add(EventLog::Severity::Error, "second", "10:00:01");

    ASSERT_EQ(log.getEntries().size(), 2u);
    EXPECT_EQ(log.getEntries().front().text, "first");
    EXPECT_EQ(log.getEntries().front().time, "10:00:00");
    EXPECT_EQ(log.getEntries().back().text, "second");
    EXPECT_EQ(log.getEntries().back().severity, EventLog::Severity::Error);
}

TEST(EventLog, DropsOldestBeyondCapacity)
{
    EventLog log(3);
    for (int i = 0; i < 5; ++i)
        log.add(EventLog::Severity::Info, std::to_string(i), "00:00:00");

    ASSERT_EQ(log.getEntries().size(), 3u);
    EXPECT_EQ(log.getEntries().front().text, "2");
    EXPECT_EQ(log.getEntries().back().text, "4");
}

TEST(EventLog, ClockTimeIsHoursMinutesSeconds)
{
    const auto time = EventLog::clockTime();
    ASSERT_EQ(time.size(), 8u);
    EXPECT_EQ(time[2], ':');
    EXPECT_EQ(time[5], ':');
}
