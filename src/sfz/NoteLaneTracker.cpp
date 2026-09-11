#include "NoteLaneTracker.hpp"

#include <algorithm>
#include <cmath>

namespace cella::sfz {
namespace {

float laneVoltageAt(const float* values, size_t channels, size_t lane,
    float disconnectedValue) noexcept
{
    if (!values || channels == 0)
        return disconnectedValue;
    return lane < channels ? values[lane] : 0.0f;
}

float gateVoltageAt(const float* values, size_t channels, size_t lane) noexcept
{
    if (!values || channels == 0)
        return 0.0f;
    const float voltage = channels == 1
        ? values[0]
        : laneVoltageAt(values, channels, lane, 0.0f);
    return std::isfinite(voltage) ? voltage : 0.0f;
}

bool requireEventCapacity(EventWriter& writer, size_t additional) noexcept
{
    if (additional == 0)
        return true;
    if (!writer.events || writer.count > writer.capacity
        || additional > writer.capacity - writer.count) {
        writer.dropped += additional;
        return false;
    }
    return true;
}

} // namespace

PitchDecomposition decomposePitch(float volts, int octave) noexcept
{
    if (!std::isfinite(volts))
        volts = 0.0f;
    const float absolute = 60.0f + 12.0f * volts + 12.0f * octave;
    const int rounded = static_cast<int>(std::lround(absolute));
    const int base = std::clamp(rounded, 0, 127);
    return { absolute, static_cast<uint8_t>(base), absolute - base };
}

float velocityFromVoltage(float volts) noexcept
{
    if (!std::isfinite(volts))
        return 0.0f;
    return std::clamp(volts * 0.1f, 0.0f, 1.0f);
}

bool EventWriter::push(const TimedEngineEvent& event) noexcept
{
    if (!events || count >= capacity) {
        ++dropped;
        return false;
    }
    events[count++] = event;
    return true;
}

void NoteLaneTracker::process(const NoteLaneFrame& frame, EventWriter& writer) noexcept
{
    const size_t requestedLanes = std::clamp<size_t>(
        std::max<size_t>(1, frame.pitchChannels), 1, MaxNoteLanes);

    struct LaneFrameState {
        PitchDecomposition pitch;
        bool rising;
        bool falling;
    };
    // Every entry below requestedLanes is assigned before it is read. Avoid
    // clearing all 16 entries on every audio frame when only a few are used.
    std::array<LaneFrameState, MaxNoteLanes> frameStates;

    // Determine the complete event cost before changing any lane state. A
    // short buffer then drops one input frame, while note-offs and pitch
    // changes remain pending and can be retried on the next frame.
    size_t requiredEvents = 0;
    if (requestedLanes < laneCount_) {
        for (size_t lane = requestedLanes; lane < laneCount_; ++lane)
            requiredEvents += lanes_[lane].active ? 1 : 0;
    }
    for (size_t lane = 0; lane < requestedLanes; ++lane) {
        const LaneState& state = lanes_[lane];
        LaneFrameState& current = frameStates[lane];
        const float gate = gateVoltageAt(
            frame.gateVoltages, frame.gateChannels, lane);
        current.pitch = decomposePitch(
            laneVoltageAt(frame.pitchVoltages, frame.pitchChannels, lane, 0.0f),
            frame.octave);
        current.rising = !state.gateHigh && gate >= GateHighVoltage;
        current.falling = state.gateHigh && gate <= GateLowVoltage;
        if (current.rising) {
            requiredEvents += 2;
        } else if (current.falling && state.active) {
            ++requiredEvents;
        } else if (state.active) {
            const float heldOffset = current.pitch.absoluteSemitones - state.note;
            requiredEvents += std::abs(heldOffset - state.pitchOffset)
                    >= NoteLaneTracker::PitchChangeThresholdSemitones
                ? 1
                : 0;
        }
    }
    if (!requireEventCapacity(writer, requiredEvents))
        return;

    if (requestedLanes < laneCount_) {
        for (size_t lane = requestedLanes; lane < laneCount_; ++lane) {
            LaneState& state = lanes_[lane];
            if (state.active) {
                writer.push(TimedEngineEvent::noteOff(frame.frameOffset,
                    static_cast<uint8_t>(lane), state.note));
            }
            state = {};
        }
    }
    laneCount_ = requestedLanes;

    for (size_t lane = 0; lane < laneCount_; ++lane) {
        LaneState& state = lanes_[lane];
        const LaneFrameState& current = frameStates[lane];
        if (current.rising)
            state.gateHigh = true;
        else if (current.falling)
            state.gateHigh = false;

        if (current.rising) {
            state.active = true;
            state.note = current.pitch.baseNote;
            state.pitchOffset = current.pitch.noteOffsetSemitones;
            const float velocity = frame.velocityChannels == 0
                ? 0.8f
                : velocityFromVoltage(laneVoltageAt(frame.velocityVoltages,
                    frame.velocityChannels, lane, 0.0f));
            writer.push(TimedEngineEvent::noteOn(frame.frameOffset,
                static_cast<uint8_t>(lane), state.note, velocity));
            writer.push(TimedEngineEvent::notePitch(frame.frameOffset,
                static_cast<uint8_t>(lane), state.note, state.pitchOffset));
        } else if (current.falling && state.active) {
            writer.push(TimedEngineEvent::noteOff(frame.frameOffset,
                static_cast<uint8_t>(lane), state.note));
            state.active = false;
        } else if (state.active) {
            // The note chosen on the gate edge is immutable. Crossing a
            // semitone boundary changes only this note instance's offset.
            const float heldOffset = current.pitch.absoluteSemitones - state.note;
            if (std::abs(heldOffset - state.pitchOffset)
                >= PitchChangeThresholdSemitones) {
                state.pitchOffset = heldOffset;
                writer.push(TimedEngineEvent::notePitch(frame.frameOffset,
                    static_cast<uint8_t>(lane), state.note, state.pitchOffset));
            }
        }
    }
}

bool NoteLaneTracker::reset(EventWriter* writer, uint32_t frameOffset) noexcept
{
    size_t requiredEvents = 0;
    if (writer) {
        for (size_t lane = 0; lane < laneCount_; ++lane)
            requiredEvents += lanes_[lane].active ? 1 : 0;
        if (!requireEventCapacity(*writer, requiredEvents))
            return false;
    }

    for (size_t lane = 0; lane < laneCount_; ++lane) {
        if (writer && lanes_[lane].active) {
            writer->push(TimedEngineEvent::noteOff(frameOffset,
                static_cast<uint8_t>(lane), lanes_[lane].note));
        }
        lanes_[lane] = {};
    }
    for (size_t lane = laneCount_; lane < lanes_.size(); ++lane)
        lanes_[lane] = {};
    laneCount_ = 1;
    return true;
}

bool NoteLaneTracker::noteActive(size_t lane) const noexcept
{
    return lane < laneCount_ && lanes_[lane].active;
}

uint8_t NoteLaneTracker::activeNote(size_t lane) const noexcept
{
    return lane < laneCount_ ? lanes_[lane].note : 0;
}

} // namespace cella::sfz
