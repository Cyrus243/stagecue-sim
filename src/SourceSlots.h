#pragma once

#include "Mixer.h"

#include <array>
#include <memory>
#include <vector>

namespace sim
{
    /** UI-thread owner of the sources the Mixer renders. The Mixer only sees raw
        pointers, so a replaced source is parked here until the Mixer's epoch shows
        that no callback can still be using it. That way the audio thread never
        frees one (a file can hold hundreds of MB). UI thread only. */
    class SourceSlots
    {
    public:
        explicit SourceSlots(Mixer& mixerToUse) : mixer(mixerToUse) {}

        void set(int input, std::shared_ptr<ISource> source)
        {
            const int index = Mixer::clamp(input);
            mixer.setSource(index, source.get());
            // Read after the store: any callback still holding the old pointer started
            // before it and bumps the epoch past this value when it returns.
            retired.push_back({ std::move(owners[index]), mixer.getEpoch() });
            owners[index] = std::move(source);
        }

        [[nodiscard]] std::shared_ptr<ISource> get(int input) const { return owners[Mixer::clamp(input)]; }

        /** Frees retired sources no callback can reach any more. With audioStopped
            (no callback running) everything retired goes. */
        void collect(bool audioStopped)
        {
            const auto now = mixer.getEpoch();
            std::erase_if(retired, [&](const Retired& r) { return audioStopped || now > r.epoch; });
        }

        [[nodiscard]] std::size_t retiredCount() const noexcept { return retired.size(); }

    private:
        struct Retired
        {
            std::shared_ptr<ISource> source;
            std::uint64_t epoch = 0;
        };

        Mixer& mixer;
        std::array<std::shared_ptr<ISource>, kMaxChannels> owners;
        std::vector<Retired> retired;
    };
}
