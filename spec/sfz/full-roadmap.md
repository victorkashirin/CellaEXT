# Cella SFZ — full product roadmap

## Product goal

Build a dependable, expressive, Rack-native sample instrument. The target is
not merely “more opcodes” than another module. Cella SFZ should eventually win
on the combination of:

- preservation of stereo source material;
- broad and visible SFZ compatibility;
- non-blocking loading and scalable sample memory;
- deep CV control designed for Rack rather than adapted from a MIDI plugin;
- actionable diagnostics when an instrument is incomplete or unsupported;
- multiple audio buses and performance controls for large libraries;
- reproducible behavior in saved patches.

The current comparison baseline is the locally audited Squinky Labs SFZ Player
at commit `3701a8c8931e5e8073e00cd5632db3966befe54f`: polyphonic
V/OCT/GATE/velocity inputs, per-lane exponential and sample-position modulation,
one mono stream per lane on a polyphonic output, background loading, keyswitch
selection, full-RAM loading, and a deliberately limited SFZ subset. It folds
stereo WAVs to mono. Treat it as a feature and performance baseline, not as a
code base to copy. See [the competitive source audit](competitive-analysis.md).

## Strategic architecture

### Playback engine

Use the pinned
[`sfizioso`](https://github.com/rullopat/sfizioso) engine behind the
Cella-owned `SamplerEngine` interface introduced in V1. Sfizioso is a current,
independent C++17 fork of sfizz which adds per-channel note, pitch, CC, and
aftertouch routing, MPE behavior, independent same-pitch note ownership, and
fixed-channel SFZ routing. Those additions align unusually well with Rack's
polyphonic cables. Therefore:

1. keep the pinned source at `deps/sfizioso/`, alongside `deps/ebur128/`, with
   provenance and recursive pins recorded in `deps/README.md`;
2. keep a reproducible source snapshot and all dependency notices;
3. pin a reviewed commit rather than following `main` implicitly;
4. prefer upstream contributions and keep unavoidable local changes as a
   reviewable patch series;
5. add Cella conformance tests before updating or changing engine code;
6. never expose sfizioso-specific types in saved patches or the module/UI layer;
7. review maintenance health and alternatives at every major Rack/toolchain
   change;
8. budget ownership for security, compiler, filesystem, and audio-decoder
   maintenance if upstream stops.

Do not write a new SFZ parser and sampler merely to avoid this dependency. That
is a separate multi-release project and only becomes justified if the V1 exit
record shows an unfixable architectural or distribution problem.

### Three-thread model

```text
UI/main thread            loader/index worker             audio thread
file choice/settings ---> parse, validate, preload -----> block-boundary swap
browser queries --------> metadata/cache results -------> compact status only
                         retire old engines <------------ retired-engine token
CV/gate ports ------------------------------------------------> timed events
LEFT/RIGHT and buses <---------------------------------------- rendered blocks
```

All file discovery, decoding, parsing, sample preloading, cache mutation, and
large-object destruction stay off the audio thread. Queues are bounded, state
transitions are explicit, and the currently sounding engine remains immutable
except through its real-time event/render API.

### Polyphony meanings

Keep these concepts distinct in code, UI, tests, and documentation:

- **Rack note-lane polyphony:** up to 16 V/OCT/GATE/velocity channels feeding
  one instrument.
- **Sampler voice polyphony:** region voices created by layers, release
  samples, round robins, pedal behavior, and overlapping notes; user-settable
  with deterministic stealing.
- **Audio-bus count:** one stereo main bus initially, then SFZ `output=` buses.
- **Polyphonic audio output:** one stereo signal per Rack note lane. Squinky
  already preserves lanes, although only as mono. Cella's lane-isolation design
  must therefore be prototyped before the first release whose compatibility or
  routing is presented as superior.

## Competitive definition of done

Cella SFZ can credibly claim to surpass the comparison player only after it:

- preserves stereo samples and offers a correct mono fold-down;
- passes the published Cella compatibility corpus and reports unsupported
  opcodes instead of failing mysteriously;
- supplies at least the comparison player's V/OCT, gate, velocity, exponential
  pitch modulation, and expressive modulation capability;
- offers lane-preserving audio routing as well as mixed stereo, unless a
  deliberate incompatibility decision is documented after user testing;
- loads and swaps instruments without blocking audio;
- handles instruments larger than the practical full-RAM limit through a
  measured streaming/preload strategy;
- provides useful keyswitch/CC access from CV;
- has comparable or better CPU at like-for-like quality, or clearly exposes the
  quality/CPU tradeoff;
- ships on all VCV-supported Cella targets with patch-compatible state.

Do not use “full SFZ support” as a marketing phrase. Publish a generated opcode
support report tied to the exact shipped engine commit.

## Delivery phases

### Phase 0 — engine and distribution decision

Deliver the command-line spike from the V1 plan before module UI work.

- Pin and build the engine on all target toolchains.
- Audit licenses and notices for the engine and enabled decoders.
- Measure binary size, build time, 1/4/8/16-note CPU, preload RAM, and load
  behavior.
- Verify duplicate-note semantics, long releases, loops, stereo, and clean
  teardown.
- Implement the minimum `Rack-16` expression spike and verify 16 independent
  continuous pitch ramps without using a Manager channel.
- Decide whether to use upstream-as-is, a Cella patch set, a fork, or another
  engine. Record the decision and upgrade policy.

Exit: one reproducible engine version and a green cross-platform probe.

### Phase 1 — V1 four-gate vertical slice

Implement [the V1 plan](v1-plan.md) exactly. Resist adding browser or modulation
features until the four prototype gates pass.

Exit: stereo is preserved, 16 Rack note lanes work, all 16 bend independently
and continuously through V/OCT, loading is off-thread, and `v1-results.md`
records the measurements and go/no-go decision.

### Phase 2 — release-grade core

- Harden cancellation, deletion during load, sample-rate changes, engine swap,
  corrupt files, missing samples, include cycles, path encodings, and long
  filenames.
- Add tail policy on instrument replacement: hard stop, short crossfade, or
  allow old release tails within a bounded resource budget.
- Add voice limit, deterministic stealing mode, panic/all-notes-off, clipping
  indicator, and output headroom calibration.
- Add a user-selectable 1/16/32/64/128-frame render quantum with displayed
  latency; default from benchmarks.
- Persist path, engine settings, and compatibility schema. Add **Locate…** for
  a missing root SFZ without guessing across the user's disk.
- Establish crash-safe error reporting and a compact diagnostic log retrievable
  from the context menu.
- Add focused sanitizers/fuzzing for Cella's path/event boundary and malformed
  SFZ smoke corpora outside the release build.

Exit: suitable for an experimental public release without known real-time or
patch-lifecycle hazards.

### Phase 3 — comparison parity and output architecture

- Prototype stereo lane preservation before ports and engine ownership are
  frozen. Compare source-channel render taps, per-lane engines with shared
  immutable samples, and a dedicated variant/expander.
- Test instrument-wide state explicitly: sustain/sostenuto, choke/off groups,
  keyswitches, round robins, random selection, voice limits, and release tails
  must not become 16 unrelated instruments by accident.
- If viable, add `POLY LEFT` and `POLY RIGHT`, where output channel `n` contains
  only Rack input lane `n`, while retaining the mixed `LEFT`/`RIGHT` contract.
- Add polyphonic `BEND`/exponential FM, gate/pitch trigger-delay selection, and
  a keyswitch selector with engine labels so the common Squinky performance
  patch can be reproduced.
- Benchmark Cella and Squinky from the same optimized Rack build with identical
  generated fixtures at 1/4/8/16 lanes. Publish output routing, event latency,
  CPU, RAM, and load-time differences.
- Treat audio-rate sample-position/phase modulation as a product choice. If
  implemented, label it as a Cella extension and define loop/boundary safety;
  if omitted, document the competitor-only workflow rather than calling it
  ordinary SFZ compatibility.

Exit: Cella matches the comparison player's core Rack patch workflow while
preserving stereo, and the lane-output architecture is either shipped or has a
recorded, evidence-backed decision.

### Phase 4 — expressive Rack performance

- Extend polyphonic `BEND`/exponential FM with per-note pressure, timbre, and a
  configurable slew using sfizioso's channel-aware APIs and MPE expression
  contexts.
- Productize three explicit expression profiles. `Global` retains sfizioso's
  non-MPE compatibility behavior: 16 source-owned notes but one shared
  expression channel. `MPE-15` uses a Manager plus 15 Member channels and
  follows MPE semantics. `Rack-16`, proven during V1, is a Cella/sfizioso
  engine extension: all 16 source channels receive independent
  pitch/pressure/timbre expression, with no Manager channel and no MPE-only
  message filtering. Do not pretend the Manager channel is a sixteenth
  independent MPE Member.
- Design `Rack-16` around sfizioso's existing source address, note-instance,
  per-channel expression-context, and release-snapshot machinery rather than
  sixteen unrelated synths. When a Rack lane is reused, an old release tail
  retains its note-off expression snapshot and cannot be bent by the new note.
- Add conformance tests where all 16 held lanes receive distinct continuous
  bend ramps, including duplicate base notes, simultaneous layers, lane reuse,
  and release tails. No bend may leak between lanes.
- Add sustain/sostenuto gates and polyphonic release velocity.
- Add eight assignable CV inputs. Each slot selects MIDI CC or a named SFZ
  control, polarity, 0–10/±5 V range, scale, offset, curve, and smoothing.
- Use engine-reported labels and defaults where available.
- Add trigger/gate/retrigger/legato modes and a retrigger-delay option for
  simultaneously changing Rack pitch and gate signals.
- Add keyswitch support: current switch display, direct panel selection, CV
  selection, latch/momentary modes, and optional suppression of keyswitch audio.
- Add deterministic random/sequence reset for round robins where the engine
  permits it, so patches can be reproducible.

Exit: all baseline performance controls are matched and Cella adds practical
Rack-native CC/keyswitch control beyond Squinky's panel keyswitch popup.

### Phase 5 — compatibility laboratory

- Generate an opcode manifest from the pinned engine and expose it in the
  manual and build artifacts.
- Build small, redistributable fixtures by behavior family: hierarchy and
  inheritance, key/velocity ranges, sequence/random, loop and offset, envelopes,
  filters/EQ, crossfades, trigger modes, choke/off groups, release samples,
  CC conditions/modulation, tempo conditions, tuning, and sample formats.
- For every loaded instrument, report parsed region/group count, missing
  samples, unknown opcodes, unsupported opcodes, substituted behavior, and
  source locations where possible.
- Support includes, variables/macros, `default_path`, quoted/Unicode paths, and
  case-sensitivity diagnostics consistently across platforms.
- Add optional reload-on-change for the root SFZ, includes, and samples, using
  a debounced worker and pop-free engine swap.
- Define a compatibility tier: `Verified`, `Playable with warnings`, or
  `Unsupported`; never infer success merely because parsing completed.

Exit: a generated compatibility report and a representative corpus prevent
silent regressions.

### Phase 6 — large-library performance

- Expose and tune preload size/disk streaming with presets such as `RAM`,
  `Balanced`, and `Low memory`; show actual preloaded and streamed memory.
- Make file handles, decoder buffers, and read-ahead bounded. Benchmark cold
  cache, warm cache, slow disk, and simultaneous module instances.
- Investigate a read-only decoded-sample cache shared across engines and module
  instances. Key it by canonical path plus file identity; never share mutable
  voice state.
- Add background library warm-up, cancellation, and priority so a new load
  cannot starve active streaming.
- Add configurable resampling quality and publish CPU/aliasing measurements.
- Profile dense layered instruments, pedals with release samples, and 16-note
  glissandi; improve hot paths only from profiles.
- Detect overload and degrade predictably: bounded voice stealing and a visible
  warning, never an audio-thread disk wait.

Exit: at least one instrument substantially larger than available preload RAM
plays reliably from disk under the agreed stress patch.

### Phase 7 — multi-output and mixing

- Verify and, if necessary, extend the engine API for SFZ `output=` routing.
- Keep the base module's main stereo `LEFT/RIGHT` ports patch-compatible.
- Add a Cella SFZ output expander exposing additional stereo buses in groups
  that fit Rack ergonomics, up to the engine's verified limit.
- Provide bus name/number, per-bus meter, mute/solo, and defined downmix behavior
  when no expander is attached.
- Keep expander communication double-buffered using Rack's message mechanism;
  no unsynchronized cross-module pointers.

Exit: drum close/room channels or orchestral sections using `output=` can be
routed separately and sum identically to the main mix.

### Phase 8 — instrument browser and recovery

- Add recent files, favorites, user-defined roots, filename/metadata search,
  and audition-safe selection. Index only opted-in roots on a worker.
- Show instrument name, author, key range, keyswitches, CC labels, region count,
  sample formats, memory/preload estimate, and compatibility warnings.
- Add missing-sample recovery with a user-chosen replacement root and a preview
  of all remappings before applying them.
- Support portable patch references: retain absolute path, a safe patch-relative
  candidate, and an optional content fingerprint. Never embed a commercial
  library into a patch automatically.
- Add file-history privacy controls and a clear-cache action.

Exit: a moved library can be deliberately relinked, and a large opted-in
collection can be searched without interrupting audio.

### Phase 9 — authoring and debugging tools

- Add region activity, voice count, voice-steal, keyswitch, and CC monitors
  with throttled UI snapshots.
- Add **Open SFZ in external editor**, **Reload**, **Copy diagnostics**, and
  direct file/line hints for parse warnings.
- Add a region inspector showing effective inherited values after
  `<global>`, `<master>`, `<group>`, and `<region>` merging.
- Add an offline validator CLI built on the same parser/compatibility database.
- Integrate the earlier SF2→SFZ interest as a separate offline import tool:
  extract legal user-owned samples with correct WAV headers, combine preset and
  instrument zones/bags, preserve generator precedence, stereo links, loops,
  tuning, envelopes, velocity/key ranges, and emit a conversion report. Do not
  put SF2 conversion work on the audio thread.
- Optionally import DecentSampler only after its license, path, and behavioral
  compatibility tests are explicit.

Exit: instrument authors can locate unsupported or incorrectly inherited
behavior without reverse-engineering the player.

### Phase 10 — advanced musical features

- Scala/MTS tuning and per-note microtonal pitch with polyphonic Rack CV.
- Tempo/time-signature/transport inputs for tempo-aware opcodes.
- User curves, modulation depth CV, velocity transforms, and note-condition
  utilities.
- Reverse, sample start/loop modulation, granular, or further phase-modulation
  features only when they follow SFZ behavior or are clearly labeled Cella
  extensions.

Exit: advanced features remain deterministic, documented, and compatible with
ordinary SFZ playback when disabled.

### Phase 11 — stable release and maintenance

- Freeze all public parameter/input/output IDs and model slugs; add migrations
  for every older schema fixture.
- Complete manual, factory presets using redistributable/generated samples,
  tooltips, screenshots, and light/dark panels.
- Publish supported formats/opcodes, measured latency, CPU/quality profiles,
  memory model, known incompatibilities, and third-party notices.
- Test clean builds and packages on every target; validate patches saved on one
  OS with libraries relinked on another.
- Establish an engine/toolchain update cadence, CVE review, regression corpus,
  and a documented rollback procedure for the pinned dependency.

Exit: normal Cella release quality, VCV Library submission readiness, and a
maintainable ownership model.

## Test strategy

### Layers

1. **Pure unit tests:** voltage conversion, event identity/order, lane state,
   path rules, queue bounds, fold-down, gain, and persistence migration.
2. **Engine contract tests:** generated SFZ/WAV fixtures rendered to buffers;
   stereo, loops, groups, envelopes, release samples, CCs, voice stealing, and
   reload.
3. **Golden audio tests:** short deterministic impulses/envelopes and spectral
   metrics. Use tolerances appropriate to resampler or engine upgrades; avoid
   fragile whole-file bit equality where behavior is still correct.
4. **Real-time tests:** audio-thread allocation/lock/I/O probes, queue overflow,
   load cancellation, sample-rate change, delete-during-load, and stress tails.
5. **Cross-platform path tests:** spaces, Unicode, relative paths, case
   mismatch, missing includes/samples, long paths, and moved libraries.
6. **Rack smoke tests:** user-owned listening/UI checks after automated tests;
   never report a successful compile as a Rack pass.

### Reference workload

Maintain small generated/redistributable instruments for automated tests and a
separate local list of well-known large libraries for manual compatibility and
performance checks. Do not commit commercial or ambiguously licensed samples.

For every release candidate record at minimum:

- engine commit and Cella patch-set revision;
- Rack SDK and compiler versions;
- 44.1/48/96 kHz results;
- adapter quantum and measured event-to-output latency;
- load time, preload RAM, streaming RAM, and 1/4/8/16-note CPU;
- voice-steal and dropout count under the stress patch;
- compatibility-corpus pass/warn/fail counts.

## Priorities if time is limited

Build in this order:

1. V1 correctness and real-time safety.
2. Release-grade lifecycle and diagnostics.
3. Comparison parity: stereo lane output, pitch modulation, keyswitch workflow.
4. Expressive CV/CC/keyswitch support.
5. Compatibility corpus and transparent warnings.
6. Streaming and large-library performance.
7. Browser/relinking.
8. Multi-output expander.
9. Authoring/import and advanced extensions.

Stereo correctness, stuck-note safety, and non-blocking audio are release
requirements. Browser polish, SF2 import, visualization, and exotic extensions
are not allowed to delay those foundations.

## Open decisions to resolve with prototypes

- Exact pinned engine commit and whether Cella uses a patch set or permanent
  fork.
- Render quantum default after latency/CPU measurements.
- How to render isolated stereo lanes without duplicating sample RAM or
  breaking instrument-wide state; resolve in Phase 3, not Phase 10.
- Verify that sfizioso's channel-aware API independently releases duplicate
  same-note Rack lanes under layers, sustain, and release samples.
- The exact public API and upstreaming strategy for the `Rack-16` sfizioso
  expression profile. The required behavior is decided: 16 independent source
  channels without an MPE Manager; the remaining question is the cleanest
  reviewable engine surface.
- Whether sample data can be shared safely across module instances.
- Multi-output API shape and practical maximum bus count.
- Patch-portability policy for external libraries.
- Numeric CPU, RAM, load-time, and dropout budgets on the reference machines.

These are experiment outputs, not choices to guess in the specification.
