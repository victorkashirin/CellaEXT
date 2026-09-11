#include "BlockAdapter.hpp"

#include <algorithm>

namespace cella::sfz {

BlockAdapter::BlockAdapter(SamplerEngine& engine) noexcept
    : engine_(&engine)
{
}

bool BlockAdapter::push(const TimedEngineEvent& event) noexcept
{
    if (eventCount_ == events_.size()) {
        ++droppedEventCount_;
        return false;
    }
    events_[eventCount_++] = { event, nextSequence_++ };
    return true;
}

bool BlockAdapter::render(float* left, float* right, int frames) noexcept
{
    if (!left || !right || frames <= 0 || frames > MaxFrames) {
        droppedEventCount_ += eventCount_;
        clear();
        return false;
    }

    std::sort(events_.begin(), events_.begin() + eventCount_,
        [](const QueuedEvent& a, const QueuedEvent& b) {
            if (a.event.frameOffset != b.event.frameOffset)
                return a.event.frameOffset < b.event.frameOffset;
            return a.sequence < b.sequence;
        });

    for (size_t i = 0; i < eventCount_; ++i) {
        TimedEngineEvent event = events_[i].event;
        if (event.frameOffset >= static_cast<uint32_t>(frames)) {
            ++droppedEventCount_;
            continue;
        }
        engine_->enqueue(event);
    }
    engine_->render(left, right, frames);
    clear();
    return true;
}

void BlockAdapter::clear() noexcept
{
    eventCount_ = 0;
    nextSequence_ = 0;
}

} // namespace cella::sfz
