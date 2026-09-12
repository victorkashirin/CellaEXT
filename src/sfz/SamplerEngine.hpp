#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cella::sfz {

enum class EngineEventType : uint8_t {
    NoteOn,
    NoteOff,
    NotePitch,
    NoteBend,
    Pressure,
    Timbre,
    SourceCC,
    KeyswitchOn,
    KeyswitchOff,
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
    static TimedEngineEvent noteBend(uint32_t frameOffset, uint8_t channel,
        float semitones) noexcept;
    static TimedEngineEvent pressure(uint32_t frameOffset, uint8_t channel,
        float normalized) noexcept;
    static TimedEngineEvent timbre(uint32_t frameOffset, uint8_t channel,
        float normalized) noexcept;
    static TimedEngineEvent sourceCC(uint32_t frameOffset, uint8_t channel,
        uint8_t cc, float normalized) noexcept;
    static TimedEngineEvent keyswitchOn(uint32_t frameOffset, uint8_t channel,
        uint8_t note) noexcept;
    static TimedEngineEvent keyswitchOff(uint32_t frameOffset, uint8_t channel,
        uint8_t note) noexcept;
    static TimedEngineEvent chokeLaneTails(uint32_t frameOffset, uint8_t channel,
        bool includeHeldVoices) noexcept;
};

struct NamedController {
    uint8_t number { 0 };
    std::string label;
    float defaultValue { 0.0f };
};

struct LatchedKeyswitch {
    uint8_t note { 0 };
    std::string label;
};

// Immutable after load(). Strings and vectors remain on the loading/UI side;
// Rack expander messages contain numeric indices and values only.
struct InstrumentMetadata {
    std::vector<NamedController> namedControllers;
    std::vector<LatchedKeyswitch> latchedKeyswitches;
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
    // Runtime telemetry is sampled by Cella immediately after render(). Test
    // doubles can leave the default zero values and never depend on sfizioso.
    virtual int activeVoiceCount() const noexcept;
    virtual int voiceLimit() const noexcept;
    virtual const InstrumentMetadata& instrumentMetadata() const noexcept;
};

} // namespace cella::sfz
