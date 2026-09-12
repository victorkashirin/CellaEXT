#pragma once

#include "SamplerEngine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cella::sfz {

constexpr size_t ExpressionLaneCount = 16;
constexpr size_t NamedControlCount = 4;
constexpr size_t ExpressionInputCount = 3 + NamedControlCount;
constexpr uint32_t ExpressionMessageMagic = 0x43455850; // CEXP

struct PolyVoltage {
    uint8_t channels { 0 };
    std::array<float, ExpressionLaneCount> values {};
};

// Expander -> sampler. This deliberately contains no pointers, strings, or
// dynamically-sized containers and is copied through Rack's double buffer.
struct ExpressionMessage {
    uint32_t magic { ExpressionMessageMagic };
    uint32_t sequence { 0 };
    std::array<PolyVoltage, ExpressionInputCount> inputs {};
    std::array<int16_t, NamedControlCount> assignments { -1, -1, -1, -1 };
    std::array<int16_t, ExpressionLaneCount> articulationIndices {
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1
    };
};

// Sampler -> expander. Metadata strings stay in the immutable UI snapshot;
// this numeric summary is sufficient for deterministic audio-side matching.
struct ExpressionFeedback {
    uint32_t magic { ExpressionMessageMagic };
    uint32_t metadataGeneration { 0 };
    uint8_t activeLanes { 1 };
    uint8_t keyswitchCount { 0 };
    std::array<uint8_t, 128> assignableCCs {};
    std::array<int16_t, ExpressionLaneCount> activeArticulations {};
};

float normalizedVoltage(float volts) noexcept;
float polyVoltageForLane(const PolyVoltage& input, size_t lane,
    float disconnectedValue) noexcept;

class SelectorQuantizer {
public:
    static constexpr float HysteresisFraction = 0.08f;

    int process(size_t lane, float voltage, int switchCount) noexcept;
    void reset(int selection = 0) noexcept;

private:
    std::array<int16_t, ExpressionLaneCount> selections_ {};
};

const NamedController* findNamedController(
    const InstrumentMetadata& metadata, int cc) noexcept;

} // namespace cella::sfz
