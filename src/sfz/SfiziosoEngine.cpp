#include "SfiziosoEngine.hpp"

#include <sfizz.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace cella::sfz {

struct SfiziosoEngine::Impl {
    ::sfz::Sfizz synth;
    EngineStats stats;
    InstrumentMetadata metadata;
    std::array<float, 16> notePitch {};
    std::array<float, 16> noteBend {};
};

namespace {

struct MessageReply {
    bool received { false };
    float number { 0.0f };
    std::string text;
    std::array<uint8_t, 64> blob {};
    size_t blobSize { 0 };
};

void receiveMessage(void* data, int, const char*, const char* sig,
    const sfizz_arg_t* args)
{
    auto& reply = *static_cast<MessageReply*>(data);
    reply.received = true;
    if (!sig || !args)
        return;
    if (std::strcmp(sig, "f") == 0)
        reply.number = args[0].f;
    else if (std::strcmp(sig, "i") == 0)
        reply.number = static_cast<float>(args[0].i);
    else if (std::strcmp(sig, "s") == 0 && args[0].s)
        reply.text = args[0].s;
    else if (std::strcmp(sig, "b") == 0 && args[0].b) {
        reply.blobSize = std::min<size_t>(reply.blob.size(), args[0].b->size);
        std::copy_n(args[0].b->data, reply.blobSize, reply.blob.begin());
    }
}

MessageReply query(::sfz::Sfizz& synth, const std::string& path)
{
    MessageReply reply;
    auto client = ::sfz::Sfizz::createClient(&reply);
    ::sfz::Sfizz::setReceiveCallback(*client, receiveMessage);
    synth.sendMessage(*client, 0, path.c_str(), "", nullptr);
    return reply;
}

const char* standardCCLabel(int cc) noexcept
{
    switch (cc) {
    case 0: return "Bank Select";
    case 1: return "Modulation";
    case 2: return "Breath";
    case 4: return "Foot Controller";
    case 5: return "Portamento Time";
    case 6: return "Data Entry";
    case 7: return "Volume";
    case 8: return "Balance";
    case 10: return "Pan";
    case 11: return "Expression";
    case 12: return "Effect Control 1";
    case 13: return "Effect Control 2";
    case 16: return "General Purpose 1";
    case 17: return "General Purpose 2";
    case 18: return "General Purpose 3";
    case 19: return "General Purpose 4";
    case 32: return "Bank Select LSB";
    case 33: return "Modulation LSB";
    case 34: return "Breath LSB";
    case 36: return "Foot Controller LSB";
    case 37: return "Portamento Time LSB";
    case 38: return "Data Entry LSB";
    case 39: return "Volume LSB";
    case 40: return "Balance LSB";
    case 42: return "Pan LSB";
    case 43: return "Expression LSB";
    case 44: return "Effect Control 1 LSB";
    case 45: return "Effect Control 2 LSB";
    case 48: return "General Purpose 1 LSB";
    case 49: return "General Purpose 2 LSB";
    case 50: return "General Purpose 3 LSB";
    case 51: return "General Purpose 4 LSB";
    case 64: return "Sustain";
    case 65: return "Portamento";
    case 66: return "Sostenuto";
    case 67: return "Soft Pedal";
    case 68: return "Legato";
    case 69: return "Hold 2";
    case 70: return "Sound Variation";
    case 71: return "Resonance";
    case 72: return "Release";
    case 73: return "Attack";
    case 75: return "Decay";
    case 76: return "Vibrato Rate";
    case 77: return "Vibrato Depth";
    case 78: return "Vibrato Delay";
    case 79: return "Sound Control 10";
    case 80: return "General Purpose 5";
    case 81: return "General Purpose 6";
    case 82: return "General Purpose 7";
    case 83: return "General Purpose 8";
    case 84: return "Portamento Control";
    case 88: return "High Resolution Velocity";
    case 91: return "Reverb";
    case 92: return "Tremolo";
    case 93: return "Chorus";
    case 94: return "Celeste";
    case 95: return "Phaser";
    case 96: return "Data Increment";
    case 97: return "Data Decrement";
    case 98: return "NRPN LSB";
    case 99: return "NRPN MSB";
    case 100: return "RPN LSB";
    case 101: return "RPN MSB";
    case 120: return "All Sound Off";
    case 121: return "Reset All Controllers";
    case 122: return "Local Control";
    case 123: return "All Notes Off";
    case 124: return "Omni Off";
    case 125: return "Omni On";
    case 126: return "Mono";
    case 127: return "Poly";
    default: return nullptr;
    }
}

bool blobBit(const MessageReply& reply, int bit) noexcept
{
    const size_t byte = static_cast<size_t>(bit / 8);
    return byte < reply.blobSize
        && (reply.blob[byte] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
}

bool isAssignableCC(int cc) noexcept
{
    // CC74 has its own TIMBRE input. These three channel-mode messages have
    // destructive/reset semantics in sfizz's MIDI path and are not continuous
    // modulation sources even if an unusual SFZ mentions them.
    return cc >= 0 && cc < 128 && cc != 74 && cc != 120 && cc != 121
        && cc != 123;
}

InstrumentMetadata collectMetadata(::sfz::Sfizz& synth)
{
    InstrumentMetadata metadata;

    std::array<std::string, 128> explicitLabels;
    for (const auto& [number, label] : synth.getCCLabels()) {
        if (number < explicitLabels.size())
            explicitLabels[number] = label;
    }

    // This is sfizz's post-parse controller set, so it includes controllers
    // discovered through includes, definitions and normalized legacy opcodes.
    // Limit the public assignment boundary to ordinary MIDI CCs: SourceCC and
    // the Rack expander protocol deliberately carry an 8-bit controller ID.
    const MessageReply usedCCs = query(synth, "/cc/slots");
    for (int number = 0; number < 128; ++number) {
        if (!isAssignableCC(number)
            || (explicitLabels[number].empty() && !blobBit(usedCCs, number)))
            continue;
        std::string label = explicitLabels[number];
        if (label.empty()) {
            if (const char* standard = standardCCLabel(number))
                label = standard;
            else
                label = "CC " + std::to_string(number);
        }
        const MessageReply defaultValue = query(
            synth, "/cc" + std::to_string(number) + "/default");
        metadata.namedControllers.push_back({ static_cast<uint8_t>(number),
            label, std::clamp(defaultValue.number, 0.0f, 1.0f) });
    }

    const MessageReply slots = query(synth, "/sw/last/slots");
    for (int note = 0; note < 128; ++note) {
        const size_t byte = static_cast<size_t>(note / 8);
        const uint8_t mask = static_cast<uint8_t>(1u << (note % 8));
        if (byte >= slots.blobSize || (slots.blob[byte] & mask) == 0)
            continue;
        const MessageReply label = query(synth,
            "/sw/last/" + std::to_string(note) + "/label");
        metadata.latchedKeyswitches.push_back(
            { static_cast<uint8_t>(note), label.text });
    }
    return metadata;
}

} // namespace

SfiziosoEngine::SfiziosoEngine()
    : impl_(std::make_unique<Impl>())
{
    impl_->synth.setNumVoices(VoiceCount);
    impl_->synth.setRack16Enabled(true);
}

SfiziosoEngine::~SfiziosoEngine() = default;

LoadReport SfiziosoEngine::load(const std::string& path)
{
    LoadReport report;
    if (path.empty()) {
        report.message = "No SFZ path was provided";
        return report;
    }

    report.success = impl_->synth.loadSfzFile(path);
    report.regionCount = report.success ? impl_->synth.getNumRegions() : 0;
    report.preloadedSampleCount = report.success
        ? impl_->synth.getNumPreloadedSamples()
        : 0;
    if (report.success) {
        // sfizioso preloads stereo float buffers. Short files use less and
        // sample offsets can use more, so this is deliberately an estimate.
        constexpr size_t bytesPerStereoFrame = 2 * sizeof(float);
        const size_t bytesPerSample = static_cast<size_t>(impl_->synth.getPreloadSize())
            * bytesPerStereoFrame;
        if (bytesPerSample > 0) {
            report.estimatedPreloadedSampleBytes = report.preloadedSampleCount
                    > std::numeric_limits<size_t>::max() / bytesPerSample
                ? std::numeric_limits<size_t>::max()
                : report.preloadedSampleCount * bytesPerSample;
        }
    }
    report.message = report.success ? std::string() : "Could not load SFZ: " + path;
    impl_->metadata = report.success ? collectMetadata(impl_->synth)
                                     : InstrumentMetadata {};
    impl_->stats = { report.success, report.regionCount, report.preloadedSampleCount,
        report.estimatedPreloadedSampleBytes };
    return report;
}

void SfiziosoEngine::setSampleRate(float sampleRate)
{
    if (sampleRate > 0.0f)
        impl_->synth.setSampleRate(sampleRate);
}

void SfiziosoEngine::setMaximumBlockSize(int frames)
{
    if (frames > 0)
        impl_->synth.setSamplesPerBlock(frames);
}

void SfiziosoEngine::setTuningFrequency(float frequency)
{
    if (frequency > 0.0f)
        impl_->synth.setTuningFrequency(frequency);
}

void SfiziosoEngine::enqueue(const TimedEngineEvent& event) noexcept
{
    const int delay = static_cast<int>(event.frameOffset);
    const int channel = std::min<int>(event.channel, 15);
    const int note = std::min<int>(event.note, 127);
    switch (event.type) {
    case EngineEventType::NoteOn:
        impl_->synth.hdNoteOn(delay, channel, note,
            std::clamp(event.value, 0.0f, 1.0f));
        break;
    case EngineEventType::NoteOff:
        impl_->synth.hdNoteOff(delay, channel, note,
            std::clamp(event.value, 0.0f, 1.0f));
        break;
    case EngineEventType::NotePitch:
        impl_->notePitch[channel] = event.value;
        impl_->synth.hdNotePitch(delay, channel,
            impl_->notePitch[channel] + impl_->noteBend[channel]);
        break;
    case EngineEventType::NoteBend:
        impl_->noteBend[channel] = event.value;
        impl_->synth.hdNotePitch(delay, channel,
            impl_->notePitch[channel] + impl_->noteBend[channel]);
        break;
    case EngineEventType::Pressure:
        impl_->synth.hdChannelAftertouch(delay, channel,
            std::clamp(event.value, 0.0f, 1.0f));
        break;
    case EngineEventType::Timbre:
        impl_->synth.hdcc(delay, channel, 74,
            std::clamp(event.value, 0.0f, 1.0f));
        break;
    case EngineEventType::SourceCC:
        impl_->synth.hdcc(delay, channel, note,
            std::clamp(event.value, 0.0f, 1.0f));
        break;
    case EngineEventType::KeyswitchOn:
        impl_->synth.hdNoteOn(delay, channel, note, 1.0f);
        break;
    case EngineEventType::KeyswitchOff:
        impl_->synth.hdNoteOff(delay, channel, note, 0.0f);
        break;
    case EngineEventType::ChokeLaneTails:
        impl_->synth.chokeSourceTails(delay, channel, event.includeHeldVoices);
        break;
    }
}

void SfiziosoEngine::render(float* left, float* right, int frames) noexcept
{
    if (!left || !right || frames <= 0)
        return;
    float* outputs[] { left, right };
    impl_->synth.renderBlock(outputs, static_cast<size_t>(frames));
}

void SfiziosoEngine::renderPolyphonic(float* const* left, float* const* right,
    int channels, int frames) noexcept
{
    if (!left || !right || channels < 1 || channels > MaxAudioOutputLanes
        || frames <= 0)
        return;
    std::array<float*, 2 * MaxAudioOutputLanes> outputs {};
    for (int channel = 0; channel < channels; ++channel) {
        if (!left[channel] || !right[channel])
            return;
        outputs[2 * channel] = left[channel];
        outputs[2 * channel + 1] = right[channel];
    }
    impl_->synth.renderBlockBySourceChannel(outputs.data(),
        static_cast<size_t>(frames), channels);
}

EngineStats SfiziosoEngine::stats() const noexcept
{
    return impl_->stats;
}

int SfiziosoEngine::activeVoiceCount() const noexcept
{
    return impl_->synth.getNumActiveVoices();
}

int SfiziosoEngine::voiceLimit() const noexcept
{
    return impl_->synth.getNumVoices();
}

const InstrumentMetadata& SfiziosoEngine::instrumentMetadata() const noexcept
{
    return impl_->metadata;
}

} // namespace cella::sfz
