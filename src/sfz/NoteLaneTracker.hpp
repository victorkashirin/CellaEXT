#pragma once

#include "SamplerEngine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cella::sfz {

constexpr size_t MaxNoteLanes = 16;

struct PitchDecomposition {
    float absoluteSemitones { 60.0f };
    uint8_t baseNote { 60 };
    float noteOffsetSemitones { 0.0f };
};

PitchDecomposition decomposePitch(float volts, int octave) noexcept;
float velocityFromVoltage(float volts) noexcept;

struct NoteLaneFrame {
    const float* pitchVoltages { nullptr };
    size_t pitchChannels { 0 };
    const float* gateVoltages { nullptr };
    size_t gateChannels { 0 };
    const float* velocityVoltages { nullptr };
    size_t velocityChannels { 0 };
    int octave { 0 };
    uint32_t frameOffset { 0 };
};

struct EventWriter {
    TimedEngineEvent* events { nullptr };
    size_t capacity { 0 };
    size_t count { 0 };
    size_t dropped { 0 };

    bool push(const TimedEngineEvent& event) noexcept;
};

class NoteLaneTracker {
public:
    static constexpr float GateHighVoltage = 1.0f;
    static constexpr float GateLowVoltage = 0.1f;
    // One tenth of a cent keeps musical pitch motion continuous while
    // suppressing sub-resolution CV jitter and redundant sfizz events.
    static constexpr float PitchChangeThresholdSemitones = 0.001f;

    void process(const NoteLaneFrame& frame, EventWriter& writer) noexcept;
    bool reset(EventWriter* writer = nullptr, uint32_t frameOffset = 0) noexcept;

    size_t laneCount() const noexcept { return laneCount_; }
    size_t activeCount() const noexcept;
    bool noteActive(size_t lane) const noexcept;
    uint8_t activeNote(size_t lane) const noexcept;

private:
    struct LaneState {
        bool gateHigh { false };
        bool active { false };
        uint8_t note { 60 };
        float pitchOffset { 0.0f };
    };

    std::array<LaneState, MaxNoteLanes> lanes_ {};
    size_t laneCount_ { 1 };
};

} // namespace cella::sfz
