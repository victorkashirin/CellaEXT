# Dependencies

## sfizioso

- Upstream: <https://github.com/rullopat/sfizioso>
- Cella fork: <https://github.com/victorkashirin/sfizioso>
- Base: `a87b25e41868b9fce61199109de6f21e87c837e3` (`v1.2.2`)
- Pin: `2f7eafa140f0f3ff7b9662414b2605738654d699`
- Build: static/headless `libsfizz.a` through `deps/sfizioso/rack.mk`
- Options: no libsndfile, no OpenMP, C++17

The Cella fork adds the Rack-16 expression profile, Rack build fixes,
active-voice rendering optimization, source-scoped release-tail control,
Xcode 26 compatibility, and per-lane controller/keyswitch routing used by
Cella SFZ. It also exposes a post-voice, pre-shared-effect source-channel
render path for Cella's stereo polyphonic outputs. Those changes are preserved
as seven commits on the `cella-rack` branch: `e8a5727a`, `d0f101ca`, `0c8dec55`,
`3fbba4cc`, `86f218eb`, `31cb3edc`, and `2f7eafa1`.

Prepare a checkout with:

```sh
git submodule update --init --recursive
make sfz-bootstrap
```

The bootstrap script verifies the pinned commit and all recursive submodule
pins. It does not modify the sfizioso working tree.
