#include "components.hpp"
#include "plugin.hpp"
#include "sfz/BlockAdapter.hpp"
#include "sfz/Expression.hpp"
#include "sfz/NoteLaneTracker.hpp"
#include "sfz/SfiziosoEngine.hpp"

#include <osdialog.h>
#include <patch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kSchemaVersion = 2;
constexpr int kDefaultRenderQuantum = 32;
constexpr int kMaximumRenderQuantum = 64;

bool isRenderQuantum(int frames) noexcept
{
    return frames == 16 || frames == 32 || frames == 64;
}
// Replacement fades are deliberately short, but long enough to move both
// sides of an engine swap through digital silence instead of introducing a
// discontinuity. The duration is sample-rate independent.
constexpr float kLifecycleFadeSeconds = 0.005f;
// Sfizz's default -7.35 dB master gain and squared CC7 value of 100/127 put a
// unity sample at about 0.266 before velocity. Fifteen volts makes the default
// 0.8-velocity path comparable to Squinky at its defaults, while a full-scale
// single voice at maximum velocity remains below 4 V at LEVEL 100%.
constexpr float kNominalOutputVolts = 15.0f;
// Default single-voice audio has at least 6 dB of headroom before limiting.
constexpr float kLimiterKneeVolts = 16.0f;
constexpr float kLimiterCeilingVolts = 20.0f;

float softLimit(float voltage) noexcept
{
    const float magnitude = std::abs(voltage);
    if (magnitude <= kLimiterKneeVolts)
        return voltage;
    const float limited = kLimiterKneeVolts
        + (kLimiterCeilingVolts - kLimiterKneeVolts)
            * std::tanh((magnitude - kLimiterKneeVolts)
                / (kLimiterCeilingVolts - kLimiterKneeVolts));
    return std::copysign(limited, voltage);
}

class LoaderThreadReaper {
public:
    LoaderThreadReaper()
        : thread_([this]() { run(); })
    {
    }

    ~LoaderThreadReaper()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    void adopt(std::thread& worker) noexcept
    {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workers_.push_back(std::move(worker));
            }
            cv_.notify_one();
        } catch (...) {
            // Allocation failure is the only case where retaining joinability
            // is impossible. Preserve non-blocking module destruction.
            if (worker.joinable())
                worker.detach();
        }
    }

private:
    void run() noexcept
    {
        for (;;) {
            std::thread worker;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || !workers_.empty(); });
                if (workers_.empty()) {
                    if (stopping_)
                        return;
                    continue;
                }
                worker = std::move(workers_.back());
                workers_.pop_back();
            }
            if (worker.joinable()) {
                try {
                    worker.join();
                } catch (...) {
                    if (worker.joinable())
                        worker.detach();
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::thread thread_;
    bool stopping_ { false };
};

LoaderThreadReaper& loaderThreadReaper()
{
    static LoaderThreadReaper reaper;
    return reaper;
}

struct LexicalPath {
    std::string root;
    std::vector<std::string> components;
};

LexicalPath splitPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    LexicalPath result;
    size_t offset = 0;
    if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
        const size_t serverEnd = path.find('/', 2);
        size_t shareStart = serverEnd == std::string::npos
            ? std::string::npos
            : serverEnd + 1;
        while (shareStart != std::string::npos && shareStart < path.size()
            && path[shareStart] == '/') {
            ++shareStart;
        }
        const size_t shareEnd = shareStart == std::string::npos
            ? std::string::npos
            : path.find('/', shareStart);
        if (serverEnd > 2 && shareStart < path.size()) {
            result.root = "//" + path.substr(2, serverEnd - 2) + "/"
                + path.substr(shareStart,
                    shareEnd == std::string::npos ? std::string::npos
                                                  : shareEnd - shareStart);
            std::transform(result.root.begin(), result.root.end(), result.root.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            offset = shareEnd == std::string::npos ? path.size() : shareEnd + 1;
        } else {
            result.root = "/";
            offset = 1;
        }
    } else if (path.size() >= 2 && path[1] == ':') {
        result.root = path.substr(0, 2);
        result.root[0] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(result.root[0])));
        offset = 2;
    } else if (!path.empty() && path[0] == '/') {
        result.root = "/";
        offset = 1;
    }
    while (offset < path.size()) {
        while (offset < path.size() && path[offset] == '/')
            ++offset;
        const size_t end = path.find('/', offset);
        const std::string component = path.substr(offset,
            end == std::string::npos ? std::string::npos : end - offset);
        if (!component.empty() && component != ".") {
            if (component == ".." && !result.components.empty()
                && result.components.back() != "..")
                result.components.pop_back();
            else
                result.components.push_back(component);
        }
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
    return result;
}

std::string relativeToCurrentPatch(const std::string& path)
{
    if (path.empty() || !APP || !APP->patch || APP->patch->path.empty())
        return {};
    try {
        const LexicalPath target = splitPath(system::getAbsolute(path));
        const LexicalPath base = splitPath(
            system::getAbsolute(system::getDirectory(APP->patch->path)));
        if (target.root != base.root)
            return {};
        size_t common = 0;
        while (common < target.components.size()
            && common < base.components.size()
            && target.components[common] == base.components[common])
            ++common;
        std::string relative;
        for (size_t index = common; index < base.components.size(); ++index)
            relative += relative.empty() ? ".." : "/..";
        for (size_t index = common; index < target.components.size(); ++index) {
            if (!relative.empty())
                relative += '/';
            relative += target.components[index];
        }
        return relative;
    } catch (...) {
        return {};
    }
}

std::string persistedRelativeCandidate(const std::string& relative)
{
    if (relative.empty() || !APP || !APP->patch || APP->patch->path.empty())
        return {};
    try {
        return system::getAbsolute(system::join(
            system::getDirectory(APP->patch->path), relative));
    } catch (...) {
        return {};
    }
}

std::string shorten(const std::string& text, size_t maximum)
{
    if (text.size() <= maximum)
        return text;
    if (maximum <= 3)
        return text.substr(0, maximum);
    return text.substr(0, maximum - 3) + "...";
}

std::string shortenMiddle(const std::string& text, size_t maximum)
{
    if (text.size() <= maximum)
        return text;
    if (maximum <= 3)
        return text.substr(0, maximum);
    const size_t available = maximum - 3;
    const size_t beginning = (available + 1) / 2;
    return text.substr(0, beginning) + "..."
        + text.substr(text.size() - (available - beginning));
}

bool hasSfzExtension(const std::string& path)
{
    std::string extension = system::getExtension(path);
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".sfz";
}

std::string filenameWithoutSfzExtension(const std::string& filename)
{
    const std::string extension = system::getExtension(filename);
    return hasSfzExtension(filename)
        ? filename.substr(0, filename.size() - extension.size())
        : filename;
}

struct FolderSnapshot {
    std::vector<std::string> instruments;
    size_t selectedIndex { std::numeric_limits<size_t>::max() };
    bool inspected { false };

    bool hasSelection() const noexcept
    {
        return selectedIndex < instruments.size();
    }
};

FolderSnapshot snapshotSfzFolder(const std::string& selectedPath)
{
    FolderSnapshot snapshot;
    if (selectedPath.empty())
        return snapshot;
    try {
        const std::string selected = system::getAbsolute(selectedPath);
        const std::string directory = system::getDirectory(selected);
        if (directory.empty() || !system::isDirectory(directory))
            return snapshot;
        for (const std::string& entry : system::getEntries(directory)) {
            if (!system::isFile(entry))
                continue;
            std::string extension = system::getExtension(entry);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (extension == ".sfz")
                snapshot.instruments.push_back(system::getAbsolute(entry));
        }
        snapshot.inspected = true;
        std::sort(snapshot.instruments.begin(), snapshot.instruments.end());
        const auto selectedIt = std::find(
            snapshot.instruments.begin(), snapshot.instruments.end(), selected);
        if (selectedIt != snapshot.instruments.end()) {
            snapshot.selectedIndex = static_cast<size_t>(
                std::distance(snapshot.instruments.begin(), selectedIt));
        }
    } catch (...) {
        snapshot = {};
    }
    return snapshot;
}

std::string folderPosition(size_t oneBasedIndex, size_t count)
{
    if (oneBasedIndex == 0 || count == 0 || oneBasedIndex > count)
        return {};
    const size_t width = std::to_string(count).size();
    return rack::string::f("%0*zu/%zu", static_cast<int>(width),
        oneBasedIndex, count);
}

std::string memoryLabel(size_t bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
        return rack::string::f("%.1f GB", static_cast<double>(bytes)
                / (1024.0 * 1024.0 * 1024.0));
    if (bytes >= 1024u * 1024u)
        return rack::string::f("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024u)
        return rack::string::f("%.1f KB", static_cast<double>(bytes) / 1024.0);
    return rack::string::f("%zu B", bytes);
}

enum class VoicePressure : uint8_t {
    Normal,
    Amber,
    Red,
};

VoicePressure voicePressureFor(int voices, int limit) noexcept
{
    if (limit <= 0)
        return VoicePressure::Normal;
    const int clampedVoices = std::max(0, voices);
    if (static_cast<int64_t>(clampedVoices) * 100
        >= static_cast<int64_t>(limit) * 90)
        return VoicePressure::Red;
    if (static_cast<int64_t>(clampedVoices) * 100
        >= static_cast<int64_t>(limit) * 75)
        return VoicePressure::Amber;
    return VoicePressure::Normal;
}

class VoicePeakHold {
public:
    static constexpr uint64_t HoldNanoseconds = 400000000ull;

    int observe(int current, uint64_t nowNanoseconds) noexcept
    {
        current = std::max(0, current);
        if (nowNanoseconds >= expiresAtNanoseconds_ || current >= peak_) {
            peak_ = current;
            expiresAtNanoseconds_ = nowNanoseconds + HoldNanoseconds;
        }
        return std::max(current, peak_);
    }

    void reset() noexcept
    {
        peak_ = 0;
        expiresAtNanoseconds_ = 0;
    }

private:
    int peak_ { 0 };
    uint64_t expiresAtNanoseconds_ { 0 };
};

enum class DisplayWarning : uint8_t {
    None,
    EventDrop,
    OutputLimiting,
};

struct DisplayRuntimeSnapshot {
    int heldLanes { 0 };
    int activeVoices { 0 };
    int peakVoices { 0 };
    int voiceLimit { 0 };
    int assignedCcCount { 0 };
    uint64_t eventDropCount { 0 };
    uint64_t outputLimitingCount { 0 };
    uint64_t audioTimeNanoseconds { 0 };
    uint64_t eventDropWarningUntilNanoseconds { 0 };
    uint64_t outputLimitingWarningUntilNanoseconds { 0 };
};

DisplayWarning displayWarningFor(const DisplayRuntimeSnapshot& runtime) noexcept
{
    if (runtime.audioTimeNanoseconds
        < runtime.eventDropWarningUntilNanoseconds)
        return DisplayWarning::EventDrop;
    if (runtime.audioTimeNanoseconds
        < runtime.outputLimitingWarningUntilNanoseconds)
        return DisplayWarning::OutputLimiting;
    return DisplayWarning::None;
}

} // namespace

struct CellaSFZ : Module {
    // These IDs are part of the patch format. Append new IDs; never reorder.
    enum ParamId {
        LEVEL_PARAM = 0,
        OCTAVE_PARAM,
        TUNE_PARAM,
        LOAD_PARAM,
        PREVIOUS_PARAM,
        NEXT_PARAM,
        POLY_OUTPUT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        VOCT_INPUT = 0,
        GATE_INPUT,
        VELOCITY_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT = 0,
        RIGHT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LIGHTS_LEN = 0
    };

    static_assert(LEVEL_PARAM == 0 && OCTAVE_PARAM == 1 && TUNE_PARAM == 2
            && LOAD_PARAM == 3 && PREVIOUS_PARAM == 4 && NEXT_PARAM == 5
            && POLY_OUTPUT_PARAM == 6 && PARAMS_LEN == 7,
        "Cella SFZ parameter IDs are part of the patch format");
    static_assert(VOCT_INPUT == 0 && GATE_INPUT == 1 && VELOCITY_INPUT == 2
            && INPUTS_LEN == 3,
        "Cella SFZ input IDs are part of the patch format");
    static_assert(LEFT_OUTPUT == 0 && RIGHT_OUTPUT == 1 && OUTPUTS_LEN == 2,
        "Cella SFZ output IDs are part of the patch format");
    static_assert(static_cast<size_t>(cella::sfz::MaxAudioOutputLanes)
            == cella::sfz::MaxNoteLanes,
        "every Rack note lane needs one stereo output lane");

    enum class StatusState : uint8_t {
        Unloaded,
        Loading,
        Ready,
        Error,
        Missing,
    };

    enum class TailBehavior : uint8_t {
        PreserveAll,
        KeepNewest,
        CutOnRetrigger,
    };

    enum class DisplayPage : uint8_t {
        Play = 0,
        Info = 1,
    };

    struct StatusSnapshot {
        StatusSnapshot() = default;
        StatusSnapshot(StatusState state, std::string filename,
            std::string message, int regionCount, size_t estimatedSampleBytes)
            : state(state)
            , filename(std::move(filename))
            , message(std::move(message))
            , regionCount(regionCount)
            , estimatedSampleBytes(estimatedSampleBytes)
        {
        }

        StatusState state { StatusState::Unloaded };
        std::string filename;
        std::string message;
        int regionCount { 0 };
        size_t estimatedSampleBytes { 0 };
        std::string absolutePath;
        size_t folderIndex { 0 }; // one-based; zero means unavailable
        size_t folderCount { 0 };
        size_t preloadedSampleCount { 0 };
        size_t assignableCcCount { 0 };
        size_t keyswitchCount { 0 };
        uint64_t generation { 0 };
    };

    struct EngineBundle {
        std::unique_ptr<cella::sfz::SamplerEngine> engine;
        cella::sfz::BlockAdapter adapter;
        float appliedTuningHz { 440.0f };
        float sampleRate { 48000.0f };
        uint64_t generation { 0 };
        std::string sourcePath;
        cella::sfz::InstrumentMetadata metadata;

        EngineBundle()
            : engine(std::make_unique<cella::sfz::SfiziosoEngine>())
            , adapter(*engine)
        {
        }

        explicit EngineBundle(std::unique_ptr<cella::sfz::SamplerEngine> sampler)
            : engine(std::move(sampler))
            , adapter(*engine)
        {
        }
    };

    enum class LoadKind : uint8_t {
        Instrument,
        SampleRateRebuild,
        Unload,
    };

    struct LoadRequest {
        uint64_t generation { 0 };
        std::string path;
        std::string fallbackPath;
        float sampleRate { 48000.0f };
        float tuningHz { 440.0f };
        LoadKind kind { LoadKind::Instrument };
        FolderSnapshot folder;
    };

    using EngineFactory =
        std::function<std::unique_ptr<cella::sfz::SamplerEngine>()>;

    struct LoaderState {
        explicit LoaderState(EngineFactory factory)
            : engineFactory(std::move(factory))
        {
        }

        ~LoaderState()
        {
            delete pendingEngine.exchange(nullptr, std::memory_order_acq_rel);
            delete retiredEngine.exchange(nullptr, std::memory_order_acq_rel);
        }

        EngineFactory engineFactory;
        std::shared_ptr<const StatusSnapshot> statusSnapshot =
            std::make_shared<const StatusSnapshot>();
        std::shared_ptr<const StatusSnapshot> lastSuccessfulStatus =
            std::make_shared<const StatusSnapshot>();
        std::shared_ptr<const std::string> selectedPath =
            std::make_shared<const std::string>();
        std::shared_ptr<const cella::sfz::InstrumentMetadata> instrumentMetadata =
            std::make_shared<const cella::sfz::InstrumentMetadata>();
        std::atomic<EngineBundle*> pendingEngine { nullptr };
        std::atomic<EngineBundle*> retiredEngine { nullptr };
        std::atomic<uint64_t> pendingGeneration { 0 };
        std::atomic<float> pendingSampleRate { 0.0f };
        std::atomic<uint64_t> retiredEngineCount { 0 };
        std::atomic<bool> failedInstrumentRecoveryRequested { false };
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<LoadRequest> loadRequest;
        std::string requestedPath;
        // Successful paths are bounded to the active generation and the most
        // recent ready generation. The audio thread publishes activation by ID
        // without copying or destroying strings.
        std::unordered_map<uint64_t, std::string> successfulPaths;
        std::atomic<uint64_t> activeGeneration { 0 };
        std::atomic<uint64_t> newestLoadGeneration { 0 };
        std::atomic<uint64_t> unloadGeneration { 0 };
        std::atomic<bool> stopRequested { false };
        bool workerBusy { false }; // guarded by mutex
    };

    std::shared_ptr<LoaderState> loader_;
    std::atomic<float> observedSampleRate_ { 48000.0f };
    std::atomic<int> renderQuantumSetting_ { kDefaultRenderQuantum };
    std::atomic<TailBehavior> tailBehaviorSetting_ { TailBehavior::PreserveAll };
    std::atomic<int> displayPageSetting_ {
        static_cast<int>(DisplayPage::Play) };
    std::atomic<bool> sampleRateTransitionRequested_ { false };
    std::atomic<bool> quantumTransitionRequested_ { false };
    EngineBundle* activeEngine_ { nullptr }; // audio thread only
    std::thread loaderThread_;

    cella::sfz::NoteLaneTracker lanes_;
    std::array<cella::sfz::TimedEngineEvent,
        cella::sfz::BlockAdapter::MaxEvents>
        captureEvents_ {};
    size_t captureEventCount_ { 0 };
    size_t droppedEventCount_ { 0 };
    size_t publishedCaptureDrops_ { 0 };
    size_t publishedAdapterDrops_ { 0 };
    EngineBundle* publishedAdapterEngine_ { nullptr };
    std::atomic<int> heldLaneCount_ { 0 };
    std::atomic<int> activeVoiceCount_ { 0 };
    std::atomic<int> peakVoiceCount_ { 0 };
    std::atomic<int> voiceLimit_ { 0 };
    std::atomic<int> assignedCcCount_ { 0 };
    std::atomic<uint64_t> eventDropCount_ { 0 };
    std::atomic<uint64_t> outputLimitingCount_ { 0 };
    std::atomic<uint64_t> audioTimeNanoseconds_ { 0 };
    std::atomic<uint64_t> eventDropWarningUntilNanoseconds_ { 0 };
    std::atomic<uint64_t> outputLimitingWarningUntilNanoseconds_ { 0 };
    VoicePeakHold voicePeakHold_;
    int captureFrame_ { 0 };
    int activeRenderQuantum_ { kDefaultRenderQuantum };
    int playbackFrame_ { kDefaultRenderQuantum };
    using PlaybackBuffer = std::array<std::array<float, kMaximumRenderQuantum>,
        cella::sfz::MaxAudioOutputLanes>;
    PlaybackBuffer playbackLeft_ {};
    PlaybackBuffer playbackRight_ {};
    bool playbackPolyphonic_ { false };
    int playbackChannels_ { 1 };
    std::array<float, cella::sfz::MaxNoteLanes> pitchVoltages_ {};
    std::array<float, cella::sfz::MaxNoteLanes> gateVoltages_ {};
    std::array<float, cella::sfz::MaxNoteLanes> velocityVoltages_ {};
    std::array<cella::sfz::ExpressionFeedback, 2> expressionFeedback_ {};
    std::array<float, cella::sfz::ExpressionLaneCount> bendState_ {};
    std::array<float, cella::sfz::ExpressionLaneCount> pressureState_ {};
    std::array<float, cella::sfz::ExpressionLaneCount> timbreState_ {};
    std::array<std::array<float, cella::sfz::ExpressionLaneCount>,
        cella::sfz::NamedControlCount> namedState_ {};
    std::array<int16_t, cella::sfz::NamedControlCount> namedAssignments_
        { -1, -1, -1, -1 };
    std::array<int16_t, cella::sfz::ExpressionLaneCount> articulationState_ {};

    enum class LifecycleFade : uint8_t {
        Steady,
        FadingOut,
        Silent,
        FadingIn,
    };
    LifecycleFade lifecycleFade_ { LifecycleFade::Steady };
    float lifecycleGain_ { 1.0f };
    bool fadeInAfterRender_ { false };
    bool sampleRateTransitionActive_ { false };
    bool quantumTransitionActive_ { false };
    bool unloadTransitionActive_ { false };

    explicit CellaSFZ(EngineFactory engineFactory = {})
    {
        if (!engineFactory) {
            engineFactory = []() {
                return std::make_unique<cella::sfz::SfiziosoEngine>();
            };
        }
        loader_ = std::make_shared<LoaderState>(std::move(engineFactory));
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(LEVEL_PARAM, 0.0f, 1.0f, 0.8f, "Level", "%", 0.0f, 100.0f);
        configParam(OCTAVE_PARAM, -4.0f, 4.0f, 0.0f, "Octave");
        getParamQuantity(OCTAVE_PARAM)->snapEnabled = true;
        configParam(TUNE_PARAM, -100.0f, 100.0f, 0.0f, "Tune", " cents");
        configButton(LOAD_PARAM, "Load SFZ");
        configButton(PREVIOUS_PARAM, "Previous SFZ in folder");
        configButton(NEXT_PARAM, "Next SFZ in folder");
        configSwitch(POLY_OUTPUT_PARAM, 0.0f, 1.0f, 0.0f, "Output mode",
            { "Mixed stereo", "Polyphonic stereo" });

        configInput(VOCT_INPUT, "1 V/octave pitch");
        configInput(GATE_INPUT, "Gate");
        configInput(VELOCITY_INPUT, "Velocity");
        configOutput(LEFT_OUTPUT, "Left / mono");
        configOutput(RIGHT_OUTPUT, "Right");

        rightExpander.producerMessage = &expressionFeedback_[0];
        rightExpander.consumerMessage = &expressionFeedback_[1];
        resetExpressionState();

        loaderThread_ = std::thread([loader = loader_]() { loaderLoop(loader); });
    }

    ~CellaSFZ() override
    {
        const std::shared_ptr<LoaderState> loader = loader_;
        bool workerBusy = false;
        {
            std::lock_guard<std::mutex> lock(loader->mutex);
            loader->stopRequested.store(true, std::memory_order_release);
            loader->loadRequest.reset();
            loader->pendingGeneration.store(0, std::memory_order_release);
            workerBusy = loader->workerBusy;
        }
        loader->cv.notify_one();
        if (loaderThread_.joinable()) {
            // sfizz loading is synchronous and has no cancellation hook. Keep
            // an in-flight worker joinable under plugin-lifetime ownership so
            // module destruction stays non-blocking without allowing code to
            // run after the plugin library is unloaded.
            if (workerBusy)
                loaderThreadReaper().adopt(loaderThread_);
            else
                loaderThread_.join();
        }
        delete activeEngine_;
    }

    std::shared_ptr<const StatusSnapshot> statusSnapshot() const
    {
        return std::atomic_load_explicit(
            &loader_->statusSnapshot, std::memory_order_acquire);
    }

    std::shared_ptr<const std::string> selectedPath() const
    {
        return std::atomic_load_explicit(
            &loader_->selectedPath, std::memory_order_acquire);
    }

    std::shared_ptr<const cella::sfz::InstrumentMetadata> instrumentMetadata() const
    {
        return std::atomic_load_explicit(
            &loader_->instrumentMetadata, std::memory_order_acquire);
    }

    int renderQuantum() const noexcept
    {
        const int frames = renderQuantumSetting_.load(std::memory_order_acquire);
        return isRenderQuantum(frames) ? frames : kDefaultRenderQuantum;
    }

    bool polyphonicOutputEnabled() noexcept
    {
        return params[POLY_OUTPUT_PARAM].getValue() >= 0.5f;
    }

    void clearPlaybackBuffers() noexcept
    {
        for (auto& lane : playbackLeft_)
            lane.fill(0.0f);
        for (auto& lane : playbackRight_)
            lane.fill(0.0f);
    }

    void resetPlayback() noexcept
    {
        clearPlaybackBuffers();
        playbackPolyphonic_ = false;
        playbackChannels_ = 1;
    }

    TailBehavior tailBehavior() const noexcept
    {
        const TailBehavior behavior =
            tailBehaviorSetting_.load(std::memory_order_acquire);
        switch (behavior) {
        case TailBehavior::PreserveAll:
        case TailBehavior::KeepNewest:
        case TailBehavior::CutOnRetrigger:
            return behavior;
        }
        return TailBehavior::PreserveAll;
    }

    DisplayPage displayPage() const noexcept
    {
        const int page = displayPageSetting_.load(std::memory_order_acquire);
        return page == static_cast<int>(DisplayPage::Info)
            ? DisplayPage::Info
            : DisplayPage::Play;
    }

    void setDisplayPage(DisplayPage page) noexcept
    {
        displayPageSetting_.store(page == DisplayPage::Info
                ? static_cast<int>(DisplayPage::Info)
                : static_cast<int>(DisplayPage::Play),
            std::memory_order_release);
    }

    DisplayRuntimeSnapshot displayRuntimeSnapshot() const noexcept
    {
        return {
            heldLaneCount_.load(std::memory_order_acquire),
            activeVoiceCount_.load(std::memory_order_acquire),
            peakVoiceCount_.load(std::memory_order_acquire),
            voiceLimit_.load(std::memory_order_acquire),
            assignedCcCount_.load(std::memory_order_acquire),
            eventDropCount_.load(std::memory_order_acquire),
            outputLimitingCount_.load(std::memory_order_acquire),
            audioTimeNanoseconds_.load(std::memory_order_acquire),
            eventDropWarningUntilNanoseconds_.load(std::memory_order_acquire),
            outputLimitingWarningUntilNanoseconds_.load(std::memory_order_acquire),
        };
    }

    bool readyStatusDescribesActiveEngine(
        const StatusSnapshot& status) const noexcept
    {
        return status.state == StatusState::Ready && status.generation != 0
            && status.generation
                == loader_->activeGeneration.load(std::memory_order_acquire);
    }

    void setTailBehavior(TailBehavior behavior) noexcept
    {
        tailBehaviorSetting_.store(behavior, std::memory_order_release);
    }

    static const char* tailBehaviorKey(TailBehavior behavior) noexcept
    {
        switch (behavior) {
        case TailBehavior::PreserveAll:
            return "preserveAll";
        case TailBehavior::KeepNewest:
            return "keepNewest";
        case TailBehavior::CutOnRetrigger:
            return "cutOnRetrigger";
        }
        return "preserveAll";
    }

    static const char* tailBehaviorLabel(TailBehavior behavior) noexcept
    {
        switch (behavior) {
        case TailBehavior::PreserveAll:
            return "Preserve all";
        case TailBehavior::KeepNewest:
            return "Keep newest tail";
        case TailBehavior::CutOnRetrigger:
            return "Cut on retrigger";
        }
        return "Preserve all";
    }

    void setRenderQuantum(int frames)
    {
        if (!isRenderQuantum(frames)
            || renderQuantumSetting_.exchange(frames, std::memory_order_acq_rel)
                == frames) {
            return;
        }
        quantumTransitionRequested_.store(true, std::memory_order_release);
    }

    static void publishStatus(const std::shared_ptr<LoaderState>& loader,
        StatusSnapshot snapshot)
    {
        std::shared_ptr<const StatusSnapshot> value =
            std::make_shared<const StatusSnapshot>(std::move(snapshot));
        std::atomic_store_explicit(&loader->statusSnapshot, std::move(value),
            std::memory_order_release);
    }

    static StatusSnapshot statusForRequest(const LoadRequest& request,
        StatusState state, const std::string& message)
    {
        StatusSnapshot status;
        status.state = state;
        status.filename = system::getFilename(request.path);
        status.message = message;
        status.absolutePath = request.path.empty()
            ? std::string()
            : system::getAbsolute(request.path);
        status.folderIndex = request.folder.hasSelection()
            ? request.folder.selectedIndex + 1
            : 0;
        status.folderCount = request.folder.hasSelection()
            ? request.folder.instruments.size()
            : 0;
        status.generation = request.generation;
        return status;
    }

    static bool loadGenerationIsCurrentLocked(
        const std::shared_ptr<LoaderState>& loader, uint64_t generation) noexcept
    {
        return !loader->stopRequested.load(std::memory_order_acquire)
            && loader->newestLoadGeneration.load(std::memory_order_acquire)
                == generation;
    }

    static void retireOneOnWorker(const std::shared_ptr<LoaderState>& loader) noexcept
    {
        if (EngineBundle* retired = loader->retiredEngine.exchange(
                nullptr, std::memory_order_acq_rel)) {
            delete retired;
            loader->retiredEngineCount.fetch_add(1, std::memory_order_release);
        }
    }

    static void clearPendingOnWorker(
        const std::shared_ptr<LoaderState>& loader) noexcept
    {
        loader->pendingGeneration.store(0, std::memory_order_release);
        delete loader->pendingEngine.exchange(nullptr, std::memory_order_acq_rel);
    }

    static void publishLoadFailureOnWorker(
        const std::shared_ptr<LoaderState>& loader, const LoadRequest& request,
        StatusState state, const char* message) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(loader->mutex);
            if (loadGenerationIsCurrentLocked(loader, request.generation)) {
                publishStatus(loader, statusForRequest(request, state, message));
                if (request.kind == LoadKind::Instrument) {
                    loader->failedInstrumentRecoveryRequested.store(
                        true, std::memory_order_release);
                }
            }
        } catch (...) {
            // A status update is best-effort after a load failure. Never let a
            // secondary allocation error terminate the loader thread.
        }
    }

    static void performLoadOnWorker(
        const std::shared_ptr<LoaderState>& loader, const LoadRequest& request)
    {
        // An unload request exists to serialize cancellation and destruction of
        // any pending engine on the loader worker. The active engine is faded
        // out and detached separately at an audio block boundary.
        if (request.kind == LoadKind::Unload)
            return;

        std::string resolvedPath = request.path;
        if (!system::isFile(resolvedPath) && !request.fallbackPath.empty()
            && system::isFile(request.fallbackPath)) {
            resolvedPath = request.fallbackPath;
        }
        const std::string filename = system::getFilename(resolvedPath);
        if (!system::isFile(resolvedPath)) {
            publishLoadFailureOnWorker(
                loader, request, StatusState::Missing, "MISSING SFZ");
            return;
        }

        auto bundle = std::make_unique<EngineBundle>(loader->engineFactory());
        bundle->generation = request.generation;
        bundle->sourcePath = resolvedPath;
        bundle->engine->setSampleRate(request.sampleRate);
        bundle->sampleRate = request.sampleRate;
        // Every engine is prepared for the largest selectable quantum. The
        // audio thread can then change the actual render size without
        // reallocating or rebuilding the instrument.
        bundle->engine->setMaximumBlockSize(kMaximumRenderQuantum);
        bundle->engine->setTuningFrequency(request.tuningHz);
        bundle->appliedTuningHz = request.tuningHz;
        const cella::sfz::LoadReport report = bundle->engine->load(resolvedPath);

        if (!report.success) {
            publishLoadFailureOnWorker(loader, request, StatusState::Error,
                "LOAD FAILED - CHECK FILE");
            return;
        }
        bundle->metadata = bundle->engine->instrumentMetadata();
        FolderSnapshot folder = request.folder;
        if (!folder.inspected
            || system::getAbsolute(resolvedPath) != system::getAbsolute(request.path))
            folder = snapshotSfzFolder(resolvedPath);

        std::lock_guard<std::mutex> lock(loader->mutex);
        if (!loadGenerationIsCurrentLocked(loader, request.generation))
            return;
        loader->requestedPath = resolvedPath;
        StatusSnapshot ready;
        ready.state = StatusState::Ready;
        ready.filename = filename;
        ready.regionCount = report.regionCount;
        ready.estimatedSampleBytes = report.estimatedPreloadedSampleBytes;
        ready.absolutePath = system::getAbsolute(resolvedPath);
        ready.folderIndex = folder.hasSelection() ? folder.selectedIndex + 1 : 0;
        ready.folderCount = folder.hasSelection() ? folder.instruments.size() : 0;
        ready.preloadedSampleCount = report.preloadedSampleCount;
        ready.assignableCcCount = bundle->metadata.namedControllers.size();
        ready.keyswitchCount = bundle->metadata.latchedKeyswitches.size();
        ready.generation = request.generation;
        std::shared_ptr<const StatusSnapshot> readyValue =
            std::make_shared<const StatusSnapshot>(ready);
        std::shared_ptr<const std::string> selected =
            std::make_shared<const std::string>(resolvedPath);
        std::shared_ptr<const cella::sfz::InstrumentMetadata> metadata =
            std::make_shared<const cella::sfz::InstrumentMetadata>(bundle->metadata);
        const uint64_t activeGeneration =
            loader->activeGeneration.load(std::memory_order_acquire);
        for (auto it = loader->successfulPaths.begin();
             it != loader->successfulPaths.end();) {
            if (it->first != activeGeneration && it->first != request.generation)
                it = loader->successfulPaths.erase(it);
            else
                ++it;
        }
        loader->successfulPaths[request.generation] = resolvedPath;
        loader->failedInstrumentRecoveryRequested.store(
            false, std::memory_order_release);

        // pendingGeneration is the audio-thread handoff's release point. READY
        // is published last so observing it guarantees the complete handoff and
        // its associated display/persistence metadata are already available.
        loader->pendingSampleRate.store(request.sampleRate, std::memory_order_relaxed);
        EngineBundle* stalePending = loader->pendingEngine.exchange(
            bundle.release(), std::memory_order_acq_rel);
        loader->pendingGeneration.store(request.generation, std::memory_order_release);
        delete stalePending;
        std::atomic_store_explicit(&loader->lastSuccessfulStatus, readyValue,
            std::memory_order_release);
        std::atomic_store_explicit(&loader->selectedPath, std::move(selected),
            std::memory_order_release);
        std::atomic_store_explicit(&loader->instrumentMetadata,
            std::move(metadata), std::memory_order_release);
        std::atomic_store_explicit(&loader->statusSnapshot, std::move(readyValue),
            std::memory_order_release);
    }

    static void loaderLoop(const std::shared_ptr<LoaderState>& loader) noexcept
    {
        for (;;) {
            retireOneOnWorker(loader);

            std::optional<LoadRequest> request;
            {
                std::unique_lock<std::mutex> lock(loader->mutex);
                // A bounded timeout is the retirement signal from the audio
                // thread. This keeps engine destruction off that thread
                // without making process() call a condition-variable API.
                loader->cv.wait_for(lock, std::chrono::milliseconds(50), [&loader]() {
                    return loader->stopRequested.load(std::memory_order_acquire)
                        || loader->loadRequest.has_value()
                        || loader->retiredEngine.load(std::memory_order_acquire);
                });
                if (loader->stopRequested.load(std::memory_order_acquire))
                    break;
                if (loader->loadRequest) {
                    request = std::move(loader->loadRequest);
                    loader->loadRequest.reset();
                    loader->workerBusy = true;
                }
            }

            if (!request)
                continue;
            // A newer request also supersedes an already-built engine which
            // the audio thread has not accepted yet. Dispose it here, never in
            // process().
            clearPendingOnWorker(loader);
            try {
                performLoadOnWorker(loader, *request);
            } catch (...) {
                publishLoadFailureOnWorker(loader, *request, StatusState::Error,
                    "LOAD FAILED - CHECK FILE");
            }
            {
                std::lock_guard<std::mutex> lock(loader->mutex);
                loader->workerBusy = false;
            }
        }
        retireOneOnWorker(loader);
        clearPendingOnWorker(loader);
    }

    void loadInstrument(const std::string& path,
        const std::string& fallbackPath = {},
        std::optional<FolderSnapshot> folder = std::nullopt)
    {
        if (path.empty()
            || loader_->stopRequested.load(std::memory_order_acquire))
            return;

        LoadRequest request;
        request.path = path;
        request.fallbackPath = fallbackPath;
        request.folder = folder ? std::move(*folder) : snapshotSfzFolder(path);
        request.sampleRate = observedSampleRate_.load(std::memory_order_relaxed);
        request.tuningHz = 440.0f
            * std::pow(2.0f, params[TUNE_PARAM].getValue() / 1200.0f);
        {
            std::lock_guard<std::mutex> lock(loader_->mutex);
            if (loader_->stopRequested.load(std::memory_order_acquire))
                return;
            loader_->pendingGeneration.store(0, std::memory_order_release);
            request.generation = loader_->newestLoadGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            loader_->requestedPath = path;
            loader_->unloadGeneration.store(0, std::memory_order_release);
            StatusSnapshot loading = statusForRequest(
                request, StatusState::Loading, "LOADING");
            loader_->loadRequest = std::move(request);
            loader_->failedInstrumentRecoveryRequested.store(
                false, std::memory_order_release);
            publishStatus(loader_, std::move(loading));
        }
        loader_->cv.notify_one();
    }

    void loadInstrumentFromUi(const std::string& path)
    {
        loadInstrument(path);
    }

    void unloadInstrumentFromUi()
    {
        if (loader_->stopRequested.load(std::memory_order_acquire))
            return;

        LoadRequest request;
        request.kind = LoadKind::Unload;
        {
            std::lock_guard<std::mutex> lock(loader_->mutex);
            if (loader_->stopRequested.load(std::memory_order_acquire))
                return;
            loader_->pendingGeneration.store(0, std::memory_order_release);
            request.generation = loader_->newestLoadGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            loader_->requestedPath.clear();
            loader_->successfulPaths.clear();
            loader_->loadRequest = request;
            loader_->unloadGeneration.store(
                request.generation, std::memory_order_release);
            loader_->failedInstrumentRecoveryRequested.store(
                false, std::memory_order_release);

            const auto emptyPath = std::make_shared<const std::string>();
            const auto emptyMetadata =
                std::make_shared<const cella::sfz::InstrumentMetadata>();
            const auto unloaded = std::make_shared<const StatusSnapshot>();
            std::atomic_store_explicit(&loader_->selectedPath, emptyPath,
                std::memory_order_release);
            std::atomic_store_explicit(&loader_->instrumentMetadata,
                emptyMetadata, std::memory_order_release);
            std::atomic_store_explicit(&loader_->lastSuccessfulStatus, unloaded,
                std::memory_order_release);
            std::atomic_store_explicit(&loader_->statusSnapshot, unloaded,
                std::memory_order_release);
        }
        loader_->cv.notify_one();
    }

    std::string navigationPath() const
    {
        {
            std::lock_guard<std::mutex> lock(loader_->mutex);
            if (!loader_->requestedPath.empty())
                return loader_->requestedPath;
        }
        const std::shared_ptr<const std::string> selected = selectedPath();
        return selected ? *selected : std::string();
    }

    bool navigateInstrumentFromUi(int direction)
    {
        if (direction == 0)
            return false;

        try {
            const std::string currentPath = navigationPath();
            if (currentPath.empty())
                return false;
            const std::string currentAbsolute = system::getAbsolute(currentPath);
            const std::string directory = system::getDirectory(currentAbsolute);
            if (directory.empty())
                return false;

            FolderSnapshot folder = snapshotSfzFolder(currentAbsolute);
            if (!folder.hasSelection())
                return false;

            const size_t count = folder.instruments.size();
            const size_t index = folder.selectedIndex;
            const size_t adjacent = direction < 0
                ? (index + count - 1) % count
                : (index + 1) % count;
            const std::string adjacentPath = folder.instruments[adjacent];
            folder.selectedIndex = adjacent;
            loadInstrument(adjacentPath, {}, std::move(folder));
            return true;
        } catch (...) {
            return false;
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& event) override
    {
        if (!(event.sampleRate > 0.0f))
            return;
        const float previous = observedSampleRate_.exchange(event.sampleRate,
            std::memory_order_acq_rel);
        if (std::abs(previous - event.sampleRate) <= 1.0e-3f)
            return;

        std::lock_guard<std::mutex> lock(loader_->mutex);
        std::string reloadPath;
        const uint64_t activeGeneration =
            loader_->activeGeneration.load(std::memory_order_acquire);
        if (const auto active = loader_->successfulPaths.find(activeGeneration);
            active != loader_->successfulPaths.end()) {
            reloadPath = active->second;
        } else {
            reloadPath = loader_->requestedPath;
        }
        if (loader_->stopRequested.load(std::memory_order_acquire)
            || reloadPath.empty())
            return;

        LoadRequest request;
        loader_->pendingGeneration.store(0, std::memory_order_release);
        request.generation = loader_->newestLoadGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        request.path = reloadPath;
        request.sampleRate = event.sampleRate;
        request.tuningHz = 440.0f
            * std::pow(2.0f, params[TUNE_PARAM].getValue() / 1200.0f);
        request.kind = LoadKind::SampleRateRebuild;
        StatusSnapshot loading;
        const auto last = std::atomic_load_explicit(
            &loader_->lastSuccessfulStatus, std::memory_order_acquire);
        if (last)
            loading = *last;
        loading.state = StatusState::Loading;
        loading.filename = system::getFilename(reloadPath);
        loading.message = "LOADING";
        loading.absolutePath = reloadPath;
        loading.generation = request.generation;
        loader_->loadRequest = std::move(request);
        loader_->unloadGeneration.store(0, std::memory_order_release);
        sampleRateTransitionRequested_.store(true, std::memory_order_release);
        loader_->failedInstrumentRecoveryRequested.store(
            false, std::memory_order_release);
        publishStatus(loader_, std::move(loading));
        loader_->cv.notify_one();
    }

    void resetExpressionState() noexcept
    {
        const float unset = std::numeric_limits<float>::quiet_NaN();
        // A freshly loaded sampler can carry non-neutral SFZ controller
        // defaults (notably set_cc74). Force the first capture quantum to
        // submit the host's required neutral dedicated-expression values.
        bendState_.fill(unset);
        pressureState_.fill(unset);
        timbreState_.fill(unset);
        for (auto& control : namedState_)
            control.fill(unset);
        namedAssignments_.fill(-1);
        articulationState_.fill(-1);
    }

    const cella::sfz::ExpressionMessage* expressionMessage() const noexcept
    {
        Module* neighbor = rightExpander.module;
        if (!neighbor || neighbor->model != modelCellaSFZExpression
            || neighbor->isBypassed())
            return nullptr;
        const auto* message = static_cast<const cella::sfz::ExpressionMessage*>(
            neighbor->leftExpander.consumerMessage);
        return message && message->magic == cella::sfz::ExpressionMessageMagic
            ? message
            : nullptr;
    }

    bool pushCaptureEvent(const cella::sfz::TimedEngineEvent& event) noexcept
    {
        cella::sfz::EventWriter writer { captureEvents_.data(), captureEvents_.size(),
            captureEventCount_, droppedEventCount_ };
        const bool pushed = writer.push(event);
        captureEventCount_ = writer.count;
        droppedEventCount_ = writer.dropped;
        return pushed;
    }

    void publishEventDropTelemetry() noexcept
    {
        uint64_t additional = 0;
        if (droppedEventCount_ > publishedCaptureDrops_)
            additional += droppedEventCount_ - publishedCaptureDrops_;
        publishedCaptureDrops_ = droppedEventCount_;
        if (publishedAdapterEngine_ != activeEngine_) {
            publishedAdapterEngine_ = activeEngine_;
            publishedAdapterDrops_ = 0;
        }
        const size_t adapterDrops = activeEngine_
            ? activeEngine_->adapter.droppedEventCount()
            : 0;
        if (adapterDrops > publishedAdapterDrops_)
            additional += adapterDrops - publishedAdapterDrops_;
        publishedAdapterDrops_ = adapterDrops;
        if (additional > 0) {
            eventDropCount_.fetch_add(additional, std::memory_order_release);
            eventDropWarningUntilNanoseconds_.store(
                audioTimeNanoseconds_.load(std::memory_order_relaxed)
                    + 1000000000ull,
                std::memory_order_release);
        }
    }

    void updateBendExpression(
        const cella::sfz::ExpressionMessage* message) noexcept
    {
        for (size_t lane = 0; lane < bendState_.size(); ++lane) {
            const float volts = message
                ? cella::sfz::polyVoltageForLane(message->inputs[0], lane, 0.0f)
                : 0.0f;
            const float semitones = 12.0f * volts;
            if (!std::isfinite(bendState_[lane])
                || std::abs(semitones - bendState_[lane])
                    >= cella::sfz::NoteLaneTracker::PitchChangeThresholdSemitones) {
                if (pushCaptureEvent(cella::sfz::TimedEngineEvent::noteBend(
                        static_cast<uint32_t>(captureFrame_),
                        static_cast<uint8_t>(lane), semitones)))
                    bendState_[lane] = semitones;
            }
        }
    }

    void updateQuantumExpression(
        const cella::sfz::ExpressionMessage* message) noexcept
    {
        const auto normalizedFor = [message](size_t input, size_t lane,
                                       float fallback) noexcept {
            return message
                ? cella::sfz::normalizedVoltage(
                      cella::sfz::polyVoltageForLane(
                          message->inputs[input], lane, fallback * 10.0f))
                : fallback;
        };
        for (size_t lane = 0; lane < cella::sfz::ExpressionLaneCount; ++lane) {
            const float pressure = normalizedFor(1, lane, 0.0f);
            if (!std::isfinite(pressureState_[lane])
                || pressure != pressureState_[lane]) {
                if (pushCaptureEvent(cella::sfz::TimedEngineEvent::pressure(
                        0, static_cast<uint8_t>(lane), pressure)))
                    pressureState_[lane] = pressure;
            }
            const float timbre = normalizedFor(2, lane, 0.0f);
            if (!std::isfinite(timbreState_[lane])
                || timbre != timbreState_[lane]) {
                if (pushCaptureEvent(cella::sfz::TimedEngineEvent::timbre(
                        0, static_cast<uint8_t>(lane), timbre)))
                    timbreState_[lane] = timbre;
            }
        }

        const cella::sfz::InstrumentMetadata* metadata =
            activeEngine_ ? &activeEngine_->metadata : nullptr;
        std::array<int16_t, cella::sfz::NamedControlCount> assignments;
        for (size_t slot = 0; slot < cella::sfz::NamedControlCount; ++slot) {
            const int requested = message ? message->assignments[slot] : -1;
            const cella::sfz::NamedController* controller = metadata
                ? cella::sfz::findNamedController(*metadata, requested)
                : nullptr;
            assignments[slot] = static_cast<int16_t>(controller ? requested : -1);
        }
        // A CC has one effective value per lane. If an old patch or an
        // incompatible sender supplies duplicate assignments, the last slot
        // owns it deterministically, matching the expander's UI behavior.
        for (size_t slot = 0; slot < assignments.size(); ++slot) {
            if (assignments[slot] < 0)
                continue;
            for (size_t later = slot + 1; later < assignments.size(); ++later) {
                if (assignments[later] == assignments[slot]) {
                    assignments[slot] = -1;
                    break;
                }
            }
        }

        std::array<bool, 128> forceController {};
        for (size_t slot = 0; slot < assignments.size(); ++slot) {
            const int oldAssignment = namedAssignments_[slot];
            const int assignment = assignments[slot];
            if (oldAssignment >= 0 && oldAssignment != assignment && metadata) {
                const bool stillOwned = std::find(assignments.begin(), assignments.end(),
                                            oldAssignment)
                    != assignments.end();
                if (stillOwned) {
                    forceController[oldAssignment] = true;
                    continue;
                }
                const cella::sfz::NamedController* old =
                    cella::sfz::findNamedController(
                        *metadata, oldAssignment);
                if (old) {
                    for (size_t lane = 0; lane < cella::sfz::ExpressionLaneCount;
                         ++lane) {
                        pushCaptureEvent(cella::sfz::TimedEngineEvent::sourceCC(
                            0, static_cast<uint8_t>(lane), old->number,
                            old->defaultValue));
                    }
                }
            }
        }

        for (size_t slot = 0; slot < assignments.size(); ++slot) {
            const int assignment = assignments[slot];
            if (assignment < 0) {
                namedState_[slot].fill(std::numeric_limits<float>::quiet_NaN());
                namedAssignments_[slot] = -1;
                continue;
            }
            const cella::sfz::NamedController* controller =
                cella::sfz::findNamedController(*metadata, assignment);
            for (size_t lane = 0; lane < cella::sfz::ExpressionLaneCount; ++lane) {
                const float value = normalizedFor(3 + slot, lane,
                    controller->defaultValue);
                if (namedAssignments_[slot] != assignment
                    || forceController[assignment]
                    || !std::isfinite(namedState_[slot][lane])
                    || value != namedState_[slot][lane]) {
                    if (pushCaptureEvent(cella::sfz::TimedEngineEvent::sourceCC(
                            0, static_cast<uint8_t>(lane), controller->number,
                            value)))
                        namedState_[slot][lane] = value;
                }
            }
            namedAssignments_[slot] = static_cast<int16_t>(assignment);
        }
        assignedCcCount_.store(static_cast<int>(std::count_if(
                assignments.begin(), assignments.end(),
                [](int16_t assignment) { return assignment >= 0; })),
            std::memory_order_release);
    }

    void updateArticulations(
        const cella::sfz::ExpressionMessage* message) noexcept
    {
        // With no compatible expander, leave sfizioso's sw_default/current
        // switch untouched. Disconnecting or bypassing the expander likewise
        // preserves the last explicitly selected articulation.
        if (!message)
            return;
        const auto* metadata = activeEngine_ ? &activeEngine_->metadata : nullptr;
        const size_t count = metadata ? metadata->latchedKeyswitches.size() : 0;
        const size_t lanes = std::clamp<size_t>(
            std::max<int>(1, inputs[VOCT_INPUT].getChannels()), 1,
            cella::sfz::ExpressionLaneCount);
        for (size_t lane = 0; lane < lanes; ++lane) {
            const int desired = message->articulationIndices[lane];
            const int selected = desired >= 0
                    && static_cast<size_t>(desired) < count
                ? desired
                : -1;
            // Negative/invalid selection means that the expander has not
            // requested an override. Preserve sfizioso's sw_default or the
            // last explicit manual/CV selection.
            if (selected < 0)
                continue;
            if (selected == articulationState_[lane])
                continue;
            const uint8_t note = metadata->latchedKeyswitches[selected].note;
            pushCaptureEvent(cella::sfz::TimedEngineEvent::keyswitchOn(
                0, static_cast<uint8_t>(lane), note));
            pushCaptureEvent(cella::sfz::TimedEngineEvent::keyswitchOff(
                static_cast<uint32_t>(std::min(1, activeRenderQuantum_ - 1)),
                static_cast<uint8_t>(lane), note));
            articulationState_[lane] = static_cast<int16_t>(selected);
        }
    }

    void publishExpressionFeedback() noexcept
    {
        if (!rightExpander.producerMessage)
            return;
        auto& feedback = *static_cast<cella::sfz::ExpressionFeedback*>(
            rightExpander.producerMessage);
        feedback = {};
        feedback.magic = cella::sfz::ExpressionMessageMagic;
        feedback.metadataGeneration = activeEngine_
            ? static_cast<uint32_t>(activeEngine_->generation)
            : 0;
        feedback.activeLanes = static_cast<uint8_t>(std::clamp<size_t>(
            std::max<int>(1, inputs[VOCT_INPUT].getChannels()), 1,
            cella::sfz::ExpressionLaneCount));
        if (activeEngine_) {
            feedback.keyswitchCount = static_cast<uint8_t>(std::min<size_t>(
                activeEngine_->metadata.latchedKeyswitches.size(), 128));
            for (const auto& control : activeEngine_->metadata.namedControllers)
                feedback.assignableCCs[control.number] = 1;
        }
        feedback.activeArticulations = articulationState_;
        rightExpander.messageFlipRequested = true;
    }

    bool swapEngineAtBlockBoundary() noexcept
    {
        // The loader worker owns retirement. If it has not collected the
        // previous engine, defer another swap instead of ever destroying an
        // engine on audio.
        if (loader_->retiredEngine.load(std::memory_order_acquire))
            return false;
        EngineBundle* next = loader_->pendingEngine.exchange(
            nullptr, std::memory_order_acq_rel);
        if (!next)
            return false;
        uint64_t expectedPendingGeneration = next->generation;
        loader_->pendingGeneration.compare_exchange_strong(expectedPendingGeneration, 0,
            std::memory_order_acq_rel, std::memory_order_acquire);
        const bool superseded = next->generation
                != loader_->newestLoadGeneration.load(std::memory_order_acquire)
            || std::abs(next->sampleRate
                    - observedSampleRate_.load(std::memory_order_relaxed))
                > 1.0e-3f;
        if (superseded) {
            // Ownership has already left pendingEngine_, so hand the stale
            // bundle to the worker instead of deleting it on audio.
            loader_->retiredEngine.store(next, std::memory_order_release);
            return false;
        }
        EngineBundle* previous = activeEngine_;
        activeEngine_ = next;
        lanes_.reset();
        resetExpressionState();
        heldLaneCount_.store(0, std::memory_order_release);
        activeVoiceCount_.store(0, std::memory_order_release);
        voicePeakHold_.reset();
        peakVoiceCount_.store(0, std::memory_order_release);
        voiceLimit_.store(std::max(0, next->engine->voiceLimit()),
            std::memory_order_release);
        assignedCcCount_.store(0, std::memory_order_release);
        // Warning holds describe the engine which produced them. Keep their
        // counters monotonic, but do not carry an old engine's overlay onto
        // the newly activated instrument.
        eventDropWarningUntilNanoseconds_.store(0, std::memory_order_release);
        outputLimitingWarningUntilNanoseconds_.store(0,
            std::memory_order_release);
        captureEventCount_ = 0;
        resetPlayback();
        activeRenderQuantum_ = renderQuantum();
        playbackFrame_ = activeRenderQuantum_;
        // Publish activation only after every runtime value has been reset for
        // this generation. The UI uses this release point before showing READY.
        loader_->activeGeneration.store(next->generation, std::memory_order_release);
        if (previous)
            loader_->retiredEngine.store(previous, std::memory_order_release);
        sampleRateTransitionActive_ = false;
        quantumTransitionActive_ = false;
        return true;
    }

    bool unloadActiveEngineAtBlockBoundary(uint64_t generation) noexcept
    {
        if (generation == 0
            || generation
                != loader_->newestLoadGeneration.load(std::memory_order_acquire)
            || generation
                != loader_->unloadGeneration.load(std::memory_order_acquire)) {
            return false;
        }
        // As with replacement, never destroy an engine or overwrite the
        // single retirement handoff on the audio thread.
        if (loader_->retiredEngine.load(std::memory_order_acquire))
            return false;

        EngineBundle* previous = activeEngine_;
        activeEngine_ = nullptr;
        lanes_.reset();
        resetExpressionState();
        heldLaneCount_.store(0, std::memory_order_release);
        activeVoiceCount_.store(0, std::memory_order_release);
        voicePeakHold_.reset();
        peakVoiceCount_.store(0, std::memory_order_release);
        voiceLimit_.store(0, std::memory_order_release);
        assignedCcCount_.store(0, std::memory_order_release);
        eventDropWarningUntilNanoseconds_.store(0, std::memory_order_release);
        outputLimitingWarningUntilNanoseconds_.store(0, std::memory_order_release);
        publishedCaptureDrops_ = droppedEventCount_;
        publishedAdapterDrops_ = 0;
        publishedAdapterEngine_ = nullptr;
        captureEventCount_ = 0;
        resetPlayback();
        playbackFrame_ = activeRenderQuantum_;
        fadeInAfterRender_ = false;
        sampleRateTransitionActive_ = false;
        quantumTransitionActive_ = false;
        unloadTransitionActive_ = false;
        loader_->activeGeneration.store(0, std::memory_order_release);
        uint64_t expected = generation;
        loader_->unloadGeneration.compare_exchange_strong(expected, 0,
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (previous)
            loader_->retiredEngine.store(previous, std::memory_order_release);
        return true;
    }

    bool currentPendingEngineIsReady() const noexcept
    {
        const uint64_t pendingGeneration =
            loader_->pendingGeneration.load(std::memory_order_acquire);
        if (pendingGeneration == 0
            || pendingGeneration
                != loader_->newestLoadGeneration.load(std::memory_order_acquire)) {
            return false;
        }
        if (std::abs(loader_->pendingSampleRate.load(std::memory_order_relaxed)
                - observedSampleRate_.load(std::memory_order_relaxed))
            > 1.0e-3f) {
            return false;
        }
        return loader_->pendingEngine.load(std::memory_order_acquire) != nullptr;
    }

    void beginCaptureBlock() noexcept
    {
        const uint64_t unloadGeneration =
            loader_->unloadGeneration.load(std::memory_order_acquire);
        const bool unloadIsCurrent = unloadGeneration != 0
            && unloadGeneration
                == loader_->newestLoadGeneration.load(std::memory_order_acquire);
        if (unloadTransitionActive_ && !unloadIsCurrent) {
            // A newer load superseded the unload before detachment. Restore the
            // still-active engine while that replacement is being prepared.
            unloadTransitionActive_ = false;
            if (!sampleRateTransitionActive_ && !quantumTransitionActive_)
                lifecycleFade_ = LifecycleFade::FadingIn;
        }
        if (unloadIsCurrent && activeEngine_) {
            unloadTransitionActive_ = true;
            if (lifecycleFade_ == LifecycleFade::Steady
                || lifecycleFade_ == LifecycleFade::FadingIn) {
                lifecycleFade_ = LifecycleFade::FadingOut;
            }
        } else if (unloadIsCurrent && !activeEngine_) {
            uint64_t expected = unloadGeneration;
            loader_->unloadGeneration.compare_exchange_strong(expected, 0,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }

        if (quantumTransitionRequested_.exchange(false,
                std::memory_order_acq_rel)
            && activeEngine_) {
            if (renderQuantum() != activeRenderQuantum_) {
                quantumTransitionActive_ = true;
                if (lifecycleFade_ == LifecycleFade::Steady
                    || lifecycleFade_ == LifecycleFade::FadingIn) {
                    lifecycleFade_ = LifecycleFade::FadingOut;
                }
            } else {
                quantumTransitionActive_ = false;
            }
        }

        if (sampleRateTransitionRequested_.exchange(false,
                std::memory_order_acq_rel)
            && activeEngine_) {
            sampleRateTransitionActive_ = true;
            lifecycleFade_ = LifecycleFade::FadingOut;
        }

        const bool pendingReady = currentPendingEngineIsReady();
        if (activeEngine_
            && (lifecycleFade_ == LifecycleFade::FadingOut
                || lifecycleFade_ == LifecycleFade::Silent)
            && !sampleRateTransitionActive_ && !quantumTransitionActive_
            && !unloadTransitionActive_ && !pendingReady) {
            fadeInAfterRender_ = false;
            lifecycleFade_ = LifecycleFade::FadingIn;
        }

        if (activeEngine_ && lifecycleFade_ == LifecycleFade::Silent
            && unloadTransitionActive_ && unloadIsCurrent) {
            unloadActiveEngineAtBlockBoundary(unloadGeneration);
        }

        if (!activeEngine_ && pendingReady) {
            if (swapEngineAtBlockBoundary()) {
                lifecycleGain_ = 0.0f;
                lifecycleFade_ = LifecycleFade::Silent;
                fadeInAfterRender_ = true;
            }
        } else if (activeEngine_
            && pendingReady
            && lifecycleFade_ == LifecycleFade::Steady) {
            lifecycleFade_ = LifecycleFade::FadingOut;
        }

        if (lifecycleFade_ == LifecycleFade::Silent
            && pendingReady
            && swapEngineAtBlockBoundary()) {
            fadeInAfterRender_ = true;
        }

        if (activeEngine_ && lifecycleFade_ == LifecycleFade::Silent
            && quantumTransitionActive_ && !sampleRateTransitionActive_
            && !pendingReady) {
            activeRenderQuantum_ = renderQuantum();
            captureEventCount_ = 0;
            resetPlayback();
            playbackFrame_ = activeRenderQuantum_;
            quantumTransitionActive_ = false;
            fadeInAfterRender_ = true;
        }

        if (!activeEngine_)
            return;
        const float tuningHz = 440.0f
            * std::pow(2.0f, params[TUNE_PARAM].getValue() / 1200.0f);
        if (std::abs(tuningHz - activeEngine_->appliedTuningHz) > 1.0e-4f) {
            activeEngine_->engine->setTuningFrequency(tuningHz);
            activeEngine_->appliedTuningHz = tuningHz;
        }
        const cella::sfz::ExpressionMessage* message = expressionMessage();
        updateQuantumExpression(message);
        updateArticulations(message);
    }

    void captureInputFrame() noexcept
    {
        updateBendExpression(expressionMessage());
        const int pitchChannels = std::clamp(inputs[VOCT_INPUT].getChannels(),
            0, static_cast<int>(cella::sfz::MaxNoteLanes));
        const int gateChannels = std::clamp(inputs[GATE_INPUT].getChannels(),
            0, static_cast<int>(cella::sfz::MaxNoteLanes));
        const int velocityChannels = std::clamp(inputs[VELOCITY_INPUT].getChannels(),
            0, static_cast<int>(cella::sfz::MaxNoteLanes));
        for (int lane = 0; lane < pitchChannels; ++lane)
            pitchVoltages_[lane] = inputs[VOCT_INPUT].getVoltage(lane);
        for (int lane = 0; lane < gateChannels; ++lane)
            gateVoltages_[lane] = inputs[GATE_INPUT].getVoltage(lane);
        for (int lane = 0; lane < velocityChannels; ++lane)
            velocityVoltages_[lane] = inputs[VELOCITY_INPUT].getVoltage(lane);

        cella::sfz::EventWriter writer { captureEvents_.data(), captureEvents_.size(),
            captureEventCount_, droppedEventCount_ };
        const int octave = std::clamp(static_cast<int>(
            std::lround(params[OCTAVE_PARAM].getValue())), -4, 4);
        lanes_.process({ pitchChannels > 0 ? pitchVoltages_.data() : nullptr,
                           static_cast<size_t>(pitchChannels),
                           gateChannels > 0 ? gateVoltages_.data() : nullptr,
                           static_cast<size_t>(gateChannels),
                           velocityChannels > 0 ? velocityVoltages_.data() : nullptr,
                           static_cast<size_t>(velocityChannels), octave,
                           static_cast<uint32_t>(captureFrame_) },
            writer);
        captureEventCount_ = writer.count;
        droppedEventCount_ = writer.dropped;
        heldLaneCount_.store(static_cast<int>(lanes_.activeCount()),
            std::memory_order_release);
        publishEventDropTelemetry();
    }

    void finishCaptureBlock() noexcept
    {
        clearPlaybackBuffers();
        playbackPolyphonic_ = polyphonicOutputEnabled();
        playbackChannels_ = playbackPolyphonic_
            ? std::clamp(inputs[VOCT_INPUT].getChannels(), 1,
                  static_cast<int>(cella::sfz::MaxAudioOutputLanes))
            : 1;
        if (activeEngine_) {
            const TailBehavior behavior = tailBehavior();
            for (size_t index = 0; index < captureEventCount_; ++index) {
                const cella::sfz::TimedEngineEvent& event = captureEvents_[index];
                if (event.type == cella::sfz::EngineEventType::NoteOn
                    && behavior == TailBehavior::CutOnRetrigger) {
                    activeEngine_->adapter.push(
                        cella::sfz::TimedEngineEvent::chokeLaneTails(
                            event.frameOffset, event.channel, true));
                } else if (event.type == cella::sfz::EngineEventType::NoteOff
                    && behavior == TailBehavior::KeepNewest) {
                    // Remove earlier release generations before this physical
                    // Note Off makes the current generation the newest tail.
                    activeEngine_->adapter.push(
                        cella::sfz::TimedEngineEvent::chokeLaneTails(
                            event.frameOffset, event.channel, false));
                }
                activeEngine_->adapter.push(event);
            }
            if (playbackPolyphonic_) {
                std::array<float*, cella::sfz::MaxAudioOutputLanes> left {};
                std::array<float*, cella::sfz::MaxAudioOutputLanes> right {};
                for (size_t lane = 0; lane < left.size(); ++lane) {
                    left[lane] = playbackLeft_[lane].data();
                    right[lane] = playbackRight_[lane].data();
                }
                activeEngine_->adapter.renderPolyphonic(left.data(), right.data(),
                    static_cast<int>(left.size()), activeRenderQuantum_);
            } else {
                activeEngine_->adapter.render(playbackLeft_[0].data(),
                    playbackRight_[0].data(), activeRenderQuantum_);
            }
            const int activeVoices =
                std::max(0, activeEngine_->engine->activeVoiceCount());
            activeVoiceCount_.store(activeVoices, std::memory_order_release);
            peakVoiceCount_.store(voicePeakHold_.observe(activeVoices,
                    audioTimeNanoseconds_.load(std::memory_order_relaxed)),
                std::memory_order_release);
            voiceLimit_.store(std::max(0, activeEngine_->engine->voiceLimit()),
                std::memory_order_release);
            publishEventDropTelemetry();
        }
        captureEventCount_ = 0;
        playbackFrame_ = 0;
        if (fadeInAfterRender_) {
            fadeInAfterRender_ = false;
            lifecycleFade_ = LifecycleFade::FadingIn;
        }
    }

    float advanceLifecycleFade(float sampleRate) noexcept
    {
        if (loader_->failedInstrumentRecoveryRequested.exchange(false,
                std::memory_order_acq_rel)
            && !sampleRateTransitionActive_
            && !quantumTransitionActive_
            && (lifecycleFade_ == LifecycleFade::FadingOut
                || lifecycleFade_ == LifecycleFade::Silent)) {
            fadeInAfterRender_ = false;
            lifecycleFade_ = LifecycleFade::FadingIn;
        }

        const float outputGain = lifecycleGain_;
        const float step = 1.0f
            / std::max(1.0f, sampleRate * kLifecycleFadeSeconds);
        switch (lifecycleFade_) {
        case LifecycleFade::Steady:
            lifecycleGain_ = 1.0f;
            break;
        case LifecycleFade::FadingOut:
            if (lifecycleGain_ <= step * 1.001f) {
                lifecycleGain_ = 0.0f;
                lifecycleFade_ = LifecycleFade::Silent;
            } else {
                lifecycleGain_ -= step;
            }
            break;
        case LifecycleFade::Silent:
            lifecycleGain_ = 0.0f;
            break;
        case LifecycleFade::FadingIn:
            if (lifecycleGain_ >= 1.0f - step * 1.001f) {
                lifecycleGain_ = 1.0f;
                lifecycleFade_ = LifecycleFade::Steady;
            } else {
                lifecycleGain_ += step;
            }
            break;
        }
        return outputGain;
    }

    void process(const ProcessArgs& args) override
    {
        observedSampleRate_.store(args.sampleRate, std::memory_order_relaxed);
        const uint64_t nanosecondsPerSample = static_cast<uint64_t>(std::max(
            1.0, std::floor(1000000000.0 / std::max(1.0f, args.sampleRate))));
        audioTimeNanoseconds_.fetch_add(
            nanosecondsPerSample, std::memory_order_release);

        const bool playbackAvailable = playbackFrame_ < activeRenderQuantum_;
        const int playbackIndex = playbackFrame_;
        const bool outputPolyphonic = playbackPolyphonic_;
        const int outputChannels = outputPolyphonic ? playbackChannels_ : 1;
        std::array<float, cella::sfz::MaxAudioOutputLanes> leftSamples;
        std::array<float, cella::sfz::MaxAudioOutputLanes> rightSamples;
        for (int channel = 0; channel < outputChannels; ++channel) {
            leftSamples[channel] = playbackAvailable
                ? playbackLeft_[channel][playbackIndex]
                : 0.0f;
            rightSamples[channel] = playbackAvailable
                ? playbackRight_[channel][playbackIndex]
                : 0.0f;
        }
        if (playbackAvailable)
            ++playbackFrame_;

        if (captureFrame_ == 0)
            beginCaptureBlock();
        captureInputFrame();
        if (++captureFrame_ == activeRenderQuantum_) {
            finishCaptureBlock();
            captureFrame_ = 0;
        }

        publishExpressionFeedback();

        const float gain = params[LEVEL_PARAM].getValue() * kNominalOutputVolts;
        const float lifecycleGain = advanceLifecycleFade(args.sampleRate);
        outputs[LEFT_OUTPUT].setChannels(outputChannels);
        outputs[RIGHT_OUTPUT].setChannels(outputChannels);
        const bool rightConnected = outputs[RIGHT_OUTPUT].isConnected();
        bool limiting = false;
        for (int channel = 0; channel < outputChannels; ++channel) {
            const float left = leftSamples[channel];
            const float right = rightSamples[channel];
            const float scaledLeft = left * gain * lifecycleGain;
            const float scaledRight = right * gain * lifecycleGain;
            limiting = limiting || std::abs(scaledLeft) > kLimiterKneeVolts
                || std::abs(scaledRight) > kLimiterKneeVolts;
            const float limitedLeft = softLimit(scaledLeft);
            const float limitedRight = softLimit(scaledRight);
            outputs[LEFT_OUTPUT].setVoltage(rightConnected
                    ? limitedLeft
                    : 0.5f * (limitedLeft + limitedRight),
                channel);
            outputs[RIGHT_OUTPUT].setVoltage(limitedRight, channel);
        }
        if (limiting) {
            outputLimitingCount_.fetch_add(1, std::memory_order_release);
            outputLimitingWarningUntilNanoseconds_.store(
                audioTimeNanoseconds_.load(std::memory_order_relaxed)
                    + 1000000000ull,
                std::memory_order_release);
        }
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();
        json_object_set_new(root, "schemaVersion", json_integer(kSchemaVersion));
        json_object_set_new(root, "renderQuantum", json_integer(renderQuantum()));
        json_object_set_new(root, "tailBehavior",
            json_string(tailBehaviorKey(tailBehavior())));
        json_object_set_new(root, "displayPage",
            json_integer(static_cast<int>(displayPage())));
        const std::shared_ptr<const std::string> path = selectedPath();
        if (path && !path->empty()) {
            json_object_set_new(root, "sfzPath",
                json_stringn(path->c_str(), path->size()));
            const std::string relative = relativeToCurrentPatch(*path);
            if (!relative.empty())
                json_object_set_new(root, "sfzPathRelative",
                    json_stringn(relative.c_str(), relative.size()));
        }

        const std::shared_ptr<const StatusSnapshot> last =
            std::atomic_load_explicit(
                &loader_->lastSuccessfulStatus, std::memory_order_acquire);
        if (last && last->state == StatusState::Ready) {
            json_t* status = json_object();
            json_object_set_new(status, "regionCount", json_integer(last->regionCount));
            json_object_set_new(status, "estimatedSampleBytes",
                json_integer(static_cast<json_int_t>(std::min<size_t>(
                    last->estimatedSampleBytes,
                    static_cast<size_t>(INT64_MAX)))));
            json_object_set_new(status, "preloadedSampleCount",
                json_integer(static_cast<json_int_t>(std::min<size_t>(
                    last->preloadedSampleCount,
                    static_cast<size_t>(INT64_MAX)))));
            json_object_set_new(root, "lastSuccessfulLoad", status);
        }
        return root;
    }

    void dataFromJson(json_t* root) override
    {
        DisplayPage page = DisplayPage::Play;
        if (json_t* pageValue = json_object_get(root, "displayPage");
            json_is_integer(pageValue)
            && json_integer_value(pageValue)
                == static_cast<int>(DisplayPage::Info)) {
            page = DisplayPage::Info;
        }
        setDisplayPage(page);

        if (json_t* quantumValue = json_object_get(root, "renderQuantum");
            json_is_integer(quantumValue)) {
            const int frames = static_cast<int>(json_integer_value(quantumValue));
            if (isRenderQuantum(frames))
                setRenderQuantum(frames);
        }
        if (const char* behavior =
                json_string_value(json_object_get(root, "tailBehavior"))) {
            TailBehavior parsed = TailBehavior::PreserveAll;
            if (std::string(behavior) == "keepNewest")
                parsed = TailBehavior::KeepNewest;
            else if (std::string(behavior) == "cutOnRetrigger")
                parsed = TailBehavior::CutOnRetrigger;
            tailBehaviorSetting_.store(parsed, std::memory_order_release);
        }

        const char* absoluteValue = json_string_value(json_object_get(root, "sfzPath"));
        const char* relativeValue =
            json_string_value(json_object_get(root, "sfzPathRelative"));
        const std::string absolute = absoluteValue ? absoluteValue : "";
        const std::string relative = relativeValue ? relativeValue : "";
        if (absolute.empty() && relative.empty())
            return;

        const std::string relativeCandidate = persistedRelativeCandidate(relative);
        const std::string remembered = absolute.empty() ? relativeCandidate : absolute;
        const std::string fallback = !absolute.empty()
                && relativeCandidate != absolute
            ? relativeCandidate
            : std::string();
        std::shared_ptr<const std::string> selected =
            std::make_shared<const std::string>(remembered);
        std::atomic_store_explicit(
            &loader_->selectedPath, selected, std::memory_order_release);

        int regions = 0;
        size_t bytes = 0;
        size_t preloadedSamples = 0;
        if (json_t* status = json_object_get(root, "lastSuccessfulLoad")) {
            if (json_t* regionsValue = json_object_get(status, "regionCount");
                json_is_integer(regionsValue))
                regions = std::max<json_int_t>(0, json_integer_value(regionsValue));
            if (json_t* bytesValue = json_object_get(status, "estimatedSampleBytes");
                json_is_integer(bytesValue))
                bytes = static_cast<size_t>(std::max<json_int_t>(
                    0, json_integer_value(bytesValue)));
            if (json_t* samplesValue = json_object_get(status,
                    "preloadedSampleCount");
                json_is_integer(samplesValue)) {
                preloadedSamples = static_cast<size_t>(std::max<json_int_t>(
                    0, json_integer_value(samplesValue)));
            }
        }
        StatusSnapshot rememberedStatus;
        rememberedStatus.state = StatusState::Ready;
        rememberedStatus.filename = system::getFilename(remembered);
        rememberedStatus.regionCount = regions;
        rememberedStatus.estimatedSampleBytes = bytes;
        rememberedStatus.absolutePath = remembered;
        rememberedStatus.preloadedSampleCount = preloadedSamples;
        std::shared_ptr<const StatusSnapshot> last =
            std::make_shared<const StatusSnapshot>(rememberedStatus);
        std::atomic_store_explicit(&loader_->lastSuccessfulStatus, last,
            std::memory_order_release);
        if (remembered.empty()) {
            publishStatus(loader_,
                { StatusState::Missing, {}, "MISSING SFZ", 0, 0 });
            return;
        }
        std::atomic_store_explicit(
            &loader_->statusSnapshot, last, std::memory_order_release);
        loadInstrument(remembered, fallback);
    }
};

constexpr size_t kDisplayTitleCharacters = 27;
constexpr size_t kDisplayTitleWithPositionCharacters = 18;

enum class DisplayColorRole : uint8_t {
    Normal,
    Amber,
    Red,
};

DisplayColorRole displayColorRoleForStatus(
    CellaSFZ::StatusState state) noexcept
{
    return state == CellaSFZ::StatusState::Error
            || state == CellaSFZ::StatusState::Missing
        ? DisplayColorRole::Red
        : DisplayColorRole::Normal;
}

DisplayColorRole displayColorRoleForWarning(DisplayWarning warning) noexcept
{
    if (warning == DisplayWarning::EventDrop)
        return DisplayColorRole::Red;
    if (warning == DisplayWarning::OutputLimiting)
        return DisplayColorRole::Amber;
    return DisplayColorRole::Normal;
}

DisplayColorRole displayColorRoleForPressure(VoicePressure pressure) noexcept
{
    if (pressure == VoicePressure::Red)
        return DisplayColorRole::Red;
    if (pressure == VoicePressure::Amber)
        return DisplayColorRole::Amber;
    return DisplayColorRole::Normal;
}

struct ReadyDisplayLayout {
    std::string title;
    std::string position;
    std::string middleLeft;
    std::string middleRight;
    std::string bottomLeft;
    std::string bottomRight;
};

ReadyDisplayLayout readyDisplayLayout(
    const CellaSFZ::StatusSnapshot& status, CellaSFZ::DisplayPage page,
    const DisplayRuntimeSnapshot& runtime, DisplayWarning warning)
{
    ReadyDisplayLayout layout;
    const std::string position = folderPosition(status.folderIndex,
        status.folderCount);
    const std::string filename = filenameWithoutSfzExtension(status.filename);
    layout.position = position;
    layout.title = shortenMiddle(filename, position.empty()
            ? kDisplayTitleCharacters
            : kDisplayTitleWithPositionCharacters);
    if (page == CellaSFZ::DisplayPage::Play) {
        const int limit = std::max(0, runtime.voiceLimit);
        const int voiceWidth = std::max<int>(3,
            static_cast<int>(std::to_string(limit).size()));
        layout.middleLeft = rack::string::f("Held %02d",
            std::max(0, runtime.heldLanes));
        layout.middleRight = rack::string::f("Voices %0*d / %d", voiceWidth,
            std::max(0, runtime.activeVoices), limit);
        layout.bottomLeft = rack::string::f("Controls %d / %zu",
            std::clamp(runtime.assignedCcCount, 0,
                static_cast<int>(std::min<size_t>(
                    status.assignableCcCount,
                    cella::sfz::NamedControlCount))),
            status.assignableCcCount);
        layout.bottomRight = rack::string::f("Switches %zu",
            status.keyswitchCount);
    } else {
        layout.middleLeft = rack::string::f("Regions %d",
            std::max(0, status.regionCount));
        layout.middleRight = rack::string::f("Samples %zu",
            status.preloadedSampleCount);
        const std::string bytes = memoryLabel(status.estimatedSampleBytes);
        layout.bottomLeft = rack::string::f("Preload %s%s",
            status.estimatedSampleBytes == 0 ? "" : "~", bytes.c_str());
    }
    if (warning == DisplayWarning::EventDrop) {
        layout.bottomLeft = "EVENT DROP";
        layout.bottomRight.clear();
    } else if (warning == DisplayWarning::OutputLimiting) {
        layout.bottomLeft = "OUTPUT LIMITING";
        layout.bottomRight.clear();
    }
    return layout;
}

struct CellaSFZStatusDisplay : LedDisplay {
    CellaSFZ* module { nullptr };
    std::function<void()> loadAction;
    ui::Tooltip* tooltip_ { nullptr };

    ~CellaSFZStatusDisplay() override
    {
        destroyTooltip();
    }

    void destroyTooltip()
    {
        if (!tooltip_)
            return;
        if (tooltip_->parent)
            tooltip_->parent->removeChild(tooltip_);
        delete tooltip_;
        tooltip_ = nullptr;
    }

    std::string tooltipText() const
    {
        if (!module)
            return {};
        const auto status = module->statusSnapshot();
        if (!status)
            return {};
        std::string text = status->absolutePath;
        if (text.empty()) {
            const auto selected = module->selectedPath();
            if (selected)
                text = *selected;
        }
        if (module->displayPage() == CellaSFZ::DisplayPage::Info) {
            if (!text.empty())
                text += "\n";
            text += "Preload is an estimate of sfizioso preload buffers; "
                    "it is not process RAM or total instrument size.";
        }
        return text;
    }

    void onHover(const event::Hover& event) override
    {
        event.consume(this);
    }

    void appendDisplayContextMenu(ui::Menu* menu)
    {
        menu->addChild(createMenuLabel("SFZ instrument"));
        menu->addChild(createMenuItem("Load SFZ...", "", loadAction,
            !static_cast<bool>(loadAction)));
        const bool canUnload = module
            && (module->statusSnapshot()->state
                    != CellaSFZ::StatusState::Unloaded
                || !module->navigationPath().empty());
        menu->addChild(createMenuItem("Unload instrument", "", [module = module]() {
            if (module)
                module->unloadInstrumentFromUi();
        }, !canUnload));
    }

    void openContextMenu()
    {
        ui::Menu* menu = createMenu();
        appendDisplayContextMenu(menu);
    }

    void onButton(const event::Button& event) override
    {
        if (event.action == GLFW_PRESS
            && event.button == GLFW_MOUSE_BUTTON_RIGHT
            && (event.mods & RACK_MOD_MASK) == 0) {
            openContextMenu();
            event.consume(this);
        }
    }

    void onPathDrop(const PathDropEvent& event) override
    {
        if (!module)
            return;
        for (const std::string& path : event.paths) {
            if (!hasSfzExtension(path) || !system::isFile(path))
                continue;
            module->loadInstrumentFromUi(path);
            event.consume(this);
            return;
        }
    }

    void onEnter(const event::Enter& event) override
    {
        Widget::onEnter(event);
        const std::string text = tooltipText();
        if (!settings::tooltips || text.empty() || tooltip_)
            return;
        tooltip_ = new ui::Tooltip;
        tooltip_->text = text;
        APP->scene->addChild(tooltip_);
    }

    void onLeave(const event::Leave& event) override
    {
        Widget::onLeave(event);
        destroyTooltip();
    }

    void step() override
    {
        LedDisplay::step();
        if (tooltip_)
            tooltip_->text = tooltipText();
    }

    void draw(const DrawArgs& args) override
    {
        LedDisplay::draw(args);

        if (!module)
            return;
        const std::shared_ptr<const CellaSFZ::StatusSnapshot> status =
            module->statusSnapshot();
        if (!status)
            return;
        const std::shared_ptr<Font> font = APP->window->loadFont(
            asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        if (!font)
            return;

        const NVGcolor normalColor = nvgRGB(0xf7, 0xc5, 0xad);
        const NVGcolor mutedColor = nvgRGBA(0xf7, 0xc5, 0xad, 0xa0);
        const NVGcolor amberColor = nvgRGB(0xf5, 0xc5, 0x72);
        const NVGcolor redColor = nvgRGB(0xff, 0x7d, 0x78);
        const auto colorForRole = [&](DisplayColorRole role) {
            if (role == DisplayColorRole::Red)
                return redColor;
            if (role == DisplayColorRole::Amber)
                return amberColor;
            return normalColor;
        };
        const NVGcolor statusColor = colorForRole(
            displayColorRoleForStatus(status->state));
        DisplayRuntimeSnapshot runtime = module->displayRuntimeSnapshot();
        DisplayWarning warning = DisplayWarning::None;
        ReadyDisplayLayout layout;
        std::array<std::string, 3> centeredLines;
        bool showReadyLayout = false;
        bool showStatusLayout = false;
        std::string statusMessage;
        const auto prepareStatusLayout = [&]() {
            layout.position = folderPosition(
                status->folderIndex, status->folderCount);
            layout.title = shortenMiddle(
                filenameWithoutSfzExtension(status->filename),
                layout.position.empty()
                    ? kDisplayTitleCharacters
                    : kDisplayTitleWithPositionCharacters);
            showStatusLayout = true;
        };
        switch (status->state) {
        case CellaSFZ::StatusState::Unloaded:
            centeredLines[1] = "NO INSTRUMENT";
            break;
        case CellaSFZ::StatusState::Loading:
            prepareStatusLayout();
            statusMessage = "LOADING";
            break;
        case CellaSFZ::StatusState::Ready:
            if (!module->readyStatusDescribesActiveEngine(*status)) {
                prepareStatusLayout();
                statusMessage = "LOADING";
            } else {
                warning = displayWarningFor(runtime);
                layout = readyDisplayLayout(
                    *status, module->displayPage(), runtime, warning);
                showReadyLayout = true;
            }
            break;
        case CellaSFZ::StatusState::Error:
            prepareStatusLayout();
            statusMessage = status->message;
            break;
        case CellaSFZ::StatusState::Missing:
            prepareStatusLayout();
            statusMessage = "MISSING SFZ";
            break;
        }

        nvgSave(args.vg);
        nvgScissor(args.vg, 3.0f, 3.0f,
            box.size.x - 6.0f, box.size.y - 6.0f);
        nvgFontFaceId(args.vg, font->handle);

        const auto drawFittedText = [&](const std::string& text, float x,
                                        float y, int align, float maxWidth,
                                        float fontSize, NVGcolor textColor) {
            if (text.empty())
                return;
            nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
            nvgFontSize(args.vg, fontSize);
            float bounds[4];
            const float width = nvgTextBounds(args.vg, 0.0f, 0.0f,
                text.c_str(), nullptr, bounds);
            if (width > maxWidth)
                fontSize = std::max(7.0f, fontSize * maxWidth / width);
            nvgFontSize(args.vg, fontSize);
            nvgFillColor(args.vg, textColor);
            nvgText(args.vg, x, y, text.c_str(), nullptr);
        };

        if (showReadyLayout) {
            const float titleWidth = layout.position.empty() ? 137.0f : 104.0f;
            drawFittedText(layout.title, 7.0f, 13.5f, NVG_ALIGN_LEFT,
                titleWidth, 11.0f, normalColor);
            drawFittedText(layout.position, 151.0f, 13.5f, NVG_ALIGN_RIGHT,
                39.0f, 9.5f, mutedColor);
            drawFittedText(layout.middleLeft, 7.0f, 33.5f, NVG_ALIGN_LEFT,
                65.0f, 10.5f, normalColor);
            drawFittedText(layout.middleRight, 173.0f, 33.5f,
                NVG_ALIGN_RIGHT, 101.0f, 10.5f, normalColor);

            if (warning != DisplayWarning::None) {
                drawFittedText(layout.bottomLeft, box.size.x * 0.5f, 55.0f,
                    NVG_ALIGN_CENTER, box.size.x - 14.0f, 10.5f,
                    colorForRole(displayColorRoleForWarning(warning)));
            } else {
                drawFittedText(layout.bottomLeft, 7.0f, 55.0f,
                    NVG_ALIGN_LEFT, 96.0f, 10.0f, normalColor);
                drawFittedText(layout.bottomRight, 173.0f, 55.0f,
                    NVG_ALIGN_RIGHT, 72.0f, 10.0f, normalColor);
            }
        } else if (showStatusLayout) {
            const float titleWidth = layout.position.empty() ? 137.0f : 104.0f;
            drawFittedText(layout.title, 7.0f, 13.5f, NVG_ALIGN_LEFT,
                titleWidth, 11.0f, statusColor);
            drawFittedText(layout.position, 151.0f, 13.5f, NVG_ALIGN_RIGHT,
                39.0f, 9.5f, statusColor);
            drawFittedText(statusMessage, box.size.x * 0.5f, 43.0f,
                NVG_ALIGN_CENTER, box.size.x - 20.0f, 12.0f, statusColor);
        } else {
            const std::array<float, 3> rowY { 13.5f, 32.0f, 55.0f };
            for (size_t index = 0; index < centeredLines.size(); ++index) {
                if (centeredLines[index].empty())
                    continue;
                drawFittedText(centeredLines[index], box.size.x * 0.5f,
                    rowY[index], NVG_ALIGN_CENTER, box.size.x - 20.0f,
                    11.5f, statusColor);
            }
        }

        if (status->state == CellaSFZ::StatusState::Ready
            && module->readyStatusDescribesActiveEngine(*status)
            && module->displayPage() == CellaSFZ::DisplayPage::Play
            && runtime.voiceLimit > 0) {
            const int peak = std::max(runtime.activeVoices, runtime.peakVoices);
            const float fraction = std::clamp(static_cast<float>(peak)
                    / static_cast<float>(runtime.voiceLimit),
                0.0f, 1.0f);
            const NVGcolor meterColor = colorForRole(displayColorRoleForPressure(
                voicePressureFor(peak, runtime.voiceLimit)));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 7.0f, 44.0f, 166.0f, 1.5f);
            nvgFillColor(args.vg, nvgRGBA(0xf7, 0xc5, 0xad, 0x28));
            nvgFill(args.vg);
            if (fraction > 0.0f) {
                nvgBeginPath(args.vg);
                nvgRect(args.vg, 7.0f, 44.0f, 166.0f * fraction, 1.5f);
                nvgFillColor(args.vg, meterColor);
                nvgFill(args.vg);
            }
        }
        nvgRestore(args.vg);
    }
};

struct CellaSFZPageDot : Widget {
    CellaSFZ* module { nullptr };
    CellaSFZ::DisplayPage page { CellaSFZ::DisplayPage::Play };
    ui::Tooltip* tooltip_ { nullptr };
    bool hovered_ { false };
    bool pressed_ { false };

    ~CellaSFZPageDot() override
    {
        destroyTooltip();
    }

    void destroyTooltip()
    {
        if (!tooltip_)
            return;
        if (tooltip_->parent)
            tooltip_->parent->removeChild(tooltip_);
        delete tooltip_;
        tooltip_ = nullptr;
    }

    std::string tooltipText() const
    {
        return page == CellaSFZ::DisplayPage::Play
            ? "Show performance"
            : "Show instrument info";
    }

    void onHover(const event::Hover& event) override
    {
        event.consume(this);
    }

    void onEnter(const event::Enter& event) override
    {
        Widget::onEnter(event);
        hovered_ = true;
        if (!settings::tooltips || tooltip_)
            return;
        tooltip_ = new ui::Tooltip;
        tooltip_->text = tooltipText();
        APP->scene->addChild(tooltip_);
    }

    void onLeave(const event::Leave& event) override
    {
        Widget::onLeave(event);
        hovered_ = false;
        pressed_ = false;
        destroyTooltip();
    }

    void step() override
    {
        Widget::step();
        if (tooltip_)
            tooltip_->text = tooltipText();
    }

    void onButton(const event::Button& event) override
    {
        if (event.button != GLFW_MOUSE_BUTTON_LEFT)
            return;
        if (event.action == GLFW_PRESS) {
            pressed_ = true;
            if (module)
                module->setDisplayPage(page);
            event.consume(this);
        } else if (event.action == GLFW_RELEASE) {
            pressed_ = false;
            event.consume(this);
        }
    }

    void draw(const DrawArgs& args) override
    {
        const bool selected = module && module->displayPage() == page;
        const NVGcolor primary = nvgRGB(0xf7, 0xc5, 0xad);
        const NVGcolor bright = nvgRGB(0xff, 0xe7, 0xdc);
        const NVGcolor color = hovered_ || pressed_ ? bright : primary;
        const float cx = box.size.x * 0.5f;
        const float cy = box.size.y * 0.5f;
        if (hovered_ || pressed_) {
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, cx, cy, pressed_ ? 4.5f : 4.0f);
            nvgFillColor(args.vg, nvgRGBA(0xf7, 0xc5, 0xad,
                pressed_ ? 0x38 : 0x24));
            nvgFill(args.vg);
        }
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, selected ? 2.6f : 2.35f);
        if (selected) {
            nvgFillColor(args.vg, color);
            nvgFill(args.vg);
        } else {
            nvgStrokeWidth(args.vg, 1.0f);
            nvgStrokeColor(args.vg, hovered_
                    ? color
                    : nvgRGBA(0xf7, 0xc5, 0xad, 0x90));
            nvgStroke(args.vg);
        }
    }
};

struct CellaSFZWidget : ModuleWidget {
    dsp::SchmittTrigger loadButtonTrigger_;
    dsp::SchmittTrigger previousButtonTrigger_;
    dsp::SchmittTrigger nextButtonTrigger_;

    explicit CellaSFZWidget(CellaSFZ* module)
    {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/CellaSFZ.svg"),
            asset::plugin(pluginInstance, "res/CellaSFZ-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewGrey>(Vec(165, 0)));
        addChild(createWidget<ScrewGrey>(Vec(165, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        auto* display = createWidget<CellaSFZStatusDisplay>(Vec(0, 25));
        display->box.size = Vec(180, 64);
        display->module = module;
        display->loadAction = [this]() { loadDialog(); };
        addChild(display);
        auto* playPage = createWidget<CellaSFZPageDot>(Vec(154, 26));
        playPage->box.size = Vec(12, 15);
        playPage->module = module;
        playPage->page = CellaSFZ::DisplayPage::Play;
        addChild(playPage);
        auto* infoPage = createWidget<CellaSFZPageDot>(Vec(166, 26));
        infoPage->box.size = Vec(12, 15);
        infoPage->module = module;
        infoPage->page = CellaSFZ::DisplayPage::Info;
        addChild(infoPage);

        addParam(createParamCentered<RoundBlackKnob>(Vec(35, 130), module,
            CellaSFZ::LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(90, 130), module,
            CellaSFZ::OCTAVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(145, 130), module,
            CellaSFZ::TUNE_PARAM));
        addParam(createParamCentered<VCVButtonHuge>(Vec(90, 185), module,
            CellaSFZ::LOAD_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(55, 190), module,
            CellaSFZ::PREVIOUS_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(125, 190), module,
            CellaSFZ::NEXT_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(35, 265), module,
            CellaSFZ::VOCT_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(90, 265), module,
            CellaSFZ::GATE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(145, 265), module,
            CellaSFZ::VELOCITY_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(62, 330), module,
            CellaSFZ::LEFT_OUTPUT));
        addParam(createParamCentered<VCVButtonHugeToggle>(Vec(90, 330), module,
            CellaSFZ::POLY_OUTPUT_PARAM));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(118, 330), module,
            CellaSFZ::RIGHT_OUTPUT));
    }

    void loadDialog()
    {
        auto* module = dynamic_cast<CellaSFZ*>(this->module);
        if (!module)
            return;
        const std::shared_ptr<const std::string> selected = module->selectedPath();
        const std::string directory = selected && !selected->empty()
            ? system::getDirectory(*selected)
            : std::string();
        osdialog_filters* filters = osdialog_filters_parse("SFZ instrument:sfz");
        char* path = osdialog_file(OSDIALOG_OPEN,
            directory.empty() ? nullptr : directory.c_str(), nullptr, filters);
        osdialog_filters_free(filters);
        if (!path)
            return;
        const std::string chosen(path);
        std::free(path);
        module->loadInstrumentFromUi(chosen);
    }

    void step() override
    {
        ModuleWidget::step();
        auto* module = dynamic_cast<CellaSFZ*>(this->module);
        if (!module)
            return;
        if (loadButtonTrigger_.process(
                module->params[CellaSFZ::LOAD_PARAM].getValue()))
            loadDialog();
        if (previousButtonTrigger_.process(
                module->params[CellaSFZ::PREVIOUS_PARAM].getValue()))
            module->navigateInstrumentFromUi(-1);
        if (nextButtonTrigger_.process(
                module->params[CellaSFZ::NEXT_PARAM].getValue()))
            module->navigateInstrumentFromUi(1);
    }

    void appendContextMenu(Menu* menu) override
    {
        ModuleWidget::appendContextMenu(menu);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Load SFZ...", "", [this]() { loadDialog(); }));
        auto* sfzModule = dynamic_cast<CellaSFZ*>(module);
        if (!sfzModule)
            return;

        menu->addChild(createSubmenuItem("Render quantum",
            rack::string::f("%d frames", sfzModule->renderQuantum()),
            [sfzModule](Menu* quantumMenu) {
                for (const int frames : { 16, 32, 64 }) {
                    quantumMenu->addChild(createCheckMenuItem(
                        rack::string::f("%d frames", frames), "",
                        [sfzModule, frames]() {
                            return sfzModule->renderQuantum() == frames;
                        },
                        [sfzModule, frames]() {
                            sfzModule->setRenderQuantum(frames);
                        }));
                }
            }));
        menu->addChild(createSubmenuItem("Release tails",
            CellaSFZ::tailBehaviorLabel(sfzModule->tailBehavior()),
            [sfzModule](Menu* tailMenu) {
                for (const CellaSFZ::TailBehavior behavior : {
                         CellaSFZ::TailBehavior::PreserveAll,
                         CellaSFZ::TailBehavior::KeepNewest,
                         CellaSFZ::TailBehavior::CutOnRetrigger }) {
                    tailMenu->addChild(createCheckMenuItem(
                        CellaSFZ::tailBehaviorLabel(behavior), "",
                        [sfzModule, behavior]() {
                            return sfzModule->tailBehavior() == behavior;
                        },
                        [sfzModule, behavior]() {
                            sfzModule->setTailBehavior(behavior);
                        }));
                }
            }));
    }
};

struct CellaSFZExpression : Module {
    enum InputId {
        BEND_INPUT,
        PRESSURE_INPUT,
        TIMBRE_INPUT,
        CONTROL_1_INPUT,
        CONTROL_2_INPUT,
        CONTROL_3_INPUT,
        CONTROL_4_INPUT,
        ARTICULATION_INPUT,
        INPUTS_LEN
    };
    enum ParamId { PARAMS_LEN };
    enum OutputId { OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    static constexpr int SchemaVersion = 1;
    std::array<cella::sfz::ExpressionMessage, 2> expressionMessages_ {};
    std::array<std::atomic<int>, cella::sfz::NamedControlCount> assignments_;
    std::atomic<int> manualArticulation_ { -1 };
    std::atomic<bool> connected_ { false };
    std::atomic<int> activeLanes_ { 1 };
    std::array<std::atomic<int>, cella::sfz::ExpressionLaneCount>
        activeArticulations_;
    uint32_t sequence_ { 0 };
    uint32_t metadataGeneration_ { 0 };
    cella::sfz::SelectorQuantizer selectorQuantizer_;

    CellaSFZExpression()
    {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configInput(BEND_INPUT, "Additive 1 V/octave bend");
        configInput(PRESSURE_INPUT, "Pressure");
        configInput(TIMBRE_INPUT, "Timbre (CC74)");
        for (int slot = 0; slot < 4; ++slot)
            configInput(CONTROL_1_INPUT + slot,
                rack::string::f("Assignable control %d", slot + 1));
        configInput(ARTICULATION_INPUT, "Articulation select");
        leftExpander.producerMessage = &expressionMessages_[0];
        leftExpander.consumerMessage = &expressionMessages_[1];
        for (auto& assignment : assignments_)
            assignment.store(-1, std::memory_order_relaxed);
        for (auto& articulation : activeArticulations_)
            articulation.store(-1, std::memory_order_relaxed);
    }

    void setAssignment(size_t slot, int cc) noexcept
    {
        if (slot >= assignments_.size())
            return;
        if (cc < 0 || cc >= 128 || cc == 74 || cc == 120 || cc == 121
            || cc == 123) {
            assignments_[slot].store(-1, std::memory_order_relaxed);
            return;
        }
        for (size_t other = 0; other < assignments_.size(); ++other) {
            if (other != slot
                && assignments_[other].load(std::memory_order_relaxed) == cc) {
                assignments_[other].store(-1, std::memory_order_relaxed);
            }
        }
        assignments_[slot].store(cc, std::memory_order_relaxed);
    }

    CellaSFZ* baseModule() const noexcept
    {
        Module* neighbor = leftExpander.module;
        if (!neighbor || neighbor->model != modelCellaSFZ)
            return nullptr;
        return dynamic_cast<CellaSFZ*>(neighbor);
    }

    const cella::sfz::ExpressionFeedback* feedback() const noexcept
    {
        Module* neighbor = leftExpander.module;
        if (!neighbor || neighbor->model != modelCellaSFZ
            || neighbor->isBypassed())
            return nullptr;
        const auto* value = static_cast<const cella::sfz::ExpressionFeedback*>(
            neighbor->rightExpander.consumerMessage);
        return value && value->magic == cella::sfz::ExpressionMessageMagic
            ? value
            : nullptr;
    }

    void process(const ProcessArgs&) override
    {
        const cella::sfz::ExpressionFeedback* currentFeedback = feedback();
        connected_.store(currentFeedback != nullptr, std::memory_order_relaxed);
        int switchCount = 0;
        int lanes = 1;
        if (currentFeedback) {
            switchCount = currentFeedback->keyswitchCount;
            lanes = std::clamp<int>(currentFeedback->activeLanes, 1, 16);
            activeLanes_.store(lanes, std::memory_order_relaxed);
            for (size_t lane = 0; lane < activeArticulations_.size(); ++lane) {
                activeArticulations_[lane].store(
                    currentFeedback->activeArticulations[lane],
                    std::memory_order_relaxed);
            }
            bool resetQuantizer = metadataGeneration_
                != currentFeedback->metadataGeneration;
            metadataGeneration_ = currentFeedback->metadataGeneration;
            // Generations are local to each Cella SFZ instance, so two bases
            // can legitimately report the same value. This bounded check must
            // therefore run whenever feedback is present, not just when the
            // numeric generation changes.
            for (auto& assignment : assignments_) {
                const int cc = assignment.load(std::memory_order_relaxed);
                if (cc >= 0 && (cc >= 128
                        || currentFeedback->assignableCCs[cc] == 0))
                    assignment.store(-1, std::memory_order_relaxed);
            }
            const int manual = manualArticulation_.load(
                std::memory_order_relaxed);
            if (manual >= 0 && (switchCount == 0 || manual >= switchCount)) {
                manualArticulation_.store(-1, std::memory_order_relaxed);
                resetQuantizer = true;
            }
            if (resetQuantizer) {
                selectorQuantizer_.reset(manualArticulation_.load(
                    std::memory_order_relaxed));
            }
        } else {
            activeLanes_.store(1, std::memory_order_relaxed);
            for (auto& articulation : activeArticulations_)
                articulation.store(-1, std::memory_order_relaxed);
        }

        auto& message = *static_cast<cella::sfz::ExpressionMessage*>(
            leftExpander.producerMessage);
        message = {};
        message.magic = cella::sfz::ExpressionMessageMagic;
        message.sequence = ++sequence_;
        for (int input = 0; input < static_cast<int>(cella::sfz::ExpressionInputCount);
             ++input) {
            const int channels = std::clamp(inputs[input].getChannels(), 0, 16);
            message.inputs[input].channels = static_cast<uint8_t>(channels);
            for (int lane = 0; lane < channels; ++lane)
                message.inputs[input].values[lane] = inputs[input].getVoltage(lane);
        }
        for (size_t slot = 0; slot < assignments_.size(); ++slot) {
            message.assignments[slot] = static_cast<int16_t>(
                assignments_[slot].load(std::memory_order_relaxed));
        }

        const int manual = manualArticulation_.load(std::memory_order_relaxed);
        const int selectorChannels = std::clamp(
            inputs[ARTICULATION_INPUT].getChannels(), 0, 16);
        for (int lane = 0; lane < 16; ++lane) {
            int selection = manual;
            if (selectorChannels == 1) {
                selection = selectorQuantizer_.process(lane,
                    inputs[ARTICULATION_INPUT].getVoltage(0), switchCount);
            } else if (selectorChannels > 1 && lane < selectorChannels) {
                selection = selectorQuantizer_.process(lane,
                    inputs[ARTICULATION_INPUT].getVoltage(lane), switchCount);
            }
            message.articulationIndices[lane] = static_cast<int16_t>(selection);
        }
        leftExpander.messageFlipRequested = true;
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();
        json_object_set_new(root, "schemaVersion", json_integer(SchemaVersion));
        json_t* assignments = json_array();
        for (const auto& assignment : assignments_)
            json_array_append_new(assignments,
                json_integer(assignment.load(std::memory_order_relaxed)));
        json_object_set_new(root, "assignments", assignments);
        json_object_set_new(root, "manualArticulation",
            json_integer(manualArticulation_.load(std::memory_order_relaxed)));
        return root;
    }

    void dataFromJson(json_t* root) override
    {
        if (json_t* values = json_object_get(root, "assignments");
            json_is_array(values)) {
            for (size_t slot = 0; slot < assignments_.size(); ++slot) {
                json_t* value = json_array_get(values, slot);
                if (json_is_integer(value)) {
                    const int cc = static_cast<int>(json_integer_value(value));
                    setAssignment(slot, cc);
                }
            }
        }
        if (json_t* value = json_object_get(root, "manualArticulation");
            json_is_integer(value)) {
            manualArticulation_.store(std::clamp<int>(
                static_cast<int>(json_integer_value(value)), -1, 127),
                std::memory_order_relaxed);
        }
    }
};

std::string midiNoteName(int note)
{
    static constexpr const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    note = std::clamp(note, 0, 127);
    return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
}

struct CellaExpressionDisplay : TransparentWidget {
    CellaSFZExpression* module { nullptr };
    int slot { -1 }; // 0..3 named control, -1 articulation

    std::shared_ptr<const cella::sfz::InstrumentMetadata> metadata() const
    {
        CellaSFZ* base = module ? module->baseModule() : nullptr;
        return base ? base->instrumentMetadata() : nullptr;
    }

    std::string displayText() const
    {
        if (!module)
            return slot >= 0 ? rack::string::f("CONTROL %d", slot + 1)
                             : "ARTICULATION";
        const auto info = metadata();
        if (slot >= 0) {
            const int cc = module->assignments_[slot].load(
                std::memory_order_relaxed);
            if (info) {
                if (const auto* control = cella::sfz::findNamedController(*info, cc))
                    return shorten(control->label, 20);
            }
            return rack::string::f("ASSIGN %d", slot + 1);
        }
        if (!module->connected_.load(std::memory_order_relaxed))
            return "NO SFZ";
        const int lanes = module->activeLanes_.load(std::memory_order_relaxed);
        int selected = module->activeArticulations_[0].load(
            std::memory_order_relaxed);
        for (int lane = 1; lane < lanes; ++lane) {
            if (module->activeArticulations_[lane].load(
                    std::memory_order_relaxed) != selected)
                return "POLY";
        }
        if (!info || info->latchedKeyswitches.empty())
            return "NO SWITCHES";
        if (selected < 0
            || static_cast<size_t>(selected) >= info->latchedKeyswitches.size())
            return "SFZ DEFAULT";
        const auto& keyswitch = info->latchedKeyswitches[selected];
        return shorten(keyswitch.label.empty() ? midiNoteName(keyswitch.note)
                                                : keyswitch.label,
            20);
    }

    void draw(const DrawArgs& args) override
    {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.0f);
        nvgFillColor(args.vg, nvgRGB(0x12, 0x16, 0x1b));
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 0.8f);
        nvgStrokeColor(args.vg, nvgRGB(0x58, 0x68, 0x73));
        nvgStroke(args.vg);
        const auto font = APP->window->loadFont(
            asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        if (!font)
            return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, slot >= 0 ? 8.0f : 9.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGB(0x9c, 0xe5, 0xd8));
        const std::string text = displayText();
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f,
            text.c_str(), nullptr);
    }

    void appendSelectionMenu(ui::Menu* menu)
    {
        if (!menu || !module)
            return;
        const auto info = metadata();
        if (slot >= 0) {
            menu->addChild(createCheckMenuItem("Unassigned", "",
                [this]() {
                    return module->assignments_[slot].load(
                               std::memory_order_relaxed) < 0;
                },
                [this]() {
                    module->setAssignment(slot, -1);
                }));
            if (info) {
                for (const auto& control : info->namedControllers) {
                    if (control.number == 74)
                        continue;
                    const int cc = control.number;
                    const std::string generic = rack::string::f("CC %d", cc);
                    const std::string label = control.label == generic
                        ? generic
                        : rack::string::f("%s (CC%d)", control.label.c_str(), cc);
                    menu->addChild(createCheckMenuItem(
                        label, "",
                        [this, cc]() {
                            return module->assignments_[slot].load(
                                       std::memory_order_relaxed) == cc;
                        },
                        [this, cc]() {
                            module->setAssignment(slot, cc);
                        }));
                }
            }
        } else if (info) {
            for (size_t index = 0; index < info->latchedKeyswitches.size(); ++index) {
                const auto& keyswitch = info->latchedKeyswitches[index];
                const std::string label = keyswitch.label.empty()
                    ? midiNoteName(keyswitch.note)
                    : keyswitch.label;
                menu->addChild(createCheckMenuItem(label, "",
                    [this, index]() {
                        return module->manualArticulation_.load(
                                   std::memory_order_relaxed)
                            == static_cast<int>(index);
                    },
                    [this, index]() {
                        module->manualArticulation_.store(static_cast<int>(index),
                            std::memory_order_relaxed);
                    }));
            }
        }
    }

    void onButton(const event::Button& event) override
    {
        if (event.action != GLFW_PRESS || event.button != GLFW_MOUSE_BUTTON_LEFT
            || !module)
            return;
        ui::Menu* menu = createMenu();
        appendSelectionMenu(menu);
        event.consume(this);
    }
};

struct CellaSFZExpressionWidget : ModuleWidget {
    explicit CellaSFZExpressionWidget(CellaSFZExpression* module)
    {
        setModule(module);
        setPanel(createPanel(
            asset::plugin(pluginInstance, "res/CellaSFZExpression.svg"),
            asset::plugin(pluginInstance, "res/CellaSFZExpression-dark.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewGrey>(Vec(165, 0)));
        addChild(createWidget<ScrewGrey>(Vec(165, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        for (int input = 0; input < 3; ++input) {
            addInput(createInputCentered<ThemedPJ301MPort>(
                Vec(35 + 55 * input, 82), module,
                CellaSFZExpression::BEND_INPUT + input));
        }
        constexpr float xs[] { 48.0f, 132.0f, 48.0f, 132.0f };
        constexpr float ys[] { 169.0f, 169.0f, 242.0f, 242.0f };
        for (int slot = 0; slot < 4; ++slot) {
            auto* display = createWidget<CellaExpressionDisplay>(
                Vec(xs[slot] - 35.0f, ys[slot] - 50.0f));
            display->box.size = Vec(70, 18);
            display->module = module;
            display->slot = slot;
            addChild(display);
            addInput(createInputCentered<ThemedPJ301MPort>(Vec(xs[slot], ys[slot]),
                module, CellaSFZExpression::CONTROL_1_INPUT + slot));
        }
        auto* articulation = createWidget<CellaExpressionDisplay>(Vec(25, 292));
        articulation->box.size = Vec(130, 22);
        articulation->module = module;
        articulation->slot = -1;
        addChild(articulation);
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(90, 342), module,
            CellaSFZExpression::ARTICULATION_INPUT));
    }
};

Model* modelCellaSFZ = createModel<CellaSFZ, CellaSFZWidget>("SFZ");
Model* modelCellaSFZExpression =
    createModel<CellaSFZExpression, CellaSFZExpressionWidget>("CellaSFZExpression");
