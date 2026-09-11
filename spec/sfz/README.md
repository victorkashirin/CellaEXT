# Cella SFZ player specifications

This directory contains the planning material for a Cella SFZ player and an
export of the ChatGPT conversation that led to it.

- [V1 implementation plan](v1-plan.md) — a narrow vertical slice proving the
  four prototype gates: large-SFZ loading, stereo rendering, 16 Rack note
  lanes, and independent continuous pitch on every lane.
- [Full product roadmap](full-roadmap.md) — the staged path from the V1 slice
  to a Rack-native sampler intended to exceed the existing Squinky Labs SFZ
  Player in compatibility, stereo fidelity, workflow, and modulation.
- [Competitive source audit](competitive-analysis.md) — a code-backed baseline
  of what the pulled Squinky player actually does, what V1 deliberately does
  not match, and which later milestones constitute a credible win.
- [Conversation export](conversation.md) — the complete dialogue available
  from the referenced ChatGPT conversation.

## Working decisions

- Working module name: **Cella SFZ**.
- Provisional permanent model slug: `CellaSFZ`. Confirm before the first public
  build; a published Rack model slug must not later change without breaking
  patch compatibility.
- Use a pinned source snapshot of
  [`sfizioso`](https://github.com/rullopat/sfizioso) as the playback engine.
  It is an active, MPE-capable C++17 fork of `sfizz` with channel-aware note,
  pitch, CC, and aftertouch APIs and a Rack SDK makefile. Keep it behind a
  narrow Cella engine interface so updates remain controlled.
- Put the pinned source tree at `deps/sfizioso/`, alongside the existing
  `deps/ebur128/`. Record its commit, recursive dependency pins, enabled build
  options, licenses, and any Cella patches in `deps/README.md`; do not depend
  on a system installation or an unpinned checkout.
- V1 polyphony means up to 16 polyphonic V/OCT/GATE/VELOCITY input channels,
  each with continuously tracked independent V/OCT pitch, mixed by one sampler
  into one stereo pair. `LEFT` and `RIGHT` are monophonic Rack outputs; they
  are the two channels of the stereo mix.
- Squinky's comparison player uses a different output contract: each input
  lane produces one mono channel on a polyphonic output cable. Cella V1 tests
  stereo instrument polyphony, not yet lane-preserving audio parity. The full
  roadmap treats a stereo lane-preserving mode as an early architecture gate.
- Sfizioso's stock non-MPE mode preserves all 16 source channels for note
  ownership but collapses expression to channel 0. V1 must therefore implement
  and test a small `Rack-16` expression profile in the engine; standards-
  compliant lower-zone MPE alone provides only 15 Member lanes.
- No sample loading, parsing, file I/O, destruction of a large engine, locks,
  or unbounded allocation may occur on Rack's audio thread.

## Source material

- [sfizioso library](https://github.com/rullopat/sfizioso)
- [sfizioso inherited C/C++ API](https://github.com/rullopat/sfizioso/tree/main/src)
- [sfizz C API reference](https://sfztools.github.io/sfizz/api/sfizz.hpp/),
  useful for the inherited API surface
- [sfizz opcode support table](https://sfztools.github.io/sfizz/development/status/opcodes/)
- [SFZ opcode reference](https://sfzformat.com/opcodes/)
- [VCV Rack Plugin API Guide](https://vcvrack.com/manual/PluginGuide)
- [Squinky Labs SFZ Player manual](https://github.com/kockie69/SquinkyVCV-main/blob/master/docs/sfz-player.md)
- Local Squinky source audit: `/Users/victorkashirin/code/Rack/SquinkyVCV-main`
  at commit `3701a8c8931e5e8073e00cd5632db3966befe54f`, assessed 2026-09-10.
