# Competitive source audit — Squinky Labs SFZ Player

## Baseline and purpose

This is a source-code audit of the locally pulled comparison repository, not a
claim inferred from screenshots or marketing copy.

- Repository: `/Users/victorkashirin/code/Rack/SquinkyVCV-main`
- Origin: `https://github.com/kockie69/SquinkyVCV-main.git`
- Audited commit: `3701a8c8931e5e8073e00cd5632db3966befe54f`
- Commit date: 2025-10-26
- Audit date: 2026-09-10
- Module implementation: `src/SampModule.cpp`, `composites/Samp.h`, and
  `dsp/samp/`

The comparison is against observable behavior at that commit. Comments,
manual claims, and enum entries are not counted as working features unless the
playback path uses them.

## Important terminology correction

Squinky does not produce a single monophonic mix. Its V/OCT channel count sets
the module's channel count, and its single `AUDIO` port emits the same number
of polyphonic channels. Each Rack lane owns one mono sample stream. Stereo WAV
and FLAC input is averaged to mono before playback.

Cella V1 has the inverse tradeoff: it preserves and mixes stereo voices to a
monophonic `LEFT`/`RIGHT` pair, but does not preserve one audio channel per
Rack lane. Therefore V1 can prove stereo and musical polyphony, but it cannot
yet claim complete I/O parity.

## Source-backed capability matrix

| Area | Squinky player at audited commit | Cella V1 target | Cella path to win |
|---|---|---|---|
| Note input | V/OCT determines 0–16 lanes; poly gate and velocity | Up to 16 V/OCT/gate/velocity lanes | Match, with explicit lane lifecycle and duplicate-note tests |
| Audio output | One polyphonic port; one mono output channel per input lane | Mixed stereo `LEFT`/`RIGHT` | Add an optional stereo lane mode: polyphonic L and R ports |
| Stereo sources | Stereo WAV/FLAC is averaged `(L + R) / 2` into mono | Preserve true stereo | Immediate V1 advantage |
| Region layering | Selects at most one matching region; repairs or deletes overlaps | Whatever pinned sfizioso correctly renders | Verify simultaneous layers, crossfades, and choke behavior in corpus |
| Release behavior | Amplitude release supported; release-trigger regions discarded | Engine release envelopes and triggers subject to fixture verification | Publish verified release-trigger and pedal behavior |
| Selection | Key/velocity, random, sequence, keyswitch | Engine feature set, minimally exercised in V1 | Broad corpus plus transparent diagnostics |
| Modulation | Poly exponential FM; audio-rate sample-position/phase modulation and depth CV | Continuous independent V/OCT pitch on all 16 lanes through `Rack-16`; no separate FM input | Add separate per-lane bend/FM; evaluate phase modulation as a labeled extension |
| Keyswitch UI | Instrument-derived popup plus playable switch notes | No dedicated V1 keyswitch UI | Add panel, CV, latch/momentary, labels, and current-state feedback |
| Loading | Background parse/sample load with percentage display | Background load and atomic swap | Retain the old instrument after failed/cancelled replacement and add diagnostics |
| Formats | WAV and FLAC; unsupported extensions fail | Pinned-engine formats, not promised until verified | Generate and publish exact format/decoder matrix |
| Memory | Converts all sample data to float and keeps it in RAM | V1 may preload | Add bounded disk streaming and shared immutable sample cache |
| Resampling | Per-sample cubic interpolation | Sfizioso resampler, measured rather than assumed superior | Publish quality/CPU/latency profiles |
| Latency | Per-sample processing; optional five-sample gate delay | Block adapter quantum to be selected experimentally | Benchmark 1/16/32/64 frames and default to the lowest safe practical quantum |
| Persistence/status | Saves SFZ path and schema; loading/progress/error/range display | Save path/settings; ready/error/memory status | Missing-file locate/relink, warnings, compatibility tier, retained sound on failed reload |

## Verified competitor strengths to respect

- Four SIMD playback banks cover the 16 Rack channels, and the output port is
  assigned the V/OCT input channel count.
- Gate handling is per sample. An optional five-sample delay lets pitch settle
  before a simultaneous gate edge.
- Exponential FM is per lane. The `LFM` path changes the sample pointer at audio
  rate and has both knob and polyphonic depth-CV control.
- File parsing and sample loading run through a worker/message system. The UI
  shows load progress and instrument pitch range.
- Patch JSON stores the selected SFZ path. Instrument metadata drives a
  keyswitch popup.
- The custom player implements cubic interpolation, loops, offsets, one-shot
  behavior, velocity gain, random/sequence selection, and keyswitch state.

These are minimum comparison tests even when Cella chooses a different UX.

## Verified competitor constraints Cella can exploit

- `WaveFileLoader::convertToMono()` averages the two WAV channels and
  `FlacReader::onData()` does the same for stereo FLAC, discarding the original
  stereo field.
- `RegionPool::play()` returns only the first eligible region. The compiler
  mutates or removes overlapping key/velocity regions, so legitimate layered
  instruments can lose material.
- `CompiledRegion::shouldIgnore()` drops release-trigger regions and regions
  gated by sustain-pedal conditions.
- The schema is narrow. It recognizes key/velocity ranges, sample/default path,
  release, gain/tuning, loops/ranges, random/sequence, keyswitches, and a CC64
  special case, while the manual explicitly documents no general CC,
  instrument modulation, filters/synthesis, or release samples.
- Only WAV and FLAC loaders are selected; other extensions use the failing
  null loader.
- All decoded sample data is resident in RAM.
- A failed replacement load sets all playback banks to null. It does not keep
  the last valid instrument playable.

## Competitive product gates

### Gate A — V1 feasibility, no superiority claim

Run the same generated mono/stereo fixtures and the same 1/4/8/16-note Rack
patch through both modules. Record:

- stereo separation and mono fold-down;
- whether output channels are mixed or lane-preserving;
- note-on/retrigger/release latency;
- duplicate-pitch behavior;
- 16 simultaneous independent continuous pitch ramps and cross-lane leakage;
- CPU, peak RAM, load time, and audio-thread stalls;
- behavior after a malformed or missing replacement load.

V1 may be called successful when a representative large SFZ loads safely,
stereo is preserved, 16-lane input works, and all 16 lanes bend independently
and continuously. It must still be described as a different output contract
from Squinky.

### Gate B — credible replacement

Before calling Cella a replacement, ship both a normal mixed-stereo mode and a
verified lane-preserving mode, or document and intentionally reject the latter
after user testing. Match practical note, velocity, pitch-modulation,
keyswitch, async-load, path-persistence, loop, sequence, and random workflows.

### Gate C — credible win

A superiority claim requires measured or corpus-backed advantages, not the
choice of sfizioso alone:

- stereo remains stereo in both mixed and lane-preserving routing;
- multiple legitimate regions, release triggers, CC conditions/modulation,
  filters/envelopes, and common formats pass published fixtures;
- large instruments play with bounded memory through streaming;
- failed replacement loads leave the sounding instrument intact;
- warnings identify missing samples and unsupported behavior;
- latency and CPU are competitive under identical patches and quality modes.

## Architecture decision forced by the audit

Lane-preserving stereo cannot be assumed to fall out of a shared stereo synth:
once all voices are mixed, their Rack-lane identity is gone. Prototype this
before freezing ports or engine ownership. Candidate approaches are:

1. add an engine render bus/tap keyed by sfizioso source channel;
2. use per-lane engine instances backed by shared immutable instrument/sample
   data;
3. keep the base module mixed stereo and use a dedicated lane-output variant or
   expander.

The prototype must test pedal, choke/off groups, round robin, keyswitch state,
RAM sharing, and CPU. Sixteen isolated engines are not acceptable if they
silently multiply sample memory or make instrument-wide state incorrect.

## Audit caveats

- This was static inspection, not a completed Rack runtime benchmark.
- The competitor repository contains tests, but their existence does not prove
  the audited commit passes on the current Rack SDK or this machine.
- Enum/schema recognition does not always mean full SFZ semantics. For example,
  release trigger values parse but are deliberately ignored during compilation.
- Re-run this audit if the comparison commit changes.

## Evidence anchors

The most consequential findings can be checked directly in the pulled source:

- [polyphonic output follows V/OCT channel count](</Users/victorkashirin/code/Rack/SquinkyVCV-main/composites/Samp.h:366>)
- [per-sample gate, lane note-on, modulation, and output loop](</Users/victorkashirin/code/Rack/SquinkyVCV-main/composites/Samp.h:487>)
- [failed load clears the four playback banks](</Users/victorkashirin/code/Rack/SquinkyVCV-main/composites/Samp.h:329>)
- [WAV stereo-to-mono conversion](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/WaveLoaders.cpp:59>)
- [FLAC stereo-to-mono conversion](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/FlacReader.cpp:139>)
- [only WAV and FLAC loader selection](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/WaveLoaders.cpp:199>)
- [first eligible region wins](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/RegionPool.cpp:62>)
- [overlapping regions are repaired or removed](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/RegionPool.cpp:317>)
- [release-trigger and pedal-conditioned regions are ignored](</Users/victorkashirin/code/Rack/SquinkyVCV-main/dsp/samp/CompiledRegion.cpp:178>)
- [persisted SFZ path and schema](</Users/victorkashirin/code/Rack/SquinkyVCV-main/src/SampModule.cpp:117>)
- [comparison player's own compatibility limitations](</Users/victorkashirin/code/Rack/SquinkyVCV-main/docs/sfz-player-compatibility.md:33>)
