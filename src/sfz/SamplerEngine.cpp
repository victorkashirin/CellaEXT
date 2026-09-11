#include "SamplerEngine.hpp"

namespace cella::sfz {

TimedEngineEvent TimedEngineEvent::noteOn(uint32_t frameOffset, uint8_t channel,
    uint8_t note, float velocity) noexcept
{
    return { EngineEventType::NoteOn, frameOffset, channel, note, false, velocity };
}

TimedEngineEvent TimedEngineEvent::noteOff(uint32_t frameOffset, uint8_t channel,
    uint8_t note, float velocity) noexcept
{
    return { EngineEventType::NoteOff, frameOffset, channel, note, false, velocity };
}

TimedEngineEvent TimedEngineEvent::notePitch(uint32_t frameOffset, uint8_t channel,
    uint8_t note, float semitones) noexcept
{
    return { EngineEventType::NotePitch, frameOffset, channel, note, false,
        semitones };
}

TimedEngineEvent TimedEngineEvent::chokeLaneTails(uint32_t frameOffset,
    uint8_t channel, bool includeHeldVoices) noexcept
{
    return { EngineEventType::ChokeLaneTails, frameOffset, channel, 0,
        includeHeldVoices, 0.0f };
}

} // namespace cella::sfz
