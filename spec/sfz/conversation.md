# Conversation export: SoundFont Player Caveats

Source conversation ID: `6aa2aeba-6d40-83eb-8673-a0b1df2461a8`

Exported on 2026-09-10 from the complete dialogue returned by the referenced
ChatGPT conversation. The conversation contained 4 user/assistant turns and no
attachments. Text is preserved as returned, including its original citation
placeholders.

---

## User

lets say I want to implement sound font player for vcv rack. 
What are some gotchas and caveats? I know there are different versions of sound font format.

---

## Assistant

Yes. A SoundFont player looks like “sample lookup + pitch + envelope” until you try to make it agree with actual SoundFonts. The difficult part is mostly **interpretation of the articulation data**, not decoding the samples.

### First: decide what “SoundFont support” means

| Format/version | What matters |
|---|---|
| **SF2 2.00** | Early specification/drafts. No defined modulator system. |
| **SF2 2.01** | The practical baseline. Adds modulators and their MIDI/controller behavior. |
| **SF2 2.04** | Essentially 2.01 plus **24-bit sample support** via the `sm24` chunk. |
| **SF3** | An **unofficial extension** of SF2 using individually compressed samples, usually Ogg Vorbis. Not “SoundFont 2.04 with compression.” |
| **SFZ** | Completely different format: text definitions + external samples. Similar purpose, not the same parser or synth model. |

The official 2.04 revision history says 2.01 added modulators/NRPN support and 2.04 added 24-bit samples. citeturn218979search54 SF3 is a de-facto extension originally created for MuseScore; its `ifil` major version is 3 and compressed samples are individually encoded inside the sample-data area. citeturn714544search0

For a VCV module, I would initially define support as **SF2 2.01 + 2.04, with SF3 as a separate later feature**.

### The gotchas I would pay attention to

1. **The preset/instrument/sample hierarchy is the first major trap.** An SF2 isn't simply `key range -> sample`. It is effectively:

   `Preset -> preset zone -> Instrument -> instrument zone -> Sample`

   Both preset and instrument levels can have **global zones** whose settings inherit into local zones. The `pdta` section—the famous nine-part “hydra”—uses indexed arrays rather than convenient nested structures. It also contains terminal records and has ordering/index rules that are easy to get one element off. citeturn617801view2

   I would parse the disk representation once and immediately compile it into something much nicer for runtime, e.g. an immutable array of resolved `Region` objects.

2. **Generator inheritance is surprisingly non-obvious.** Instrument generators are generally **absolute/replacing**, while preset generators are generally **relative/additive**. But ranges aren't added: preset and instrument key/velocity ranges act as intersections. Sample-specific generators such as `sampleModes`, address offsets, `exclusiveClass`, and `overridingRootKey` aren't legal at preset level. citeturn799537view1turn414334view4

   This is one of the areas where a player can successfully open almost every SF2 while still sounding subtly wrong.

3. **There are implicit modulators that aren't stored in the file.** This is a particularly nasty one. SF2 defines default instrument-level modulators, including velocity → attenuation, velocity → filter cutoff, pitch wheel behavior, etc. A SoundFont can explicitly override or cancel them. citeturn414334view0

   Consequently, simply ignoring `pmod`/`imod` doesn't mean “no modulation.” It means your synth is implementing a different instrument.

   For an MVP you can absolutely omit modulators, but label that internally as something like **SF2 generator compatibility**, rather than assuming it is fully compliant.

4. **SoundFont units are mostly perceptual/logarithmic.** You'll encounter cents, absolute cents, timecents, centibels, and 0.1%-style units. Useful conversions are:

```text
seconds = 2^(timecents / 1200)

Hz = 8.176 * 2^(absoluteCents / 1200)

amplitude = 10^(-centibels / 200)

pitchRatio = 2^(pitchCents / 1200)
```

   Volume-envelope sustain is an **attenuation**, whereas the modulation-envelope sustain parameter is a decrease expressed in percentage units. Mixing those two up gives very strange envelopes. The specification also has special sentinel behavior such as `-32768` for essentially instantaneous/no delay in several envelope parameters. citeturn414334view2

5. **Pitch requires more metadata than it initially appears.** The sample header contains its own sample rate, original MIDI pitch and signed cents correction. Then instrument generators can supply an overriding root key, coarse/fine tuning and `scaleTuning`. citeturn324175view0turn324175view3

   Your playback increment ends up conceptually similar to:

```text
increment =
    sourceSampleRate / rackSampleRate
    * 2^(totalPitchCents / 1200)
```

   The `pitchCorrection` gets added with its sign. For example, the spec explicitly says a sample recorded 4 cents sharp should have correction `-4`. citeturn324175view1

6. **VCV's continuous pitch model doesn't map perfectly onto MIDI note selection.** This is one I would design explicitly. SoundFont regions are selected from a discrete MIDI key and velocity at **note-on**. VCV gives you continuous 1 V/oct CV.

   Suppose the gate opens at C4 and then the pitch CV slews upward. I would select the SF2 region at gate-on and thereafter treat CV changes as continuous pitch changes of that existing voice. I would **not continuously switch zones as the CV crosses semitones**. Otherwise portamento can suddenly jump between multisamples.

   In other words, separate `note-on key used for region selection` from `current playback pitch`.

7. **Loops have off-by-one semantics waiting to bite you.** `dwEnd` is effectively the boundary following the playable sample, and `dwEndloop` identifies the point equivalent to the loop start—the sample immediately before it is the last one actually played before wrapping. The format also assumes extra samples around boundaries to make interpolation safe. citeturn324175view0

   The standard loop modes are particularly important:

```text
0  no loop
1  loop continuously, including release
2  reserved → specification says treat as no loop
3  loop while key held, then leave loop and play sample tail
```

   Mode `3` is often where simplistic players fail. citeturn617801view4

   There is also a real-world compatibility complication: Polyphone/FluidSynth have introduced behavior around mode 2 for release samples that isn't part of the original SF2 specification. citeturn657970search12

8. **Stereo samples aren't interleaved stereo data.** They are separate sample entries marked left/right and connected using `wSampleLink`. The spec says they should run synchronously, and somewhat unexpectedly, the pair's pitch is controlled by the **right sample's pitch generators**. citeturn324175view1

   I would resolve stereo pairs when constructing runtime regions rather than allowing them to turn into two unrelated voices.

9. **`exclusiveClass` matters for drums.** This is how things such as closed hi-hat choking an open hi-hat are represented. Starting a voice should rapidly terminate another voice in the same preset with the same nonzero exclusive class. citeturn617801view5turn324175view3

   Ignore it and drum kits immediately feel wrong.

10. **24-bit SF2 is slightly unusual.** In SF2 2.04, the normal `smpl` chunk still contains the signed upper 16 bits. `sm24` supplies an additional low byte for each sample point. So you're reconstructing something approximately like:

```cpp
int32_t sample24 =
    (int32_t(sample16) << 8) |
    uint8_t(sample8);
```

   You must ignore `sm24` for pre-2.04 files or if its size isn't consistent with `smpl`. citeturn617801view6turn799537view0

   That makes adding 2.04 after basic 16-bit SF2 support fairly easy.

11. **SF3 deserves a separate loader path.** With SF3, compressed streams are individually encoded; they may coexist with PCM, and compressed sample start/end positions are handled differently from ordinary SF2 sample-word offsets. FluidSynth, for example, distinguishes byte positions for compressed sample data from sample-point positions for ordinary SF2 data. citeturn714544search0turn714544search1

   I'd decode an SF3 sample completely into your normal runtime PCM representation during loading. Don't let Ogg/SF3 concerns leak into the oscillator.

12. **Do not load, decompress, allocate, or free banks in Rack's audio processing path.** Rack calls `Module::process()` once per engine frame, and its engine sample rate is available through `ProcessArgs`. citeturn657970search2turn218979search2

   A good architecture is:

```text
UI / loader thread
    ↓
read SF2/SF3
    ↓
validate
    ↓
decode samples
    ↓
compile preset into immutable Regions
    ↓
atomic/shared handoff
    ↓
audio thread

audio thread
    voice lookup
    oscillators
    envelopes
    filters
    mix
```

   In particular, bank replacement needs lifetime management: an old sample buffer cannot disappear while a release tail is still referencing it.

13. **VCV polyphony and SoundFont polyphony are different concepts.** Rack cables support up to 16 polyphonic channels. citeturn218979search0 One Rack poly channel might correspond to one note, but one SoundFont note can spawn **multiple sample voices** because overlapping zones/layers can all match the same note/velocity.

   So don't make:

```text
Rack channel == sampler voice
```

   Instead make:

```text
Rack channel
    → note event
        → 1..N SF2 voices
```

   Then have a global internal voice pool, perhaps 64–256 voices.

14. **Velocity needs a product decision in a modular environment.** MIDI gives SF2 velocity naturally; VCV PITCH/GATE doesn't. I'd give the module a `VEL` polyphonic input and use 10 V → velocity 127, with a configurable/default velocity when unpatched.

   This becomes especially important because both velocity ranges and velocity-derived modulation are common in piano/orchestral SoundFonts.

15. **Interpolation quality becomes audible very quickly.** SoundFont permits significant transposition. Linear interpolation is cheap, but transposing samples upward can produce obvious aliasing. Cubic interpolation is a good initial compromise. If this is intended as a high-quality sampler, a bandlimited interpolator/resampler is eventually worthwhile.

   Also handle runtime Rack sample-rate changes. Rack's engine rate is independent of the audio device rate and can differ from it. citeturn657970search11

16. **Real-world SF2 files are often mildly broken.** Bad loop points, strange sample links, invalid ranges and specification violations exist. A perfectly strict implementation can therefore reject banks that established synths successfully play. FluidSynth intentionally takes a permissive approach to several malformed-loop cases. citeturn657970search13

   I'd distinguish between `fatal structural corruption` and `recoverable bad metadata`, and log the latter rather than refusing the whole bank.

One important testing caveat: **don't treat FluidSynth output as the specification itself**. It is probably your best practical reference implementation, but its own documentation identifies places where it intentionally diverges—for example around invalid loops, linked modulators and one stereo-pitch rule. citeturn657970search13 Use the SF2 2.04 specification as the normative reference and FluidSynth as your compatibility reference.

### If I were building the VCV module

I'd implement it in four passes.

**Pass 1** would be SF2 2.01/2.04, 16-bit samples, correctly flattened preset/instrument zones, key/velocity selection, root tuning, coarse/fine tuning, volume envelope, pan, loop modes and polyphonic voice management. No modulators yet.

**Pass 2** would add the SF2 filter, modulation envelope, LFOs, exclusive classes and the default/modulator system. At this point most banks should sound recognizably like they do in FluidSynth.

**Pass 3** would add `sm24`, stereo-link correctness and more sophisticated resampling. 24-bit support itself is actually much easier than the articulation system.

**Pass 4** would add SF3 by decoding compressed samples into the exact same internal sample representation. That keeps the synth engine completely unaware of whether the original file was SF2 or SF3.

There is also a build-vs-library decision worth making early. **FluidSynth** already implements SF2/SF3 and is LGPL; integrating it gets you broad compatibility quickly, although its MIDI-centric model is less natural for audio-rate VCV CV semantics. citeturn477463search3 **TinySoundFont** is MIT and exceptionally easy to embed, but currently explicitly lacks SoundFont modulators and chorus/reverb generators, so it is closer to a good lightweight starting point than a conformance reference. citeturn348883search5

For a module where **CV control is the interesting part**, I'd lean toward your own small playback engine and use FluidSynth for differential testing. That lets you expose things like continuous pitch, filter modulation, envelope scaling, velocity, preset selection and perhaps separate reverb/chorus send outputs in a way that feels native to Rack instead of wrapping a MIDI synth.

[SoundFont 2.04 technical specification](https://musescore.org/sites/musescore.org/files/2023-01/sfspec24.pdf?utm_source=chatgpt.com)  
[VCV Rack Plugin API guide](https://vcvrack.com/manual/PluginGuide?utm_source=chatgpt.com)  
[FluidSynth repository and implementation](https://github.com/FluidSynth/fluidsynth?utm_source=chatgpt.com)

---

## User

If we only focus on sfz, is it easier? Are there free libraries in c++/c that can help with it?

---

## Assistant

Yes. **SFZ is substantially easier than SF2 if you constrain the supported feature set.** The file is text-based, sample files are external, and the inheritance model is relatively straightforward: `<global>` → `<group>` → `<region>`, with more-specific values overriding broader ones. citeturn259841search3turn259841search5turn259841search6

There is one important qualification: SFZ does not have a clean, universally implemented “SFZ 2.x specification.” SFZ v1 is reasonably well defined, while v2 and ARIA extensions are much fuzzier. The SFZ Format documentation itself notes that v2 was never clearly standardized. citeturn259841search10

So I would define your module as something like **“SFZ v1 + selected common extensions”**, rather than promising arbitrary SFZ compatibility.

### C/C++ libraries

For a VCV Rack module, I would rank the options roughly like this:

| Library | License | My take |
|---|---|---|
| **sfizz** | BSD-2-Clause | **Best overall choice** |
| **liquidsfz** | MPL-2.0 | Good alternative, particularly for large/streamed instruments |
| **SFZPlayer from sfzq** | MIT | Smaller/simple codebase; interesting if you want to modify the engine heavily |
| **SFZero** | MIT | Older; its own author directs developers toward sfzq |

**sfizz is probably the obvious starting point.** It is specifically a C++ SFZ parser and synthesis library, has both C and C++ APIs, and is intended to be embedded in third-party applications. Its core library is BSD-2-Clause, which is convenient for a Rack plugin. citeturn259841search0turn259841search1turn259841search11

It already gives you the important API shape:

```cpp
sfizz_synth_t* synth = sfizz_create_synth();

sfizz_set_sample_rate(synth, 48000);
sfizz_set_samples_per_block(synth, blockSize);

sfizz_load_file(synth, "/samples/piano.sfz");

sfizz_send_note_on(synth, 0, 60, 100);

// ...
sfizz_render_block(synth, outputs, 2, blockSize);

sfizz_send_note_off(synth, 0, 60, 0);
```

It also exposes high-precision velocity and pitch-wheel APIs, which are useful when translating Rack CV rather than MIDI directly. citeturn529638view0turn529638view1turn529638view2

So with sfizz you aren't really writing an SFZ player anymore. You're mostly writing:

```text
VCV UI
    ↓
PITCH / GATE / VEL / CV
    ↓
Rack → sampler event translation
    ↓
sfizz
    ↓
audio
```

That is a dramatically smaller project.

**liquidsfz** is also worth looking at. It is explicitly designed as an embeddable SFZ sampler library. The current 0.4.1 release added a newer permissive parser, and it supports demand-loading/streaming of samples rather than requiring everything in RAM. Its license is MPL-2.0 rather than BSD. citeturn151083search0turn151083search5

Its API is also pleasantly small:

```cpp
LiquidSFZ::Synth synth;

synth.set_sample_rate(48000);
synth.load("piano.sfz");

synth.add_event_note_on(0, 0, 60, 100);

float* outputs[2] = { left, right };
synth.process(outputs, 1024);
```

The documented embedding interface is essentially that simple. citeturn151083search8

### If you write your own SFZ implementation

A **minimal useful SFZ sampler** is much easier than SF2.

I'd initially support:

```text
<global>
<group>
<region>

sample=
key=
lokey=
hikey=
lovel=
hivel=

pitch_keycenter=
tune=
transpose=

volume=
pan=

offset=
end=

loop_mode=
loop_start=
loop_end=

ampeg_attack=
ampeg_decay=
ampeg_sustain=
ampeg_release=

seq_length=
seq_position=

group=
off_by=
```

That already gets you a surprisingly useful sampler.

Internally, you can flatten everything at load time:

```cpp
struct Region {
    std::string sample;

    int loKey = 0;
    int hiKey = 127;

    int loVel = 0;
    int hiVel = 127;

    int rootKey = 60;

    float tune = 0;
    float volume = 0;
    float pan = 0;

    Envelope envelope;

    Loop loop;

    // ...
};
```

Then:

```text
parse global
    ↓
parse group
    ↓
parse region
    ↓
copy inherited values
    ↓
produce completely resolved Region
```

This is considerably nicer than SF2's indexed RIFF structures.

### Where SFZ starts becoming complicated

The complexity curve looks roughly like this:

```text
samples + key ranges
████

velocity layers
█████

ADSR + loops
██████

round robin
███████

choke groups / release triggers
████████

CC modulation
████████████

keyswitches
█████████████

LFOs / filters / envelopes / curves
████████████████

ARIA extensions / full compatibility
█████████████████████████
```

The parser isn't really the difficult part.

For example, SFZ supports preprocessing:

```sfz
#define $KICK 36

#include "common.sfz"

<region>
key=$KICK
sample=kick.wav
```

`#include` can itself recursively include other files, and path interpretation has specific rules. citeturn259841search9turn259841search14

You also eventually run into:

```text
round robin
random sample selection
keyswitches
release samples
legato triggers
choke/exclusive groups
CC-based switching
CC modulation
velocity curves
multiple envelopes
filters
LFOs
crossfades
aftertouch
pitch bend
sample start modulation
```

At that point you're no longer implementing a file parser; you're implementing a fairly sophisticated sampler synthesizer.

### There is also a nice advantage over SF2

SFZ inheritance is conceptually simple.

For example:

```sfz
<global>
ampeg_release=0.4

<group>
lovel=0 hivel=63

<region>
sample=piano_C4_soft.wav
key=60

<group>
lovel=64 hivel=127

<region>
sample=piano_C4_loud.wav
key=60
```

You essentially compile it into:

```text
Region 1
    sample = piano_C4_soft.wav
    key = 60
    velocity = 0..63
    release = 0.4

Region 2
    sample = piano_C4_loud.wav
    key = 60
    velocity = 64..127
    release = 0.4
```

The SFZ documentation actually describes globals and groups as effectively macro/inheritance mechanisms that players can flatten into regions. citeturn259841search8

That is a very natural runtime representation for a VCV module.

## The main VCV-specific issue with sfizz

There is one architectural mismatch worth thinking about carefully.

Rack gives you:

```text
PITCH CV: continuous
GATE: continuous
VEL CV: continuous
```

sfizz fundamentally expects note/event semantics:

```text
note-on(note, velocity)
note-off(note)
pitch wheel
CC
...
```

The obvious mapping is:

```cpp
float midiPitch = 60.0f + pitchVoltage * 12.0f;

int note = std::round(midiPitch);

float deviation = midiPitch - note;
```

On gate rising:

```cpp
noteOn(note, velocity);
```

Then use high-resolution pitch bend for:

```text
deviation + subsequent PITCH CV movement
```

sfizz exposes a normalized high-precision pitch-wheel input specifically for this kind of continuous control. citeturn529638view1

The catch is **Rack polyphony**.

Suppose your cable contains:

```text
channel 1: C4 + 0 cents
channel 2: E4 + 13 cents
channel 3: G4 - 7 cents
```

You potentially need independent continuous pitch control for every voice. A traditional MIDI-channel-oriented sampler engine isn't automatically equivalent to Rack's independently pitched poly channels.

This is probably the first thing I'd prototype before committing to an external engine.

### Another VCV/sfizz issue: block size

sfizz is designed around:

```cpp
send events for block
render block
```

Its event calls even accept a sample offset within the upcoming render block. citeturn529638view0

Rack normally calls your module's:

```cpp
process(const ProcessArgs& args)
```

one frame at a time.

You can probably initially configure sfizz for a **1-sample block**:

```cpp
sfizz_set_samples_per_block(synth, 1);
```

and render one frame per Rack `process()` call. The API supports configuring the expected block size. citeturn529638view3

I'd benchmark that early.

If that turns out expensive, you could internally render e.g. 16/32/64 frames at a time, but then you have to solve event buffering and potentially introduce latency.

### Loading is still something I'd isolate

Don't do:

```cpp
process() {
    if (loadRequested)
        sfizz_load_file(...);  // bad
}
```

SFZ loading means filesystem operations, parsing, sample loading and potentially memory allocation.

Use something closer to:

```text
UI
 ↓
request "load piano.sfz"
 ↓
worker thread
 ↓
create/load sampler
 ↓
atomic swap
 ↓
audio thread uses new sampler
```

That problem exists regardless of whether you use SF2 or SFZ.

## What I would do

For your particular VCV module, I'd seriously consider two paths.

**Fastest route:** embed **sfizz** and make the project primarily about good Rack integration. You immediately get a large amount of SFZ compatibility, sample playback, envelopes, filtering, round robins, etc. The permissive BSD license is attractive for this. citeturn259841search0turn259841search1

**More interesting DSP/project route:** write a small SFZ v1 engine yourself. Implement maybe 20–30 high-value opcodes and explicitly state that it supports an SFZ subset. That engine might actually be quite compact, especially if you use `dr_wav`/`dr_flac` or another library for sample decoding.

I would **not** spend time writing a standalone general-purpose SFZ parser and then separately write the sampler unless writing the sampler is itself the objective. sfizz already solves too much of that problem well.

And compared with the SF2 approach we discussed: **if your goal is simply “load multisampled instruments into VCV Rack,” I would choose SFZ without hesitation.** The format maps much more naturally onto a small sampler engine, and users can inspect/edit/create instruments with a text editor.

---

## User

Can ou check which features [Squinky Labs](https://library.vcvrack.com/?brand=Squinky%20Labs) [SFZ Player](https://library.vcvrack.com/squinkylabs-plug1/squinkylabs-samp) supports?

---

## Assistant

Yes. I checked the current Rack Library entry, Squinky's compatibility documentation, and its changelog. The Rack Library currently lists **SFZ Player 2.1.9**. The compatibility page is quite explicit about what the sampler does and does not implement, and I don't see later 2.x changelog entries adding materially broader SFZ support. citeturn619828view0turn386764view0turn586667view0

### SFZ support in Squinky Labs SFZ Player

| Area | Support | Details |
|---|---|---|
| Sample mapping | ✅ | `sample`, `key`, `lokey`, `hikey`, `pitch_keycenter` |
| Velocity layers | ✅ | `lovel`, `hivel` |
| Velocity → amplitude | ✅ | `amp_veltrack` |
| Release envelope | ✅ Limited | `ampeg_release` only; not a general SFZ ADSR implementation |
| Tuning | ✅ | `tune` |
| Region volume | ✅ | `volume` |
| Sample path handling | ✅ | `default_path`, relative and absolute sample paths |
| Includes | ✅ | `#include`, including nested includes |
| Round robin | ✅ | `seq_length`, `seq_position` |
| Random selection | ✅ | `lorand`, `hirand` |
| Keyswitching | ✅ | `sw_label`, `sw_last`, `sw_lokey`, `sw_hikey`, `sw_default` |
| Sample offset/end | ✅ | `offset`, `end` |
| One-shot samples | ✅ | `loop_mode=one_shot` |
| Continuous looping | ✅ | `loop_mode=continuous` |
| Sustain looping | ✅ | `loop_mode=sustain` |
| Loop points | ✅ | `loop_start` / `loop_end` and aliases |
| Wavetable-style oscillator | ✅ | `oscillator` is documented as allowing a sample to behave as an oscillator |
| WAV samples | ✅ | Supported |
| FLAC samples | ✅ | Supported |
| OGG samples | ❌ | Not supported |
| AIFF samples | ❌ | Not supported |
| Stereo sample playback | ⚠️ | Stereo input files are **converted to mono** |
| Multiple simultaneous regions per note | ❌ | **Only one sample can play per voice** |
| Release-trigger samples | ❌ | Explicitly ignored |
| CC/controller switching | ❌ | No continuous-controller support |
| SFZ modulation | ❌ | No SFZ-defined modulation |
| Filters | ❌ | No SFZ subtractive-synthesis/filter implementation |
| General envelopes/LFOs | ❌ | No full SFZ modulation/envelope engine |
| ARIA extensions | ❌ | Not supported, including ARIA's sample-location scheme |
| Disk streaming | ❌ | All samples loaded into RAM |

The opcode list above comes directly from Squinky's compatibility documentation. citeturn386764view0

### The biggest architectural limitation

The most consequential limitation is this:

> **one sample per voice**

SFZ ordinarily allows several matching `<region>` elements to fire for one note. Squinky explicitly doesn't do that. When multiple regions would play simultaneously, its loader tries to decide which one is more important and discards the others. citeturn386764view0

So an instrument like:

```sfz
<region> sample=piano.wav key=60
<region> sample=hammer_noise.wav key=60
<region> sample=resonance.wav key=60
```

might conventionally produce:

```text
piano
+ hammer noise
+ resonance
```

Squinky fundamentally reduces this toward:

```text
one selected sample
```

This explains why straightforward multisampled instruments work well while sophisticated Pianobook-style instruments can fail badly. Their documentation specifically warns about elaborate SFZ libraries for this reason. citeturn386764view0

Velocity layering isn't affected when the layers are mutually exclusive:

```sfz
<region> sample=soft.wav  lovel=1  hivel=50
<region> sample=med.wav   lovel=51 hivel=90
<region> sample=hard.wav  lovel=91 hivel=127
```

Only one region matches a given velocity anyway, so that model works perfectly well.

Likewise round-robin/random selection is explicitly supported and was considered important for drum libraries. citeturn386764view0

### Envelope implementation is extremely small

An interesting detail for your own implementation is that Squinky didn't even implement full amplitude ADSR. The documented envelope-related SFZ opcode is essentially just:

```sfz
ampeg_release=
```

plus:

```sfz
amp_veltrack=
```

They deliberately left the rest of SFZ's synthesis/modulation system out. citeturn386764view0

That's a useful data point: you can apparently get good compatibility with a lot of free SFZ libraries without implementing:

```text
ampeg_attack
ampeg_hold
ampeg_decay
ampeg_sustain

filter
filter envelope
pitch envelope

LFO
CC modulation
aftertouch modulation
...
```

Of course you'll lose instruments that depend on those.

### Sample handling is also deliberately simple

They load **everything into RAM**, decode it and convert it to 32-bit floating point. Stereo samples are converted to mono. There's no disk streaming. Their manual points out that this can make very large SFZ libraries consume gigabytes of memory. citeturn371054view0

Architecturally, that's attractive for Rack:

```text
SFZ file
   ↓
parse
   ↓
load all WAV/FLAC
   ↓
convert to normalized in-memory PCM
   ↓
audio thread has no filesystem dependency
```

It's much simpler than implementing Kontakt/sfizz-style streaming.

### Rack-specific features are actually richer than its SFZ support

The module itself provides:

```text
polyphonic V/Oct
polyphonic Gate
polyphonic Velocity

exponential FM
through-zero linear FM / phase modulation
CV-controlled linear-FM depth

pitch control
octave control
master volume
```

Its V/Oct input's channel count determines the player's Rack polyphony. Velocity maps 0–10 V to MIDI velocity 0–127. citeturn371054view0

The through-zero LFM feature is particularly interesting because **it isn't SFZ functionality at all**. It's something Squinky added to the sample playback oscillator. So they took the approach:

```text
small SFZ compatibility layer
             +
interesting modular sample oscillator
```

rather than:

```text
complete SFZ software sampler
wrapped in VCV
```

That seems like a very sensible precedent for what you're considering.

### One more subtle feature

They have an optional **5-sample Gate delay**. Their changelog explains why: it helps avoid sampling the old V/Oct value when Gate and pitch CV change effectively at the same moment. citeturn371054view0turn586667view0

That's a very VCV-specific gotcha worth stealing conceptually:

```text
sequencer frame N:
    Gate rises
    Pitch may still contain previous note

a few samples later:
    Pitch CV has stabilized
    → determine sample/note
```

You probably don't necessarily need five samples specifically, but handling simultaneous polyphonic PITCH/GATE changes deserves explicit thought.

### What I'd take from Squinky for your implementation

Squinky demonstrates that an SFZ implementation can be **much smaller than sfizz** and still be genuinely useful.

A Squinky-plus implementation target could be approximately:

```text
Parsing/hierarchy
    <control>
    <global>
    <master>
    <group>
    <region>
    #include
    default_path

Mapping
    sample
    key / lokey / hikey
    pitch_keycenter
    lovel / hivel

Pitch/amplitude
    tune
    volume
    amp_veltrack
    ampeg_release

Selection
    round robin
    random
    keyswitches

Playback
    offset / end
    one-shot
    continuous loop
    sustain loop
    loop points

Samples
    WAV
    FLAC
```

Then I would make **one deliberate improvement over Squinky immediately: allow multiple matching regions to spawn independent internal voices**.

That buys you release noises, layered attacks, resonance layers, combined articulations and many modern SFZ instruments without requiring any complicated new parser behavior. The conceptual engine becomes:

```text
Rack channel
     │
     └── note-on
          │
          ├── matching region → SampleVoice
          ├── matching region → SampleVoice
          └── matching region → SampleVoice
```

rather than:

```text
Rack channel
     │
     └── exactly one SampleVoice
```

I think **that + Squinky's opcode set would be a very strong MVP**. It is dramatically smaller than implementing SFZ broadly, while removing what is probably Squinky Player's biggest compatibility limitation.

One licensing note if you're studying their implementation rather than only its behavior: the Rack Library lists the Squinky Labs plugin as **GPL-3.0-only**. citeturn619828view0

[Squinky SFZ compatibility documentation](https://github.com/kockie69/SquinkyVCV-main/blob/master/docs/sfz-player-compatibility.md?utm_source=chatgpt.com)  
[Squinky SFZ Player manual](https://github.com/kockie69/SquinkyVCV-main/blob/master/docs/sfz-player.md?utm_source=chatgpt.com)  
[SFZ Player Rack Library entry](https://library.vcvrack.com/squinkylabs-plug1/squinkylabs-samp?utm_source=chatgpt.com)

---

## User

But we can still use sfizz? 
What else can we improve over sfz player?

---

## Assistant

Yes. **Technically and legally, sfizz is still usable.** It exposes embeddable C/C++ APIs and is BSD-2-Clause. The important change is that the upstream repository was archived on **June 21, 2026**, so I would vendor/pin a known version rather than depend on future upstream maintenance. citeturn850612search0turn850612search12turn134534search6

For a new Rack module I'd actually put a thin abstraction around it:

```cpp
class SfzEngine {
public:
    virtual bool load(std::string_view path) = 0;
    virtual void noteOn(int channel, int note, float velocity) = 0;
    virtual void noteOff(int channel, int note) = 0;
    virtual void setPitch(int channel, float semitones) = 0;
    virtual void render(float* left, float* right, int frames) = 0;
};
```

Then you can start with sfizz without coupling the whole module to it.

One especially interesting alternative is **sfizioso**, a current independent BSD-2-Clause fork of sfizz. It specifically adds MPE/per-channel pitch, CC and aftertouch routing. That's highly relevant for Rack because Rack poly channels naturally want independently modulated pitches. citeturn134534search0turn134534search1

I would evaluate it, but not automatically commit to it yet; it's a much younger fork.

## Where we could beat Squinky

I'd rank the improvements like this:

| Improvement | Value | Difficulty with sfizz |
|---|---:|---:|
| True stereo samples | ★★★★★ | Low |
| Multiple simultaneous regions/layers | ★★★★★ | Already engine-supported |
| Release samples | ★★★★★ | Already engine-supported |
| Much broader SFZ compatibility | ★★★★★ | Already engine-supported |
| Independent polyphonic V/Oct modulation | ★★★★★ | Medium/high |
| Disk/preload sample handling | ★★★★☆ | Mostly available |
| SFZ filters/envelopes/LFOs | ★★★★☆ | Already engine-supported |
| CC → CV modulation | ★★★★☆ | Mostly wrapper/UI work |
| Keyswitch control | ★★★★☆ | Mostly wrapper/UI work |
| Multiple SFZ outputs | ★★★☆☆ | Engine-supported |
| Patch browser / metadata | ★★★☆☆ | Wrapper/UI |
| Reload-on-file-change | ★★★☆☆ | Wrapper |
| Microtonal continuous pitch | ★★★★☆ | Depends on backend |

### 1. Preserve stereo

Squinky converts loaded samples to mono. citeturn341208search1

We should simply do:

```text
SFZ stereo sample
      ↓
sfizz
      ↓
LEFT OUT
RIGHT OUT
```

This alone matters enormously for sampled pianos, orchestral sections, room mics and pads.

I'd make the module native stereo from day one.

---

### 2. Proper layered regions

This is probably the largest compatibility improvement.

Instead of Squinky's simplified one-sample-per-note approach:

```text
note C4
   ↓
sample A
```

we get normal SFZ semantics:

```text
note C4
   │
   ├── sustain L
   ├── sustain R
   ├── hammer noise
   └── resonance layer
```

You don't have to implement that yourself if sfizz is the synthesis engine. Its engine maintains a configurable pool of internal voices; the sfizz codebase currently defaults to 64 voices and supports configuration up to 256. citeturn134534search9

This means substantially more real-world SFZ files should behave correctly.

---

### 3. Release-trigger samples

For example:

```sfz
<region>
sample=piano.wav
trigger=attack

<region>
sample=piano_release.wav
trigger=release
```

This is important for:

- pianos
- harpsichords
- guitars
- orchestral samples
- mechanical noises
- room/reverb tails

It's one of those things that doesn't look essential on paper but makes high-quality sample instruments feel substantially more convincing.

sfizz has substantial release-region handling already; current development even included refinements around timer behavior for release regions. citeturn655016search8

---

### 4. Full SFZ articulation rather than only mapping

sfizz claimed virtually complete SFZ v1 support by its 1.0 release and subsequently added more features such as LFOs, extended CCs, envelope modulation, loop-count behavior and other extensions. citeturn850612search3turn850612search10

So we inherit things such as:

```text
ADSR
filters
LFOs
velocity crossfades
round robin
random regions
release regions
choke groups
keyswitches
CC modulation
pitch bend
aftertouch
```

Instead of implementing a sampler synthesizer ourselves.

That changes the nature of our project from:

> SFZ implementation

to:

> Rack-native interface to a mature SFZ engine.

That's a much better scope.

---

## 5. The killer Rack feature: polyphonic CV modulation

This is where I'd spend engineering effort.

Imagine:

```text
V/OCT    16 poly
GATE     16 poly
VEL      16 poly
PRESSURE 16 poly
TIMBRE   16 poly
```

Each voice should behave independently.

For example:

```text
Rack ch 0: C4 + 0.13 semitones
Rack ch 1: E4 - 0.07 semitones
Rack ch 2: G4 + 0.42 semitones
```

Plain sfizz has a problem here: its pitch-wheel/CC state is essentially global rather than independently MIDI-channel scoped. MPE support was an open feature request before upstream was archived. citeturn850612search11

This is exactly what **sfizioso** addresses with per-channel pitch/CC/aftertouch state. citeturn134534search0

So one potential mapping becomes:

```text
Rack poly ch 0 → MPE member channel 1
Rack poly ch 1 → MPE member channel 2
Rack poly ch 2 → MPE member channel 3
...
Rack poly ch 14 → MPE member channel 15
```

That is almost suspiciously well matched to VCV's 16-channel polyphony.

Then V/OCT becomes:

```cpp
float noteF = 60.0f + volts * 12.0f;

int baseNote = std::round(noteF);
float bend = noteF - baseNote;
```

and:

```text
baseNote → note event
bend     → per-channel continuous pitch
```

Any later pitch CV movements just modify that voice's pitch.

**This would be a significant improvement over Squinky.**

---

## 6. Make arbitrary SFZ CCs accessible as CV

This could be a very Rack-native feature.

SFZ instruments often contain:

```sfz
cutoff_cc1=...
volume_cc11=...
ampeg_attack_cc20=...
```

Instead of hardcoding every possibility into the panel, give perhaps four assignable modulation inputs:

```text
CV1
CV2
CV3
CV4
```

with context-menu assignments:

```text
CV1 → MIDI CC1
CV2 → MIDI CC11
CV3 → MIDI CC74
CV4 → Aftertouch
```

And perhaps:

```text
CV1 CC: [ 1 ]
CV2 CC: [11 ]
CV3 CC: [74 ]
CV4 CC: [ 2 ]
```

Now existing SFZ programming becomes modular.

That would let someone patch:

```text
LFO → CV1 → SFZ CC1
Envelope → CV2 → SFZ CC74
Random → CV3 → SFZ CC21
```

without modifying the instrument.

I think this would be one of the module's strongest differentiators.

---

## 7. Expose keyswitches properly

Instead of forcing users to somehow generate MIDI notes outside the playable range, provide:

```text
ART CV
```

or:

```text
KS CV
```

Possible mode:

```text
0V     = first keyswitch
1V     = second
2V     = third
...
```

Or a knob plus CV:

```text
ARTICULATION
  Sustain
  Staccato
  Pizzicato
  Tremolo
```

sfizz can introspect currently loaded key/key-switch information through its API/OSC-facing facilities; its UI gained explicit displays for keyswitch ranges and active keyswitches. citeturn341208search4

A Rack-native articulation interface would be much more useful than a generic MIDI-oriented one.

---

## 8. Don't load every 8 GB piano completely into RAM

Squinky intentionally loads all sample data into memory. citeturn341208search1

sfizz has a configurable **preload system**; its API exposes preload size, and changing that setting is explicitly documented as something that should be done outside the real-time thread. citeturn655016search0

So we'd potentially offer:

```text
Sample loading
○ Stream / preload
○ Load fully into RAM
```

or simply choose sensible behavior automatically.

That allows significantly larger libraries without consuming absurd amounts of RAM.

---

## 9. Multiple outputs

sfizz added the SFZ `output=` opcode and support for multiple stereo outputs; its VST/LV2 implementations provided as many as eight stereo output pairs. citeturn655016search1

Rack is particularly well suited to this.

For version 1:

```text
L
R
```

Later:

```text
MAIN L/R
AUX 1 L/R
AUX 2 L/R
AUX 3 L/R
```

Or use an expander module:

```text
SFZ Player
    │
    └── SFZ Outputs expander
          OUT 1 L/R
          OUT 2 L/R
          OUT 3 L/R
          ...
```

This is useful for drum kits:

```sfz
kick   → output 1
snare  → output 2
hihat  → output 3
rooms  → output 4
```

Then users can patch each through different Rack effects.

That's a very natural modular interpretation of SFZ.

---

## 10. Better file management

This isn't DSP, but it matters.

I'd add:

```text
SFZ filename
instrument name

Previous SFZ
Next SFZ

Reload
Reveal path

Recently used
Favorites
```

Plus **portable patch path handling**.

The Rack patch should remember enough information to recover:

```text
/My Samples/Piano/Piano.sfz
```

without embedding sample data.

And when loading fails:

```text
Piano.sfz
Missing:
Samples/Piano/C4_vel4.wav
```

rather than merely:

```text
Could not load instrument
```

For SFZ debugging, this is disproportionately useful.

---

## 11. Hot reload

SFZ being text makes this particularly attractive:

```text
edit piano.sfz
save
      ↓
Rack notices timestamp change
      ↓
reload instrument
```

Optionally:

```text
AUTO RELOAD ✓
```

This would make the module not only a player but also an excellent **SFZ authoring environment**.

That could be genuinely useful to people building sample libraries.

---

## 12. Expose SFZ diagnostics

sfizz knows quite a lot after parsing.

We could show:

```text
Piano.sfz

Regions       384
Samples       126
Voices        17 / 128
Memory        218 MB
Key range     A0–C8
Keyswitches   C0–F0
```

And possibly:

```text
Current note C4

Matched regions:
  piano_C4_v3.wav
  hammer_C4.wav
  resonance_C4.wav
```

That last feature would be extremely useful for debugging complex SFZs.

---

# The design I'd choose

Something like a **12–14 HP module**, not a huge sampler GUI:

```text
┌─────────────────────────┐
│ Piano in 162            │
│ C1───────────────C8     │
│                         │
│ PITCH        FINE       │
│   ○            ○        │
│                         │
│ V/OCT    ●              │
│ GATE     ●              │
│ VEL      ●              │
│                         │
│ CV1      ●  CC  1       │
│ CV2      ●  CC 11       │
│ CV3      ●  CC 74       │
│                         │
│ ART      ●              │
│                         │
│           L ●    ● R    │
└─────────────────────────┘
```

Keep the panel simple. Put configuration in the context menu/display.

Underneath:

```text
             Rack module
                  │
        ┌─────────┴─────────┐
        │ CV/event adapter  │
        └─────────┬─────────┘
                  │
          sfizz/sfizioso
                  │
        ┌─────────┴─────────┐
        │ SFZ voice engine  │
        │ sample streaming  │
        │ EG / LFO / filter │
        │ region selection  │
        └─────────┬─────────┘
                  │
               stereo
```

### One technical item I'd prototype first

Before building UI, I'd make a command-line/Rack test module that proves just four things:

1. load a large SFZ,
2. render stereo reliably inside Rack,
3. play all 16 Rack poly channels,
4. continuously bend those **16 channels independently**.

If #4 works cleanly with sfizioso, I'd strongly favor it. If not, I'd pin **sfizz 1.2.3** and decide whether independent-pitch handling warrants a small engine patch.

The result could be substantially more capable than Squinky **without writing an SFZ engine at all**. Most of the project becomes Rack integration—which is exactly where the useful differentiation is.


