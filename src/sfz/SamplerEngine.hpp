#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cella::sfz {

enum class EngineEventType : uint8_t {
    NoteOn,
    NoteOff,
    NotePitch,
    ChokeLaneTails,
};

// An event's frameOffset is relative to the next render() call.
struct TimedEngineEvent {
    EngineEventType type { EngineEventType::NoteOn };
    uint32_t frameOffset { 0 };
    uint8_t channel { 0 };
    uint8_t note { 0 };
    bool includeHeldVoices { false };
    float value { 0.0f };

    static TimedEngineEvent noteOn(uint32_t frameOffset, uint8_t channel,
        uint8_t note, float velocity) noexcept;
    static TimedEngineEvent noteOff(uint32_t frameOffset, uint8_t channel,
        uint8_t note, float velocity = 0.5f) noexcept;
    static TimedEngineEvent notePitch(uint32_t frameOffset, uint8_t channel,
        uint8_t note, float semitones) noexcept;
    static TimedEngineEvent chokeLaneTails(uint32_t frameOffset, uint8_t channel,
        bool includeHeldVoices) noexcept;
};

struct LoadReport {
    bool success { false };
    int regionCount { 0 };
    size_t preloadedSampleCount { 0 };
    size_t estimatedPreloadedSampleBytes { 0 };
    std::string message;
};

struct EngineStats {
    bool loaded { false };
    int regionCount { 0 };
    size_t preloadedSampleCount { 0 };
    size_t estimatedPreloadedSampleBytes { 0 };
};

// Cella's Rack-independent sampler boundary. load(), setSampleRate(), and
// setMaximumBlockSize() are control-thread operations; enqueue() and render()
// are bounded real-time operations once the engine is ready.
class SamplerEngine {
public:
    virtual ~SamplerEngine() = default;

    virtual LoadReport load(const std::string& path) = 0;
    virtual void setSampleRate(float sampleRate) = 0;
    virtual void setMaximumBlockSize(int frames) = 0;
    virtual void setTuningFrequency(float frequency) = 0;
    virtual void enqueue(const TimedEngineEvent& event) noexcept = 0;
    virtual void render(float* left, float* right, int frames) noexcept = 0;
    virtual EngineStats stats() const noexcept = 0;
};

} // namespace cella::sfz
