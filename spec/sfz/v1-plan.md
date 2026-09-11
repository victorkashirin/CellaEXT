# Cella SFZ V1 — four-gate feasibility slice

## Outcome

Deliver a buildable Cella module which loads one `.sfz`, accepts a polyphonic
Rack keyboard signal, and plays all held notes as a stereo mix. V1 exists to
answer four questions with measured evidence:

1. Can a representative large, user-owned SFZ load off the audio thread to a
   playable state, with load time, referenced sample bytes, peak memory, and
   audio-thread stalls measured?
2. Can the chosen engine preserve genuinely different left and right sample
   content through the Rack adapter?
3. Can it play and release a 16-channel Rack chord reliably within an
   acceptable CPU, memory, and latency budget?
4. Can changes to all 16 V/OCT lanes continuously and independently bend their
   sounding notes without retriggering, cross-lane leakage, or an MPE Manager
   consuming one lane?

This is a feasibility release, not yet a general-purpose SFZ player.

The pulled Squinky comparison module uses a different output model: it maps
each input lane to one mono channel on a polyphonic audio output, after folding
stereo samples to mono. V1 intentionally tests 16-note instrument polyphony
mixed to true stereo. It does not yet prove lane-preserving audio parity; see
[the competitive source audit](competitive-analysis.md).

## User-visible contract

### Panel

Use the working name **Cella SFZ** and an approximately 10–12 HP panel.

Controls:

- `LEVEL`, 0–100%, default 80%.
- `OCTAVE`, integer −4…+4, default 0.
- `TUNE`, −100…+100 cents, default 0. Apply it through sfizioso's real-time
  reference-tuning API as `440 * 2^(TUNE / 1200)` Hz so it retunes the whole
  instrument, including held notes. This knob remains global; a separate
  audio-rate bend input is out of scope.
- `LOAD` button. A context-menu **Load SFZ…** action may duplicate it.

Context menu:

- `Render quantum`: 16, 32, or 64 frames; default 32. Smaller values reduce
  input latency and normally cost more CPU. Engines allocate for the 64-frame
  maximum once; changing the selected quantum then happens in place at an
  audio-block boundary under a short fade through silence. It does not reload
  the instrument, reset voices, or retrigger held gates. Musical event timing
  is unchanged, although sfizioso can produce very small sample-level
  differences when its DSP block size changes.
- `Release tails`: `Preserve all` (default), `Keep newest tail`, or
  `Cut on retrigger`. The latter modes apply independently per Rack note lane.
  `Keep newest tail` removes earlier released generations immediately before
  the current note-off, so that note's complete layered/release-trigger
  generation becomes the retained tail. `Cut on retrigger` removes every prior
  generation immediately before note-on. Both use sfizioso's short fast-release
  ramp rather than an abrupt voice reset.

Inputs:

- `V/OCT` — primary polyphonic input, up to 16 channels, 0 V = C4/MIDI 60.
  It chooses the base note on the gate edge and continuously controls pitch
  afterward; it is not merely sampled and quantized at note-on.
- `GATE` — polyphonic; a monophonic cable broadcasts to all pitch channels.
- `VELOCITY` — polyphonic 0–10 V mapped to normalized 0–1. When unpatched,
  use a fixed 0.8.

Outputs:

- `LEFT` — monophonic left side of the mixed stereo signal.
- `RIGHT` — monophonic right side. If only `LEFT` is connected, output a
  defined mono fold-down `(L + R) * 0.5` on `LEFT`; when `RIGHT` is connected,
  preserve the stereo pair.

Display:

- unloaded: `NO INSTRUMENT`;
- loading: filename plus `LOADING`;
- ready: filename, region count, and sample-memory estimate;
- error: a short actionable message while retaining the last valid
  instrument.

### Note behavior

- The number of logical note lanes is `max(1, V/OCT channels)`, capped at 16.
- On each lane's gate rise, calculate the absolute continuous pitch
  `p = 60 + 12 * V/OCT + 12 * OCTAVE`, start
  `baseNote = clamp(round(p), 0, 127)`, and immediately apply the fractional
  remainder `p - baseNote` as that note instance's pitch offset.
- Map TUNE to the engine's A4 reference-tuning frequency. This keeps cents
  continuous without depending on an instrument's `bend_up`/`bend_down`
  opcodes or consuming a per-lane expression channel.
- Retain the exact note number started by each lane and use it for note-off;
  never derive note-off from the lane's current pitch voltage.
- While the gate remains high, track V/OCT continuously and update that lane's
  note-instance pitch offset to `p - baseNote`. Do not retrigger at semitone
  boundaries. V1 must verify at least ±12 semitones of continuous travel; wider
  supported range is recorded rather than assumed.
- Shrinking the V/OCT channel count releases all removed lanes.
- Map Rack lane `n` to sfizioso source MIDI channel `n` and use its
  channel-aware note-on/note-off overloads. This preserves note identity when
  two lanes play the same pitch and permits `lochan`/`hichan` regions to see
  the original source channel.
- Do not enable MIDI MPE for the V1 Rack path. Add a distinct `Rack-16`
  sfizioso expression profile where all source channels 0…15 are independent
  expression owners and there is no Manager channel or MPE message filtering.
- Expose a direct semitone-offset event at the Cella engine boundary. Do not
  emulate it through the instrument's `bend_up`/`bend_down` opcodes: Rack's
  1 V/octave tracking must not vary with the loaded SFZ's pitch-wheel setup.
- Bind expression to a note-instance ID, not only a reusable channel number.
  On note-off, freeze the release tail at its last pitch so a newly triggered
  note on the same Rack lane cannot bend the old tail.
- Preserve all release generations by default. The optional `Keep newest tail`
  policy retains only the newest still-rendering generation on a retriggered
  lane, while `Cut on retrigger` fast-releases every previous generation on
  that lane. Neither policy affects voices owned by other lanes.
- Separate pitch-bend and audio-rate FM inputs are roadmap items; continuous
  per-lane pitch from `V/OCT` is required in V1.
- Retriggering and two lanes holding the same MIDI note must still be tested at
  the public engine boundary. The expected result is independent release by
  source channel; do not infer correctness only from the API signature.

## Scope boundaries

Included:

- stereo WAV playback as supplied by the SFZ engine;
- polyphonic note-on, note-off, and velocity;
- continuous, independent per-lane V/OCT pitch through the `Rack-16`
  expression profile;
- SFZ features already implemented by the pinned engine, without promising a
  specific opcode set beyond the test fixtures;
- one instrument per module;
- patch persistence of the selected absolute path and panel settings;
- asynchronous load and safe engine replacement;
- macOS, Windows, and Linux Rack plugin builds.

Excluded:

- per-note polyphonic audio outputs;
- CC inputs, sustain, aftertouch, keyswitch controls, a separate bend input,
  audio-rate FM, or standards-mode MPE;
- SF2/DecentSampler import UI;
- multiple SFZ `output=` buses or an output expander;
- browser, favorites, library indexing, waveform display, or region editor;
- missing-sample relinking;
- automatic hot reload;
- guaranteed compatibility with arbitrary third-party SFZ libraries;
- copying samples into Rack patch storage.

## Architecture

```text
Rack UI thread                    Loader worker
  choose path ── load request ──> parse SFZ + open/preload samples
                                      │
                                      └── ready EngineBundle
                                                │ lock-free handoff
Rack audio thread                                v at block boundary
  scan 16 lanes -> ordered timed events -> BlockAdapter -> sfizioso Synth
                                                    │
                                             float L/R block
                                                    │
                                     gain + Rack voltage conversion
                                                    │
                                               LEFT / RIGHT
```

### Engine boundary

Define a small Cella-owned interface so the module does not depend directly on
`sfizioso` types:

```cpp
struct SamplerEngine {
    virtual ~SamplerEngine() = default;
    virtual LoadReport load(const std::string& path) = 0; // worker only
    virtual void setSampleRate(float sampleRate) = 0;     // non-RT transition
    virtual void enqueue(const TimedEngineEvent&) = 0;    // note/pitch, bounded RT
    virtual void render(float* left, float* right, int frames) = 0;
    virtual EngineStats stats() const = 0;
};
```

Keep the interface event- and block-oriented. This makes a future sfizioso
update, another engine, or a Cella engine replaceable without rewriting the
Rack module and UI.

### Dependency integration

1. Place the dependency at `deps/sfizioso/`, parallel to the existing vendored
   `deps/ebur128/`. This path is part of the build contract; do not use a system
   installation or require a developer-specific checkout elsewhere.
2. Pin an exact `sfizioso` commit and record the commit, assessment date,
   upstream sfizz baseline, recursive dependency pins, enabled options, and
   local patches in `deps/README.md`. The repository was assessed at
   `a87b25e41868b9fce61199109de6f21e87c837e3` (2026-09-08), but re-check and
   deliberately choose the pin when implementation begins.
3. Vendor a reproducible source snapshot or use a recursive Git submodule at
   that exact path. Whichever mechanism is chosen, a clean offline source tree
   prepared by the documented bootstrap step must contain sfizioso's required
   nested dependencies; release builds may not follow mutable branches.
4. Include `deps/sfizioso/rack.mk` from the root `Makefile` and build its static
   headless target. The inherited build interface is still named `SFIZZ_*`:

   ```make
   SFIZZ_RACK_PLUGIN_DIR := .
   SFIZZ_USE_SNDFILE := 0
   include deps/sfizioso/rack.mk
   LDFLAGS += $(SFIZZ_LINK_FLAGS)
   $(TARGET): $(SFIZZ_TARGET)
   ```

   `rack.mk` must apply its private `SFIZZ_C_FLAGS` and `SFIZZ_CXX_FLAGS` only
   to sfizioso objects. Keep `SFIZZ_USE_SNDFILE=0` so the default `dr_libs`
   decoder path is self-contained. Confirm include order and C++17 flags
   against Rack's toolchain with a clean build rather than assuming the snippet
   is sufficient.
5. Add every required license/notice to the Cella distribution.
6. Verify every official VCV Library toolchain: macOS x64/arm64, Windows x64,
   and Linux x64, before beginning panel polish. Record binary-size and clean
   build-time changes. Linux arm64 is tracked separately but is not a release
   gate until VCV publishes a Linux arm64 Rack SDK and plugin toolchain.
7. Maintain the required `Rack-16` expression work as a small Cella patch
   series and propose the protocol-neutral API upstream. Keep the profile
   separate from MPE policy and avoid changes scattered through vendored code.
8. Retain sfizioso's `LICENSE`, `NOTICE`, and `AUTHORS.md` attribution and audit
   the notices for every enabled bundled dependency.

### Real-time adapter

- Use selectable 16-, 32-, and 64-frame render quanta with preallocated
  event/audio buffers and a 32-frame default. Prepare the sampler once for the
  64-frame maximum, then change the actual render size in place under the
  lifecycle fade without rebuilding it. This lets users trade CPU for latency
  while held voices and release tails continue across the transition. For
  reference, 16/32/64 frames are 0.33/0.67/1.33 ms at 48 kHz, before engine
  latency. Treat the choice as a DSP setting: block-dependent floating-point
  and smoothing results need not be bit-identical.
- Collect events with their sample offset, keep them ordered, submit them, and
  call the engine's stereo block renderer once per quantum.
- The audio callback may inspect ports, update fixed-size lane state, push to a
  bounded queue, render, apply gain, and copy samples. It may not parse, open,
  log, allocate unbounded memory, wait on a lock, or destroy an engine.
- Build a replacement engine completely on the worker. Publish it through a
  bounded handoff and swap only on a block boundary. Return the retired engine
  to the worker for destruction.
- A failed or cancelled load leaves the current engine sounding.
- On Rack sample-rate change, silence safely, rebuild/reconfigure away from
  the audio callback, then crossfade into the ready engine.
- Convert normalized engine audio to Rack audio voltage with a documented
  nominal scale and soft safety limiter. Normal operation must retain at least
  6 dB of headroom before the limiter.

### State persistence

Store only:

- schema version;
- selected absolute SFZ path;
- optional path relative to the patch, when one can be derived safely;
- selected render quantum and release-tail policy;
- last successful load status for display only.

Rack automatically persists knob values. On patch load, request the instrument
asynchronously. A missing file shows `MISSING SFZ` and keeps the module silent;
it must not block patch loading or recursively search the disk.

## Implementation sequence

### Validation cadence

- During normal implementation, building and running the relevant focused
  tests on the native macOS target is sufficient. Developers do not need to
  run the complete cross-platform, large-library, and acceptance suite after
  every code change.
- Run the full supported-target and representative-library suite at milestone
  completion, before a release, and when explicitly validating portability or
  dependency/build-system changes. A change made specifically for another
  toolchain should also be checked on that target.
- Keep V1 feasibility research and its cross-platform probes local for now.
  Do not add SFZ builds or probes to GitHub Actions until the project moves
  from research toward code intended to be pushed and continuously built.

### Milestone 0 — dependency and API spike

- Build the pinned engine as a static dependency on every supported target.
- Compile a tiny command-line probe which loads a generated two-channel SFZ,
  sends a triad, independently bends its notes, renders a WAV, and exits.
- Run a separate load probe against a named, representative large user-owned
  SFZ. Record the SFZ's referenced sample bytes and region count, time to ready,
  peak resident memory, cancellation behavior, and first-note success. Keep the
  library outside the repository.
- Measure 1/16/32/64-frame CPU and event-to-output latency, clean build time,
  plugin binary size, and whether channel-aware same-pitch notes release
  independently.
- Prototype the `Rack-16` engine profile and prove with an audio-frequency or
  zero-crossing test that 16 simultaneous, distinct bend ramps reach only
  their intended note instances. Do this before panel work.
- Stop and revisit the integration if one clean target cannot build, the
  dependency cannot be distributed license-compliantly, or channel-aware
  duplicate notes are not deterministic. Also stop if `Rack-16` cannot isolate
  all 16 bends without invasive, unmaintainable engine changes.

### Milestone 1 — Rack-independent core

- Add `src/sfz/SamplerEngine.*`, `SfiziosoEngine.*`, `BlockAdapter.*`, and
  `NoteLaneTracker.*`.
- Generate tiny mono and stereo WAV/SFZ fixtures during tests; do not commit a
  third-party instrument.
- Unit-test voltage-to-note/velocity conversion, continuous pitch decomposition,
  event ordering, channel-count shrink, held-note bend without retrigger,
  release-tail snapshot, retrigger, duplicate-note cases, and source-scoped tail
  policies with delayed layers and release-trigger regions.
- Unit-test stereo routing with deliberately different left/right impulses.

### Milestone 2 — minimal module and panel

- Add `src/SFZ.cpp`, append `modelCellaSFZ` in `src/plugin.hpp` and
  `src/plugin.cpp`, append the module to `plugin.json`, and add light/dark panel
  SVGs.
- Preserve enum IDs from the first public build onward; append future IDs.
- Implement inputs, controls, stereo outputs, status display, load action, and
  JSON path persistence.
- Test live render-quantum changes with held gates: the engine and its voices
  remain active, the gate is not retriggered, and the setting persists.
- Keep all UI-to-engine communication bounded and one-way; the display reads a
  small atomic status snapshot rather than engine internals.

### Milestone 3 — asynchronous lifecycle

- Implement one loader worker per module or a documented shared loader service.
- Coalesce rapid load requests and make module destruction cancel-safe.
- Perform pop-free engine replacement with a short output fade.
- Ensure patch load, autosave, deletion during load, and audio-device
  sample-rate changes are safe.

### Milestone 4 — feasibility validation

- Build and run focused automated tests at 44.1, 48, and 96 kHz.
- In Rack, play 1, 4, 8, and 16-note chords and exercise changing polyphony,
  repeated notes, same-note duplicates, fast gates, long releases, and module
  deletion while sound is active.
- Drive 16 deliberately different continuous V/OCT ramps simultaneously. Test
  held notes, duplicate base notes, semitone crossings, release tails, and lane
  reuse while checking for pitch leakage or unintended retriggers.
- Test a true stereo fixture, a mono fixture, one looped fixture, malformed
  SFZ, missing sample, and a library large enough to expose loading stalls.
- Compare the output of the command-line engine probe and the Rack adapter for
  the same event list. Apart from the documented gain and adapter latency,
  samples should match within floating-point tolerance.
- Profile an optimized build; debug-build CPU numbers are not acceptance data.
- Install the audited Squinky build beside Cella and run the same generated
  fixtures and Rack patch through both. Record mixed-versus-lane output,
  stereo separation, latency, CPU, RAM, load time, and failed-reload behavior.

## Acceptance gates

V1 passes only when all are true:

- The recorded large SFZ reaches the ready state and plays its first note;
  loading, cancellation, or replacement causes no audio-thread stall over 2 ms.
- A left-only test impulse is absent from `RIGHT`, a right-only impulse is
  absent from `LEFT`, and stereo phase/polarity is preserved.
- C-major notes arriving on separate Rack channels sound simultaneously and
  release without stuck or prematurely killed notes, including two lanes on
  the same note.
- All 16 held lanes follow independent continuous V/OCT ramps over at least
  ±12 semitones without retriggering or affecting another lane. Reusing one
  lane does not retune the previous note's release tail.
- Gate-to-audio latency is fixed and measured for the selected quantum. The V1
  result must report its difference from Squinky's per-sample path with its
  optional five-sample gate delay; any worse latency is an explicit tradeoff,
  not hidden in a generic pass.
- Loading or replacing the large instrument produces no audible click in the
  Rack smoke test.
- A failed load leaves the previous instrument playable.
- No file I/O, parser work, engine destruction, blocking lock, or unbounded
  allocation is observed on the audio thread.
- The plugin builds from a clean checkout for every supported VCV target and
  the distribution contains all dependency notices.
- At 48 kHz, an agreed reference machine sustains 16 held input notes below a
  recorded CPU ceiling. Set that numeric ceiling from Milestone 0 rather than
  inventing it in advance.

## V1 exit record

When validation finishes, add `spec/sfz/v1-results.md` containing:

- pinned engine commit and any Cella patches;
- test machine and Rack/SDK version;
- large-SFZ identity, referenced sample bytes, region count, first-note result,
  load/cancel time, and peak RAM;
- measured latency, 1/4/8/16-note CPU, and plugin size;
- opcode/sample formats exercised;
- duplicate-note result;
- 16-lane continuous-bend range, accuracy, leakage, and release-tail results;
- stereo fixture result;
- known limitations and the decision: proceed, change engine, or stop.
- side-by-side Squinky numbers from the exact audited commit and a statement
  that mixed stereo is not yet lane-preserving output parity.
