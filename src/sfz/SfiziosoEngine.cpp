#include "SfiziosoEngine.hpp"

#include <sfizz.hpp>

#include <algorithm>
#include <limits>

namespace cella::sfz {

struct SfiziosoEngine::Impl {
    ::sfz::Sfizz synth;
    EngineStats stats;
};

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
        impl_->synth.hdNotePitch(delay, channel, event.value);
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

EngineStats SfiziosoEngine::stats() const noexcept
{
    return impl_->stats;
}

} // namespace cella::sfz
