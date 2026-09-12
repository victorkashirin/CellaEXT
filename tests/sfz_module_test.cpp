#include "../src/SFZ.cpp"

#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include <unistd.h>

Plugin* pluginInstance = nullptr;

namespace {
thread_local bool trackRealtimeAllocations = false;
thread_local size_t realtimeAllocations = 0;

void noteAllocation() noexcept
{
    if (trackRealtimeAllocations)
        ++realtimeAllocations;
}

void* allocateForTest(size_t size)
{
    noteAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void* allocateAlignedForTest(size_t size, size_t alignment)
{
    noteAllocation();
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) == 0)
        return memory;
    throw std::bad_alloc();
}
}

void* operator new(size_t size) { return allocateForTest(size); }
void* operator new[](size_t size) { return allocateForTest(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }
void* operator new(size_t size, std::align_val_t alignment)
{
    return allocateAlignedForTest(size, static_cast<size_t>(alignment));
}
void* operator new[](size_t size, std::align_val_t alignment)
{
    return allocateAlignedForTest(size, static_cast<size_t>(alignment));
}
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, size_t, std::align_val_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, size_t, std::align_val_t) noexcept
{
    std::free(memory);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocateForTest(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocateForTest(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

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
    void renderPolyphonic(float* const* left, float* const* right, int channels,
        int frames) noexcept override
    {
        for (int channel = 0; channel < channels; ++channel) {
            std::fill(left[channel], left[channel] + frames, 0.0f);
            std::fill(right[channel], right[channel] + frames, 0.0f);
        }
    }
    cella::sfz::EngineStats stats() const noexcept override { return {}; }

    std::shared_ptr<BlockingLoadState> state;
};

struct RecordingEngineState {
    std::mutex mutex;
    std::vector<int> maximumBlockSizes;
    std::vector<int> renderedBlockSizes;
    std::vector<int> polyphonicBlockSizes;
    std::vector<int> polyphonicChannelCounts;
    std::vector<cella::sfz::TimedEngineEvent> events;
    cella::sfz::InstrumentMetadata metadata;
    std::atomic<int> activeVoices { 0 };
    std::atomic<int> voiceLimit { 128 };
    std::atomic<int> activeVoiceQueries { 0 };
    size_t preloadedSampleCount { 0 };
    size_t estimatedPreloadedBytes { 0 };
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
        report.preloadedSampleCount = state->preloadedSampleCount;
        report.estimatedPreloadedSampleBytes = state->estimatedPreloadedBytes;
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
    void renderPolyphonic(float* const* left, float* const* right, int channels,
        int frames) noexcept override
    {
        for (int channel = 0; channel < channels; ++channel) {
            std::fill(left[channel], left[channel] + frames, 0.0f);
            std::fill(right[channel], right[channel] + frames, 0.0f);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->polyphonicBlockSizes.push_back(frames);
        state->polyphonicChannelCounts.push_back(channels);
    }
    cella::sfz::EngineStats stats() const noexcept override { return {}; }
    int activeVoiceCount() const noexcept override
    {
        state->activeVoiceQueries.fetch_add(1, std::memory_order_relaxed);
        return state->activeVoices.load(std::memory_order_relaxed);
    }
    int voiceLimit() const noexcept override
    {
        return state->voiceLimit.load(std::memory_order_relaxed);
    }
    const cella::sfz::InstrumentMetadata& instrumentMetadata() const noexcept override
    {
        return state->metadata;
    }

    std::shared_ptr<RecordingEngineState> state;
};

struct RealtimeProbeState {
    std::atomic<int> loadCalls { 0 };
    std::atomic<int> configureCalls { 0 };
    std::atomic<int> enqueueCalls { 0 };
    std::atomic<int> renderCalls { 0 };
    cella::sfz::InstrumentMetadata metadata;
};

struct RealtimeProbeEngine final : cella::sfz::SamplerEngine {
    explicit RealtimeProbeEngine(RealtimeProbeState* state)
        : state(state)
    {
    }

    cella::sfz::LoadReport load(const std::string&) override
    {
        state->loadCalls.fetch_add(1, std::memory_order_relaxed);
        cella::sfz::LoadReport report;
        report.success = true;
        report.regionCount = 1;
        return report;
    }
    void setSampleRate(float) override
    {
        state->configureCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void setMaximumBlockSize(int) override
    {
        state->configureCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void setTuningFrequency(float) override
    {
        state->configureCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void enqueue(const cella::sfz::TimedEngineEvent&) noexcept override
    {
        state->enqueueCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void render(float* left, float* right, int frames) noexcept override
    {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        state->renderCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void renderPolyphonic(float* const* left, float* const* right, int channels,
        int frames) noexcept override
    {
        for (int channel = 0; channel < channels; ++channel) {
            std::fill(left[channel], left[channel] + frames, 0.0f);
            std::fill(right[channel], right[channel] + frames, 0.0f);
        }
        state->renderCalls.fetch_add(1, std::memory_order_relaxed);
    }
    cella::sfz::EngineStats stats() const noexcept override { return {}; }
    const cella::sfz::InstrumentMetadata& instrumentMetadata() const noexcept override
    {
        return state->metadata;
    }

    RealtimeProbeState* state;
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
    module.clearPlaybackBuffers();
    module.playbackLeft_[0][0] = left;
    module.playbackRight_[0][0] = right;
    module.playbackPolyphonic_ = false;
    module.playbackChannels_ = 1;
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
    require(modelCellaSFZ->slug == "SFZ",
        "base model retains the manifest compatibility slug");
    require(modelCellaSFZExpression->slug == "CellaSFZExpression",
        "expression expander uses its manifest slug");
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
    static_assert(CellaSFZ::POLY_OUTPUT_PARAM == 6);
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
    requireNear(module.params[CellaSFZ::POLY_OUTPUT_PARAM].getValue(), 0.0f,
        1.0e-6f, "polyphonic output defaults to mixed stereo");
    require(module.renderQuantum() == 32, "render quantum defaults to 32 frames");
    require(module.tailBehavior() == CellaSFZ::TailBehavior::PreserveAll,
        "release tails are preserved by default");
    require(module.displayPage() == CellaSFZ::DisplayPage::Play,
        "display defaults to the Play page");
}

void testDisplayFormattingAndPersistence()
{
    require(folderPosition(3, 8) == "3/8", "single-digit folder position");
    require(folderPosition(3, 18) == "03/18", "folder index pads to total width");
    require(folderPosition(3, 120) == "003/120", "three-digit folder padding");
    require(folderPosition(0, 18).empty(), "unknown folder index is omitted");
    require(filenameWithoutSfzExtension("Grand Piano.SFZ") == "Grand Piano",
        "display extension removal is case-insensitive");
    require(hasSfzExtension("Grand Piano.SFZ")
            && hasSfzExtension("grand piano.sfz")
            && !hasSfzExtension("grand piano.wav"),
        "SFZ file interaction filter is case-insensitive and type-specific");
    require(shortenMiddle("Distinguishing-Beginning-And-End", 15)
            == "Distin...nd-End",
        "long filenames retain distinguishing ends");
    require(memoryLabel(0) == "0 B", "zero preload bytes are explicit");
    require(memoryLabel(1024) == "1.0 KB", "preload bytes use binary KB");
    require(memoryLabel(1024ull * 1024ull * 1024ull) == "1.0 GB",
        "large preload bytes use binary GB");

    CellaSFZ::StatusSnapshot status;
    status.state = CellaSFZ::StatusState::Ready;
    status.filename = "Grand Piano.SFZ";
    status.folderIndex = 3;
    status.folderCount = 18;
    status.regionCount = 384;
    status.preloadedSampleCount = 127;
    status.estimatedSampleBytes = 46ull * 1024ull * 1024ull;
    status.assignableCcCount = 17;
    status.keyswitchCount = 6;
    DisplayRuntimeSnapshot runtime { 4, 23, 23, 128, 4, 0, 0, 0, 0, 0 };
    auto layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Play,
        runtime, DisplayWarning::None);
    require(layout.title == "Grand Piano" && layout.position == "03/18",
        "Play page separates the instrument and folder position");
    require(layout.middleLeft == "Held 04"
            && layout.middleRight == "Voices 023 / 128",
        "Play page expands held-lane and voice labels");
    require(layout.bottomLeft == "Controls 4 / 17"
            && layout.bottomRight == "Switches 6",
        "Play page expands controller and switches labels");

    layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Info,
        runtime, DisplayWarning::None);
    require(layout.title == "Grand Piano" && layout.position == "03/18",
        "Info page separates the instrument and folder position");
    require(layout.middleLeft == "Regions 384"
            && layout.middleRight == "Samples 127",
        "Info page expands region and sample labels");
    require(layout.bottomLeft == "Preload ~46.0 MB"
            && layout.bottomRight.empty(),
        "Info page labels estimated preload memory");
    layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Info,
        runtime, DisplayWarning::OutputLimiting);
    require(layout.bottomLeft == "OUTPUT LIMITING"
            && layout.bottomRight.empty(),
        "runtime warning replaces both bottom fields");
    layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Play,
        runtime, DisplayWarning::EventDrop);
    require(layout.title == "Grand Piano" && layout.position == "03/18"
            && layout.middleLeft == "Held 04"
            && layout.middleRight == "Voices 023 / 128"
            && layout.bottomLeft == "EVENT DROP"
            && layout.bottomRight.empty(),
        "warning replacement and restoration preserve the selected Play page");
    layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Play,
        runtime, DisplayWarning::None);
    require(layout.bottomLeft == "Controls 4 / 17"
            && layout.bottomRight == "Switches 6",
        "Play page bottom fields restore after warning expiry");
    layout = readyDisplayLayout(status, CellaSFZ::DisplayPage::Info,
        runtime, DisplayWarning::None);
    require(layout.bottomLeft == "Preload ~46.0 MB",
        "Info page bottom field restores after warning expiry");

    CellaSFZ::StatusSnapshot emptyStatus;
    emptyStatus.state = CellaSFZ::StatusState::Ready;
    emptyStatus.filename = "Empty.sfz";
    layout = readyDisplayLayout(emptyStatus, CellaSFZ::DisplayPage::Info,
        {}, DisplayWarning::None);
    require(layout.middleLeft == "Regions 0"
            && layout.middleRight == "Samples 0"
            && layout.bottomLeft == "Preload 0 B",
        "zero region and preload metadata remain explicit");
    emptyStatus.regionCount = 1234567;
    emptyStatus.preloadedSampleCount = 987654;
    emptyStatus.estimatedSampleBytes = 5ull * 1024ull * 1024ull * 1024ull;
    emptyStatus.keyswitchCount = 128;
    layout = readyDisplayLayout(emptyStatus, CellaSFZ::DisplayPage::Info,
        {}, DisplayWarning::None);
    require(layout.middleLeft == "Regions 1234567"
            && layout.middleRight == "Samples 987654"
            && layout.bottomLeft == "Preload ~5.0 GB",
        "large region, preload-count, and byte values format without wrapping");
    layout = readyDisplayLayout(emptyStatus, CellaSFZ::DisplayPage::Play,
        {}, DisplayWarning::None);
    require(layout.bottomLeft == "Controls 0 / 0"
            && layout.bottomRight == "Switches 128",
        "large keyswitch and zero controller values format without wrapping");

    require(voicePressureFor(95, 128) == VoicePressure::Normal,
        "voice pressure remains normal below 75 percent");
    require(voicePressureFor(96, 128) == VoicePressure::Amber,
        "voice pressure is amber at 75 percent");
    require(voicePressureFor(115, 128) == VoicePressure::Amber,
        "voice pressure remains amber below 90 percent");
    require(voicePressureFor(116, 128) == VoicePressure::Red,
        "voice pressure is red at 90 percent and above");
    require(displayColorRoleForStatus(CellaSFZ::StatusState::Unloaded)
                == DisplayColorRole::Normal
            && displayColorRoleForStatus(CellaSFZ::StatusState::Loading)
                == DisplayColorRole::Normal
            && displayColorRoleForStatus(CellaSFZ::StatusState::Ready)
                == DisplayColorRole::Normal,
        "ordinary and loading states use the panel-matched normal color");
    require(displayColorRoleForStatus(CellaSFZ::StatusState::Error)
                == DisplayColorRole::Red
            && displayColorRoleForStatus(CellaSFZ::StatusState::Missing)
                == DisplayColorRole::Red,
        "persistent load failures use red");
    require(displayColorRoleForWarning(DisplayWarning::None)
                == DisplayColorRole::Normal
            && displayColorRoleForWarning(DisplayWarning::EventDrop)
                == DisplayColorRole::Red
            && displayColorRoleForWarning(DisplayWarning::OutputLimiting)
                == DisplayColorRole::Amber,
        "warning labels use their specified color roles");
    require(displayColorRoleForPressure(VoicePressure::Normal)
                == DisplayColorRole::Normal
            && displayColorRoleForPressure(VoicePressure::Amber)
                == DisplayColorRole::Amber
            && displayColorRoleForPressure(VoicePressure::Red)
                == DisplayColorRole::Red,
        "voice-pressure meter uses threshold color roles");
    VoicePeakHold peak;
    require(peak.observe(80, 1000000000ull) == 80,
        "voice peak begins at current value");
    require(peak.observe(20, 1399999999ull) == 80,
        "voice peak holds for 400 ms");
    require(peak.observe(20, 1400000000ull) == 20,
        "expired voice peak returns immediately to current value");

    runtime.audioTimeNanoseconds = 2000000000ull;
    runtime.eventDropWarningUntilNanoseconds = 3000000000ull;
    runtime.outputLimitingWarningUntilNanoseconds = 3000000000ull;
    require(displayWarningFor(runtime) == DisplayWarning::EventDrop,
        "event drop has warning priority");
    runtime.audioTimeNanoseconds = 3000000000ull;
    require(displayWarningFor(runtime) == DisplayWarning::None,
        "warning expires at its audio-side deadline");

    CellaSFZ module;
    module.setDisplayPage(CellaSFZ::DisplayPage::Info);
    json_t* saved = module.dataToJson();
    require(json_integer_value(json_object_get(saved, "displayPage")) == 1,
        "Info page persists as displayPage 1");
    CellaSFZ restored;
    restored.dataFromJson(saved);
    require(restored.displayPage() == CellaSFZ::DisplayPage::Info,
        "persisted Info page restores");
    json_decref(saved);

    for (json_int_t invalid : { -1, 2, 999 }) {
        json_t* root = json_object();
        json_object_set_new(root, "displayPage", json_integer(invalid));
        restored.setDisplayPage(CellaSFZ::DisplayPage::Info);
        restored.dataFromJson(root);
        require(restored.displayPage() == CellaSFZ::DisplayPage::Play,
            "invalid display page falls back to Play");
        json_decref(root);
    }
    json_t* missing = json_object();
    restored.setDisplayPage(CellaSFZ::DisplayPage::Info);
    restored.dataFromJson(missing);
    require(restored.displayPage() == CellaSFZ::DisplayPage::Play,
        "missing display page falls back to Play");
    json_decref(missing);
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
        require(module.statusSnapshot()->folderIndex == 2
                && module.statusSnapshot()->folderCount == 3,
            "display folder position uses the navigation snapshot");
        require(module.navigationPath() == middle,
            "successful fallback becomes the navigation path");

        require(module.navigateInstrumentFromUi(1), "navigate to next SFZ");
        waitForState(module, CellaSFZ::StatusState::Ready,
            "next SFZ loads");
        require(*module.selectedPath() == last,
            "next navigation accepts case-insensitive SFZ extensions");
        require(module.statusSnapshot()->folderIndex == 3
                && module.statusSnapshot()->folderCount == 3,
            "display and next navigation agree on adjacency");

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

void testFolderSnapshotFailures()
{
    const FolderSnapshot unreadable = snapshotSfzFolder(
        "/tmp/cella-directory-does-not-exist/instrument.sfz");
    require(!unreadable.inspected && !unreadable.hasSelection(),
        "unreadable directory has no stale folder position");

    char directoryTemplate[] = "/tmp/cella-sfz-absent-XXXXXX";
    char* directoryValue = mkdtemp(directoryTemplate);
    require(directoryValue, "create absent-selection directory");
    const std::string directory(directoryValue);
    const std::string instrument = system::join(directory, "present.SFZ");
    const std::string absent = system::join(directory, "selected.txt");
    {
        std::ofstream sfz(instrument);
        sfz << "<region> sample=*sine\n";
        std::ofstream other(absent);
        other << "not an sfz\n";
    }
    const FolderSnapshot snapshot = snapshotSfzFolder(absent);
    require(snapshot.inspected && snapshot.instruments.size() == 1
            && !snapshot.hasSelection(),
        "selected file absent from snapshot omits its index");
    unlink(instrument.c_str());
    unlink(absent.c_str());
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

void testDisplayRuntimeTelemetry()
{
    char validPath[] = "/tmp/cella-sfz-display-runtime-XXXXXX.sfz";
    const int descriptor = mkstemps(validPath, 4);
    require(descriptor >= 0, "create display telemetry SFZ path");
    close(descriptor);
    {
        std::ofstream sfz(validPath);
        sfz << "<region> sample=*sine key=60\n";
    }

    const auto state = std::make_shared<RecordingEngineState>();
    state->preloadedSampleCount = 127;
    state->estimatedPreloadedBytes = 46ull * 1024ull * 1024ull;
    state->metadata.namedControllers = {
        { 20, "Brightness", 0.0f }, { 21, "Noise", 0.0f }
    };
    state->metadata.latchedKeyswitches = {
        { 24, "Legato" }, { 25, "Staccato" }
    };
    CellaSFZ module([state]() {
        return std::make_unique<RecordingEngine>(state);
    });
    module.loadInstrumentFromUi(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "display telemetry fixture loads");
    const auto status = module.statusSnapshot();
    require(status->preloadedSampleCount == 127
            && status->estimatedSampleBytes == 46ull * 1024ull * 1024ull,
        "loader publishes preload count and estimated bytes");
    require(status->assignableCcCount == 2 && status->keyswitchCount == 2,
        "loader publishes controller and keyswitch counts");
    require(!module.readyStatusDescribesActiveEngine(*status),
        "READY metadata stays gated while its engine is pending activation");
    require(module.swapEngineAtBlockBoundary(),
        "display telemetry engine activates at an audio boundary");
    require(module.readyStatusDescribesActiveEngine(*status),
        "READY metadata becomes visible only for the active generation");
    require(state->activeVoiceQueries.load(std::memory_order_relaxed) == 0,
        "active voices are not queried before a sampler render");

    state->activeVoices.store(37, std::memory_order_relaxed);
    state->voiceLimit.store(128, std::memory_order_relaxed);
    module.finishCaptureBlock();
    DisplayRuntimeSnapshot runtime = module.displayRuntimeSnapshot();
    require(runtime.activeVoices == 37 && runtime.voiceLimit == 128,
        "render quantum publishes current voices and configured limit");
    require(runtime.peakVoices == 37,
        "audio render publishes the observed voice peak");
    require(state->activeVoiceQueries.load(std::memory_order_relaxed) == 1,
        "active voices are sampled exactly after render");

    module.inputs[CellaSFZ::VOCT_INPUT].channels = 4;
    module.inputs[CellaSFZ::GATE_INPUT].channels = 1;
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(10.0f, 0);
    module.captureInputFrame();
    require(module.displayRuntimeSnapshot().heldLanes == 4,
        "mono gate broadcast counts every logical pitch lane as held");
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(0.0f, 0);
    module.captureInputFrame();
    require(module.displayRuntimeSnapshot().heldLanes == 0,
        "note-off clears held count while release voices remain sampler-owned");
    require(module.displayRuntimeSnapshot().activeVoices == 37,
        "release-tail voice telemetry is independent of held lanes");

    state->activeVoices.store(5, std::memory_order_relaxed);
    module.audioTimeNanoseconds_.store(399999999ull, std::memory_order_relaxed);
    module.finishCaptureBlock();
    require(module.displayRuntimeSnapshot().peakVoices == 37,
        "audio-side voice peak survives UI-independent render quanta");
    module.audioTimeNanoseconds_.store(400000000ull, std::memory_order_relaxed);
    module.finishCaptureBlock();
    require(module.displayRuntimeSnapshot().peakVoices == 5,
        "audio-side voice peak returns immediately to current at expiry");

    const uint64_t beforeDrop = module.displayRuntimeSnapshot().eventDropCount;
    ++module.droppedEventCount_;
    module.publishEventDropTelemetry();
    runtime = module.displayRuntimeSnapshot();
    require(runtime.eventDropCount == beforeDrop + 1,
        "capture rejection publishes a monotonic EVENT DROP counter");
    require(runtime.eventDropWarningUntilNanoseconds
            == runtime.audioTimeNanoseconds + 1000000000ull,
        "EVENT DROP hold is anchored to audio event time");
    seedPlayback(module, 2.0f, -2.0f);
    module.process(processArgs());
    runtime = module.displayRuntimeSnapshot();
    require(runtime.outputLimitingCount > 0
            && runtime.outputLimitingWarningUntilNanoseconds
                == runtime.audioTimeNanoseconds + 1000000000ull,
        "either output entering the limiter knee publishes a warning event");
    const uint64_t firstLimitingDeadline =
        runtime.outputLimitingWarningUntilNanoseconds;
    seedPlayback(module, 2.0f, -2.0f);
    module.process(processArgs());
    runtime = module.displayRuntimeSnapshot();
    require(runtime.outputLimitingWarningUntilNanoseconds
            > firstLimitingDeadline,
        "repeated limiting restarts the warning hold from the latest event");
    runtime.audioTimeNanoseconds = std::max(
        runtime.eventDropWarningUntilNanoseconds,
        runtime.outputLimitingWarningUntilNanoseconds);
    require(displayWarningFor(runtime) == DisplayWarning::None,
        "hidden displays do not replay an expired runtime warning");

    unlink(validPath);
}

void testDisplayFileInteractionsAndUnload()
{
    char directoryTemplate[] = "/tmp/cella-sfz-display-actions-XXXXXX";
    char* directoryValue = mkdtemp(directoryTemplate);
    require(directoryValue, "create display interaction directory");
    const std::string directory(directoryValue);
    const std::string sfzPath = system::join(directory, "Drop Target.SFZ");
    const std::string ignoredPath = system::join(directory, "Ignore.txt");
    {
        std::ofstream sfz(sfzPath);
        sfz << "<region> sample=*sine key=60\n";
        std::ofstream ignored(ignoredPath);
        ignored << "not an instrument\n";
    }

    const auto state = std::make_shared<BlockingLoadState>();
    CellaSFZ module([state]() {
        return std::make_unique<BlockingEngine>(state);
    });
    // Rack finalizes Widget destruction through APP's event state, which is
    // intentionally absent in this headless module test. Match the existing UI
    // menu tests by keeping these small widgets alive until process exit.
    auto* display = new CellaSFZStatusDisplay;
    display->module = &module;
    display->loadAction = []() {};

    auto* menu = new ui::Menu;
    display->appendDisplayContextMenu(menu);
    bool hasLoad = false;
    bool hasUnload = false;
    for (Widget* child : menu->children) {
        auto* item = dynamic_cast<ui::MenuItem*>(child);
        if (!item)
            continue;
        hasLoad |= item->text == "Load SFZ...";
        hasUnload |= item->text == "Unload instrument";
    }
    require(hasLoad && hasUnload,
        "display context menu exposes load and unload actions");

    widget::EventContext ignoredContext;
    const std::vector<std::string> ignoredPaths { ignoredPath };
    CellaSFZStatusDisplay::PathDropEvent ignoredEvent(ignoredPaths);
    ignoredEvent.context = &ignoredContext;
    display->onPathDrop(ignoredEvent);
    require(!ignoredContext.consumed
            && module.statusSnapshot()->state
                == CellaSFZ::StatusState::Unloaded,
        "display ignores dropped non-SFZ files");

    widget::EventContext acceptedContext;
    const std::vector<std::string> acceptedPaths { ignoredPath, sfzPath };
    CellaSFZStatusDisplay::PathDropEvent acceptedEvent(acceptedPaths);
    acceptedEvent.context = &acceptedContext;
    display->onPathDrop(acceptedEvent);
    require(acceptedContext.consumed && acceptedContext.target == display,
        "display consumes the first valid SFZ from a dropped path list");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (!state->entered) {
            require(state->cv.wait_until(lock, deadline)
                    != std::cv_status::timeout,
                "dropped SFZ enters the asynchronous loader");
        }
    }
    const auto loading = module.statusSnapshot();
    require(loading->state == CellaSFZ::StatusState::Loading
            && loading->filename == "Drop Target.SFZ"
            && loading->folderIndex == 1 && loading->folderCount == 1,
        "loading status preserves the top-row filename and folder position");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->cv.notify_all();
    waitForState(module, CellaSFZ::StatusState::Ready,
        "dropped SFZ completes loading");
    require(module.swapEngineAtBlockBoundary(),
        "dropped SFZ activates at an audio boundary");

    const uint64_t retirementCount =
        module.loader_->retiredEngineCount.load(std::memory_order_acquire);
    module.unloadInstrumentFromUi();
    require(module.statusSnapshot()->state == CellaSFZ::StatusState::Unloaded
            && module.selectedPath()->empty()
            && module.navigationPath().empty(),
        "unload immediately clears display and persistence metadata");
    module.beginCaptureBlock();
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::FadingOut,
        "unload starts the pop-free lifecycle fade");
    const int fadeFrames = static_cast<int>(48000.0f * kLifecycleFadeSeconds);
    for (int frame = 0; frame < fadeFrames; ++frame)
        module.advanceLifecycleFade(48000.0f);
    require(module.lifecycleFade_ == CellaSFZ::LifecycleFade::Silent,
        "unload reaches silence before detaching the engine");
    module.beginCaptureBlock();
    require(!module.activeEngine_
            && module.loader_->activeGeneration.load(std::memory_order_acquire) == 0,
        "unload detaches the engine only at a silent audio boundary");
    const DisplayRuntimeSnapshot runtime = module.displayRuntimeSnapshot();
    require(runtime.heldLanes == 0 && runtime.activeVoices == 0
            && runtime.peakVoices == 0 && runtime.voiceLimit == 0
            && runtime.assignedCcCount == 0,
        "unload clears all engine-dependent display telemetry");
    waitForRetirement(module, retirementCount,
        "loader worker retires the unloaded engine");

    const auto cancelledState = std::make_shared<BlockingLoadState>();
    CellaSFZ cancelled([cancelledState]() {
        return std::make_unique<BlockingEngine>(cancelledState);
    });
    cancelled.loadInstrumentFromUi(sfzPath);
    {
        std::unique_lock<std::mutex> lock(cancelledState->mutex);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (!cancelledState->entered) {
            require(cancelledState->cv.wait_until(lock, deadline)
                    != std::cv_status::timeout,
                "loading fixture enters before unload cancellation");
        }
    }
    cancelled.unloadInstrumentFromUi();
    {
        std::lock_guard<std::mutex> lock(cancelledState->mutex);
        cancelledState->release = true;
    }
    cancelledState->cv.notify_all();
    const auto cancellationDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    for (;;) {
        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(cancelled.loader_->mutex);
            busy = cancelled.loader_->workerBusy
                || cancelled.loader_->loadRequest.has_value();
        }
        if (!busy)
            break;
        require(std::chrono::steady_clock::now() < cancellationDeadline,
            "unload cancels an in-flight load before timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(cancelled.statusSnapshot()->state
                == CellaSFZ::StatusState::Unloaded
            && !cancelled.loader_->pendingEngine.load(std::memory_order_acquire)
            && !cancelled.activeEngine_,
        "an unloaded in-flight request cannot republish or activate its engine");

    unlink(sfzPath.c_str());
    unlink(ignoredPath.c_str());
    rmdir(directory.c_str());
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
    module.eventDropWarningUntilNanoseconds_.store(
        1000000000ull, std::memory_order_release);
    module.outputLimitingWarningUntilNanoseconds_.store(
        1000000000ull, std::memory_order_release);
    module.swapEngineAtBlockBoundary();
    require(module.activeEngine_
            && module.activeEngine_->generation != activeGeneration,
        "successful replacement swaps at an audio block boundary");
    require(module.displayRuntimeSnapshot().eventDropWarningUntilNanoseconds == 0
            && module.displayRuntimeSnapshot()
                    .outputLimitingWarningUntilNanoseconds == 0,
        "successful replacement does not inherit the outgoing engine's warnings");
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
        const auto choke = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type
                    == cella::sfz::EngineEventType::ChokeLaneTails;
            });
        const auto noteOn = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type == cella::sfz::EngineEventType::NoteOn;
            });
        require(choke != state->events.end() && noteOn != state->events.end()
                && choke < noteOn && choke->includeHeldVoices,
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
        const auto choke = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type
                    == cella::sfz::EngineEventType::ChokeLaneTails;
            });
        const auto noteOn = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type == cella::sfz::EngineEventType::NoteOn;
            });
        require(choke != state->events.end() && noteOn != state->events.end()
                && choke < noteOn && choke->includeHeldVoices,
            "cut mode remains ordered before a later retrigger");
        state->events.clear();
    }

    module.setTailBehavior(CellaSFZ::TailBehavior::KeepNewest);
    module.inputs[CellaSFZ::GATE_INPUT].setVoltage(0.0f);
    for (int frame = 0; frame < 16; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto choke = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type
                    == cella::sfz::EngineEventType::ChokeLaneTails;
            });
        const auto noteOff = std::find_if(state->events.begin(), state->events.end(),
            [](const auto& event) {
                return event.type == cella::sfz::EngineEventType::NoteOff;
            });
        require(choke != state->events.end() && noteOff != state->events.end()
                && choke < noteOff && !choke->includeHeldVoices,
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

void testMaximumDensityEventBound()
{
    char validPath[] = "/tmp/cella-sfz-density-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create density SFZ path");
    close(descriptor);

    const auto state = std::make_shared<RecordingEngineState>();
    state->events.reserve(cella::sfz::BlockAdapter::MaxEvents);
    CellaSFZ module([state]() {
        return std::make_unique<RecordingEngine>(state);
    });
    CellaSFZExpression expander;
    module.model = modelCellaSFZ;
    expander.model = modelCellaSFZExpression;
    module.rightExpander.module = &expander;
    expander.leftExpander.module = &module;
    module.setRenderQuantum(64);
    module.setTailBehavior(CellaSFZ::TailBehavior::CutOnRetrigger);
    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "density recording engine loads");
    require(module.swapEngineAtBlockBoundary(),
        "density recording engine activates");

    module.inputs[CellaSFZ::VOCT_INPUT].channels = 16;
    module.inputs[CellaSFZ::GATE_INPUT].channels = 16;
    auto& message = *static_cast<cella::sfz::ExpressionMessage*>(
        expander.leftExpander.consumerMessage);
    message = {};
    message.magic = cella::sfz::ExpressionMessageMagic;
    message.inputs[0].channels = 16;
    message.inputs[1].channels = 16;

    for (int frame = 0; frame < 64; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->events.clear();
    }

    for (int lane = 0; lane < 16; ++lane)
        message.inputs[1].values[lane] = 1.0f;
    for (int frame = 0; frame < 64; ++frame) {
        const float bend = 0.01f * static_cast<float>(frame + 1);
        for (int lane = 0; lane < 16; ++lane) {
            message.inputs[0].values[lane] = bend;
            module.inputs[CellaSFZ::GATE_INPUT].setVoltage(
                frame % 2 == 0 ? 10.0f : 0.0f, lane);
        }
        module.process(processArgs());
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->events.size() == 3088,
            "64-frame maximum-density block retains expression, notes and chokes");
    }
    require(module.droppedEventCount_ == 0
            && module.activeEngine_->adapter.droppedEventCount() == 0,
        "maximum-density capture and expanded adapter queues drop no events");
    unlink(validPath);
}

void testRealtimeExpressionPath()
{
    char validPath[] = "/tmp/cella-sfz-realtime-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create realtime-probe SFZ path");
    close(descriptor);
    {
        std::ofstream sfz(validPath);
        sfz << "<control> label_cc20=Brightness set_cc20=0\n"
               "<region> sample=*sine key=60 volume_oncc20=24 "
               "volume_smoothcc20=10 ampeg_release=0.1\n";
    }

    RealtimeProbeState state;
    state.metadata.namedControllers = { { 20, "Brightness", 0.25f } };
    {
        CellaSFZ module([&state]() {
            return std::make_unique<RealtimeProbeEngine>(&state);
        });
        CellaSFZExpression expander;
        module.model = modelCellaSFZ;
        expander.model = modelCellaSFZExpression;
        module.rightExpander.module = &expander;
        expander.leftExpander.module = &module;
        module.setRenderQuantum(64);
        module.setTailBehavior(CellaSFZ::TailBehavior::CutOnRetrigger);
        module.loadInstrument(validPath);
        waitForState(module, CellaSFZ::StatusState::Ready,
            "realtime-probe engine loads");
        require(module.swapEngineAtBlockBoundary(),
            "realtime-probe engine activates");

        module.inputs[CellaSFZ::VOCT_INPUT].channels = 16;
        module.inputs[CellaSFZ::GATE_INPUT].channels = 16;
        auto& message = *static_cast<cella::sfz::ExpressionMessage*>(
            expander.leftExpander.consumerMessage);
        message = {};
        message.magic = cella::sfz::ExpressionMessageMagic;
        for (auto& input : message.inputs)
            input.channels = 16;
        message.assignments[0] = 20;

        // Warm all lazy state and align the probe with a capture boundary.
        for (int frame = 0; frame < 128; ++frame)
            module.process(processArgs());
        require(module.captureFrame_ == 0,
            "realtime probe begins at a capture boundary");

        const int loadCalls = state.loadCalls.load(std::memory_order_relaxed);
        const int configureCalls =
            state.configureCalls.load(std::memory_order_relaxed);
        const int enqueueCalls =
            state.enqueueCalls.load(std::memory_order_relaxed);
        const int renderCalls = state.renderCalls.load(std::memory_order_relaxed);
        module.params[CellaSFZ::POLY_OUTPUT_PARAM].setValue(1.0f);

        // Holding the loader mutex turns an accidental process-side lock into
        // a deterministic deadlock. The fixed-size probe engine also records
        // every control/configuration call made from process().
        std::unique_lock<std::mutex> loaderLock(module.loader_->mutex);
        realtimeAllocations = 0;
        trackRealtimeAllocations = true;
        for (int frame = 0; frame < 64; ++frame) {
            for (int lane = 0; lane < 16; ++lane) {
                const float changing = 0.01f
                    * static_cast<float>((frame + lane) % 64);
                message.inputs[0].values[lane] = changing;
                message.inputs[1].values[lane] = changing;
                message.inputs[2].values[lane] = changing;
                message.inputs[3].values[lane] = changing;
                module.inputs[CellaSFZ::GATE_INPUT].setVoltage(
                    frame % 2 == 0 ? 10.0f : 0.0f, lane);
            }
            module.process(processArgs());
        }
        trackRealtimeAllocations = false;
        loaderLock.unlock();

        require(realtimeAllocations == 0,
            "steady-state expression processing performs no heap allocation");
        require(state.loadCalls.load(std::memory_order_relaxed) == loadCalls
                && state.configureCalls.load(std::memory_order_relaxed)
                    == configureCalls,
            "audio processing performs no load or engine configuration work");
        require(state.renderCalls.load(std::memory_order_relaxed)
                    == renderCalls + 1
                && state.enqueueCalls.load(std::memory_order_relaxed)
                    > enqueueCalls,
            "realtime probe renders exactly one dense expression block");
        require(module.droppedEventCount_ == 0
                && module.activeEngine_->adapter.droppedEventCount() == 0,
            "realtime expression path has bounded queues with no drops");
    }

    // Exercise the real sampler after warming its voice and smoother state.
    // This complements the boundary probe above by covering sfizioso's held-
    // voice expression/modulation/render path under the same allocation hook.
    cella::sfz::SfiziosoEngine engine;
    engine.setSampleRate(48000.0f);
    engine.setMaximumBlockSize(64);
    require(engine.load(validPath).success,
        "real realtime-probe SFZ loads");
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    std::array<std::array<float, 64>, cella::sfz::MaxAudioOutputLanes> polyLeft {};
    std::array<std::array<float, 64>, cella::sfz::MaxAudioOutputLanes> polyRight {};
    std::array<float*, cella::sfz::MaxAudioOutputLanes> polyLeftPointers {};
    std::array<float*, cella::sfz::MaxAudioOutputLanes> polyRightPointers {};
    for (int lane = 0; lane < cella::sfz::MaxAudioOutputLanes; ++lane) {
        polyLeftPointers[lane] = polyLeft[lane].data();
        polyRightPointers[lane] = polyRight[lane].data();
    }
    engine.enqueue(cella::sfz::TimedEngineEvent::sourceCC(0, 0, 20, 0.25f));
    engine.enqueue(cella::sfz::TimedEngineEvent::noteOn(0, 0, 60, 1.0f));
    for (int block = 0; block < 4; ++block)
        engine.render(left.data(), right.data(), 64);

    realtimeAllocations = 0;
    trackRealtimeAllocations = true;
    // Starting previously unused voices must not lazily grow controller
    // smoother storage on the realtime thread.
    for (int lane = 1; lane < 16; ++lane) {
        engine.enqueue(cella::sfz::TimedEngineEvent::sourceCC(
            0, static_cast<uint8_t>(lane), 20, 0.25f));
        engine.enqueue(cella::sfz::TimedEngineEvent::noteOn(
            0, static_cast<uint8_t>(lane), 60, 1.0f));
        engine.enqueue(cella::sfz::TimedEngineEvent::notePitch(
            0, static_cast<uint8_t>(lane), 60, 0.0f));
    }
    for (int block = 0; block < 8; ++block) {
        const float value = static_cast<float>(block + 1) / 8.0f;
        engine.enqueue(cella::sfz::TimedEngineEvent::noteBend(
            7, 0, value * 12.0f));
        engine.enqueue(cella::sfz::TimedEngineEvent::pressure(0, 0, value));
        engine.enqueue(cella::sfz::TimedEngineEvent::timbre(0, 0, value));
        engine.enqueue(cella::sfz::TimedEngineEvent::sourceCC(
            0, 0, 20, value));
        if (block < 4) {
            engine.render(left.data(), right.data(), 64);
        } else {
            engine.renderPolyphonic(polyLeftPointers.data(),
                polyRightPointers.data(), cella::sfz::MaxAudioOutputLanes, 64);
        }
    }
    trackRealtimeAllocations = false;
    require(realtimeAllocations == 0,
        "real sampler voice starts and held expression perform no heap allocation");
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

    CellaSFZ polyphonic;
    polyphonic.outputs[CellaSFZ::LEFT_OUTPUT].channels = 1;
    polyphonic.outputs[CellaSFZ::RIGHT_OUTPUT].channels = 1;
    polyphonic.clearPlaybackBuffers();
    polyphonic.playbackPolyphonic_ = true;
    polyphonic.playbackChannels_ = 3;
    polyphonic.playbackLeft_[0][0] = 0.25f;
    polyphonic.playbackRight_[0][0] = -0.25f;
    polyphonic.playbackLeft_[1][0] = 0.5f;
    polyphonic.playbackRight_[1][0] = -0.5f;
    polyphonic.playbackLeft_[2][0] = 0.75f;
    polyphonic.playbackRight_[2][0] = -0.75f;
    polyphonic.playbackFrame_ = 0;
    polyphonic.process(processArgs());
    require(polyphonic.outputs[CellaSFZ::LEFT_OUTPUT].getChannels() == 3
            && polyphonic.outputs[CellaSFZ::RIGHT_OUTPUT].getChannels() == 3,
        "polyphonic mode publishes the captured V/OCT lane count on both jacks");
    for (int channel = 0; channel < 3; ++channel) {
        const float expected = 3.0f * static_cast<float>(channel + 1);
        requireNear(polyphonic.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(channel),
            expected, 1.0e-6f, "polyphonic left lane routing");
        requireNear(polyphonic.outputs[CellaSFZ::RIGHT_OUTPUT].getVoltage(channel),
            -expected, 1.0e-6f, "polyphonic right lane routing");
    }

    CellaSFZ polyphonicMono;
    polyphonicMono.outputs[CellaSFZ::LEFT_OUTPUT].channels = 1;
    polyphonicMono.outputs[CellaSFZ::RIGHT_OUTPUT].channels = 0;
    polyphonicMono.clearPlaybackBuffers();
    polyphonicMono.playbackPolyphonic_ = true;
    polyphonicMono.playbackChannels_ = 2;
    polyphonicMono.playbackLeft_[0][0] = 1.0f;
    polyphonicMono.playbackRight_[0][0] = 0.0f;
    polyphonicMono.playbackLeft_[1][0] = 0.0f;
    polyphonicMono.playbackRight_[1][0] = 1.0f;
    polyphonicMono.playbackFrame_ = 0;
    polyphonicMono.process(processArgs());
    requireNear(polyphonicMono.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(0), 6.0f,
        1.0e-6f, "polyphonic left-only fold-down lane zero");
    requireNear(polyphonicMono.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(1), 6.0f,
        1.0e-6f, "polyphonic left-only fold-down lane one");
}

void testPolyphonicRenderSelection()
{
    const auto state = std::make_shared<RecordingEngineState>();
    CellaSFZ module;
    module.activeEngine_ = new CellaSFZ::EngineBundle(
        std::make_unique<RecordingEngine>(state));
    module.inputs[CellaSFZ::VOCT_INPUT].channels = 5;
    module.params[CellaSFZ::POLY_OUTPUT_PARAM].setValue(1.0f);
    module.finishCaptureBlock();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->renderedBlockSizes.empty()
                && state->polyphonicBlockSizes.size() == 1,
            "polyphonic toggle selects the isolated render path at a block boundary");
        require(state->polyphonicChannelCounts.back()
                == cella::sfz::MaxAudioOutputLanes,
            "sampler renders every source lane so tails keep advancing");
    }
    require(module.playbackPolyphonic_ && module.playbackChannels_ == 5,
        "polyphonic playback exposes the current V/OCT channel count");

    module.params[CellaSFZ::POLY_OUTPUT_PARAM].setValue(0.0f);
    module.finishCaptureBlock();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->renderedBlockSizes.size() == 1
                && state->polyphonicBlockSizes.size() == 1,
            "disabling the toggle restores the mixed stereo render path");
    }
    require(!module.playbackPolyphonic_ && module.playbackChannels_ == 1,
        "mixed playback publishes one channel on each jack");

    module.outputs[CellaSFZ::LEFT_OUTPUT].channels = 1;
    module.outputs[CellaSFZ::RIGHT_OUTPUT].channels = 1;
    seedPlayback(module, 0.25f, -0.25f);
    module.captureFrame_ = module.activeRenderQuantum_ - 1;
    module.params[CellaSFZ::POLY_OUTPUT_PARAM].setValue(1.0f);
    module.process(processArgs());
    require(module.outputs[CellaSFZ::LEFT_OUTPUT].getChannels() == 1,
        "mode change does not reinterpret the final sample of the old block");
    requireNear(module.outputs[CellaSFZ::LEFT_OUTPUT].getVoltage(), 3.0f,
        1.0e-6f, "mode change preserves old-block left audio");
    requireNear(module.outputs[CellaSFZ::RIGHT_OUTPUT].getVoltage(), -3.0f,
        1.0e-6f, "mode change preserves old-block right audio");
    module.process(processArgs());
    require(module.outputs[CellaSFZ::LEFT_OUTPUT].getChannels() == 5
            && module.outputs[CellaSFZ::RIGHT_OUTPUT].getChannels() == 5,
        "polyphonic channel count begins with the newly rendered block");
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

void testExpressionExpanderStreamsAndRestoration()
{
    char validPath[] = "/tmp/cella-sfz-expression-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create expression module path");
    close(descriptor);

    const auto state = std::make_shared<RecordingEngineState>();
    state->metadata.namedControllers = {
        { 20, "Brightness", 0.25f }, { 21, "Noise", 0.75f }
    };
    state->metadata.latchedKeyswitches = {
        { 24, "Legato" }, { 25, "" }
    };
    CellaSFZ module([state]() {
        return std::make_unique<RecordingEngine>(state);
    });
    CellaSFZExpression expander;
    module.model = modelCellaSFZ;
    expander.model = modelCellaSFZExpression;
    module.rightExpander.module = &expander;
    expander.leftExpander.module = &module;

    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "expression recording engine loads");
    require(module.swapEngineAtBlockBoundary(),
        "expression recording engine activates");

    auto& message = *static_cast<cella::sfz::ExpressionMessage*>(
        expander.leftExpander.consumerMessage);
    message = {};
    message.magic = cella::sfz::ExpressionMessageMagic;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 0,
        "connected expander with no assignments publishes zero assigned CCs");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        std::array<bool, 16> bendNeutral {}, pressureNeutral {}, timbreNeutral {};
        for (const auto& event : state->events) {
            if (event.channel >= 16 || event.value != 0.0f)
                continue;
            bendNeutral[event.channel] |= event.type
                == cella::sfz::EngineEventType::NoteBend;
            pressureNeutral[event.channel] |= event.type
                == cella::sfz::EngineEventType::Pressure;
            timbreNeutral[event.channel] |= event.type
                == cella::sfz::EngineEventType::Timbre;
        }
        require(std::all_of(bendNeutral.begin(), bendNeutral.end(), [](bool v) { return v; })
                && std::all_of(pressureNeutral.begin(), pressureNeutral.end(), [](bool v) { return v; })
                && std::all_of(timbreNeutral.begin(), timbreNeutral.end(), [](bool v) { return v; }),
            "new engine receives neutral dedicated expression on every lane");
        state->events.clear();
    }
    module.articulationState_.fill(-1);

    module.inputs[CellaSFZ::VOCT_INPUT].channels = 16;
    module.inputs[CellaSFZ::GATE_INPUT].channels = 16;
    for (int lane = 0; lane < 16; ++lane) {
        module.inputs[CellaSFZ::VOCT_INPUT].setVoltage(0.01f * lane, lane);
        module.inputs[CellaSFZ::GATE_INPUT].setVoltage(10.0f, lane);
    }
    message = {};
    message.magic = cella::sfz::ExpressionMessageMagic;
    message.inputs[0].channels = 16;
    message.inputs[1].channels = 16;
    message.inputs[2].channels = 16;
    message.inputs[3].channels = 16;
    message.assignments[0] = 20;
    for (int lane = 0; lane < 16; ++lane) {
        message.inputs[0].values[lane] = 0.1f * (lane + 1);
        message.inputs[1].values[lane] = 0.5f * (lane + 1);
        message.inputs[2].values[lane] = 0.25f * (lane + 1);
        message.inputs[3].values[lane] = 10.0f * lane / 15.0f;
        message.articulationIndices[lane] = lane % 2;
    }
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "connected expander publishes one distinct valid CC assignment");

    const uint64_t originalGeneration = module.activeEngine_->generation;
    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "expression replacement engine loads");
    const auto replacementStatus = module.statusSnapshot();
    require(replacementStatus->generation != originalGeneration
            && !module.readyStatusDescribesActiveEngine(*replacementStatus),
        "replacement metadata remains hidden while the previous engine is active");
    require(module.swapEngineAtBlockBoundary(),
        "expression replacement activates at an audio boundary");
    require(module.readyStatusDescribesActiveEngine(*replacementStatus)
            && module.displayRuntimeSnapshot().assignedCcCount == 0,
        "instrument replacement resets assigned CC telemetry before READY");
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "valid expander assignments are recounted for the replacement instrument");

    std::array<bool, 16> bend {}, pressure {}, timbre {}, named {}, switches {};
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& event : state->events) {
            const size_t lane = event.channel;
            if (event.type == cella::sfz::EngineEventType::NoteBend) {
                bend[lane] = true;
                requireNear(event.value, 1.2f * (lane + 1), 1.0e-5f,
                    "lane bend remains distinct");
            } else if (event.type == cella::sfz::EngineEventType::Pressure) {
                pressure[lane] = true;
                requireNear(event.value, std::min(1.0f, 0.05f * (lane + 1)), 1.0e-5f,
                    "lane pressure remains distinct");
            } else if (event.type == cella::sfz::EngineEventType::Timbre) {
                timbre[lane] = true;
                requireNear(event.value, 0.025f * (lane + 1), 1.0e-5f,
                    "lane timbre remains distinct");
            } else if (event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 20) {
                named[lane] = true;
                requireNear(event.value, lane / 15.0f, 1.0e-5f,
                    "lane named control remains distinct");
            } else if (event.type == cella::sfz::EngineEventType::KeyswitchOn) {
                switches[lane] = true;
                require(event.note == 24 + lane % 2,
                    "lane keyswitch remains distinct");
            }
        }
        state->events.clear();
    }
    for (int lane = 0; lane < 16; ++lane) {
        require(bend[lane] && pressure[lane] && timbre[lane] && named[lane]
                && switches[lane],
            "all 16 expression lanes reach the engine independently");
    }

    message.inputs[0].channels = 1;
    message.inputs[0].values[0] = 0.5f;
    message.inputs[1].channels = 3;
    message.inputs[1].values[0] = 1.0f;
    message.inputs[1].values[1] = 2.0f;
    message.inputs[1].values[2] = 3.0f;
    message.inputs[2].channels = 0;
    message.inputs[3].channels = 0;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "assignment count stays stable across expression channel changes");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        int broadcastBends = 0;
        bool removedPressureReset = false;
        bool timbreReset = false;
        bool namedDefault = false;
        for (const auto& event : state->events) {
            if (event.type == cella::sfz::EngineEventType::NoteBend
                && std::abs(event.value - 6.0f) < 1.0e-5f)
                ++broadcastBends;
            if (event.type == cella::sfz::EngineEventType::Pressure
                && event.channel == 3 && event.value == 0.0f)
                removedPressureReset = true;
            if (event.type == cella::sfz::EngineEventType::Timbre
                && event.channel == 15 && event.value == 0.0f)
                timbreReset = true;
            if (event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 20 && event.channel == 15
                && std::abs(event.value - 0.25f) < 1.0e-6f)
                namedDefault = true;
        }
        require(broadcastBends == 15,
            "monophonic bend updates every lane whose value changed");
        for (float bend : module.bendState_)
            requireNear(bend, 6.0f, 1.0e-6f,
                "monophonic bend state broadcasts to every note lane");
        require(removedPressureReset && timbreReset && namedDefault,
            "channel shrink and cable removal restore expression defaults");
        state->events.clear();
    }

    // Bend is captured on every Rack frame, rather than being reduced to the
    // render-quantum boundary like the normalized expression controls.
    for (int frame = 0; frame < 5; ++frame)
        module.process(processArgs());
    message.inputs[0].values[0] = 0.75f;
    for (int frame = 5; frame < 32; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        int offsetBends = 0;
        for (const auto& event : state->events) {
            if (event.type == cella::sfz::EngineEventType::NoteBend
                && event.frameOffset == 5
                && std::abs(event.value - 9.0f) < 1.0e-5f)
                ++offsetBends;
        }
        require(offsetBends == 16,
            "bend changes retain their sample offset for every broadcast lane");
        state->events.clear();
    }
    message.inputs[0].values[0] = 0.5f;

    message.inputs[1].channels = 1;
    message.inputs[1].values[0] = 9.0f;
    message.assignments[0] = 21;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "reassignment keeps one assigned CC counted");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        bool oldDefault = false;
        bool newValue = false;
        for (const auto& event : state->events) {
            oldDefault |= event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 20 && std::abs(event.value - 0.25f) < 1.0e-6f;
            newValue |= event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 21 && std::abs(event.value - 0.75f) < 1.0e-6f;
        }
        require(oldDefault && newValue,
            "reassignment restores old CC then applies new CC default");
        state->events.clear();
    }

    message.assignments[0] = 21;
    message.assignments[1] = 21;
    message.inputs[4].channels = 1;
    message.inputs[4].values[0] = 2.0f;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "duplicate assignments count as one assigned CC");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        std::array<float, 16> finalValues {};
        std::array<bool, 16> seen {};
        for (const auto& event : state->events) {
            if (event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 21) {
                finalValues[event.channel] = event.value;
                seen[event.channel] = true;
            }
        }
        for (int lane = 0; lane < 16; ++lane) {
            require(seen[lane] && std::abs(finalValues[lane] - 0.2f) < 1.0e-6f,
                "duplicate CC assignments have one deterministic last-slot owner");
        }
        state->events.clear();
    }

    message.assignments[0] = -1;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 1,
        "removing one duplicate keeps the remaining assignment counted");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type == cella::sfz::EngineEventType::SourceCC
                            && event.note == 21
                            && std::abs(event.value - 0.75f) < 1.0e-6f;
                    }),
            "removing a duplicate slot does not reset a CC still owned elsewhere");
        state->events.clear();
    }

    module.rightExpander.module = nullptr;
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    require(module.displayRuntimeSnapshot().assignedCcCount == 0,
        "expander removal publishes zero assigned CCs");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        bool bendReset = false;
        bool pressureReset = false;
        bool ccReset = false;
        for (const auto& event : state->events) {
            bendReset |= event.type == cella::sfz::EngineEventType::NoteBend
                && event.channel == 15 && event.value == 0.0f;
            pressureReset |= event.type == cella::sfz::EngineEventType::Pressure
                && event.channel == 15 && event.value == 0.0f;
            ccReset |= event.type == cella::sfz::EngineEventType::SourceCC
                && event.note == 21 && std::abs(event.value - 0.75f) < 1.0e-6f;
        }
        require(bendReset && pressureReset && ccReset,
            "hot-disconnecting the expander restores neutral and SFZ defaults");
    }
    unlink(validPath);
}

void testAbsentExpanderPreservesInstrumentKeyswitch()
{
    char validPath[] = "/tmp/cella-sfz-no-expander-valid-XXXXXX";
    const int descriptor = mkstemp(validPath);
    require(descriptor >= 0, "create no-expander SFZ path");
    close(descriptor);

    const auto state = std::make_shared<RecordingEngineState>();
    state->metadata.latchedKeyswitches = {
        { 24, "First" }, { 25, "Declared default" }
    };
    CellaSFZ module([state]() {
        return std::make_unique<RecordingEngine>(state);
    });
    module.model = modelCellaSFZ;
    module.loadInstrument(validPath);
    waitForState(module, CellaSFZ::StatusState::Ready,
        "no-expander recording engine loads");
    require(module.swapEngineAtBlockBoundary(),
        "no-expander recording engine activates");
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type == cella::sfz::EngineEventType::KeyswitchOn
                            || event.type == cella::sfz::EngineEventType::KeyswitchOff;
                    }),
            "absent expander leaves the instrument keyswitch default untouched");
        state->events.clear();
    }


    CellaSFZExpression expander;
    expander.model = modelCellaSFZExpression;
    expander.leftExpander.module = &module;
    module.rightExpander.module = &expander;
    require(std::all_of(expander.expressionMessages_[1].articulationIndices.begin(),
                expander.expressionMessages_[1].articulationIndices.end(),
                [](int16_t selection) { return selection == -1; }),
        "new expander message begins without an articulation override");
    expander.process(processArgs());
    *static_cast<cella::sfz::ExpressionMessage*>(
        expander.leftExpander.consumerMessage)
        = *static_cast<cella::sfz::ExpressionMessage*>(
            expander.leftExpander.producerMessage);
    for (int frame = 0; frame < 32; ++frame)
        module.process(processArgs());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(std::none_of(state->events.begin(), state->events.end(),
                    [](const cella::sfz::TimedEngineEvent& event) {
                        return event.type == cella::sfz::EngineEventType::KeyswitchOn
                            || event.type == cella::sfz::EngineEventType::KeyswitchOff;
                    }),
            "attached untouched expander leaves sw_default untouched");
    }
    unlink(validPath);
}

void testExpanderMatchingAndPersistence()
{
    CellaSFZ base;
    CellaSFZExpression expander;
    base.model = modelCellaSFZ;
    expander.model = modelCellaSFZExpression;
    base.rightExpander.module = &expander;
    expander.leftExpander.module = &base;
    expander.assignments_[0].store(20, std::memory_order_relaxed);
    expander.assignments_[1].store(21, std::memory_order_relaxed);
    auto& feedback = *static_cast<cella::sfz::ExpressionFeedback*>(
        base.rightExpander.consumerMessage);
    feedback = {};
    feedback.magic = cella::sfz::ExpressionMessageMagic;
    feedback.metadataGeneration = 1;
    feedback.keyswitchCount = 2;
    feedback.activeLanes = 2;
    feedback.assignableCCs[20] = 1;
    feedback.activeArticulations[0] = 0;
    feedback.activeArticulations[1] = 1;
    expander.process(processArgs());
    require(expander.assignments_[0].load(std::memory_order_relaxed) == 20
            && expander.assignments_[1].load(std::memory_order_relaxed) == -1,
        "instrument change retains only CCs exposed by the new instrument");

    // Generation numbers are local to a base module. Moving an expander
    // directly between two bases at generation 1 must still revalidate its
    // saved controls and manual articulation against the new instrument.
    CellaSFZ secondBase;
    secondBase.model = modelCellaSFZ;
    base.rightExpander.module = nullptr;
    secondBase.rightExpander.module = &expander;
    expander.leftExpander.module = &secondBase;
    auto& secondFeedback = *static_cast<cella::sfz::ExpressionFeedback*>(
        secondBase.rightExpander.consumerMessage);
    secondFeedback = {};
    secondFeedback.magic = cella::sfz::ExpressionMessageMagic;
    secondFeedback.metadataGeneration = 1;
    secondFeedback.keyswitchCount = 1;
    secondFeedback.assignableCCs[23] = 1;
    expander.assignments_[0].store(20, std::memory_order_relaxed);
    expander.manualArticulation_.store(1, std::memory_order_relaxed);
    expander.process(processArgs());
    require(expander.assignments_[0].load(std::memory_order_relaxed) == -1
            && expander.manualArticulation_.load(std::memory_order_relaxed) == -1,
        "moving between equal-generation bases revalidates saved selections");

    secondBase.rightExpander.module = nullptr;
    base.rightExpander.module = &expander;
    expander.leftExpander.module = &base;
    expander.process(processArgs());
    expander.assignments_[0].store(20, std::memory_order_relaxed);

    auto& message = *static_cast<cella::sfz::ExpressionMessage*>(
        expander.leftExpander.producerMessage);
    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].channels = 1;
    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].setVoltage(10.0f);
    expander.process(processArgs());
    require(std::all_of(message.articulationIndices.begin(),
                message.articulationIndices.end(),
                [](int16_t selection) { return selection == 1; }),
        "one-channel articulation CV broadcasts its quantized selection");

    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].channels = 2;
    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].setVoltage(0.0f, 0);
    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].setVoltage(10.0f, 1);
    expander.process(processArgs());
    require(message.articulationIndices[0] == 0
            && message.articulationIndices[1] == 1,
        "polyphonic articulation CV maps channel-for-lane");

    // Rack UI widgets expect a live APP event state during destruction. This
    // headless executable deliberately leaves these few menu fixtures for the
    // process to reclaim after exercising the real menu/widget code paths.
    auto* articulationDisplay = new CellaExpressionDisplay;
    articulationDisplay->module = &expander;
    articulationDisplay->slot = -1;
    require(articulationDisplay->displayText() == "POLY",
        "articulation display reports divergent active lane selections");

    auto metadata = std::make_shared<cella::sfz::InstrumentMetadata>();
    metadata->namedControllers = {
        { 20, "Brightness", 0.25f }, { 23, "CC 23", 0.0f },
        { 74, "Reserved", 0.5f }
    };
    metadata->latchedKeyswitches = { { 24, "Legato" }, { 25, "" } };
    std::atomic_store_explicit(&base.loader_->instrumentMetadata,
        std::shared_ptr<const cella::sfz::InstrumentMetadata>(metadata),
        std::memory_order_release);
    feedback.activeArticulations[0] = 1;
    feedback.activeArticulations[1] = 1;
    expander.process(processArgs());
    require(articulationDisplay->displayText() == "C#1",
        "unlabeled keyswitches use musical note names");
    auto* namedDisplay = new CellaExpressionDisplay;
    namedDisplay->module = &expander;
    namedDisplay->slot = 0;
    require(namedDisplay->displayText() == "Brightness",
        "assigned control display uses the instrument label");

    auto* assignmentMenu = new ui::Menu;
    namedDisplay->appendSelectionMenu(assignmentMenu);
    ui::MenuItem* brightnessItem = nullptr;
    bool hasUnassigned = false;
    bool hasNumericFallback = false;
    bool hasReservedTimbre = false;
    for (Widget* child : assignmentMenu->children) {
        auto* item = dynamic_cast<ui::MenuItem*>(child);
        if (!item)
            continue;
        hasUnassigned |= item->text == "Unassigned";
        hasNumericFallback |= item->text == "CC 23";
        hasReservedTimbre |= item->text.find("CC74") != std::string::npos;
        if (item->text == "Brightness (CC20)")
            brightnessItem = item;
    }
    require(hasUnassigned && brightnessItem && hasNumericFallback
            && !hasReservedTimbre,
        "assignment menu exposes labeled and fallback CCs but excludes CC74");
    expander.assignments_[0].store(-1, std::memory_order_relaxed);
    expander.assignments_[1].store(20, std::memory_order_relaxed);
    brightnessItem->doAction(false);
    require(expander.assignments_[0].load(std::memory_order_relaxed) == 20,
        "assignment menu action stores the selected CC number");
    require(expander.assignments_[1].load(std::memory_order_relaxed) == -1,
        "assignment menu gives each CC a single slot owner");

    auto* articulationMenu = new ui::Menu;
    articulationDisplay->appendSelectionMenu(articulationMenu);
    bool hasLabeledSwitch = false;
    bool hasNamedFallback = false;
    for (Widget* child : articulationMenu->children) {
        auto* item = dynamic_cast<ui::MenuItem*>(child);
        if (!item)
            continue;
        hasLabeledSwitch |= item->text == "Legato";
        hasNamedFallback |= item->text == "C#1";
    }
    require(hasLabeledSwitch && hasNamedFallback,
        "articulation menu exposes labeled and note-named switches");

    expander.manualArticulation_.store(1, std::memory_order_relaxed);
    expander.inputs[CellaSFZExpression::ARTICULATION_INPUT].channels = 0;
    expander.process(processArgs());
    require(std::all_of(message.articulationIndices.begin(),
                message.articulationIndices.end(),
                [](int16_t selection) { return selection == 1; }),
        "manual articulation selection applies when selector CV is absent");

    json_t* saved = expander.dataToJson();
    CellaSFZExpression restored;
    restored.dataFromJson(saved);
    require(restored.assignments_[0].load(std::memory_order_relaxed) == 20,
        "named control assignment persists by CC number");
    require(restored.manualArticulation_.load(std::memory_order_relaxed) == 1,
        "manual articulation persists");
    json_decref(saved);
}

} // namespace

int main()
{
    try {
        testLexicalPathRoots();
        std::puts("PASS: cross-platform lexical path roots");
        testPermanentIdsAndDefaults();
        std::puts("PASS: permanent IDs and defaults");
        testDisplayFormattingAndPersistence();
        std::puts("PASS: display formatting, thresholds, warnings, and persistence");
        testFolderNavigation();
        std::puts("PASS: previous/next folder navigation");
        testFolderSnapshotFailures();
        std::puts("PASS: unavailable and absent folder snapshots");
        testAsynchronousMissingLoad();
        std::puts("PASS: asynchronous coalesced loading");
        testDisplayRuntimeTelemetry();
        std::puts("PASS: audio-safe display runtime telemetry");
        testDisplayFileInteractionsAndUnload();
        std::puts("PASS: display file interactions and pop-free unload");
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
        testMaximumDensityEventBound();
        std::puts("PASS: maximum-density expression and tail event bound");
        testRealtimeExpressionPath();
        std::puts("PASS: allocation-free lock-free realtime expression path");
        testOutputCalibrationAndRouting();
        std::puts("PASS: output calibration and routing");
        testPolyphonicRenderSelection();
        std::puts("PASS: polyphonic render selection and channel routing");
        testJsonPersistence();
        std::puts("PASS: JSON persistence");
        testExpressionExpanderStreamsAndRestoration();
        std::puts("PASS: 16-lane expression expander streams and restoration");
        testAbsentExpanderPreservesInstrumentKeyswitch();
        std::puts("PASS: absent expander preserves instrument keyswitch state");
        testExpanderMatchingAndPersistence();
        std::puts("PASS: expander matching and persistence");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("All SFZ module tests passed");
    return 0;
}
