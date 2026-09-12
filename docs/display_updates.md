# Cella SFZ display updates

## Outcome

Replace the current static ready-state display with a compact two-page display
that answers four user questions:

1. Which instrument is loaded, and where is it in the current folder?
2. What is the instrument doing now?
3. Which expression and articulation controls are available?
4. Is playback healthy, or does the user need to act?

The display spans the full 180-pixel module width and extends from the header
separator at y=25 to y=89, for a height of 64 pixels. It uses Rack's standard
`LedDisplay` surface and the largest monospaced text that fits each reserved
field. Two closely spaced circles in the top-right corner directly select the
default **Play** page or the **Info** page. Pages never rotate automatically.

Loading, missing-file, and error states continue to override normal page
content. Runtime warnings temporarily override the lowest-priority normal
content without changing the selected page.

## Display layout and page control

Use the top row for the filename at left and folder position near the right.
Use the other rows as paired left/right fields so related values occupy the
available width instead of forming centered text blocks.

The page controls are two horizontally arranged circles in the top-right
corner. Each has a 12 × 15 pixel hit target:

- the filled circle identifies the selected page;
- the outlined circle identifies the other page;
- hovering and pressing brighten the corresponding circle;
- each circle has its own `Show performance` or `Show instrument info` tooltip;
- a primary-button click directly selects that circle's page.

The top-row text must not draw beneath the circles. The remaining rows may use
the full inset width.

Dropping one or more operating-system paths on the display loads the first
existing path with a case-insensitive `.sfz` extension. Other file types are
ignored and the accepted drop is consumed so Rack does not handle it elsewhere.
Right-clicking the display opens a display-specific menu containing `Load SFZ...`
and `Unload instrument`. Unload clears persisted load metadata immediately,
fades the active engine to silence, detaches it at an audio-block boundary, and
retires it on the loader worker.

Persist the selected page per module as a non-audio setting named
`displayPage`, with `0` for Play and `1` for Info. Missing, invalid, or future
values fall back to Play. Append this field to module JSON without changing any
existing parameter, input, output, light, or model IDs. The page control must be
safe to use while the module is unloaded, loading, or reporting an error.

## Page 1 — Play

Play is the default page and prioritizes information useful while performing.
Its ready-state layout is:

```text
Grand Piano                 03/18  ● ○
Held 04               Voices 023 / 128
Controls 4 / 17             Switches 6
```

The examples are illustrative; spacing may be tightened after checking the
rendered display at 100% Rack zoom.

### Instrument and folder position

The first row contains the one-based index and total count of `.sfz` files in
the loaded file's directory, followed by the filename without its `.sfz`
extension. Use zero padding only when needed to keep the index from shifting as
the user navigates. Examples are `3/8`, `03/18`, and `003/120`.

Use the same directory enumeration, case-insensitive extension filter, absolute
path normalization, and sort order for the display and the previous/next
buttons. The two features must never disagree about which file is adjacent.
Compute the folder snapshot off the audio thread for every explicit load and
previous/next action. Continuous directory watching is out of scope. If the
directory cannot be read or the loaded file is not present in the resulting
list, omit the index rather than displaying `0/N` or stale data.

Truncate only the filename portion after reserving room for the index. Prefer a
middle ellipsis so both the distinguishing beginning and ending remain visible;
omit the `.sfz` extension before truncation. The complete absolute path remains
available through a tooltip or context-menu information item.

### Held notes and voices

`Held` is the number of logical Rack note lanes whose gate-started note has not
yet received note-off. It is not the number of connected polyphonic channels.
Mono gate broadcast across several pitch lanes therefore counts every held
lane, while release tails do not count as held.

`Voices` is sfizioso's current active-voice count and the configured global voice
limit, currently 128. Active voices include attacks, sustained layers, release
tails, and release-triggered regions. Do not derive or display a separate tail
count: one logical note can create several region voices, so subtracting held
lanes from active voices is not meaningful.

Read the active-voice count after each sampler render quantum and publish it to
the UI through an atomic numeric snapshot. The numeric value shows the latest
published count. Draw a thin voice-pressure meter beneath the second row, using
the greater of the current value and a 400 ms peak hold. The meter returns to
the current value immediately after the hold expires; no slow animated decay is
required.

Voice-pressure color thresholds are:

- normal display color below 75% of the configured limit;
- amber from 75% through 89%;
- red at 90% and above.

These colors indicate pressure only. Reaching a threshold is not itself an
overload warning and must not claim that a voice was stolen.

### Controllers and switches

`Controls A / N` reports named/assignable MIDI controls:

- `A` is the number of distinct, valid CC assignments currently received from
  a connected Cella SFZ Expression expander;
- `N` is the number of assignable CCs exposed by the loaded instrument;
- without a compatible expander, display `Controls 0 / N`;
- clamp display values to the actual supported metadata and assignment limits
  rather than silently wrapping a field.

`Switches N` is the number of detected latched keyswitch articulations exposed by the
instrument. It does not report the current articulation because different Rack
lanes can select different articulations; that state remains on the Expression
expander.

If no controls or switches are available, display zero rather than hiding the
field. This distinguishes a known absence from unavailable metadata.

## Page 2 — Info

Info contains load-time facts useful for inspecting an instrument:

```text
Grand Piano                 03/18  ○ ●
Regions 384                   Samples 127
Preload ~46.0 MB
```

The first row is the filename without extension. The second row contains folder
position and parsed region count. The third row contains the number of sample
files represented by sfizioso's preload statistics and the estimated bytes in
their preload buffers.

`Preload` must be described in the display tooltip as an estimate, not process RSS,
total instrument size, or a promise that all sample data is memory-resident.
Prefix the byte value with `~` whenever it is nonzero. Use binary units with one
decimal place (`KB`, `MB`, `GB`) and avoid displaying raw bytes unless the value
is below 1 KB.

Do not display a `DISK`/`RAM` mode. Sfizioso preloads the beginning of sample
files and can load remaining data in the background, so ordinary playback is a
hybrid rather than one exclusive mode. A future disk indicator may be added
only when it is backed by actionable telemetry such as a late-read or streaming
starvation event.

## Status and warning overlays

### Persistent load states

Persistent load states replace normal page fields but leave the page control
visible. Loading, missing-file, and error layouts retain the filename at the
top-left and folder position at the top-right, with a larger status label below:

- unloaded: `NO INSTRUMENT`;
- loading: shortened filename, folder position, and `LOADING` in the normal
  panel-matched gold;
- missing SFZ: shortened filename plus `MISSING SFZ` in red;
- load failure: shortened filename plus the existing actionable failure message
  in red.

Loading a replacement must not expose metadata or runtime counts from the new
instrument until its engine bundle is ready. If the previous instrument remains
audible during a failed replacement, the error overlay takes precedence; after
the error is dismissed by a successful load, all normal pages describe the
active engine only.

### Runtime warnings

Runtime warnings replace the third row on either page and use amber or red
without changing the selected page. Hold a warning for at least 1 second after
its most recent event so it can be read. A repeated event restarts the hold.
When the hold expires, restore the selected page's normal third row.

Warning priority, from highest to lowest, is:

1. `EVENT DROP` — a Cella capture or adapter queue rejected an event; red.
2. `DISK TOO SLOW` — sfizioso reported a late sample read or starvation; red.
3. `VOICE LIMIT` — the engine reported an actual hard allocation failure or
   global-limit voice steal; amber.
4. `OUTPUT LIMITING` — either channel entered Cella's soft-limiter knee; amber.

Do not infer `VOICE LIMIT` merely from `activeVoices == maxVoices`. Sfizioso has
overflow voices and instrument-defined polyphony behavior, so equality alone
does not prove loss. Until the engine boundary provides a trustworthy hard-limit
or steal event, show only the red voice-pressure meter and omit this warning.

Similarly, omit `DISK TOO SLOW` until sfizioso exposes reliable late-read
telemetry. Do not substitute generic disk activity. `EVENT DROP` and
`OUTPUT LIMITING` can be implemented from existing Cella-side state.

CPU overload is not a display warning in this change. Rack already provides
module CPU reporting, while a sampler-local render-time estimate can omit host
and adapter work. It may be reconsidered later if it can represent missed audio
deadlines rather than merely high activity.

## Data and real-time boundaries

Extend the Cella-owned snapshots and sampler interface rather than reading
sfizioso state directly from the UI thread.

- The loader worker publishes filename, path, folder position, region count,
  preload sample count, estimated preload bytes, assignable CC count, and
  keyswitch count as immutable load metadata.
- The audio thread publishes only bounded numeric runtime state: held lanes,
  current voices, voice limit, warning flags/counters, and warning timestamps or
  equivalent countdowns.
- The UI reads snapshots and formats text. It performs no engine calls.
- No directory access, string construction, allocation, locks, NanoVG work, or
  page interaction runs on the audio thread.
- Sampling or publishing display telemetry must not change event ordering,
  render cadence, voice ownership, tail behavior, or generated audio.

Add active-voice and voice-limit accessors to `SamplerEngine` so test doubles
remain independent of sfizioso. Keep warning counters monotonic where practical;
the UI can detect a changed counter without racing a one-sample boolean pulse.

## Accessibility and visual behavior

- Text and meter state must remain understandable without color. Numeric values
  and explicit warning labels carry meaning; color is reinforcement.
- Preserve the existing high-contrast dark display treatment in light and dark
  panel themes.
- The page control must have hover feedback and a tooltip because its artwork is
  intentionally small.
- Do not flash the complete display. Only the meter or warning text changes
  color.
- Keep normal values stable: folder metadata changes only on a folder snapshot,
  controller counts change on metadata/assignment updates, and rapidly changing
  voice telemetry uses the specified peak hold.

## Validation and acceptance criteria

Automated tests must verify:

- default and persisted page selection, including invalid JSON fallback;
- deterministic folder index/count matching previous/next navigation, mixed-case
  `.sfz` extensions, unreadable directories, and a selected file absent from the
  directory snapshot;
- held-lane counting for mono broadcast, polyphonic gates, note-off, channel
  shrink, retrigger, and release tails;
- active-voice publication after render without UI-thread engine access;
- current and peak-held voice values, threshold boundaries, and peak expiry;
- assigned/available CC counts across expander connection, duplicate
  assignments, instrument replacement, and expander removal;
- keyswitch, region, preload-count, and estimated-byte formatting at zero and at
  large values;
- warning priority, one-second hold, repeated-event extension, and restoration
  of both selected pages;
- omission of unsupported voice-limit and disk-starvation warnings;
- case-insensitive display drops, rejected non-SFZ drops, loading-state folder
  metadata, display-menu actions, and pop-free worker-retired unload;
- no changes to existing module IDs, normal path persistence, asynchronous loading,
  expression behavior, or audio output.

Hands-on Rack validation must cover 100%, 75%, and 50% zoom in both panel themes.
At 100%, all example rows and the page control must be legible without overlap.
At smaller zoom levels, the control must remain clickable and warning colors
must not obscure text. Confirm page switching during unloaded, loading, ready,
and error states, and play a layered release-heavy instrument to verify that the
voice display remains readable rather than flickering.

This update is complete when both pages, persisted on-display pagination,
available telemetry, warning overlays, tests, and hands-on layout checks are in
place. Trustworthy engine telemetry for disk starvation and actual voice loss is
explicitly optional; labels for those conditions must remain absent until the
underlying events can be measured.

## Deferred interaction

- automatic page rotation;
- raw region enable/disable controls;
- an SFZ region, group, microphone, or articulation editor;
- waveform, keyboard-range, and velocity-layer visualization;
- continuous directory watching or library indexing;
- generic disk-activity and sampler-local CPU indicators.

If instrument muting or layer control is added later, prefer named groups,
microphone layers, or articulations over individual raw regions. A large SFZ can
contain hundreds or thousands of regions whose numeric identities are not a
stable or useful performance interface.
