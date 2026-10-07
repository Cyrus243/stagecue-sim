#include "Meters.h"
#include "SpscRing.h"

#include <gtest/gtest.h>

using namespace sim;

TEST(PeakMeter, KeepsMaxUntilConsumed)
{
    PeakMeter meter;
    meter.push(0.2f);
    meter.push(0.7f);
    meter.push(0.4f);
    EXPECT_FLOAT_EQ(meter.consume(), 0.7f);
    EXPECT_FLOAT_EQ(meter.consume(), 0.0f);
}

TEST(MeterBallistics, InstantAttackExponentialReleaseAndFloor)
{
    MeterBallistics meter;
    EXPECT_NEAR(meter.update(0.5f, 0.033), gainToDb(0.5f), 1e-4);

    // One release time constant later the level has dropped by 1/e.
    const float released = meter.update(0.0f, kMeterReleaseSeconds);
    EXPECT_NEAR(released, gainToDb(0.5f / static_cast<float>(std::exp(1.0))), 1e-3);

    EXPECT_FLOAT_EQ(meter.update(0.0f, 10.0), kMeterFloorDb);
}

TEST(SpscRing, PushesWholeBlocksOrNothing)
{
    SpscRing ring(4);
    const float block[] = { 1, 2, 3 };
    EXPECT_TRUE(ring.push(block, 3));
    EXPECT_FALSE(ring.push(block, 2));
    EXPECT_EQ(ring.size(), 3u);

    float out[4] = {};
    EXPECT_EQ(ring.pop(out, 4), 3u);
    EXPECT_FLOAT_EQ(out[2], 3.0f);

    // Wraps around the end of the buffer.
    EXPECT_TRUE(ring.push(block, 3));
    EXPECT_EQ(ring.pop(out, 2), 2u);
    EXPECT_FLOAT_EQ(out[1], 2.0f);
    EXPECT_EQ(ring.size(), 1u);
}
