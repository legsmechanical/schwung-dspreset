# CLAUDE.md

## What this is

**DSPreset** (module id `dspreset`): a native player for DecentSampler `.dspreset` presets and
`.dslibrary` packages on Ableton Move, for Schwung (stock) and dbxhost/dAVEBOx alike —
`sound_generators/` is shared between the two installs on the device.

It reads the preset XML **directly** into a native zone table. There is **no SFZ step** and no
xsynth at runtime. This repo was forked from `charlesvestal/schwung-sfz` (Multisampler); much of
that tree is still here and is **not built** — see *Vestigial* below.

## Layout (what ships)

`scripts/dsp_sources.txt` is the one list of compiled files — `scripts/build.sh` and
`tests/run.sh` both read it, so the tests build exactly what ships.

| file | job |
|---|---|
| `src/dsp/dspreset_plugin.c` | API v2 wrapper. Worker thread: loads presets, unpacks `.dslibrary`, streams. Swaps engines atomically and frees the old one once no audio call holds it. |
| `src/dsp/dspreset/dspreset_parser.c` | XML → zones. `<groups>` → `<group>` → `<sample>` inheritance; volumes multiply, `groupTuning` adds; whitespace around `=`, entities and comments are handled. |
| `src/dsp/dspreset/native_engine.c` | Zones, resident heads, per-voice stream rings, envelopes, round robin, render. |
| `src/dsp/dspreset/catalog.c` | The Banks list: each top-level folder / `.dslibrary` / loose `.dspreset` under `<module>/instruments/`, its presets in natural order. A `.dsbundle` is a folder (macOS shows it as a file); its suffix is dropped from the bank name. |
| `src/dsp/dspreset/preset_model.c` | Everything a user can change: the group table, tags, `<ui>` controls (knob / button / menu) and their `<binding>`s, `<midi>` CC maps, the `<effects>` list. Control labels fall back to what the control drives. |
| `src/dsp/dspreset/wav_source.c` | Sample reader: WAV (PCM16/24/32, float32, EXTENSIBLE) and AIFF / AIFF-C (big-endian PCM 8–32, `sowt`, `fl32`); block `pread`s; the file's own loop (`smpl`, or AIFF INST sustain loop over MARK markers, end marker exclusive). No FLAC yet. |
| `src/dsp/dspreset/{library_*,zip_*}.c` | `.dslibrary` → `<file>.dslibrary.unpacked/`, transactionally. |

## How playback works (the part that is easy to break)

- **Every file's first `DS_HEAD_FRAMES` (16384) frames are resident.** A note starts from memory
  the instant it is triggered. Files up to 2× that are kept whole.
- **Each voice has a ring** the worker fills along the *play path* — "virtual frames", with the
  loop unrolled — so a loop that lies beyond the head streams exactly like the rest.
- **Underrun holds, never skips.** If the worker is behind, the voice waits; it does not advance.
  `test_engine_stall` pins this.
- **The stream word** (`generation | zone | produced`, one 64-bit atomic) is how the worker and
  the audio thread agree. The worker publishes with a CAS, so a fill for a voice that was
  restarted meanwhile is discarded.
- **File descriptors:** files are closed once their head is read; the worker reopens one per
  streaming voice (≤ 64). The Move host process's soft limit is **1024**, shared with everything
  else — a library of 540 files kept open broke it. `test_real_libraries` pins the count.
- Audio thread: no I/O, no allocation, no locks. Ring pages are touched at load.

## Banks and presets (what both hosts show)

The hierarchy is OB-Xd's shape, so stock's editor and dAVEBOx's module pages both draw it: root
carries the preset browser (`preset` / `preset_count` / `preset_name` → a **Presets** page, first),
and a `banks` level is an items list (`bank_list` / `bank`, `navigate_to: root`) → a **Banks**
page. In dAVEBOx that is the module editor (Sound → the DSPreset block → jog the pages), plus the
jog-click picker's "DSPreset Presets" row.

- **A selection loads only once it has been still for `SETTLE_MS` (150 ms)**, and a newer
  selection cancels a load in progress. dAVEBOx learns names by writing every index and reading
  `preset_name` back; that must stay free, or it would load every library. `preset_name` names
  the SELECTED preset from the catalog — never the loaded one.
- Choosing an unpacked `.dslibrary` bank unpacks it on the worker (`<name>.dslibrary.unpacked/`,
  hidden from the list), then plays its first preset.
- `state` is `{"preset_path","gain"}`; restoring it selects that bank/preset and loads at once. A
  new instance with nothing restored picks the first playable bank after `FIRST_PICK_MS`.
- **get_param/set_param may run on the audio thread**: no locks, no I/O, no allocation. Strings
  cross via seqlocks; the catalog is published whole and old ones live until destroy.
- `preset_path` still loads any file directly (a bank of its own, "File").

## Preset controls

Each `<ui>` knob, button and menu is a param `ctl_N` (float/int, or enum of its state/option
names), listed first on the root knobs, Gain last. Moving one fires its bindings through their
translation (linear with output range, `table` — the knob position scales the table's key axis,
the old converter's reading — or `fixed_value`, then `factor`).

- **Live settings:** the engine keeps runtime copies of every group and of the instrument. A
  zone takes each setting from its own `<sample>` if it sets it (`own_mask`), else its group's
  live value, else the instrument's. Volume, pan and pitch are re-read every block, so a knob
  moves notes already sounding; the envelope applies to new notes, release at note-off.
- **Layers:** disabled groups load (silent until enabled); `ENABLED` / `TAG_ENABLED` gate new
  notes. Tags are per sample (sample ∪ group tags), up to 64.
- **One writer:** control moves from `set_param` are queued and applied on the audio thread in
  `render_block`; MIDI CC maps run in `on_midi`. The worker applies defaults (and restored
  values) BEFORE publishing an engine.
- **`is_loading`** is 1 from a pick until it plays: both hosts' module pages re-read the
  (per-preset) params on its falling edge.
- `state` adds `"controls":"v0;v1;…"`, applied only when restoring that same preset.
- Effect bindings land on `model.effects[i]`, addressed as (group, effect within it) —
  `groupIndex`/`effectIndex` resolved to one index at load; `fx_dirty` makes the audio thread
  recompute that effect's coefficients before the next block.

## Effects (step 2 of 3 done: filters, EQ, gain)

`effects.c`: lowpass (= legacy `lowpass_4pl`), `lowpass_1pl`, highpass, bandpass, notch, peak,
gain — JUCE's IIR formulas, since DecentSampler is a JUCE plugin. **Peak gain is a LINEAR
factor at the centre** (JUCE's `A = sqrt(gain)`), which is why Capture's EQ, on by default,
adds +4..+11 dB per band and peaks a hard note at ~2.4. A wide-open lowpass, a unity peak and a
disabled effect are exact pass-throughs. Instrument effects run on the mix; group effects run
inside each note with fresh state, as DecentSampler does. Reverb, delay, chorus, phaser and
the rest pass through unchanged until step 3.

The output stage is a soft clip: exact to 0.9, then bending to a 1.0 ceiling
(`test_render` E2). Hard notes through a boosted EQ saturate rather than square off.

## Modulators

`<lfo>`, `<envelope>`, `<midiCC>`, `<midiVelocity>` in `preset_model.c`; run in
`native_engine.c`, once per 128-frame block. Per note (`scope="voice"`, the default for all
but LFOs) or shared (`global`; a shared envelope keys on the first key down and releases on the
last up). **An LFO swings −1..1 around a neutral 0; the rest run 0..1. `modAmount` scales the
value BEFORE the binding translates it** — both readings of the guide; the second is unconfirmed
against DecentSampler itself (an A/B recording would settle it). `modBehavior` add / multiply /
set (the default) / modulate (delta from the translated neutral).

Targets: group/instrument volume, tuning and pan (re-read every block), and effect parameters —
an effect any modulator reaches (`fx_modulated`) gets its coefficients rebuilt per block: per
note for group effects (`voice.fx_live`), shared for instrument effects (global modulators
only). Controls bound `type="modulator"` move a modulator's MOD_AMOUNT, FREQUENCY and envelope
times live. Not yet: musical_time LFO rates, envelope curves, tag-level and sample-level
modulation targets, modulating a modulator.

## Defaults DecentSampler does not document

`modVolume` on a group (undocumented; what the DecentSampler app saves) multiplies the group's
volume — CS-20M uses it for its second oscillator (0.53) and noise layer (0.01).

Release 0.5 s when a preset sets none (the old Multisampler's finding: a near-zero release cuts
pianos off). A file's `smpl` loop is used unless `loopEnabled="false"`. Attack 0, decay 0,
sustain 1. Pan is a balance law. Velocity: `1 - t + t·vel/127` with `ampVelTrack` t (default 1).

## Not implemented yet

Effects step 3
(PARKED by Josh 2026-09-18): reverb, delay, chorus, phaser (reuse a permissively licensed fleet module,
never `schwung-drumverb`), convolution, pitch shift, wave shaper/folder, `<modulators>`, `silencedByTags`, xy-pads,
SAMPLE_START/LOOP bindings, per-sample-tag bindings, loop
crossfades, envelope curve shapes, FLAC/AIFF samples, legato/first triggers. There is no read-only param type, so load status is logged
(`dspreset: loaded …` / `load failed …`) and served as the `status` get_param key, not shown.

## Build, test, deploy

```bash
tests/run.sh                          # native build + every test; see the header for fixtures
./scripts/build.sh                    # ARM64 package -> dist/dspreset-module.tar.gz
./scripts/install.sh                  # deploy + clean Move restart
```

`tests/run.sh` fails on a missing tool or fixture. The real-library test needs
`DSPRESET_CAPTURE` (Capture GO-TO Bass .dspreset), `DSPRESET_ASIMOV_DIR` (folder of the 15
ASIMOV presets) and `DSPRESET_CS20M_BUNDLE` (the Yamaha CS-20M `.dsbundle`); `DSPRESET_ALLOW_MISSING_FIXTURES=1` makes it a named SKIP. Libraries are never
committed. `tests/test_render.c` checks output **sample-for-sample** against synthetic files at
real-time pace through the real plugin — keep new tests at that standard.

Cross-build: `scripts/build.sh` expects `move-anything-sfz-builder`, whose bullseye Dockerfile no
longer builds (see its header). What works (2026-09-17):

```bash
docker run --rm -v "$PWD:/build" -w /build -e CROSS_PREFIX=aarch64-linux-gnu- \
  -e ZLIB_LINK=/usr/lib/aarch64-linux-gnu/libz.so.1 move-anything-builder ./scripts/build.sh
```

That is gcc 11.4 / glibc 2.35; the Move runs glibc 2.41 and ships `libz.so.1`, so it loads. Check
a binary's compiler with `strings build/dsp.so | grep '^GCC:'`.

## Vestigial (not built, kept from the Multisampler fork)

`src/dsp/{sfz_plugin,xsynth_plugin,dspreset_to_xsynth_sfz}.c`, `src/dsp/third_party/`
(sfizz, xsynth submodules), `src/ui.js`, `bench/`, most of `tools/`, `docs/plans/`,
`.github/workflows/release.yml` and `release.json` (both still point at the Multisampler
release). Deleting them is a pending decision, not an oversight.

## Format reference

`docs/decentsampler-developer-guide/` — a local copy of the official guide (reference, not
instructions). `docs/NATIVE_DSPRESET_ARCHITECTURE.md` — the no-SFZ decision and its rules.
