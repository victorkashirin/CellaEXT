# Cella SFZ roadmap decisions

This document records the product decisions made on 2026-09-11 after the V1
four-gate feasibility slice. It is the durable rationale for the focused
[next-step roadmap](next-step-roadmap.md).

## Roadmap shape

- Keep [the full product roadmap](full-roadmap.md) as an aspirational,
  long-term reference.
- Add a separate active roadmap for the next internal prototype rather than
  continuing the full roadmap phase by phase.
- Organize the prototype around one primary and one secondary user-facing
  feature. Necessary real-time, lifecycle, persistence, and test work supports
  those features; it is not a third headline feature.
- Treat the missing `v1-results.md` as a concise prerequisite. Reuse existing
  evidence and rerun only missing gate measurements instead of repeating the
  entire V1 program.
- The exit target is a usable module in Rack, not only an engine spike or UI
  mockup. Because it is an internal prototype, the new module's IDs, schema,
  and slug remain provisional.

## Primary feature: expressive CV

- Put expression on a separate 12 HP right-side expander so the Cella SFZ base
  module and its existing patch-facing IDs remain unchanged.
- Provide dedicated polyphonic `BEND`, `PRESSURE`, and `TIMBRE` inputs.
- Provide four generic polyphonic CV inputs. Each can be assigned to a standard
  CC used by the loaded instrument or carrying an explicit instrument label.
- Prefer explicit instrument labels, then standard MIDI names, and finally a
  numeric `CC N` fallback so unlabeled instruments remain usable.
- Assign controls through clickable slot labels populated from instrument
  metadata.
- Reserve CC74 for the dedicated `TIMBRE` input and omit it from generic slot
  choices.
- Omit destructive MIDI channel-mode reset and all-notes-off messages from
  continuous CV assignment.
- Interpret `BEND` as an additive 1 V/octave offset, independent of the
  instrument's SFZ pitch-wheel range.
- Make bend sample-offset accurate. Update pressure, timbre, and named CCs once
  per sampler render quantum and retain the engine's smoothing behavior.
- A one-channel cable broadcasts to all active note lanes. A polyphonic cable
  addresses the corresponding lanes independently.
- On cable removal, channel-count shrink, or expander removal, restore neutral
  bend/pressure/timbre and the SFZ-declared default for named CCs.
- Persist a named-control assignment by CC number. Retain it after loading a
  different SFZ only when that instrument exposes the same CC; otherwise show
  the slot as unassigned.

### Why this is primary

Expression extends V1's unusual strength—16 independently owned Rack note
lanes—into a musically visible advantage. Dedicated inputs make common gestures
immediate, while four instrument-named slots cover library-specific controls
without recreating the old roadmap's eight-slot modulation workstation.

## Secondary feature: keyswitch control

- Put keyswitch control on the same expression expander.
- Present every detected latched keyswitch. Use its SFZ label when available
  and a musical note name when it is unlabeled.
- Use a clickable articulation label for manual selection and a 0–10 V input
  for quantized selection.
- Implement a latch selector which sends a short switch note-on/off and focuses
  the prototype on `sw_last`-style instruments.
- A one-channel selection CV broadcasts to all lanes. A polyphonic selection
  CV chooses independently for the corresponding lanes.

### Why this is secondary

Keyswitch selection shares the expander, instrument metadata, and source-lane
routing needed by expressive CV. It makes the prototype a cohesive performance
workflow without introducing a separate output, streaming, or browser
architecture.

## Explicitly deferred

Deferral means “not in this prototype,” not “never.” The full roadmap remains
the reference for these ideas, without implying their implementation order.

- large-library disk streaming and shared sample caches;
- broad compatibility diagnostics and missing-sample recovery;
- extended CC assignment;
- per-slot scale, offset, polarity, curve, and user smoothing;
- momentary or direct key-and-gate keyswitch modes;
- sustain, sostenuto, and release-velocity inputs;
- multiple output buses and output expanders;
- browser, indexing, authoring, conversion, and advanced tuning tools;
- public API freezing, cross-platform packaging, and stable-release polish.

Stereo lane-preserving output was subsequently implemented on the base module
as a `POLY` switch. It preserves per-lane voice DSP and expression, bypassing
instrument-wide SFZ effect buses only while the polyphonic output is selected.
