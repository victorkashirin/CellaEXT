#include "sfz/BlockAdapter.hpp"
#include "sfz/NoteLaneTracker.hpp"
#include "sfz/SfiziosoEngine.hpp"
#include "sfizz/ExpressionContext.h"
#include <sfizz.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace cella::sfz;

namespace {

constexpr int SampleRate = 48000;
constexpr double Pi = 3.14159265358979323846;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(float actual, float expected, float tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message + " (actual " + std::to_string(actual)
            + ", expected " + std::to_string(expected) + ")");
    }
}

void writeU16(std::ostream& out, uint16_t value)
{
    const char bytes[] = { static_cast<char>(value), static_cast<char>(value >> 8) };
    out.write(bytes, sizeof(bytes));
}

void writeU32(std::ostream& out, uint32_t value)
{
    const char bytes[] = { static_cast<char>(value), static_cast<char>(value >> 8),
        static_cast<char>(value >> 16), static_cast<char>(value >> 24) };
    out.write(bytes, sizeof(bytes));
}

void writePcm16Wav(const fs::path& path, int channels,
    const std::vector<int16_t>& interleaved)
{
    require(channels == 1 || channels == 2, "fixture channel count");
    require(interleaved.size() % static_cast<size_t>(channels) == 0,
        "fixture interleaving");
    std::ofstream out(path, std::ios::binary);
    require(static_cast<bool>(out), "create WAV fixture");
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * 2);
    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVEfmt ", 8);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, static_cast<uint16_t>(channels));
    writeU32(out, SampleRate);
    writeU32(out, SampleRate * channels * 2);
    writeU16(out, static_cast<uint16_t>(channels * 2));
    writeU16(out, 16);
    out.write("data", 4);
    writeU32(out, dataBytes);
    for (int16_t value : interleaved)
        writeU16(out, static_cast<uint16_t>(value));
}

struct Fixtures {
    fs::path directory;
    fs::path monoSfz;
    fs::path stereoSfz;
    fs::path layeredReleaseSfz;

    Fixtures()
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        directory = fs::temp_directory_path()
            / ("cella-sfz-core-" + std::to_string(stamp));
        fs::create_directories(directory);

        std::vector<int16_t> mono(SampleRate);
        for (size_t i = 0; i < mono.size(); ++i) {
            mono[i] = static_cast<int16_t>(12000.0
                * std::sin(2.0 * Pi * 220.0 * i / SampleRate));
        }
        writePcm16Wav(directory / "mono.wav", 1, mono);

        std::vector<int16_t> stereo(2048 * 2, 0);
        stereo[256 * 2] = 24000;
        stereo[1024 * 2 + 1] = -18000;
        writePcm16Wav(directory / "stereo.wav", 2, stereo);

        monoSfz = directory / "mono.sfz";
        std::ofstream monoDocument(monoSfz);
        monoDocument
            << "<global> ampeg_attack=0 ampeg_decay=0 ampeg_sustain=100 ampeg_release=0.25\n"
               "<region> sample=mono.wav key=60 pitch_keycenter=60 "
               "loop_mode=loop_continuous loop_start=0 loop_end=47999\n";
        require(static_cast<bool>(monoDocument), "create mono SFZ fixture");
        monoDocument.close();

        stereoSfz = directory / "stereo.sfz";
        std::ofstream stereoDocument(stereoSfz);
        stereoDocument
            << "<global> ampeg_attack=0 ampeg_decay=0 ampeg_sustain=100 ampeg_release=0\n"
               "<region> sample=stereo.wav key=60 pitch_keycenter=60\n";
        require(static_cast<bool>(stereoDocument), "create stereo SFZ fixture");

        layeredReleaseSfz = directory / "layered-release.sfz";
        std::ofstream layeredReleaseDocument(layeredReleaseSfz);
        layeredReleaseDocument
            << "<global> key=60 pitch_keycenter=60 ampeg_attack=0 "
               "ampeg_decay=0 ampeg_sustain=100 ampeg_release=0.5\n"
               "<group> trigger=attack loop_mode=loop_continuous "
               "loop_start=0 loop_end=47999\n"
               "<region> sample=mono.wav\n"
               "<region> sample=mono.wav delay=0.01 tune=700\n"
               "<group> trigger=release\n"
               "<region> sample=mono.wav loop_mode=one_shot tune=-500\n";
        require(static_cast<bool>(layeredReleaseDocument),
            "create layered release-trigger SFZ fixture");
    }

    ~Fixtures()
    {
        std::error_code error;
        fs::remove_all(directory, error);
    }
};

class RecordingEngine final : public SamplerEngine {
public:
    LoadReport load(const std::string&) override { return { true, 1, 1, 65536, {} }; }
    void setSampleRate(float) override { }
    void setMaximumBlockSize(int) override { }
    void setTuningFrequency(float) override { }
    void enqueue(const TimedEngineEvent& event) noexcept override { events.push_back(event); }
    void render(float* left, float* right, int frames) noexcept override
    {
        ++renderCalls;
        for (int i = 0; i < frames; ++i)
            left[i] = right[i] = 0.0f;
    }
    EngineStats stats() const noexcept override { return { true, 1, 1, 65536 }; }

    std::vector<TimedEngineEvent> events;
    int renderCalls { 0 };
};

void testConversions()
{
    auto pitch = decomposePitch(0.0f, 0);
    require(pitch.baseNote == 60, "0 V maps to MIDI 60");
    requireNear(pitch.noteOffsetSemitones, 0.0f, 1.0e-6f, "0 V offset");

    pitch = decomposePitch(1.0f / 24.0f, 0);
    require(pitch.baseNote == 61, "half-semitone pitch rounds up");
    requireNear(pitch.noteOffsetSemitones, -0.5f, 1.0e-5f,
        "fractional remainder follows rounded note");

    pitch = decomposePitch(-10.0f, 0);
    require(pitch.baseNote == 0, "low pitch clamps base note");
    requireNear(pitch.noteOffsetSemitones, -60.0f, 1.0e-5f,
        "clamped pitch retains continuous offset");

    pitch = decomposePitch(0.25f, 2);
    require(pitch.baseNote == 87, "octave and voltage conversion");
    requireNear(velocityFromVoltage(-1.0f), 0.0f, 1.0e-6f, "velocity low clamp");
    requireNear(velocityFromVoltage(7.5f), 0.75f, 1.0e-6f, "velocity scaling");
    requireNear(velocityFromVoltage(12.0f), 1.0f, 1.0e-6f, "velocity high clamp");
    require(decomposePitch(std::numeric_limits<float>::quiet_NaN(), 0).baseNote == 60,
        "non-finite pitch is sanitized");
    requireNear(velocityFromVoltage(std::numeric_limits<float>::infinity()),
        0.0f, 1.0e-6f, "non-finite velocity is sanitized");
}

void testEventOrdering()
{
    RecordingEngine engine;
    BlockAdapter adapter(engine);
    adapter.push(TimedEngineEvent::noteOn(7, 0, 60, 0.8f));
    adapter.push(TimedEngineEvent::notePitch(2, 0, 60, 1.0f));
    adapter.push(TimedEngineEvent::noteOff(7, 0, 60));
    adapter.push(TimedEngineEvent::noteOn(32, 1, 61, 0.8f));
    std::array<float, 16> left {}, right {};
    adapter.render(left.data(), right.data(), 16);
    require(engine.renderCalls == 1, "adapter renders once per block");
    require(engine.events.size() == 3, "out-of-range event rejected");
    require(engine.events[0].frameOffset == 2, "events sorted by offset");
    require(engine.events[1].type == EngineEventType::NoteOn
            && engine.events[2].type == EngineEventType::NoteOff,
        "same-offset insertion order is stable");
    require(adapter.eventCount() == 0, "adapter clears rendered events");
    require(adapter.droppedEventCount() == 1, "adapter reports invalid event");
}

void testLaneTracking()
{
    NoteLaneTracker tracker;
    std::array<TimedEngineEvent, 64> events {};
    EventWriter writer { events.data(), events.size() };

    float pitches[] { 0.0f, 4.0f / 12.0f, 7.0f / 12.0f };
    float gate = 10.0f;
    float velocity = 5.0f;
    tracker.process({ pitches, 3, &gate, 1, &velocity, 1, 0, 0 }, writer);
    require(writer.count == 6, "three gate rises emit note and pitch events");
    require(tracker.laneCount() == 3, "pitch channels define lane count");
    for (size_t lane = 0; lane < 3; ++lane) {
        require(events[lane * 2].type == EngineEventType::NoteOn,
            "note precedes initial pitch");
        require(events[lane * 2].channel == lane, "lane maps to source channel");
        requireNear(events[lane * 2].value, lane == 0 ? 0.5f : 0.0f, 1.0e-6f,
            "velocity channels map lane-for-lane without mono broadcast");
    }

    writer.count = 0;
    float movedPitch = 1.1f;
    tracker.process({ &movedPitch, 1, &gate, 1, nullptr, 0, 0, 4 }, writer);
    require(writer.count == 3, "shrink releases two lanes and bends survivor");
    require(events[0].type == EngineEventType::NoteOff && events[0].channel == 1
            && events[0].note == 64,
        "shrink releases exact lane-one note");
    require(events[1].type == EngineEventType::NoteOff && events[1].channel == 2
            && events[1].note == 67,
        "shrink releases exact lane-two note");
    require(events[2].type == EngineEventType::NotePitch && events[2].channel == 0,
        "held pitch bends without note-on");
    requireNear(events[2].value, 13.2f, 1.0e-4f,
        "held bend remains relative to gate-edge note across semitones");
    require(tracker.activeNote(0) == 60, "held base note never changes");

    writer.count = 0;
    float jitteredPitch = movedPitch
        + NoteLaneTracker::PitchChangeThresholdSemitones / 24.0f;
    tracker.process({ &jitteredPitch, 1, &gate, 1, nullptr, 0, 0, 5 }, writer);
    require(writer.count == 0,
        "sub-threshold pitch jitter does not emit a redundant event");

    jitteredPitch = movedPitch
        + NoteLaneTracker::PitchChangeThresholdSemitones / 12.0f;
    tracker.process({ &jitteredPitch, 1, &gate, 1, nullptr, 0, 0, 6 }, writer);
    require(writer.count == 1 && events[0].type == EngineEventType::NotePitch,
        "accumulated pitch movement emits at the deadband boundary");

    writer.count = 0;
    float gateOff = 0.0f;
    tracker.process({ &movedPitch, 1, &gateOff, 1, nullptr, 0, 0, 8 }, writer);
    require(writer.count == 1 && events[0].type == EngineEventType::NoteOff
            && events[0].note == 60,
        "note-off uses gate-edge note after pitch travel");

    writer.count = 0;
    float silentPitch = -1.0f;
    tracker.process({ &silentPitch, 1, &gateOff, 1, nullptr, 0, 0, 9 }, writer);
    require(writer.count == 0, "released lane emits no expression into its tail");

    writer.count = 0;
    tracker.process({ &silentPitch, 1, &gate, 1, nullptr, 0, 0, 10 }, writer);
    require(writer.count == 2 && events[0].type == EngineEventType::NoteOn
            && events[0].note == 48,
        "lane retriggers a newly decomposed note");
    requireNear(events[0].value, 0.8f, 1.0e-6f,
        "unpatched velocity defaults to 0.8");

    writer.count = 0;
    const float invalidGate = std::numeric_limits<float>::quiet_NaN();
    tracker.process(
        { &silentPitch, 1, &invalidGate, 1, nullptr, 0, 0, 11 }, writer);
    require(writer.count == 1 && events[0].type == EngineEventType::NoteOff
            && events[0].note == 48,
        "non-finite gate voltage releases rather than sticking a note");
}

void testBoundedEventFailure()
{
    NoteLaneTracker tracker;
    std::array<TimedEngineEvent, 2> events {};
    float pitch = 0.0f;
    float gate = 10.0f;

    EventWriter shortWriter { events.data(), 1 };
    tracker.process({ &pitch, 1, &gate, 1, nullptr, 0, 0, 0 }, shortWriter);
    require(shortWriter.count == 0 && shortWriter.dropped == 2,
        "undersized writer rejects the complete input frame");
    require(!tracker.noteActive(0), "rejected note-on does not mutate lane state");

    EventWriter writer { events.data(), events.size() };
    tracker.process({ &pitch, 1, &gate, 1, nullptr, 0, 0, 1 }, writer);
    require(writer.count == 2 && tracker.noteActive(0),
        "preserved gate edge retries when capacity is available");

    float gateOff = 0.0f;
    EventWriter noRoom { events.data(), 0 };
    tracker.process({ &pitch, 1, &gateOff, 1, nullptr, 0, 0, 2 }, noRoom);
    require(noRoom.count == 0 && noRoom.dropped == 1 && tracker.noteActive(0),
        "rejected note-off preserves active state");

    EventWriter release { events.data(), 1 };
    tracker.process({ &pitch, 1, &gateOff, 1, nullptr, 0, 0, 3 }, release);
    require(release.count == 1 && !tracker.noteActive(0),
        "preserved note-off retries when capacity is available");

    writer = { events.data(), events.size() };
    tracker.process({ &pitch, 1, &gate, 1, nullptr, 0, 0, 4 }, writer);
    EventWriter resetNoRoom { events.data(), 0 };
    require(!tracker.reset(&resetNoRoom, 5) && tracker.noteActive(0),
        "failed reset preserves active notes");
    EventWriter resetWriter { events.data(), 1 };
    require(tracker.reset(&resetWriter, 6) && resetWriter.count == 1
            && !tracker.noteActive(0),
        "reset succeeds atomically with sufficient capacity");
}

void testBlockAdapterBounds()
{
    RecordingEngine engine;
    BlockAdapter adapter(engine);
    for (size_t i = 0; i < BlockAdapter::MaxEvents; ++i) {
        require(adapter.push(TimedEngineEvent::notePitch(0, 0, 60, 0.0f)),
            "worst-case event capacity is accepted");
    }
    require(!adapter.push(TimedEngineEvent::notePitch(0, 0, 60, 0.0f)),
        "events beyond the fixed bound are rejected");
    adapter.clear();

    std::array<float, BlockAdapter::MaxFrames + 1> left, right;
    left.fill(1.0f);
    right.fill(1.0f);
    adapter.push(TimedEngineEvent::noteOn(0, 0, 60, 0.8f));
    require(!adapter.render(left.data(), right.data(), BlockAdapter::MaxFrames + 1)
            && engine.renderCalls == 0 && adapter.eventCount() == 0,
        "oversized render quantum is rejected");
    require(std::all_of(left.begin(), left.end(), [](float value) { return value == 1.0f; })
            && std::all_of(right.begin(), right.end(), [](float value) { return value == 1.0f; }),
        "rejected render quantum does not touch unknown-size output buffers");
}

void testIdleExpressionFlush()
{
    sfz::ExpressionContext context;
    context.configure(/*controllerSlots=*/0, /*polyPressureSlots=*/1,
        /*eventsPerTimeline=*/8);
    require(!context.hasPendingEvents(), "configured expression context starts clean");

    context.flushEvents();
    require(!context.hasPendingEvents(), "idle flush leaves expression context clean");

    require(context.controllerEvent(3, 11, 0.25f),
        "unused controller retains scalar state");
    require(!context.hasPendingEvents(),
        "scalar-only controller does not schedule timeline flush work");

    std::array<bool, sfz::config::numCCs> used {};
    used[74] = true;
    context.configureSfizzControllers(used);
    require(context.controllerEvent(4, 74, 0.5f)
            && context.pressureEvent(6, 0.75f),
        "sample-accurate expression events are accepted");
    require(context.hasPendingEvents(), "timeline writes mark expression context dirty");

    context.flushEvents();
    const sfz::EventVector* controllerEvents = context.controllerEvents(74);
    require(!context.hasPendingEvents(), "flush clears expression dirty state");
    require(controllerEvents != nullptr && controllerEvents->size() == 1
            && controllerEvents->front().delay == 0
            && controllerEvents->front().value == 0.5f,
        "controller timeline collapses to its final value");
    require(context.pressureEvents().size() == 1
            && context.pressureEvents().front().delay == 0
            && context.pressureEvents().front().value == 0.75f,
        "pressure timeline collapses to its final value");

    context.reset();
    require(!context.hasPendingEvents(), "reset leaves expression context clean");
}

void testDuplicateNotes()
{
    NoteLaneTracker tracker;
    std::array<TimedEngineEvent, 16> events {};
    EventWriter writer { events.data(), events.size() };
    float pitches[] { 0.0f, 0.0f };
    float gates[] { 10.0f, 10.0f };
    tracker.process({ pitches, 2, gates, 2, nullptr, 0, 0, 0 }, writer);
    require(writer.count == 4, "duplicate notes start independently");
    require(events[0].note == 60 && events[2].note == 60
            && events[0].channel == 0 && events[2].channel == 1,
        "duplicate notes retain distinct source channels");

    writer.count = 0;
    gates[0] = 0.0f;
    tracker.process({ pitches, 2, gates, 2, nullptr, 0, 0, 1 }, writer);
    require(writer.count == 1 && events[0].type == EngineEventType::NoteOff
            && events[0].channel == 0,
        "one duplicate releases without releasing the other");
    require(!tracker.noteActive(0) && tracker.noteActive(1),
        "duplicate lane state remains independent");
}

std::vector<std::array<float, 2>> renderBlocks(SamplerEngine& engine,
    int frames, const std::vector<TimedEngineEvent>& initialEvents = {},
    int quantum = 64)
{
    BlockAdapter adapter(engine);
    std::vector<std::array<float, 2>> audio(static_cast<size_t>(frames));
    int rendered = 0;
    bool first = true;
    while (rendered < frames) {
        const int block = std::min(quantum, frames - rendered);
        if (first) {
            for (const TimedEngineEvent& event : initialEvents)
                adapter.push(event);
            first = false;
        }
        std::array<float, BlockAdapter::MaxFrames> left {}, right {};
        adapter.render(left.data(), right.data(), block);
        for (int i = 0; i < block; ++i)
            audio[static_cast<size_t>(rendered + i)] = { left[i], right[i] };
        rendered += block;
    }
    return audio;
}

double toneMagnitude(const std::vector<std::array<float, 2>>& audio,
    size_t offset, size_t frames, double frequency)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * Pi * frequency * i / SampleRate;
        const double window = 0.5 - 0.5 * std::cos(2.0 * Pi * i / (frames - 1));
        real += audio[offset + i][0] * window * std::cos(phase);
        imaginary -= audio[offset + i][0] * window * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / frames;
}

void configure(SfiziosoEngine& engine)
{
    engine.setSampleRate(SampleRate);
    engine.setMaximumBlockSize(64);
}

void testGeneratedFixtures(const Fixtures& fixtures)
{
    SfiziosoEngine mono;
    configure(mono);
    const LoadReport monoReport = mono.load(fixtures.monoSfz.string());
    require(monoReport.success && monoReport.regionCount == 1,
        "generated mono fixture loads");
    require(monoReport.estimatedPreloadedSampleBytes > 0
            && mono.stats().estimatedPreloadedSampleBytes
                == monoReport.estimatedPreloadedSampleBytes,
        "load report and engine stats expose sample-memory estimate");
    require(SfiziosoEngine::VoiceCount == 128,
        "engine wrapper retains the validated voice capacity");
    auto monoAudio = renderBlocks(mono, 1024,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 0.0f) });
    float monoPeak = 0.0f;
    float monoDifference = 0.0f;
    for (const auto& frame : monoAudio) {
        monoPeak = std::max(monoPeak, std::abs(frame[0]));
        monoDifference = std::max(monoDifference, std::abs(frame[0] - frame[1]));
    }
    require(monoPeak > 0.01f, "mono fixture produces audio");
    require(monoDifference < 1.0e-5f, "mono sample feeds both stereo outputs equally");

    SfiziosoEngine stereo;
    configure(stereo);
    const LoadReport stereoReport = stereo.load(fixtures.stereoSfz.string());
    require(stereoReport.success && stereoReport.regionCount == 1,
        "generated stereo fixture loads");
    auto stereoAudio = renderBlocks(stereo, 1800,
        { TimedEngineEvent::noteOn(0, 0, 60, 1.0f),
            TimedEngineEvent::notePitch(0, 0, 60, 0.0f) });
    size_t leftPeakIndex = 0;
    size_t rightPeakIndex = 0;
    for (size_t i = 1; i < stereoAudio.size(); ++i) {
        if (std::abs(stereoAudio[i][0]) > std::abs(stereoAudio[leftPeakIndex][0]))
            leftPeakIndex = i;
        if (std::abs(stereoAudio[i][1]) > std::abs(stereoAudio[rightPeakIndex][1]))
            rightPeakIndex = i;
    }
    require(std::abs(stereoAudio[leftPeakIndex][0]) > 0.01f
            && std::abs(stereoAudio[rightPeakIndex][1]) > 0.01f,
        "stereo impulses reach their outputs");
    require(leftPeakIndex + 500 < rightPeakIndex,
        "different left/right impulses retain timing separation");
    require(std::abs(stereoAudio[leftPeakIndex][1]) < 1.0e-5f,
        "left-only impulse is absent from right");
    require(std::abs(stereoAudio[rightPeakIndex][0]) < 1.0e-5f,
        "right-only impulse is absent from left");
    require(stereoAudio[leftPeakIndex][0] > 0.0f
            && stereoAudio[rightPeakIndex][1] < 0.0f,
        "stereo impulse polarity is preserved");
}

void testEngineNoteIdentity(const Fixtures& fixtures)
{
    SfiziosoEngine duplicate;
    configure(duplicate);
    require(duplicate.load(fixtures.monoSfz.string()).success,
        "identity fixture loads");
    auto both = renderBlocks(duplicate, 16384,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 12.0f),
            TimedEngineEvent::noteOn(0, 1, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 1, 60, -12.0f) });
    const size_t window = 8192;
    const size_t offset = both.size() - window;
    require(toneMagnitude(both, offset, window, 440.0) > 0.001
            && toneMagnitude(both, offset, window, 110.0) > 0.001,
        "same-note lanes bend independently through engine boundary");

    auto survivor = renderBlocks(duplicate, SampleRate / 2,
        { TimedEngineEvent::noteOff(0, 0, 60) });
    const size_t survivorOffset = survivor.size() - window;
    require(toneMagnitude(survivor, survivorOffset, window, 440.0) < 0.0002
            && toneMagnitude(survivor, survivorOffset, window, 110.0) > 0.001,
        "channel-aware note-off releases only its duplicate");

    SfiziosoEngine reuse;
    configure(reuse);
    require(reuse.load(fixtures.monoSfz.string()).success,
        "release-tail fixture loads");
    renderBlocks(reuse, SampleRate / 8,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 12.0f) });
    auto reused = renderBlocks(reuse, 8192,
        { TimedEngineEvent::noteOff(0, 0, 60),
            TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, -12.0f) });
    require(toneMagnitude(reused, 1024, 4096, 440.0) > 0.001
            && toneMagnitude(reused, 1024, 4096, 110.0) > 0.001,
        "lane reuse leaves the old release tail at its pitch snapshot");
}

void testEngineTailPolicies(const Fixtures& fixtures)
{
    constexpr size_t AnalysisOffset = 1024;
    constexpr size_t AnalysisFrames = 4096;

    SfiziosoEngine cut;
    configure(cut);
    require(cut.load(fixtures.monoSfz.string()).success,
        "cut-tail fixture loads");
    renderBlocks(cut, SampleRate / 8,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 12.0f) });
    auto cutAudio = renderBlocks(cut, 8192,
        { TimedEngineEvent::noteOff(0, 0, 60),
            TimedEngineEvent::chokeLaneTails(0, 0, true),
            TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, -12.0f) });
    require(toneMagnitude(cutAudio, AnalysisOffset, AnalysisFrames, 440.0)
                < 0.0002
            && toneMagnitude(cutAudio, AnalysisOffset, AnalysisFrames, 110.0)
                > 0.001,
        "cut policy fast-releases the old generation before retrigger");

    SfiziosoEngine keepNewest;
    configure(keepNewest);
    require(keepNewest.load(fixtures.monoSfz.string()).success,
        "keep-newest fixture loads");
    renderBlocks(keepNewest, SampleRate / 8,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 12.0f) });
    renderBlocks(keepNewest, 2048,
        { TimedEngineEvent::chokeLaneTails(0, 0, false),
            TimedEngineEvent::noteOff(0, 0, 60),
            TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 0.0f) });
    auto keptAudio = renderBlocks(keepNewest, 8192,
        { TimedEngineEvent::chokeLaneTails(0, 0, false),
            TimedEngineEvent::noteOff(0, 0, 60),
            TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, -12.0f) });
    require(toneMagnitude(keptAudio, AnalysisOffset, AnalysisFrames, 440.0)
                < 0.0002,
        "keep-newest policy removes generations older than the newest tail");
    require(toneMagnitude(keptAudio, AnalysisOffset, AnalysisFrames, 220.0)
                > 0.001
            && toneMagnitude(keptAudio, AnalysisOffset, AnalysisFrames, 110.0)
                > 0.001,
        "keep-newest policy retains the newest tail and the retriggered note");
}

void renderSfizz(::sfz::Sfizz& synth, int frames)
{
    std::array<float, BlockAdapter::MaxFrames> left {};
    std::array<float, BlockAdapter::MaxFrames> right {};
    float* outputs[] { left.data(), right.data() };
    while (frames > 0) {
        const int block = std::min(frames, BlockAdapter::MaxFrames);
        synth.renderBlock(outputs, static_cast<size_t>(block));
        frames -= block;
    }
}

void testLayeredReleaseTailOwnership(const Fixtures& fixtures)
{
    ::sfz::Sfizz synth;
    synth.setSampleRate(SampleRate);
    synth.setSamplesPerBlock(BlockAdapter::MaxFrames);
    synth.setNumVoices(SfiziosoEngine::VoiceCount);
    synth.setRack16Enabled(true);
    require(synth.loadSfzFile(fixtures.layeredReleaseSfz.string()),
        "layered release-tail fixture loads");

    synth.noteOn(0, 0, 60, 100);
    renderSfizz(synth, 1024);
    require(synth.getNumActiveVoices() == 2,
        "one note generation includes its immediate and delayed attack layers");

    synth.noteOff(0, 0, 60, 0);
    renderSfizz(synth, 1024);
    require(synth.getNumActiveVoices() == 3,
        "released generation includes both attack tails and its release trigger");

    synth.noteOn(0, 0, 60, 100);
    renderSfizz(synth, 1024);
    require(synth.getNumActiveVoices() == 5,
        "retrigger adds a complete layered held generation");

    // Keep-newest is dispatched immediately before the current Note Off. It
    // must remove the complete previous generation while leaving every held
    // layer available to become the newest tail.
    synth.chokeSourceTails(0, 0, false);
    synth.noteOff(0, 0, 60, 0);
    renderSfizz(synth, 4096);
    require(synth.getNumActiveVoices() == 3,
        "keep-newest retains all layers and the release trigger of one generation "
        "(actual " + std::to_string(synth.getNumActiveVoices()) + ")");

    synth.noteOn(0, 1, 60, 100);
    renderSfizz(synth, 1024);
    synth.noteOff(0, 1, 60, 0);
    renderSfizz(synth, 1024);
    require(synth.getNumActiveVoices() == 6,
        "a second source owns an independent released generation");

    synth.chokeSourceTails(0, 0, true);
    renderSfizz(synth, 4096);
    require(synth.getNumActiveVoices() == 3,
        "choking one source leaves every tail owned by another source intact");
}

void testQuantumAudioStability(const Fixtures& fixtures)
{
    constexpr int Frames = 8192;
    const std::vector<TimedEngineEvent> events {
        TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
        TimedEngineEvent::notePitch(0, 0, 60, 5.25f),
    };

    std::array<std::vector<std::array<float, 2>>, 3> renders;
    constexpr std::array<int, 3> quanta { 16, 32, 64 };
    for (size_t index = 0; index < quanta.size(); ++index) {
        SfiziosoEngine engine;
        engine.setSampleRate(SampleRate);
        engine.setMaximumBlockSize(BlockAdapter::MaxFrames);
        require(engine.load(fixtures.monoSfz.string()).success,
            "quantum comparison fixture loads");
        renders[index] = renderBlocks(engine, Frames, events, quanta[index]);
    }

    for (size_t index = 1; index < renders.size(); ++index) {
        double squaredSignal = 0.0;
        double squaredDifference = 0.0;
        double maximumDifference = 0.0;
        for (size_t frame = 0; frame < renders[0].size(); ++frame) {
            for (size_t channel = 0; channel < 2; ++channel) {
                const double reference = renders[0][frame][channel];
                const double difference = renders[index][frame][channel] - reference;
                squaredSignal += reference * reference;
                squaredDifference += difference * difference;
                maximumDifference = std::max(maximumDifference,
                    std::abs(difference));
            }
        }
        const double relativeRms = std::sqrt(squaredDifference / squaredSignal);
        require(relativeRms < 0.01 && maximumDifference < 0.01,
            "render quantum keeps block-dependent sample differences small");
    }

    SfiziosoEngine live;
    configure(live);
    require(live.load(fixtures.monoSfz.string()).success,
        "live quantum-transition fixture loads");
    renderBlocks(live, 2048,
        { TimedEngineEvent::noteOn(0, 0, 60, 0.8f),
            TimedEngineEvent::notePitch(0, 0, 60, 5.25f) },
        64);
    const auto sixteenFrameContinuation = renderBlocks(live, 4096, {}, 16);
    const auto thirtyTwoFrameContinuation = renderBlocks(live, 4096, {}, 32);
    const double continuedFrequency = 220.0 * std::pow(2.0, 5.25 / 12.0);
    require(toneMagnitude(sixteenFrameContinuation, 0,
                sixteenFrameContinuation.size(), continuedFrequency)
                > 0.001
            && toneMagnitude(thirtyTwoFrameContinuation, 0,
                   thirtyTwoFrameContinuation.size(), continuedFrequency)
                > 0.001,
        "changing actual render size in place preserves the sounding voice");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> unitTests {
        { "voltage conversion", testConversions },
        { "event ordering", testEventOrdering },
        { "lane tracking, shrink, bend, release, retrigger", testLaneTracking },
        { "duplicate note lanes", testDuplicateNotes },
        { "bounded event failure", testBoundedEventFailure },
        { "block adapter bounds", testBlockAdapterBounds },
        { "idle expression timeline flushing", testIdleExpressionFlush },
    };

    int failures = 0;
    for (const auto& test : unitTests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }

    try {
        Fixtures fixtures;
        testGeneratedFixtures(fixtures);
        std::cout << "PASS: generated mono/stereo fixtures and stereo routing\n";
        testEngineNoteIdentity(fixtures);
        std::cout << "PASS: engine duplicate/release-tail identity\n";
        testEngineTailPolicies(fixtures);
        std::cout << "PASS: engine release-tail policies\n";
        testLayeredReleaseTailOwnership(fixtures);
        std::cout << "PASS: delayed/layered/release-trigger tail ownership\n";
        testQuantumAudioStability(fixtures);
        std::cout << "PASS: render-quantum audio stability\n";
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: generated fixture integration: " << error.what() << '\n';
    }

    std::cout << (failures == 0 ? "All SFZ core tests passed\n"
                                : std::to_string(failures) + " SFZ core test(s) failed\n");
    return failures == 0 ? 0 : 1;
}
