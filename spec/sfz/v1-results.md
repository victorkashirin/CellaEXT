# Cella SFZ V1 — implementation results

## Decision

**V1 passes. Proceed with the pinned sfizioso engine and the focused
expression prototype.** The implementation establishes the intended
asynchronous engine boundary, true-stereo render path, 16-lane note and pitch
behavior, source-aware duplicate-note ownership, and continuous per-note
pitch behavior. The automated suite passes, the project owner reports a
successful complete Rack acceptance run including the large-instrument and
smoke-test matrix, and the plugin builds on all four supported VCV targets.

The operator reported the acceptance outcome but did not provide the raw
instrument identity or numerical telemetry for inclusion in this document.
Those fields are marked as not retained; this is a record-detail limitation,
not an unperformed-test or failed-gate claim.

## Revisions and environment

Validation was recorded on 2026-09-11.

- Cella-EXT: `8d435fe7190b2628c0e0a47f9cdf09b4375985d4`
- Plugin version: `2.0.0`
- sfizioso base: `a87b25e41868b9fce61199109de6f21e87c837e3`
  (`v1.2.2`)
- sfizioso pin: `3fbba4cc3218efa99dc265945244d6b16b0e1df2`
- Rack SDK and installed Rack Free: `2.6.6`
- Native host: Apple M3, 8 cores, 24 GiB RAM; macOS 14.6.1 arm64
- Native toolchain: Apple clang 16.0.0; Xcode 16.2

The dependency bootstrap re-verified the pinned sfizioso commit and all
recursive submodule pins before the native test run.

## Engine patches

The Cella `cella-rack` branch contains four commits above the assessed
sfizioso base:

| Commit | Purpose |
|---|---|
| `e8a5727a` | Add the non-MPE Rack-16 expression profile and direct per-note semitone pitch API. |
| `d0f101ca` | Avoid inactive-voice render work and unnecessary expression timeline flushing. |
| `0c8dec55` | Add source-channel-scoped fast release for the host tail policies. |
| `3fbba4cc` | Make the bundled `atomic_queue` compile with the newer Apple toolchain. |

The build uses the headless static target with libsndfile and OpenMP disabled.
The distributable contains sfizioso's `LICENSE`, `NOTICE`, and `AUTHORS.md`
plus the notices for all enabled bundled dependencies.

## Verification summary

| V1 gate | Recorded result | Status |
|---|---|---|
| Asynchronous large-instrument loading | Worker loading, coalescing, atomic handoff, failed-replacement recovery, non-blocking module destruction, and worker-side retirement pass with generated fixtures. The operator's representative large instrument reached ready and played its first note; cancellation/replacement remained responsive, produced no audible click, and caused no unacceptable audio-thread stall. | Pass |
| True stereo | Deliberately separated positive-left and negative-right PCM impulses remain on their respective outputs; opposite-channel leakage is below the test tolerance of `1e-5`; polarity is preserved. Mono input reaches both engine outputs equally. | Pass |
| Sixteen Rack note lanes | The adapter has a fixed 16-lane bound and preserves lane/source-channel identity. Automated duplicate-note ownership passes, and the operator confirmed the 1/4/8/16-note Rack chord, release, changing-polyphony, repeated-note, fast-gate, and long-release matrix with acceptable CPU and memory. | Pass |
| Independent continuous V/OCT pitch | Held-note tracking crosses semitone boundaries without a new note-on; the automated tracker travel is `+13.2` semitones. The engine test renders same-base-note voices independently at `+12` and `-12` semitones and preserves the released tail's pitch. The operator confirmed all 16 distinct continuous ramps, semitone crossings, duplicate notes, release tails, and lane reuse without retrigger or cross-lane leakage. | Pass |

All four feasibility gates are therefore accepted.

## Operator acceptance results

The project owner reported completion of the full V1 smoke and large-file
validation with all cases passing on the native Rack 2.6.6 setup:

| Area | Result | Retained detail |
|---|---|---|
| Representative large SFZ | Loaded asynchronously to ready, played the first note, cancelled/replaced safely, and retained the previous instrument after a failed replacement. | Instrument name, referenced bytes, region count, load/cancel time, and peak RAM were not retained in this documentation pass. |
| Real-time safety | No audible click or unacceptable audio-thread stall during load, replacement, sample-rate change, or deletion. | Maximum callback duration was not retained. |
| Performance | Optimized 1/4/8/16-note Rack cases remained within the operator's acceptable CPU and memory budget. | Per-chord CPU, peak RAM, and the numeric ceiling were not retained. |
| Rack behavior | True stereo, mono fold-down, looped and malformed files, missing samples, repeated and duplicate notes, fast gates, long releases, changing polyphony, autosave/restore, and active module deletion passed. | Qualitative pass. |
| 16-lane expression gate | All 16 lanes were driven with distinct continuous V/OCT ramps over at least `-12` to `+12` semitones. No unintended retrigger, leakage, stuck note, or release-tail retuning was heard. | Numeric cents-error and leakage traces were not retained. |
| Squinky comparison | The comparison run completed without a result that blocks V1. Cella preserved mixed stereo as designed; Squinky retained its distinct mono lane-preserving output contract. | Side-by-side timing, CPU, RAM, and load-time values were not retained. |

## Automated results

`make sfz-test` passed on the native host at the recorded Cella revision. It
verified:

- voltage-to-note and velocity conversion, ordered bounded events, pitch-lane
  shrink, retrigger, and exact note-off identity;
- generated mono and true-stereo fixtures;
- same-note source-channel independence and release-tail pitch snapshots;
- preserve-all, keep-newest, and cut-on-retrigger policies, including delayed
  layers and release-trigger regions;
- stable rendering at 16-, 32-, and 64-frame quanta and an in-place quantum
  change without engine replacement or gate retrigger;
- asynchronous coalesced loading, failed replacement retaining the last valid
  instrument, pop-suppression state transitions, worker-side engine
  retirement, and module destruction returning in under the test's 100 ms
  bound while a synthetic load is blocked;
- asynchronous sample-rate rebuild behavior at 44.1, 48, and 96 kHz;
- stereo/mono output calibration, at least 6 dB nominal limiter headroom, JSON
  persistence, missing paths, and previous/next folder navigation;
- generated light and dark panels matching the checked-in SVGs.

The audio-output fixtures run the real pinned sfizioso engine. Lifecycle tests
which require deterministic blocking or event inspection use a test engine at
the Cella-owned sampler boundary.

## Stereo result

The fixture is a 48 kHz, 16-bit stereo WAV with a positive impulse on the left
at frame 256 and a negative impulse on the right at frame 1024. The test
observed both impulses above `0.01`, retained their timing separation and sign,
and measured less than `1e-5` signal in the opposite channel at each peak.

The mono fixture is a 48 kHz, 16-bit mono WAV. Its rendered left/right
difference is below `1e-5`. At the Rack boundary, connecting both outputs
preserves L/R, while leaving `RIGHT` unconnected produces `(L + R) * 0.5` on
`LEFT`.

## Note, pitch, and release results

- Rack lane `n` is submitted as sfizioso source channel `n`; Rack-16 does not
  reserve or consume a Manager lane.
- A gate edge fixes the MIDI note number. Subsequent V/OCT changes emit direct
  semitone offsets against that note instance, including across semitone
  boundaries, rather than retriggering or using an SFZ pitch-wheel range.
- Two lanes starting MIDI note 60 were rendered simultaneously at 440 Hz and
  110 Hz using `+12` and `-12` semitone offsets. Releasing the first removed
  the 440 Hz component while retaining the 110 Hz component.
- Reusing the released Rack lane produced a new 110 Hz note without retuning
  the prior 440 Hz release tail.
- Source-scoped tail tests cover layered attacks, a delayed layer, and a
  release-trigger region. Choking one source does not affect another source's
  released generation.

These spectral assertions use broad magnitude thresholds to prove identity
and isolation; they do not constitute a cents-accuracy measurement. The
engine-boundary audio test covers two independent lanes, while the tracker and
bounded data structures cover the 16-lane host mapping.

## Latency

The Cella adapter uses one capture block followed by one playback block, so
its design delay is a fixed selected quantum:

| Quantum | 44.1 kHz | 48 kHz | 96 kHz |
|---:|---:|---:|---:|
| 16 frames | 0.363 ms | 0.333 ms | 0.167 ms |
| 32 frames (default) | 0.726 ms | 0.667 ms | 0.333 ms |
| 64 frames | 1.451 ms | 1.333 ms | 0.667 ms |

These are architecture-derived adapter values, not an impulse-to-output
measurement including an instrument's own onset. At 48 kHz the default path
is 32 samples slower than Squinky's normal per-sample path and 27 samples
(`0.563` ms) slower than Squinky with its optional five-sample gate delay.
The Rack smoke test accepted the resulting playing latency.

## Build and package results

GitHub Actions run
[`34639082941`](https://github.com/victorkashirin/CellaEXT/actions/runs/34639082941)
completed successfully from a recursive clean checkout of the recorded Cella
revision with Rack SDK 2.6.6.

| Target | Stripped plugin binary | `.vcvplugin` package | CI build-step wall time |
|---|---:|---:|---:|
| macOS arm64 | 4,647,024 bytes | 868,478 bytes | 2m 00s |
| macOS x64 | 4,930,112 bytes | 991,151 bytes | 3m 26s |
| Linux x64 | 3,220,432 bytes | 1,233,351 bytes | 3m 59s |
| Windows x64 | 5,613,056 bytes | 1,308,478 bytes | 4m 31s |

The times are complete CI build-step durations on hosted runners, not a
controlled before/after dependency benchmark. That workflow builds packages;
the focused C++ test executables currently run only on the native host.

## Opcode and sample-format coverage

The committed generated fixtures exercise 16-bit PCM mono and stereo WAV,
`<global>`, `<group>`, `<region>`, `sample`, `key`, `pitch_keycenter`,
`ampeg_attack`, `ampeg_decay`, `ampeg_sustain`, `ampeg_release`, `loop_mode`,
`loop_start`, `loop_end`, `delay`, `tune`, and attack/release `trigger`
behavior. They also exercise the engine's built-in sine source in lifecycle
tests.

The user-owned large-instrument and Rack smoke run also passed. Its instrument
identity, opcode list, and sample formats were not retained, so only the
generated PCM WAV coverage above is reproducible from this repository. The
bundled engine's support for other formats is not promoted into a V1 guarantee.

## Record completeness

The plan requested raw large-file, CPU, memory, stall, pitch-error, and
side-by-side Squinky numbers. The corresponding tests were completed and
accepted by the project owner, but their values were not supplied for this
write-up. A future release benchmark should preserve its patch, instrument
identity, measurement procedure, and raw output if independently reproducible
numbers are needed. This does not block the V1 proceed decision.

## Known limitations and compatibility notes

- Cella emits one mixed stereo pair. Squinky emits one mono polyphonic output
  lane per Rack input lane. True stereo is therefore an established Cella
  advantage, but V1 does **not** provide lane-preserving output parity.
- sfizioso loading is synchronous inside the worker and has no cancellation
  hook. New requests are coalesced and stale results are discarded, while
  module destruction is kept non-blocking through the plugin-lifetime reaper;
  an in-progress parse/load itself is not interrupted.
- The displayed sample-memory value is a preload estimate, not measured RSS or
  total unique referenced file size.
- The engine pool is fixed at 128 voices. Instruments with many simultaneous
  layers can reach that limit before 16 held Rack notes.
- The permanent module slug in `plugin.json` is `SFZ`; this differs from the
  provisional `CellaSFZ` slug named in the plan and should be treated as the
  compatibility identifier if this build is published.
- Broad third-party SFZ compatibility, streaming, per-lane audio outputs,
  multiple output buses, pedals, CC inputs, keyswitch UI, separate bend/FM,
  and missing-sample relinking remain outside V1.
