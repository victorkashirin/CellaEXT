#pragma once

#include "SamplerEngine.hpp"

#include <memory>

namespace cella::sfz {

class SfiziosoEngine final : public SamplerEngine {
public:
    static constexpr int VoiceCount = 128;

    SfiziosoEngine();
    ~SfiziosoEngine() override;

    SfiziosoEngine(const SfiziosoEngine&) = delete;
    SfiziosoEngine& operator=(const SfiziosoEngine&) = delete;

    LoadReport load(const std::string& path) override;
    void setSampleRate(float sampleRate) override;
    void setMaximumBlockSize(int frames) override;
    void setTuningFrequency(float frequency) override;
    void enqueue(const TimedEngineEvent& event) noexcept override;
    void render(float* left, float* right, int frames) noexcept override;
    EngineStats stats() const noexcept override;
    int activeVoiceCount() const noexcept override;
    int voiceLimit() const noexcept override;
    const InstrumentMetadata& instrumentMetadata() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cella::sfz
