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

TimedEngineEvent TimedEngineEvent::noteBend(uint32_t frameOffset,
    uint8_t channel, float semitones) noexcept
{
    return { EngineEventType::NoteBend, frameOffset, channel, 0, false,
        semitones };
}

TimedEngineEvent TimedEngineEvent::pressure(uint32_t frameOffset,
    uint8_t channel, float normalized) noexcept
{
    return { EngineEventType::Pressure, frameOffset, channel, 0, false,
        normalized };
}

TimedEngineEvent TimedEngineEvent::timbre(uint32_t frameOffset,
    uint8_t channel, float normalized) noexcept
{
    return { EngineEventType::Timbre, frameOffset, channel, 74, false,
        normalized };
}

TimedEngineEvent TimedEngineEvent::sourceCC(uint32_t frameOffset,
    uint8_t channel, uint8_t cc, float normalized) noexcept
{
    return { EngineEventType::SourceCC, frameOffset, channel, cc, false,
        normalized };
}

TimedEngineEvent TimedEngineEvent::keyswitchOn(uint32_t frameOffset,
    uint8_t channel, uint8_t note) noexcept
{
    return { EngineEventType::KeyswitchOn, frameOffset, channel, note, false,
        1.0f };
}

TimedEngineEvent TimedEngineEvent::keyswitchOff(uint32_t frameOffset,
    uint8_t channel, uint8_t note) noexcept
{
    return { EngineEventType::KeyswitchOff, frameOffset, channel, note, false,
        0.0f };
}

TimedEngineEvent TimedEngineEvent::chokeLaneTails(uint32_t frameOffset,
    uint8_t channel, bool includeHeldVoices) noexcept
{
    return { EngineEventType::ChokeLaneTails, frameOffset, channel, 0,
        includeHeldVoices, 0.0f };
}

const InstrumentMetadata& SamplerEngine::instrumentMetadata() const noexcept
{
    static const InstrumentMetadata empty;
    return empty;
}

int SamplerEngine::activeVoiceCount() const noexcept
{
    return 0;
}

int SamplerEngine::voiceLimit() const noexcept
{
    return 0;
}

} // namespace cella::sfz
