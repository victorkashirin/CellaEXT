# Cella-EXT

Cella-EXT is a VCV Rack plugin for Cella modules that require substantial
external dependencies. Its first module is **Cella SFZ**, a polyphonic,
true-stereo SFZ sampler powered by a pinned Cella sfizioso fork.

## Build

Clone recursively, prepare the pinned dependency, then build with the Rack SDK:

```sh
git submodule update --init --recursive
make sfz-bootstrap
make
```

Set `RACK_DIR` if the Rack SDK is not available at `../Rack-SDK`.

Run the transferred V1 verification suite with:

```sh
make sfz-test
```

## Polyphonic stereo output

`LEFT` and `RIGHT` produce the instrument's mixed stereo output by default.
Enable the panel `POLY` button to make both jacks polyphonic: channel `n` on
each jack contains only V/OCT lane `n`, preserving that lane's left and right
sample audio. The channel count follows the V/OCT input. If only `LEFT` is
connected, each lane receives its own `(L + R) / 2` fold-down.

Polyphonic mode keeps voice-local envelopes, filters, panning, pitch, pressure,
timbre, and CC modulation. It bypasses instrument-wide SFZ `<effect>` buses;
disable `POLY` to hear the normal shared-effect stereo mix.

## SFZ Expression expander

Place **SFZ Expression** directly to the right of **Cella SFZ**. Its `BEND`
input adds polyphonic 1 V/oct pitch, while `PRESSURE`, `TIMBRE`, and the four
assignable control inputs accept polyphonic 0–10 V expression. A monophonic
cable broadcasts to all active note lanes.

Click a control label to assign a standard CC used by the loaded instrument.
Explicit SFZ labels take priority; otherwise the menu uses a standard MIDI name
or a numeric `CC N` fallback. Click the articulation display to choose a
detected latched keyswitch manually, or patch 0–10 V into `ARTICULATION`;
polyphonic selections show `POLY` when lanes differ. Assignments are stored by
CC number and are retained across instrument changes only when the new
instrument exposes the same CC.

## Panel generation

The checked-in light and dark panels contain outlined text and are generated
from the bundled font. Set up the development-only Python dependency, then
regenerate or verify them with:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tools/requirements.txt
make PYTHON=.venv/bin/python sfz-panel
make PYTHON=.venv/bin/python sfz-panel-check
```

The implementation specification and roadmap live in [`spec/sfz/`](spec/sfz/).
