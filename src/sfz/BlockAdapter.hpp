#pragma once

#include "SamplerEngine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cella::sfz {

// Enough room for the worst case of a tail-control, note-on and initial pitch
// event on every lane and frame at the largest selectable quantum. No
// allocation occurs while processing.
class BlockAdapter {
public:
    static constexpr int MaxFrames = 64;
    static constexpr size_t MaxEvents = 16 * 3 * MaxFrames;

    explicit BlockAdapter(SamplerEngine& engine) noexcept;

    bool push(const TimedEngineEvent& event) noexcept;
    bool render(float* left, float* right, int frames) noexcept;
    void clear() noexcept;

    size_t eventCount() const noexcept { return eventCount_; }
    size_t droppedEventCount() const noexcept { return droppedEventCount_; }

private:
    struct QueuedEvent {
        TimedEngineEvent event;
        uint32_t sequence { 0 };
    };

    SamplerEngine* engine_;
    std::array<QueuedEvent, MaxEvents> events_ {};
    size_t eventCount_ { 0 };
    size_t droppedEventCount_ { 0 };
    uint32_t nextSequence_ { 0 };
};

} // namespace cella::sfz
