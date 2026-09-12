# Cella SFZ — focused expression prototype roadmap

## Outcome

Deliver a usable internal Rack prototype centered on one primary feature,
expressive CV, and one secondary feature, keyswitch control. Both live on a
new 12 HP right-side expander so the existing Cella SFZ panel and patch-facing
IDs remain unchanged.

This is the active next-step roadmap. The [full product roadmap](full-roadmap.md)
remains a long-term idea catalogue, not the implementation sequence for this
prototype. The reasoning behind the choices below is recorded in
[roadmap-decisions.md](roadmap-decisions.md).

## Prerequisite — close the V1 evidence record

Create the `v1-results.md` required by the V1 plan before expression work
begins. Capture the existing test and build evidence, then rerun only the
measurements missing from the four gates. Do not repeat the complete V1 effort
when current evidence remains valid.

Exit: the record confirms asynchronous loading, true stereo, 16 note lanes,
and independent continuous V/OCT pitch, or identifies a blocking regression to
fix first.

## Primary feature — expression expander

Build a usable 12 HP expander immediately to the right of Cella SFZ with:

- dedicated polyphonic `BEND`, `PRESSURE`, and `TIMBRE` inputs;
- four polyphonic named-control inputs;
- a readable label for each named-control slot which opens the loaded
  instrument's assignment menu when clicked.

Standard CCs used by the loaded instrument appear in assignment menus, as do
CCs with explicit instrument labels. Prefer the instrument label, then a
standard MIDI controller name, and finally `CC N` as the display fallback.
CC74 is excluded because the dedicated `TIMBRE` input owns it; destructive MIDI
channel-mode messages are also excluded. Persist assignments by CC number.
After an instrument change, retain an assignment only when the new instrument
exposes that same CC; otherwise leave the slot unassigned.

### Voltage and lane behavior

- `BEND` is an additive 1 V/octave pitch offset and does not use SFZ
  `bend_up`/`bend_down` ranges.
- `PRESSURE`, `TIMBRE`, and named-control voltages map 0–10 V to normalized
  0–1, clamped at the endpoints.
- A monophonic input broadcasts to every active note lane. Polyphonic channel
  `n` controls note lane `n`.
- When a polyphonic cable loses channels, restore defaults on the removed
  lanes. When a cable or the expander is removed, restore neutral bend,
  pressure, and timbre plus each SFZ-declared CC default.
- Bend changes retain sample offsets within the render block. Pressure,
  timbre, and named controls update once per render quantum and use the
  sampler's own modulation smoothing.
- Released voices retain the expression values present at note-off. Reusing a
  Rack lane must not alter an earlier release tail.

## Secondary feature — articulation selector

Add keyswitch selection to the same expander:

- show every detected latched keyswitch, using its SFZ label when present and
  its musical note name otherwise;
- make the displayed articulation clickable for manual selection;
- provide a 0–10 V selector input, quantized across the detected switches with
  hysteresis at boundaries;
- issue a short switch note-on/off when selection changes, targeting
  `sw_last`-style latching instruments;
- broadcast a one-channel selector CV to every note lane and map polyphonic
  channels to their corresponding lanes;
- display `POLY` when active lanes hold different articulation selections.

Momentary `sw_down`/`sw_up` workflows and arbitrary keyswitch pitch/gate inputs
are outside this prototype.

## Engine and expander boundary

Extend the Cella-owned sampler boundary rather than exposing sfizioso types:

- add fixed real-time events for additive note bend, pressure, timbre,
  source-scoped CC, and keyswitch note-on/off;
- expose immutable loaded-instrument metadata containing named CC number,
  label and default, plus detected latched keyswitch note and optional label;
- transfer only bounded, fixed-size numeric messages across Rack's
  double-buffered expander connection;
- keep metadata strings, menu construction, loading, parsing, allocation, and
  engine destruction off the audio thread;
- accept only one compatible expander directly to the base module's right and
  ignore absent or incompatible neighbors safely.

Existing Cella SFZ parameter, input, output, and model IDs do not change. The
new expander's model slug, IDs, and persistence schema are provisional during
the internal prototype.

## Validation and exit criteria

Automated tests must prove:

- 16 distinct simultaneous bend, pressure, timbre, and named-CC streams do not
  leak across lanes;
- duplicate notes, retriggers, lane reuse, and release tails preserve note and
  expression ownership;
- mono broadcast, polyphonic lane mapping, partial channel shrink, cable
  removal, and expander hot-disconnection restore the specified values;
- named-control metadata, defaults, CC74 filtering, assignment menus,
  persistence, and instrument-change matching behave deterministically;
- labeled and unlabeled latched switches, manual selection, global/polyphonic
  CV selection, quantization hysteresis, and audible articulation changes work;
- expander messaging and expression processing perform no audio-thread locks,
  allocation, file access, or unbounded queue growth;
- all existing V1 tests remain green.

Also complete a native plugin build and hands-on Rack check of panel
legibility, assignment workflow, modulation response, engine reload, and
expander connect/disconnect behavior.

The prototype is complete when the real 12 HP expander is usable in Rack and
both feature sets pass the focused validation. Cross-platform packaging,
public API freezing, broad library certification, and stable-release polish
are not exit gates.

## Deferred, without ordering

- large-library streaming and shared sample caches;
- broad compatibility diagnostics and missing-sample recovery;
- extended CC assignment and per-slot transforms;
- direct or momentary keyswitch modes, sustain, sostenuto, and release velocity;
- multiple output buses, browsing, authoring, conversion, and advanced tuning.

Stereo lane-preserving output was implemented after this expression prototype:
the base module's `POLY` switch changes its existing left/right jacks between
mixed stereo and one stereo pair per Rack note lane. Shared SFZ effect buses are
bypassed in that mode while source-specific expression remains voice-local.
