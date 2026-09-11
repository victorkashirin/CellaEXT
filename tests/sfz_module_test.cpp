#include "../src/SFZ.cpp"

#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <unistd.h>

Plugin* pluginInstance = nullptr;

namespace {

struct BlockingLoadState {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered { false };
    bool release { false };
    bool destructorReturned { false };
    std::atomic<int> destroyed { 0 };
};

struct BlockingEngine final : cella::sfz::SamplerEngine {
    explicit BlockingEngine(std::shared_ptr<BlockingLoadState> state)
        : state(std::move(state))
    {
    }

    ~BlockingEngine() override
    {
        state->destroyed.fetch_add(1, std::memory_order_release);
    }

    cella::sfz::LoadReport load(const std::string&) override
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->entered = true;
        state->cv.notify_all();
        state->cv.wait(lock, [this]() { return state->release; });
        cella::sfz::LoadReport report;
        report.success = true;
        report.regionCount = 1;
        return report;
    }

    void setSampleRate(float) override { }
    void setMaximumBlockSize(int) override { }
    void setTuningFrequency(float) override { }
    void enqueue(const cella::sfz::TimedEngineEvent&) noexcept override { }
    void render(float* left, float* right, int frames) noexcept override
    {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
    }
    cella::sfz::EngineStats stats() const noexcept override { return {}; }

    std::shared_ptr<BlockingLoadState> state;
};

struct RecordingEngineState {
    std::mutex mutex;
    std::vector<int> maximumBlockSizes;
    std::vector<int> renderedBlockSizes;
    std::vector<cella::sfz::TimedEngineEvent> events;
};

struct RecordingEngine final : cella::sfz::SamplerEngine {
    explicit RecordingEngine(std::shared_ptr<RecordingEngineState> state)
        : state(std::move(state))
    {
    }

    cella::sfz::LoadReport load(const std::string&) override
    {
        cella::sfz::LoadReport report;
        report.success = true;
        report.regionCount = 1;
        return report;
    }
    void setSampleRate(float) override { }
    void setMaximumBlockSize(int frames) override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->maximumBlockSizes.push_back(frames);
    }
    void setTuningFrequency(float) override { }
    void enqueue(const cella::sfz::TimedEngineEvent& event) noexcept override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->events.push_back(event);
    }
    void render(float* left, float* right, int frames) noexcept override
    {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->renderedBlockSizes.push_back(frames);
    }
    cella::sfz::EngineStats stats() const noexcept override { return {}; }

    std::shared_ptr<RecordingEngineState> state;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(float actual, float expected, float tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
    }
}

Module::ProcessArgs processArgs()
{
    return { 48000.0f, 1.0f / 48000.0f, 0 };
}

void waitForState(CellaSFZ& module, CellaSFZ::StatusState expected,
    const std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (module.statusSnapshot()->state != expected) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void waitForRetirement(CellaSFZ& module, uint64_t previousCount,
    const std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (module.loader_->retiredEngineCount.load(std::memory_order_acquire)
        == previousCount) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void seedPlayback(CellaSFZ& module, float left, float right)
{
    module.playbackLeft_.fill(0.0f);
    module.playbackRight_.fill(0.0f);
    module.playbackLeft_[0] = left;
    module.playbackRight_[0] = right;
    module.playbackFrame_ = 0;
}

void testLexicalPathRoots()
{
    const LexicalPath first = splitPath(R"(\\server-a\samples\Cello\main.sfz)");
    const LexicalPath same = splitPath(R"(\\SERVER-A\SAMPLES\Piano\main.sfz)");
    const LexicalPath other = splitPath(R"(\\server-b\samples\Cello\main.sfz)");
    require(first.root == "//server-a/samples", "UNC root includes server and share");
    require(first.root == same.root, "UNC root comparison is case-insensitive");
    require(first.root != other.root, "different UNC servers cannot share a relative path");
}

void testPermanentIdsAndDefaults()
{
    require(kDefaultRenderQuantum == 32,
        "SFZ module defaults to the selected 32-frame quantum");
    require(kMaximumRenderQuantum == cella::sfz::BlockAdapter::MaxFrames,
        "module buffers cover every selectable render quantum");
    static_assert(CellaSFZ::LEVEL_PARAM == 0);
    static_assert(CellaSFZ::OCTAVE_PARAM == 1);
    static_assert(CellaSFZ::TUNE_PARAM == 2);
    static_assert(CellaSFZ::LOAD_PARAM == 3);
    static_assert(CellaSFZ::PREVIOUS_PARAM == 4);
    static_assert(CellaSFZ::NEXT_PARAM == 5);
    static_assert(CellaSFZ::VOCT_INPUT == 0);
    static_assert(CellaSFZ::GATE_INPUT == 1);
    static_assert(CellaSFZ::VELOCITY_INPUT == 2);
    static_assert(CellaSFZ::LEFT_OUTPUT == 0);
    static_assert(CellaSFZ::RIGHT_OUTPUT == 1);

    CellaSFZ module;
    requireNear(module.params[CellaSFZ::LEVEL_PARAM].getValue(), 0.8f, 1.0e-6f,
        "LEVEL default");
    requireNear(module.params[CellaSFZ::OCTAVE_PARAM].getValue(), 0.0f, 1.0e-6f,
        "OCTAVE default");
    requireNear(module.params[CellaSFZ::TUNE_PARAM].getValue(), 0.0f, 1.0e-6f,
        "TUNE default");
    requireNear(module.params[CellaSFZ::PREVIOUS_PARAM].getValue(), 0.0f,
        1.0e-6f, "PREVIOUS default");
    requireNear(module.params[CellaSFZ::NEXT_PARAM].getValue(), 0.0f,
        1.0e-6f, "NEXT default");
    require(module.renderQuantum() == 32, "render quantum defaults to 32 frames");
    require(module.tailBehavior() == CellaSFZ::TailBehavior::PreserveAll,
        "release tails are preserved by default");
}

void testFolderNavigation()
{
    char directoryTemplate[] = "/tmp/cella-sfz-navigation-XXXXXX";
    char* directoryValue = mkdtemp(directoryTemplate);
    require(directoryValue, "create navigation directory");
    const std::string directory(directoryValue);
    const std::string first = system::join(directory, "01-first.sfz");
    const std::string middle = system::join(directory, "02-middle.sfz");
    const std::string last = system::join(directory, "03-last.SFZ");
    const std::string ignored = system::join(directory, "04-ignore.txt");
    for (const std::string& path : { first, middle, last, ignored }) {
        std::ofstream file(path);
        file << "<region> sample=*sine key=60 pitch_keycenter=60\n";
        require(static_cast<bool>(file), "write navigation fixture");
    }

    {
        auto state = std::make_shared<RecordingEngineState>();
        CellaSFZ module([state]() {
            return std::make_unique<RecordingEngine>(state);
        });
        module.loadInstrument(
            system::join(directory, "missing/02-middle.sfz"), middle);
        waitForState(module, CellaSFZ::StatusState::Ready,
            "navigation fallback fixture loads");
        require(module.navigationPath() == middle,
            "successful fallback becomes the navigation path");

        require(module.navigateInstrumentFromUi(1), "navigate to next SFZ");
        waitForState(module, CellaSFZ::StatusState::Ready,
            "next SFZ loads");
        require(*module.selectedPath() == last,
            "next navigation accepts case-insensitive SFZ extensions");

        require(module.navigateInstrumentFromUi(1), "wrap to first SFZ");
        waitForState(module, CellaSFZ::StatusState::Ready,
            "wrapped SFZ loads");
        require(*module.selectedPath() == first,
            "next navigation wraps at the end of the folder");

        require(module.navigateInstrumentFromUi(-1), "wrap to last SFZ");
        waitForState(module, CellaSFZ::StatusState::Ready,
            "reverse-wrapped SFZ loads");
        require(*module.selectedPath() == last,
            "previous navigation wraps at the start of the folder");
    }

    unlink(first.c_str());
    unlink(middle.c_str());
    unlink(last.c_str());
    unlink(ignored.c_str());
    rmdir(directory.c_str());
}

void testAsynchronousMissingLoad()
{
    CellaSFZ module;
    const std::string first = "/tmp/cella-missing-first.sfz";
    const std::string second = "/tmp/cella-missing-latest.sfz";
    module.loadInstrumentFromUi(first);
    module.loadInstrumentFromUi(second);

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    for (;;) {
        const auto status = module.statusSnapshot();
        if (status->state == CellaSFZ::StatusState::Missing) {
            require(status->filename == system::getFilename(second),
                "coalesced loader publishes only the newest request");
            break;
        }
        require(std::chrono::steady_clock::now() < deadline,
            "asynchronous missing-file load completed before timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void testAsynchronousSuccessfulLoadAndFailedReplacement()
{
    char validPath[] = "/tmp/cella-sfz-worker-valid-XXXXXX";
    const int validDescriptor = mkstemp(validPath);
    require(validDescriptor >= 0, "create valid worker SFZ path");
    close(validDescriptor);
    {
        std::ofstream sfz(validPath);
        sfz << "<region> sample=*sine key=60 pitch_keycenter=60\n";
        require(static_cast<bool>(sfz), "write valid worker SFZ");
    }

    char invalidPath[] = "/tmp/cella-sfz-worker-invalid-XXXXXX";
    const int invalidDescriptor = mkstemp(invalidPath);
    require(invalidDescriptor >= 0, "create invalid worker SFZ path");
    close(invalidDescriptor);

    CellaSFZ module;
    module.loadInstrumentFromUi(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "asynchronous valid load completed before timeout");
    require(module.loader_->pendingEngine.load(std::memory_order_acquire),
        "successful worker load publishes an engine handoff");
    require(*module.selectedPath() == validPath,
        "successful worker load persists its path");

    module.swapEngineAtBlockBoundary();
    require(module.activeEngine_, "audio boundary accepts the worker engine");
    const uint64_t activeGeneration = module.activeEngine_->generation;

    module.loadInstrumentFromUi(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "queued replacement completes before timeout");
    require(module.loader_->pendingEngine.load(std::memory_order_acquire),
        "completed replacement waits for an audio boundary");
    module.loadInstrumentFromUi(invalidPath);
    waitForState(module, CellaSFZ::StatusState::Error,
        "asynchronous invalid load completed before timeout");
    require(!module.loader_->pendingEngine.load(std::memory_order_acquire),
        "newest request coalesces an older pending replacement");
    require(module.activeEngine_->generation == activeGeneration,
        "failed replacement leaves the current engine active");

    module.loadInstrumentFromUi(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "asynchronous replacement completed before timeout");
    const uint64_t retirementCount =
        module.loader_->retiredEngineCount.load(std::memory_order_acquire);
    module.swapEngineAtBlockBoundary();
    require(module.activeEngine_
            && module.activeEngine_->generation != activeGeneration,
        "successful replacement swaps at an audio block boundary");
    waitForRetirement(module, retirementCount,
        "loader worker retired the replaced engine before timeout");

    unlink(validPath);
    unlink(invalidPath);
}

void testPopFreeLifecycleFade()
{
    CellaSFZ stale;
    stale.activeEngine_ = new CellaSFZ::EngineBundle;
    CellaSFZ::EngineBundle* staleReplacement = new CellaSFZ::EngineBundle;
    staleReplacement->generation = 1;
    stale.loader_->pendingSampleRate.store(48000.0f, std::memory_order_relaxed);
    stale.loader_->pendingEngine.store(staleReplacement, std::memory_order_release);
    stale.loader_->pendingGeneration.store(1, std::memory_order_release);
    stale.loader_->newestLoadGeneration.store(2, std::memory_order_release);
    stale.beginCaptureBlock();
    require(stale.lifecycleFade_ == CellaSFZ::LifecycleFade::Steady,
        "superseded pending engine does not fade the current instrument");
    requireNear(stale.lifecycleGain_, 1.0f, 1.0e-6f,
        "superseded pending engine leaves output at unity");

    CellaSFZ module;
    module.activeEngine_ = new CellaSFZ::EngineBundle;
    CellaSFZ::EngineBundle* replacement = new CellaSFZ::EngineBundle;
    replacement->generation = 1;
    module.loader_->pendingSampleRate.store(48000.0f, std::memory_order_relaxed);
    module.loader_->pendingEngine.store(replacement, std::memory_order_release);
    module.loader_->pendingGeneration.store(1, std::memory_order_release);
    module.loader_->newestLoadGeneration.store(1, std::memory_order_release);

    module.beginCaptureBlock();
    require(module.activeEngine_ != replacement,
        "replacement waits for fade-out instead of swapping immediately");
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::FadingOut,
        "pending replacement starts fade-out");

    float previous = 1.0f;
    const int fadeFrames = static_cast<int>(48000.0f * kLifecycleFadeSeconds);
    for (int frame = 0; frame < fadeFrames; ++frame) {
        const float gain = module.advanceLifecycleFade(48000.0f);
        require(gain <= previous + 1.0e-6f, "fade-out gain is monotonic");
        previous = gain;
    }
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent,
        "replacement reaches digital silence before swapping");
    requireNear(module.lifecycleGain_, 0.0f, 1.0e-6f,
        "fade-out ends at zero gain");

    const uint64_t retirementCount =
        module.loader_->retiredEngineCount.load(std::memory_order_acquire);
    module.beginCaptureBlock();
    require(module.activeEngine_ == replacement,
        "silent replacement swaps only at a block boundary");
    module.finishCaptureBlock();
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::FadingIn,
        "new engine fades in after its first rendered block");

    previous = 0.0f;
    for (int frame = 0; frame < fadeFrames; ++frame) {
        const float gain = module.advanceLifecycleFade(48000.0f);
        require(gain + 1.0e-6f >= previous, "fade-in gain is monotonic");
        previous = gain;
    }
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Steady,
        "replacement returns to steady gain");
    requireNear(module.lifecycleGain_, 1.0f, 1.0e-6f,
        "fade-in ends at unity gain");
    waitForRetirement(module, retirementCount,
        "fade replacement engine is retired by the worker");
}

void testCancellationSafeDestruction()
{
    char validPath[] = "/tmp/cella-sfz-cancel-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create cancellation SFZ path");
    close(descriptor);

    const auto state = std::make_shared<BlockingLoadState>();
    auto module = std::make_unique<CellaSFZ>([state]() {
        return std::make_unique<BlockingEngine>(state);
    });
    module->loadInstrument(validPath);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(state->cv.wait_for(lock, std::chrono::seconds(2),
                    [&state]() { return state->entered; }),
            "blocking loader entered before timeout");
    }

    std::thread watchdog([state]() {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::milliseconds(250),
            [state]() { return state->destructorReturned; });
        state->release = true;
        state->cv.notify_all();
    });
    const auto start = std::chrono::steady_clock::now();
    module.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->destructorReturned = true;
        state->release = true;
    }
    state->cv.notify_all();
    watchdog.join();

    require(elapsed < std::chrono::milliseconds(100),
        "module destruction does not wait for an in-flight load");
    const auto destructionDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (state->destroyed.load(std::memory_order_acquire) == 0) {
        require(std::chrono::steady_clock::now() < destructionDeadline,
            "abandoned loader engine is eventually destroyed");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    unlink(validPath);
}

void testSampleRateRebuildAndFailureSafety()
{
    char validPath[] = "/tmp/cella-sfz-rate-valid-XXXXXX";
    const int validDescriptor = mkstemp(validPath);
    require(validDescriptor >= 0, "create sample-rate SFZ path");
    close(validDescriptor);
    {
        std::ofstream sfz(validPath);
        sfz << "<region> sample=*sine key=60 pitch_keycenter=60\n";
    }

    CellaSFZ module;
    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "initial sample-rate fixture loads");
    require(module.swapEngineAtBlockBoundary(), "activate initial sample-rate engine");
    const uint64_t initialGeneration = module.activeEngine_->generation;

    module.onSampleRateChange({ 96000.0f, 1.0f / 96000.0f });
    waitForState(module, CellaSFZ::StatusState::Ready,
        "sample-rate rebuild completes asynchronously");
    const auto pendingDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (!module.loader_->pendingEngine.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < pendingDeadline,
            "sample-rate rebuild publishes an engine");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    requireNear(module.loader_->pendingEngine.load(
                    std::memory_order_acquire)->sampleRate,
        96000.0f, 1.0e-3f, "replacement engine uses the new sample rate");

    Module::ProcessArgs rateArgs { 96000.0f, 1.0f / 96000.0f, 0 };
    for (int frame = 0;
         frame < 1200 && module.activeEngine_->generation == initialGeneration;
         ++frame) {
        module.process(rateArgs);
    }
    require(module.activeEngine_->generation != initialGeneration,
        "sample-rate replacement fades and swaps during processing");
    requireNear(module.activeEngine_->sampleRate, 96000.0f, 1.0e-3f,
        "active engine records the new sample rate");
    for (int frame = 0; frame < 600; ++frame)
        module.process(rateArgs);
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Steady,
        "successful sample-rate replacement finishes its fade-in");

    char supersededPath[] = "/tmp/cella-sfz-rate-superseded-XXXXXX";
    const int supersededDescriptor = mkstemp(supersededPath);
    require(supersededDescriptor >= 0, "create superseded SFZ path");
    close(supersededDescriptor);
    {
        std::ofstream sfz(supersededPath);
        sfz << "<region> sample=*sine key=67 pitch_keycenter=67\n";
    }
    module.loadInstrument(supersededPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "superseded valid replacement loads");
    require(module.loader_->pendingEngine.load(std::memory_order_acquire)
            && module.loader_->pendingEngine.load(
                   std::memory_order_acquire)->sourcePath
                == supersededPath,
        "valid replacement is pending before it is superseded");

    char invalidPath[] = "/tmp/cella-sfz-rate-invalid-XXXXXX";
    const int invalidDescriptor = mkstemp(invalidPath);
    require(invalidDescriptor >= 0, "create invalid sample-rate SFZ path");
    close(invalidDescriptor);
    module.loadInstrument(invalidPath);
    waitForState(module, CellaSFZ::StatusState::Error,
        "invalid replacement fails before sample-rate change");

    module.onSampleRateChange({ 44100.0f, 1.0f / 44100.0f });
    waitForState(module, CellaSFZ::StatusState::Ready,
        "sample-rate change reloads the last successful instrument");
    CellaSFZ::EngineBundle* pending =
        module.loader_->pendingEngine.load(std::memory_order_acquire);
    require(pending && pending->sourcePath == validPath,
        "sample-rate rebuild follows the active rather than superseded engine");
    requireNear(pending->sampleRate, 44100.0f, 1.0e-3f,
        "last successful instrument rebuild uses the current sample rate");

    const uint64_t before44100 = module.activeEngine_->generation;
    Module::ProcessArgs args44100 { 44100.0f, 1.0f / 44100.0f, 0 };
    for (int frame = 0;
         frame < 1200 && module.activeEngine_->generation == before44100;
         ++frame) {
        module.process(args44100);
    }
    require(module.activeEngine_->generation != before44100,
        "last successful instrument becomes active after the rate change");
    for (int frame = 0; frame < 600; ++frame)
        module.process(args44100);

    unlink(validPath);
    module.onSampleRateChange({ 48000.0f, 1.0f / 48000.0f });
    module.setRenderQuantum(64);
    waitForState(module, CellaSFZ::StatusState::Missing,
        "missing source reports failed sample-rate rebuild");
    Module::ProcessArgs failedRateArgs { 48000.0f, 1.0f / 48000.0f, 0 };
    for (int frame = 0; frame < 600; ++frame)
        module.process(failedRateArgs);
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent,
        "failed sample-rate rebuild leaves the old-rate engine silent");
    requireNear(module.lifecycleGain_, 0.0f, 1.0e-6f,
        "failed sample-rate rebuild cannot fade the old-rate engine back in");
    requireNear(module.activeEngine_->sampleRate, 44100.0f, 1.0e-3f,
        "failed sample-rate rebuild retains but does not play the old-rate engine");
    require(module.activeRenderQuantum_ == 32,
        "quantum change cannot revive or render the stale-rate engine");

    unlink(invalidPath);
    unlink(supersededPath);
}

void testSelectableQuantumAndTailEvents()
{
    char validPath[] = "/tmp/cella-sfz-settings-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create settings SFZ path");
    close(descriptor);

    const auto state = std::make_shared<RecordingEngineState>();
    CellaSFZ module([state]() {
        return std::make_unique<RecordingEngine>(state);
    });
    module.setRenderQuantum(64);
    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "64-frame settings fixture loads");
    require(module.swapEngineAtBlockBoundary(), "activate 64-frame settings engine");
    CellaSFZ::EngineBundle* const activeEngine = module.activeEngine_;
    const uint64_t activeGeneration = activeEngine->generation;
    require(module.activeRenderQuantum_ == 64,
        "initial engine adopts the selected 64-frame quantum");

    module.inputs[CellaSFZ::VOCT_INPUT].channels = 1;
    module.inputs[CellaSFZ::VOCT_INPUT].setVoltage(0.0f);
    module.inputs[CellaSFZ::GATE_INPUT].channels = 1;
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(10.0f);
    module.setTailBehavior(CellaSFZ::TailBehavior::CutOnRetrigger);
    for (int frame = 0; frame < 64; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(!state->maximumBlockSizes.empty()
                && state->maximumBlockSizes.back() == 64,
            "every engine is prepared for the maximum selectable block");
        require(!state->renderedBlockSizes.empty()
                && state->renderedBlockSizes.back() == 64,
            "module renders one 64-frame block");
        require(state->events.size() >= 3
                && state->events[0].type
                    == cella::sfz::EngineEventType::ChokeLaneTails
                && state->events[0].includeHeldVoices
                && state->events[1].type == cella::sfz::EngineEventType::NoteOn,
            "cut mode chokes every old voice before note on");
        state->events.clear();
    }

    module.setRenderQuantum(16);
    for (int frame = 0;
         frame < 1024
         && !(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent
             && module.quantumTransitionActive_ && module.captureFrame_ != 0);
         ++frame) {
        module.process(processArgs());
    }
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent
            && module.quantumTransitionActive_ && module.captureFrame_ != 0,
        "quantum transition reaches silence between capture boundaries");

    module.setRenderQuantum(64);
    for (int frame = 0;
         frame < 128
         && (module.quantumTransitionActive_
             || module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent);
         ++frame) {
        module.process(processArgs());
    }
    require(module.activeRenderQuantum_ == 64
            && !module.quantumTransitionActive_
            && module.lifecycleFade_ == CellaSFZ::LifecycleFade::FadingIn,
        "reverting at silence keeps the active quantum and restarts fade-in");
    for (int frame = 0;
         frame < 1024
         && module.lifecycleFade_ != CellaSFZ::LifecycleFade::Steady;
         ++frame) {
        module.process(processArgs());
    }
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Steady,
        "cancelled quantum transition returns to steady output");

    module.setRenderQuantum(16);
    require(module.statusSnapshot()->state == CellaSFZ::StatusState::Ready,
        "quantum change does not reload the instrument");
    for (int frame = 0;
         frame < 1024
         && !(module.activeRenderQuantum_ == 16 && module.captureFrame_ == 0
             && !module.quantumTransitionActive_);
         ++frame) {
        module.process(processArgs());
    }
    require(module.activeRenderQuantum_ == 16,
        "fade-protected audio boundary adopts the selected 16-frame quantum");
    require(module.activeEngine_ == activeEngine
            && module.activeEngine_->generation == activeGeneration,
        "quantum transition preserves the loaded engine and its voices");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->renderedBlockSizes.back() == 16,
            "module renders one 16-frame block after the transition");
        require(state->maximumBlockSizes.size() == 1,
            "quantum transition neither rebuilds nor reconfigures the engine");
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type == cella::sfz::EngineEventType::NoteOn;
                    }),
            "held gates are not retriggered by a quantum transition");
        state->events.clear();
    }

    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(0.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->events.clear();
    }
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(10.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->events.size() >= 3
                && state->events[0].type
                    == cella::sfz::EngineEventType::ChokeLaneTails
                && state->events[0].includeHeldVoices
                && state->events[1].type == cella::sfz::EngineEventType::NoteOn,
            "cut mode remains ordered before a later retrigger");
        state->events.clear();
    }

    module.setTailBehavior(CellaSFZ::TailBehavior::KeepNewest);
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(0.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->events.size() >= 2
                && state->events[0].type
                    == cella::sfz::EngineEventType::ChokeLaneTails
                && !state->events[0].includeHeldVoices
                && state->events[1].type == cella::sfz::EngineEventType::NoteOff,
            "keep-newest mode removes earlier tails before releasing current note");
        state->events.clear();
    }

    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(10.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type
                            == cella::sfz::EngineEventType::ChokeLaneTails;
                    }),
            "keep-newest mode does not choke its retained tail on note on");
        state->events.clear();
    }

    module.setTailBehavior(CellaSFZ::TailBehavior::PreserveAll);
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(0.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type
                            == cella::sfz::EngineEventType::ChokeLaneTails;
                    }),
            "preserve mode emits no host tail limit");
    }

    unlink(validPath);
}

void testOutputCalibrationAndRouting()
{
    constexpr float sixDecibels = 1.995262315f;
    // Sfizz defaults: -7.35 dB master gain and squared CC7 at 100/127.
    constexpr float sfizzDefaultUnityGain = 0.2660067f;
    require(kLimiterKneeVolts
            / (kNominalOutputVolts * sfizzDefaultUnityGain) >= sixDecibels,
        "default full-scale single-voice audio needs at least 6 dB before limiting");
    requireNear(softLimit(kNominalOutputVolts), kNominalOutputVolts, 1.0e-6f,
        "nominal full-scale audio must remain linear");
    require(softLimit(100.0f) > kLimiterKneeVolts
            && softLimit(100.0f) <= kLimiterCeilingVolts,
        "positive overload must be softly limited at the ceiling");
    requireNear(softLimit(-100.0f), -softLimit(100.0f), 1.0e-6f,
        "limiter must preserve polarity symmetry");

    CellaSFZ stereo;
    stereo.outputs[CellaSFZ::LEFT_OUTPUT].channels = 1;
    stereo.outputs[CellaSFZ::RIGHT_OUTPUT].channels = 1;
    seedPlayback(stereo, 1.0f, -0.5f);
    stereo.process(processArgs());
    requireNear(stereo.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(), 12.0f, 1.0e-6f,
        "stereo left output");
    requireNear(stereo.outputs[CellaSFZ::RIGHT_OUTPUT].getVoltage(), -6.0f, 1.0e-6f,
        "stereo right output");

    CellaSFZ mono;
    mono.outputs[CellaSFZ::LEFT_OUTPUT].channels = 1;
    mono.outputs[CellaSFZ::RIGHT_OUTPUT].channels = 0;
    seedPlayback(mono, 1.0f, -0.5f);
    mono.process(processArgs());
    requireNear(mono.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(), 3.0f, 1.0e-6f,
        "left-only output must use (L + R) * 0.5");
}

void testJsonPersistence()
{
    char temporaryPath[] = "/tmp/cella-sfz-state-XXXXXX";
    const int descriptor = mkstemp(temporaryPath);
    require(descriptor >= 0, "create temporary SFZ path");
    close(descriptor);
    {
        std::ofstream sfz(temporaryPath);
        sfz << "<region> sample=*sine key=60 pitch_keycenter=60\n";
    }

    CellaSFZ source;
    source.setRenderQuantum(64);
    source.setTailBehavior(CellaSFZ::TailBehavior::KeepNewest);
    std::shared_ptr<const std::string> selected =
        std::make_shared<const std::string>(temporaryPath);
    std::atomic_store_explicit(&source.loader_->selectedPath, selected,
        std::memory_order_release);
    std::shared_ptr<const CellaSFZ::StatusSnapshot> ready =
        std::make_shared<const CellaSFZ::StatusSnapshot>(
            CellaSFZ::StatusSnapshot { CellaSFZ::StatusState::Ready,
                system::getFilename(temporaryPath), {}, 17, 123456 });
    std::atomic_store_explicit(&source.loader_->lastSuccessfulStatus, ready,
        std::memory_order_release);

    json_t* state = source.dataToJson();
    require(json_integer_value(json_object_get(state, "schemaVersion"))
            == kSchemaVersion,
        "schema version persists");
    require(json_integer_value(json_object_get(state, "renderQuantum")) == 64,
        "render quantum persists");
    require(std::string(json_string_value(json_object_get(state, "tailBehavior")))
            == "keepNewest",
        "tail behavior persists");
    require(std::string(json_string_value(json_object_get(state, "sfzPath")))
            == temporaryPath,
        "absolute path persists");
    json_t* savedStatus = json_object_get(state, "lastSuccessfulLoad");
    require(json_integer_value(json_object_get(savedStatus, "regionCount")) == 17,
        "region count persists");
    require(json_integer_value(json_object_get(savedStatus, "estimatedSampleBytes"))
            == 123456,
        "sample-memory estimate persists");

    CellaSFZ restored;
    restored.dataFromJson(state);
    require(*restored.selectedPath() == temporaryPath, "absolute path restores");
    require(restored.renderQuantum() == 64, "render quantum restores");
    require(restored.tailBehavior() == CellaSFZ::TailBehavior::KeepNewest,
        "tail behavior restores");
    waitForState(restored, CellaSFZ::StatusState::Ready,
        "patch restore loads asynchronously without a module widget");
    const std::shared_ptr<const CellaSFZ::StatusSnapshot> restoredStatus =
        restored.statusSnapshot();
    require(restoredStatus->regionCount == 1,
        "completed patch restore replaces persisted display metadata");
    require(restored.loader_->pendingEngine.load(std::memory_order_acquire),
        "patch restore publishes a ready engine without a UI step");
    require(restored.swapEngineAtBlockBoundary(),
        "restored engine activates at an audio boundary");
    require(restored.activeRenderQuantum_ == 64,
        "restored setting selects the active render quantum");
    json_decref(state);
    unlink(temporaryPath);

    json_t* missingState = json_object();
    json_object_set_new(missingState, "schemaVersion", json_integer(kSchemaVersion));
    json_object_set_new(missingState, "sfzPath",
        json_string("/tmp/cella-definitely-missing/instrument.sfz"));
    CellaSFZ missing;
    missing.dataFromJson(missingState);
    waitForState(missing, CellaSFZ::StatusState::Missing,
        "missing patch path is checked asynchronously");
    require(missing.statusSnapshot()->state == CellaSFZ::StatusState::Missing,
        "missing path reports MISSING SFZ");
    require(!missing.loader_->pendingEngine.load(std::memory_order_acquire),
        "missing path does not publish an engine");
    json_decref(missingState);
}

} // namespace

int main()
{
    try {
        testLexicalPathRoots();
        std::puts("PASS: cross-platform lexical path roots");
        testPermanentIdsAndDefaults();
        std::puts("PASS: permanent IDs and defaults");
        testFolderNavigation();
        std::puts("PASS: previous/next folder navigation");
        testAsynchronousMissingLoad();
        std::puts("PASS: asynchronous coalesced loading");
        testAsynchronousSuccessfulLoadAndFailedReplacement();
        std::puts("PASS: asynchronous engine handoff and failed replacement");
        testPopFreeLifecycleFade();
        std::puts("PASS: pop-free lifecycle fade and worker retirement");
        testCancellationSafeDestruction();
        std::puts("PASS: cancellation-safe destruction during in-flight load");
        testSampleRateRebuildAndFailureSafety();
        std::puts("PASS: sample-rate rebuild and failure safety");
        testSelectableQuantumAndTailEvents();
        std::puts("PASS: selectable quantum and release-tail events");
        testOutputCalibrationAndRouting();
        std::puts("PASS: output calibration and routing");
        testJsonPersistence();
        std::puts("PASS: JSON persistence");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("All SFZ module tests passed");
    return 0;
}
