#include "components.hpp"
#include "plugin.hpp"
#include "sfz/BlockAdapter.hpp"
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

std::string memoryLabel(size_t bytes)
{
    if (bytes >= 1024u * 1024u)
        return rack::string::f("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024u)
        return rack::string::f("%.1f KB", static_cast<double>(bytes) / 1024.0);
    return rack::string::f("%zu B", bytes);
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
            && PARAMS_LEN == 6,
        "Cella SFZ parameter IDs are part of the patch format");
    static_assert(VOCT_INPUT == 0 && GATE_INPUT == 1 && VELOCITY_INPUT == 2
            && INPUTS_LEN == 3,
        "Cella SFZ input IDs are part of the patch format");
    static_assert(LEFT_OUTPUT == 0 && RIGHT_OUTPUT == 1 && OUTPUTS_LEN == 2,
        "Cella SFZ output IDs are part of the patch format");

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

    struct StatusSnapshot {
        StatusState state { StatusState::Unloaded };
        std::string filename;
        std::string message;
        int regionCount { 0 };
        size_t estimatedSampleBytes { 0 };
    };

    struct EngineBundle {
        std::unique_ptr<cella::sfz::SamplerEngine> engine;
        cella::sfz::BlockAdapter adapter;
        float appliedTuningHz { 440.0f };
        float sampleRate { 48000.0f };
        uint64_t generation { 0 };
        std::string sourcePath;

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
    };

    struct LoadRequest {
        uint64_t generation { 0 };
        std::string path;
        std::string fallbackPath;
        float sampleRate { 48000.0f };
        float tuningHz { 440.0f };
        LoadKind kind { LoadKind::Instrument };
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
        std::atomic<bool> stopRequested { false };
        bool workerBusy { false }; // guarded by mutex
    };

    std::shared_ptr<LoaderState> loader_;
    std::atomic<float> observedSampleRate_ { 48000.0f };
    std::atomic<int> renderQuantumSetting_ { kDefaultRenderQuantum };
    std::atomic<TailBehavior> tailBehaviorSetting_ { TailBehavior::PreserveAll };
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
    int captureFrame_ { 0 };
    int activeRenderQuantum_ { kDefaultRenderQuantum };
    int playbackFrame_ { kDefaultRenderQuantum };
    std::array<float, kMaximumRenderQuantum> playbackLeft_ {};
    std::array<float, kMaximumRenderQuantum> playbackRight_ {};
    std::array<float, cella::sfz::MaxNoteLanes> pitchVoltages_ {};
    std::array<float, cella::sfz::MaxNoteLanes> gateVoltages_ {};
    std::array<float, cella::sfz::MaxNoteLanes> velocityVoltages_ {};

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

        configInput(VOCT_INPUT, "1 V/octave pitch");
        configInput(GATE_INPUT, "Gate");
        configInput(VELOCITY_INPUT, "Velocity");
        configOutput(LEFT_OUTPUT, "Left / mono");
        configOutput(RIGHT_OUTPUT, "Right");

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

    int renderQuantum() const noexcept
    {
        const int frames = renderQuantumSetting_.load(std::memory_order_acquire);
        return isRenderQuantum(frames) ? frames : kDefaultRenderQuantum;
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
                publishStatus(loader,
                    { state, system::getFilename(request.path), message, 0, 0 });
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

        std::lock_guard<std::mutex> lock(loader->mutex);
        if (!loadGenerationIsCurrentLocked(loader, request.generation))
            return;
        loader->requestedPath = resolvedPath;
        StatusSnapshot ready { StatusState::Ready, filename, {},
            report.regionCount, report.estimatedPreloadedSampleBytes };
        std::shared_ptr<const StatusSnapshot> readyValue =
            std::make_shared<const StatusSnapshot>(ready);
        std::shared_ptr<const std::string> selected =
            std::make_shared<const std::string>(resolvedPath);
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
        const std::string& fallbackPath = {})
    {
        if (path.empty()
            || loader_->stopRequested.load(std::memory_order_acquire))
            return;

        LoadRequest request;
        request.path = path;
        request.fallbackPath = fallbackPath;
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
            loader_->loadRequest = std::move(request);
            loader_->failedInstrumentRecoveryRequested.store(
                false, std::memory_order_release);
            publishStatus(loader_, { StatusState::Loading, system::getFilename(path),
                "LOADING", 0, 0 });
        }
        loader_->cv.notify_one();
    }

    void loadInstrumentFromUi(const std::string& path)
    {
        loadInstrument(path);
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

            std::vector<std::string> instruments;
            for (const std::string& entry : system::getEntries(directory)) {
                if (!system::isFile(entry))
                    continue;
                std::string extension = system::getExtension(entry);
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                if (extension == ".sfz")
                    instruments.push_back(system::getAbsolute(entry));
            }
            std::sort(instruments.begin(), instruments.end());
            const auto current = std::find(
                instruments.begin(), instruments.end(), currentAbsolute);
            if (current == instruments.end())
                return false;

            const size_t count = instruments.size();
            const size_t index = static_cast<size_t>(
                std::distance(instruments.begin(), current));
            const size_t adjacent = direction < 0
                ? (index + count - 1) % count
                : (index + 1) % count;
            loadInstrumentFromUi(instruments[adjacent]);
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
        loader_->loadRequest = std::move(request);
        sampleRateTransitionRequested_.store(true, std::memory_order_release);
        loader_->failedInstrumentRecoveryRequested.store(
            false, std::memory_order_release);
        publishStatus(loader_, { StatusState::Loading, system::getFilename(reloadPath),
            "LOADING", 0, 0 });
        loader_->cv.notify_one();
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
        loader_->activeGeneration.store(next->generation, std::memory_order_release);
        lanes_.reset();
        captureEventCount_ = 0;
        playbackLeft_.fill(0.0f);
        playbackRight_.fill(0.0f);
        activeRenderQuantum_ = renderQuantum();
        playbackFrame_ = activeRenderQuantum_;
        if (previous)
            loader_->retiredEngine.store(previous, std::memory_order_release);
        sampleRateTransitionActive_ = false;
        quantumTransitionActive_ = false;
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
            && !pendingReady) {
            fadeInAfterRender_ = false;
            lifecycleFade_ = LifecycleFade::FadingIn;
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
            playbackLeft_.fill(0.0f);
            playbackRight_.fill(0.0f);
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
    }

    void captureInputFrame() noexcept
    {
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
    }

    void finishCaptureBlock() noexcept
    {
        playbackLeft_.fill(0.0f);
        playbackRight_.fill(0.0f);
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
            activeEngine_->adapter.render(playbackLeft_.data(), playbackRight_.data(),
                activeRenderQuantum_);
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

        float left = 0.0f;
        float right = 0.0f;
        if (playbackFrame_ < activeRenderQuantum_) {
            left = playbackLeft_[playbackFrame_];
            right = playbackRight_[playbackFrame_];
            ++playbackFrame_;
        }

        if (captureFrame_ == 0)
            beginCaptureBlock();
        captureInputFrame();
        if (++captureFrame_ == activeRenderQuantum_) {
            finishCaptureBlock();
            captureFrame_ = 0;
        }

        const float gain = params[LEVEL_PARAM].getValue() * kNominalOutputVolts;
        const float lifecycleGain = advanceLifecycleFade(args.sampleRate);
        left = softLimit(left * gain * lifecycleGain);
        right = softLimit(right * gain * lifecycleGain);
        outputs[LEFT_OUTPUT].setChannels(1);
        outputs[RIGHT_OUTPUT].setChannels(1);
        if (!outputs[RIGHT_OUTPUT].isConnected())
            outputs[LEFT_OUTPUT].setVoltage(0.5f * (left + right));
        else
            outputs[LEFT_OUTPUT].setVoltage(left);
        outputs[RIGHT_OUTPUT].setVoltage(right);
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();
        json_object_set_new(root, "schemaVersion", json_integer(kSchemaVersion));
        json_object_set_new(root, "renderQuantum", json_integer(renderQuantum()));
        json_object_set_new(root, "tailBehavior",
            json_string(tailBehaviorKey(tailBehavior())));
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
            json_object_set_new(root, "lastSuccessfulLoad", status);
        }
        return root;
    }

    void dataFromJson(json_t* root) override
    {
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
        if (json_t* status = json_object_get(root, "lastSuccessfulLoad")) {
            if (json_t* regionsValue = json_object_get(status, "regionCount");
                json_is_integer(regionsValue))
                regions = std::max<json_int_t>(0, json_integer_value(regionsValue));
            if (json_t* bytesValue = json_object_get(status, "estimatedSampleBytes");
                json_is_integer(bytesValue))
                bytes = static_cast<size_t>(std::max<json_int_t>(
                    0, json_integer_value(bytesValue)));
        }
        const StatusSnapshot rememberedStatus { StatusState::Ready,
            system::getFilename(remembered), {}, regions, bytes };
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

struct CellaSFZStatusDisplay : TransparentWidget {
    CellaSFZ* module { nullptr };

    void draw(const DrawArgs& args) override
    {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 4.0f);
        nvgFillColor(args.vg, nvgRGB(0x12, 0x16, 0x1b));
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.0f);
        nvgStrokeColor(args.vg, nvgRGB(0x58, 0x68, 0x73));
        nvgStroke(args.vg);

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

        std::array<std::string, 3> lines;
        NVGcolor color = nvgRGB(0x9c, 0xe5, 0xd8);
        switch (status->state) {
        case CellaSFZ::StatusState::Unloaded:
            lines[1] = "NO INSTRUMENT";
            break;
        case CellaSFZ::StatusState::Loading:
            lines[0] = shorten(status->filename, 22);
            lines[1] = "LOADING";
            color = nvgRGB(0xf5, 0xc5, 0x72);
            break;
        case CellaSFZ::StatusState::Ready:
            lines[0] = shorten(status->filename, 22);
            lines[1] = rack::string::f("%d REGION%s", status->regionCount,
                status->regionCount == 1 ? "" : "S");
            lines[2] = memoryLabel(status->estimatedSampleBytes);
            break;
        case CellaSFZ::StatusState::Error:
            lines[0] = shorten(status->filename, 22);
            lines[1] = status->message;
            color = nvgRGB(0xff, 0x7d, 0x78);
            break;
        case CellaSFZ::StatusState::Missing:
            lines[0] = shorten(status->filename, 22);
            lines[1] = "MISSING SFZ";
            color = nvgRGB(0xff, 0x7d, 0x78);
            break;
        }

        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 10.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, color);
        const float centerX = box.size.x * 0.5f;
        const int first = lines[0].empty() ? 1 : 0;
        const int last = lines[2].empty() ? 1 : 2;
        const float lineHeight = 14.0f;
        const float startY = box.size.y * 0.5f
            - (last - first) * lineHeight * 0.5f;
        for (int index = first; index <= last; ++index) {
            if (!lines[index].empty())
                nvgText(args.vg, centerX,
                    startY + (index - first) * lineHeight,
                    lines[index].c_str(), nullptr);
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

        auto* display = createWidget<CellaSFZStatusDisplay>(Vec(10, 35));
        display->box.size = Vec(160, 54);
        display->module = module;
        addChild(display);

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

Model* modelCellaSFZ = createModel<CellaSFZ, CellaSFZWidget>("CellaSFZ");
