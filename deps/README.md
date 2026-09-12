# Dependencies

## sfizioso

- Upstream: <https://github.com/rullopat/sfizioso>
- Cella fork: <https://github.com/victorkashirin/sfizioso>
- Base: `a87b25e41868b9fce61199109de6f21e87c837e3` (`v1.2.2`)
- Pin: `86f218eb2b3409ace8b1a17aa6f7ceb15ec24fe5`
- Build: static/headless `libsfizz.a` through `deps/sfizioso/rack.mk`
- Options: no libsndfile, no OpenMP, C++17

The Cella fork adds the Rack-16 expression profile, Rack build fixes,
active-voice rendering optimization, source-scoped release-tail control,
Xcode 26 compatibility, and per-lane controller/keyswitch routing used by
Cella SFZ. Those changes are preserved as five commits on the `cella-rack`
branch: `e8a5727a`, `d0f101ca`, `0c8dec55`, `3fbba4cc`, and `86f218eb`.

Prepare a checkout with:

```sh
git submodule update --init --recursive
make sfz-bootstrap
```

The bootstrap script verifies the pinned commit and all recursive submodule
pins. It does not modify the sfizioso working tree.
