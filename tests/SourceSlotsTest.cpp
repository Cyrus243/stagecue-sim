#include "SourceSlots.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace sim;

namespace
{
    void renderOnce(Mixer& mixer)
    {
        std::vector<float> buffer(16);
        float* channels[] = { buffer.data() };
        mixer.renderInputs(channels, 1, 16);
    }
}

TEST(SourceSlots, KeepsAReplacedSourceUntilTheAudioThreadMovesOn)
{
    Mixer mixer;
    SourceSlots slots(mixer);

    auto first = std::make_shared<SineSource>(48000.0, 440.0f, -6.0f);
    std::weak_ptr<SineSource> watch = first;
    slots.set(0, std::move(first));
    slots.set(0, std::make_shared<SineSource>(48000.0, 880.0f, -6.0f));

    // A callback may still be holding the old pointer: nothing is freed yet.
    slots.collect(false);
    EXPECT_FALSE(watch.expired());

    renderOnce(mixer);
    slots.collect(false);
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(slots.retiredCount(), 0u);
}

TEST(SourceSlots, FreesAtOnceWhenAudioIsStopped)
{
    Mixer mixer;
    SourceSlots slots(mixer);

    auto first = std::make_shared<SineSource>(48000.0, 440.0f, -6.0f);
    std::weak_ptr<SineSource> watch = first;
    slots.set(0, std::move(first));
    slots.set(0, nullptr);

    slots.collect(true);
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(slots.get(0), nullptr);
}

TEST(SourceSlots, RetiredListStaysBoundedUnderRepeatedSwaps)
{
    Mixer mixer;
    SourceSlots slots(mixer);
    for (int i = 0; i < 1000; ++i)
    {
        slots.set(i % kMaxChannels, std::make_shared<SineSource>(48000.0, 440.0f, -6.0f));
        renderOnce(mixer);
        slots.collect(false);
    }
    EXPECT_LE(slots.retiredCount(), 1u);
}
