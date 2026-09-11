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
