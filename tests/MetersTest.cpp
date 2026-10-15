#include "Meters.h"

#include <gtest/gtest.h>

#include <cmath>

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
