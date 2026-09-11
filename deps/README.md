# Dependencies

## sfizioso

- Upstream: <https://github.com/rullopat/sfizioso>
- Cella fork: <https://github.com/victorkashirin/sfizioso>
- Base: `a87b25e41868b9fce61199109de6f21e87c837e3` (`v1.2.2`)
- Pin: `0c8dec55906e57f5bdd2abb95496a652a8264f83`
- Build: static/headless `libsfizz.a` through `deps/sfizioso/rack.mk`
- Options: no libsndfile, no OpenMP, C++17

The Cella fork adds the Rack-16 expression profile, Rack build fixes,
active-voice rendering optimization, and source-scoped release-tail control
used by Cella SFZ. Those changes are preserved as three commits on the
`cella-rack` branch: `e8a5727a`, `d0f101ca`, and `0c8dec55`.

Prepare a checkout with:

```sh
git submodule update --init --recursive
make sfz-bootstrap
```

The bootstrap script verifies the pinned commit and all recursive submodule
pins. It does not modify the sfizioso working tree.
