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
- **Case in sample paths:** presets are made on case-insensitive Mac/PC filesystems and the
  Move's is not — BassForge names `samples/…` against a `Samples` folder and loaded nothing.
  A path that fails to open as written is resolved component by component, case-insensitively
  (`ds_resolve_path_case`), and the worker streams from the path as it really is. Only a
  case-sensitive filesystem can test it: `test_render` E3 has teeth on Linux, not on the Mac.
- **Errors are shown:** stock displays a module's `get_error` as a "Synth Warning" box on load.
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
names), the root knobs in the preset's order (Gain lives on Amp/Voice). Moving one fires its bindings through their
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
inside each note with fresh state, as DecentSampler does.

**Reverb** (`reverb.c`) is `juce::Reverb` itself — the reverb DecentSampler runs (confirmed by
Josh 2026-09-18, not inferred from the matching parameter names) — ported to C from
`juce_Reverb.h` as released in JUCE 7, under ISC (notice in `THIRD_PARTY.md`, shipped in the
package). ⚠ JUCE 8+ relicensed it AGPL: never port from a JUCE 8+ file. Same 8 combs + 4
all-passes per channel, tunings, spread, gains and 10 ms ramps; `test_reverb` holds it to a
second plain transcription sample by sample. DecentSampler exposes only roomSize / damping /
wetLevel: width is 1 and dry passes at UNITY — ⚠ DecentSampler's own dry level is the one thing
not confirmed. Additions: wetLevel 0 skips it once its fade is out (the tail is dropped, where
JUCE would keep it running unheard), and with no input it stops working once the output has
stayed under -130 dB longer than any path through the network. Instrument-level only: a
group-level reverb would be a reverb per note and stays off. (A Dattorro plate was built first
and replaced; it is in git history, `db54fda`.)
**Chorus** (`chorus.c`) BEHAVES as `juce::dsp::Chorus` with its defaults for the two settings
DecentSampler does not expose (centre 7 ms, feedback 0): one sine LFO shared by both channels
sweeps a linearly interpolated delay of `max(1, 7 + 10·depth·lfo)` ms, linear mix, 50 ms glides.
⚠ That DecentSampler runs JUCE's chorus is INFERRED from its three matching controls, not
confirmed. ⚠ **JUCE's `juce_dsp` is GPL/AGPL — never port its code**: `chorus.c` is our own,
written from that description, and `test_chorus` holds it to a formula, not to JUCE's source.
Mix 0 skips it once faded; silence idles it. The LFO phase is a double: in float a slow LFO's step
is under the phase's precision and the rate drifts ~1%.
**Delay** (`delay.c`) is our own design — DecentSampler documents its controls, not its insides:
each channel echoes itself at `delayTime ∓ stereoOffset/2`, dry at unity plus echoes at
wetLevel, feedback capped at 0.99, every setting gliding over 50 ms (a time change bends pitch).
The line is sized at load from the preset's own times, or 25 s when a control or modulator can
move the time or offset. `delayTimeFormat="musical_time"` is not rendered (the value → note
length mapping is undocumented). Both are instrument-level only, like the reverb, and a first
setting lands at once (`primed` is cleared after `create`'s defaults).
Phaser and the rest still pass through.

The output stage is a soft clip: exact to 0.9, then bending to a 1.0 ceiling
(`test_render` E2). Hard notes through a boosted EQ saturate rather than square off.

## The module's amp envelope (every preset)

An **Amp/Voice** page: Attack / Decay / Sustain / Release as NUMERIC knobs 1–4 (sec, sec,
%, sec) declared `viz: envelope`, an **Override** switch on knob 5 (Josh, 2026-09-18: a
switch, not stepped "Preset" knobs), Polyphony on 6, knob 7 BLANK and **Gain** on 8 (Josh,
2026-09-19). The blank is `""` in `knobs` — a real gap; `null` would be dropped and slide Gain
to knob 7. Do not tidy it out. ⚠ The four MUST sit in one row of the grid: with Override
on knob 1 they straddled the row break and both hosts drew four faders — every C test was
green. `tests/test_pages.mjs` lays the pages out with the hosts' own planner
(`DSPRESET_PAGES_DIR`) and fails if the envelope is not drawn.

**Choosing a preset turns Override off and Polyphony back to "Preset"** (Josh, 2026-09-18); a
project reopening keeps both as saved (`restoring` in `load_target`).
Override off: the preset's envelope plays, and after each load the knobs are set to it (from its
first playable zone), so switching On changes nothing until a knob moves. On: the knobs REPLACE
the preset's values — even a `<sample>`'s own — so releases can be lengthened. Zones with no amp
envelope ignore it. Copied into `engine->amp_override[]` on the audio thread before every MIDI
call and block; a Sustain moved while a note is held glides there (≥ 20 ms). `state` saves
`"amp":"on;a;d;s;r"`.

**Polyphony** (knob 6, same page): an enum `Preset, 1..64` whose index IS the number. It counts
NOTES — every layer a key plays is one note (`voice.note_id`) — not voices. Over the limit, a
new note fades the oldest out in 5 ms (`choke_note`), released notes before held ones, oldest
first within each. "Preset" = no limit of ours (DecentSampler has only per-TAG polyphony, not
implemented yet). Like Override, a CHOSEN preset resets it to "Preset" (Josh, 2026-09-18); a
project reopening keeps it. `state` saves `"polyphony"`.

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

## Voices that stop voices

`silence_voice()` in `native_engine.c` is the ONE way a voice is cut short — the module note
limit, `silencedByTags` choke groups and `<tag polyphony>` limits all go through it:
`silencingDecay` > 0 wins, else `silencingMode` normal = the voice's own release, fast = 5 ms
(not an instant zero: that clicks). The settings are the VICTIM's (sample → group →
instrument, live by binding). `make_way()` runs before every voice starts and never touches
voices of the note being started, so one key's layers cannot cut each other. `<tags><tag>` now
sets a tag's starting volume, on/off and voice limit (the oldest voice goes first).
`pitchKeyTrack` scales the key's distance from the root (0 = root pitch everywhere).

## Where a note plays

Each VOICE carries its own `ds_bounds_t` (start, end, loop, crossfade), computed at note-on
from the zone's — or recomputed when a binding moved SAMPLE_START/END or LOOP_START/END. The
worker streams from the VOICE's bounds (read field by field, then sanitised: a restart can
overwrite them mid-read and that fill is discarded by the publish CAS). For these, and for
LO/HI_NOTE, LO/HI_VEL, ROOT_NOTE and AMP_ENV_ENABLED, a binding on the group wins, then on the
instrument, then the sample's own value — every sample sets its own range, so "own wins" would
make the knob dead. Loop crossfade (`loopCrossfade`, `loopCrossfadeMode`, equal_power default):
over the last N frames before the loop end, the audio N frames before the loop start fades in;
the head path mixes it on the audio thread, the worker pre-mixes it into the ring. N is clamped
to the audio before the loop start, so a loop from frame 0 (CS-20M, DS The Synths) is unchanged.

## Controls naming controls

An `<xyPad>` is TWO knobs (X then Y, each with its `<x>`/`<y>` bindings); an axis nothing is
bound to is dropped and the other keeps the pad's name ("LowpassXY" → "Lowpass"). A binding
names a control by DecentSampler's index (`ds_index`: a pad's axes share one; plain VALUE is
X), by `parameterName` (any mixed-case `parameter` on `type="control"` — BassForge's CC maps),
or by `controlTags`. A linear binding onto a control with NO output range spans the control's
range. ⚠ Whether DecentSampler counts labels/images in `position` is undocumented ("note 1"
is missing from the guide copy): we count only knobs, buttons, menus and pads, as before.
`modulatorTags`, `sampleTags` (level sample: volume, tuning, pan, enabled per sample),
binding `enabled="false"`, and `triggerOnLoad="false"` (held back while `initialising`: the
load's control pass and the plugin's restore loop) are honoured.

## More modulators

`<random>` (-1..1, a new value per note-on — or `frequency` times a second with
`mode="periodic"` — from its own generator, seeded by `seed`), an envelope's delay
(`delayTime` / MOD_DELAY_TIME: 0 until it runs out), LFO SHAPE (fixed words sine/square/saw/
triangle), TRIGGER / `trigger="attack"` (a GLOBAL LFO or random restarts at each note-on), and
`<midi><velocity>` (a voice-scope velocity modulator; each binding's own `modAmount` scales it).

## What a note knows about the notes before it

`e->ctx_prev` (the previously TRIGGERED note, kept after release) and `e->ctx_keys_before`
(keys down before this one) are set in note-on and read by `zone_matches`: `trigger` first
(no key down) / legato (one down) / continuous (always), `previousNotes` (or the true-legato
guide's `previousNote`, with names: C3 = 60, JUCE's convention — ⚠ unconfirmed against a
real preset), `legatoInterval` (note − previous). Glide (`glideTime`/`glideMode`, default
legato = only with a key down): constant time, geometric in pitch — one `pow` per block, one
multiply per frame, in DOUBLE (float lost 5e-9 of pitch). `releaseTriggerDecay`: "NdB" = dB per
second held; a plain number = linear gain LOST per second (`1 − x·held`; the documented default
0.0 meaning "no decay" rules out `x^held`). `<midi><note>` keyswitches fire BEFORE the note
plays; `swallowNotes` also swallows its note-off; `eventType` defaults to note_on (⚠ the guide
says both note_on and any); `midiElementIndex` counts `<cc>`, `<note>`, `<velocity>` in order.

## Start delays, retriggers, and the host's tempo

`delay`/`delayUnit` (seconds, samples, beats) hold a voice silent — nothing advances — for that
many frames, sample-exact within a block. `retriggerEnabled` repeats each zone every
`retriggerInterval` (unit default: beats) while its key is held: the schedule lives in
`e->retrig[]`, NOT in the voice (a short hit ends long before its next repeat), and stops at the
key's release. "Beats" use the host's tempo: ⚠ the plugin now reads `get_bpm` from the host
struct, which BOTH hosts declare at the same offset since stock Schwung **0.7.13** (2026-03) —
the module needs a host at least that new; set `min_host_version` to 0.7.13 when it is listed
in a catalog. It is read on the WORKER (the host's fallback chain may read a settings file
once) and handed to the engine per block. `tests/test_support.h` mirrors that struct.

## Bit crusher, gate, compressor

Our own (`bitcrusher.c`, `gate.c`, `compressor.c`): DecentSampler documents what they do, not
its formulas. Instrument level only — the guide's list of per-note effects does not include
them. Crusher: hold every Nth sample, round to 2^(bits−1) steps, linear mix; 24 bits skip the
rounding (it would only disturb a float). Gate: exact 50 ms windows, `u < amount` shuts one, 5 ms
linear fades, a FIXED seed (`DS_GATE_SEED`: the same pattern every load). Compressor: a peak
follower on the louder channel (stereo-linked) after the input gain, `(level/thr)^(1/ratio − 1)`
above the threshold; autoBypass fades the whole effect (50 ms) while under the threshold. Ratio 1
and 0 dB gains are exactly transparent. A sine reads ~0.6 dB less reduction than the static
curve: the follower rides below its peaks (the test measures the curve on a steady level).

## FLAC

`flac_source.c` wraps dr_flac (vendored in `src/dsp/dspreset/third_party/`, MIT-0, notice in
THIRD_PARTY.md) over OUR descriptor, so the open-file count is unchanged. A decoder has a
position, so a FLAC `ds_wav_source_t` must never be copied to read with: the worker now opens a
whole `ds_wav_source_t` per streaming voice (`e->stream_src[]`, was a bare fd) — for WAV that
is the same file, for FLAC its own decoder; a read elsewhere seeks first (loops, restarts).
Fixtures in `tests/fixtures/flac/` are real ffmpeg encodes of `test_signal24` (README there):
decoded bit-exact, and 0 LSB through the plugin.

## A library's own menu

`DSLibraryInfo.xml` (at the bank's root, or in its one folder — a .dslibrary's usual wrapper):
its `name` becomes the bank's name; `<presetMenu>` reorders the bank's presets without moving
them (top level alphabetical — menus and unmentioned presets together; a menu in written order;
nested menus flattened to "Pads / Analog / Drift"; missing files skipped, empty menus dropped;
nothing usable → the plain list). `ds_catalog_equal` compares names too, so a rename
republishes. No installed library has one (checked on the Move, 2026-09-19).

## Not implemented yet

Effects: phaser (never `schwung-drumverb`, never JUCE 8+ or `juce_dsp` code), tempo-synced
delay, convolution, pitch shift, wave shaper/folder, stereo simulator, group-level reverb /
chorus / delay.
Also: envelope curve shapes, musical-time LFO rates. Load errors reach the user through `get_error` (stock shows a "Synth
Warning" box); status is also logged (`dspreset: loaded …` / `load failed …`).

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
instructions). **It is DecentSampler's copyrighted documentation: excluded via
`.git/info/exclude`, never committed** — history was rewritten on 2026-09-18 to take it out
before this repo went public. Re-fetch it from decentsamples.com if the folder is missing. `docs/NATIVE_DSPRESET_ARCHITECTURE.md` — the no-SFZ decision and its rules.
