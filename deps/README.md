# Dependencies

## sfizioso

- Upstream: <https://github.com/rullopat/sfizioso>
- Cella fork: <https://github.com/victorkashirin/sfizioso>
- Base: `a87b25e41868b9fce61199109de6f21e87c837e3` (`v1.2.2`)
- Pin: `3fbba4cc3218efa99dc265945244d6b16b0e1df2`
- Build: static/headless `libsfizz.a` through `deps/sfizioso/rack.mk`
- Options: no libsndfile, no OpenMP, C++17

The Cella fork adds the Rack-16 expression profile, Rack build fixes,
active-voice rendering optimization, source-scoped release-tail control,
and Xcode 26 compatibility used by Cella SFZ. Those changes are preserved
as four commits on the `cella-rack` branch: `e8a5727a`, `d0f101ca`,
`0c8dec55`, and `3fbba4cc`.

Prepare a checkout with:

```sh
git submodule update --init --recursive
make sfz-bootstrap
```

The bootstrap script verifies the pinned commit and all recursive submodule
pins. It does not modify the sfizioso working tree.
