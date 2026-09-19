# DecentSampler coverage roadmap

Built 2026-09-19 from two surveys: the 124 presets on the Move (9 instruments), run through our
own parser, and every element / attribute / parameter in DecentSampler's format guide checked
against the code. Effort: **S** = a day or less, **M** = a few days, **L** = a project of its own.

"Reference" means DecentSampler's docs say *what* a feature does but not *how it sounds*, so
matching it needs a real preset plus a recording of DecentSampler playing it (Josh renders it on
the desktop). Without that we can build something reasonable, not something faithful.

## Tier 1 — needed by the instruments on the Move now

| # | Item | Who needs it | Effort | Reference? |
|---|---|---|---|---|
| 1 | **Wave shaper** (`wave_shaper`: drive 1–1000, driveBoost, shape, outputLevel, highQuality oversampling), group-level per note; plus group-level effect bindings matched by **tags** (`level="group" tags=… effectIndex=…`), FX_DRIVE / FX_OUTPUT_LEVEL / FX_SHAPE / FX_DRIVE_BOOST | CS-20M: 21 presets, 11 driven hard (drive 32–176, output ~0.07) and currently playing clean; its Distortion knob (3 presets). BassForge VOLUME knob and drive XY pad | M | **Yes** — the transfer curve and `shape` are undocumented. Recording: CS-20M "12 Buzzy Bass" and "20 Drunken Organs", one held note each |
| 2 | **Envelope curves** (`attackCurve`/`decayCurve`/`releaseCurve`, -100..100, defaults -100/100/100) incl. modulator envelopes and ENV_*_CURVE bindings | CS-20M: 19 presets | S–M | **Yes** — the curve family is undocumented. Recording: a 2 s attack and 2 s release at curve -100, 0, 100 |
| 3 | **pitchKeyTrack** (sample/group/groups, 0..1) + PITCH_KEY_TRACK binding | DecenTron tape noise (24 presets) | S | No |
| 4 | **Per-tag polyphony** (`<tag polyphony>`, TAG_POLYPHONY) | DecenTron "Tape" tag (24 presets) | S | No |
| 5 | **Loop crossfades** (`loopCrossfade`, `loopCrossfadeMode` linear / equal_power) | DS The Synths (45, 9 samples), CS-20M (10 samples) | S–M | No |
| 6 | **XY pads** → two knobs each (X, Y), with their bindings | BassForge: 3 pads × 10 presets (lowpass, drive, highpass) | S | No |
| 7 | **CC → control by name** (`<cc><binding type="control" parameter="<parameterName>">`) | BassForge (6 CCs) | S | No |
| 8 | **Convolution reverb** (`irFile`, mix, FX_IR_FILE switched by a menu), instrument-level, partitioned FFT, IR resampled at load; CPU measured on the Move | BassForge: REV MIX knob + room menu (10 presets) | L | Partly — BassForge's default `irFile` points at a file that isn't in the library (its menu's IR/*.wav are) |
| 9 | **BassForge delay at time 0** — decide what a 0 s delay does (today: a 1-sample echo at 50% that colours every preset) | BassForge (10) | S | **Yes** — listen in DecentSampler with the DELAY knob at 0 |

## Tier 2 — well documented, buildable without a reference preset

We write our own test presets from the docs. Ordered by how often libraries in the wild use them.

| # | Item | Effort |
|---|---|---|
| 10 | **Choke groups**: `silencedByTags`, `silencingMode` fast/normal, `silencingDecay`, SILENCING_* bindings | S–M |
| 11 | **Keyswitches**: `<midi><note>` (single notes and ranges, eventType note_on/note_off/any, `enabled`, `swallowNotes`) | M |
| 12 | **Legato / first / continuous triggers**, `previousNotes`, `legatoInterval` | M |
| 13 | **Glide**: `glideTime`, `glideMode` always/legato/off, GLIDE_* bindings | M |
| 14 | **Release-trigger decay** (`releaseTriggerDecay`, dB/s or linear/s — formula documented) | S |
| 15 | **`<midi><velocity>`** mappings | S |
| 16 | **More binding targets**: SAMPLE_START/END, LOOP_START/END, ROOT_NOTE, LO/HI_NOTE, LO/HI_VEL, AMP_ENV_ENABLED, GROUP_VOLUME, LFO SHAPE, MOD_DELAY_TIME, TRIGGER | M |
| 17 | **Tag addressing** for bindings: controlTags, modulatorTags, sampleTags | S–M |
| 18 | **`triggerOnLoad="false"`** honoured | S |
| 19 | **Sample start delay + retrigger patterns** (`delay`/`delayUnit`, `retriggerEnabled`/`retriggerInterval`) | M |
| 20 | **FLAC samples** | M |
| 21 | **Library preset menu** (`presetMenu` in the library info file: order and names) | S |
| 22 | **Gate, bit crusher, compressor** — behaviour documented in enough detail to build our own | S each |

## Tier 3 — needs a reference preset + a DecentSampler recording

Documented by name and range only; our version would be a guess at the sound.

| # | Item | Effort | Note |
|---|---|---|---|
| 23 | **Phaser** (mix, modDepth, modRate, centerFrequency, feedback) | S–M | Likely juce::dsp::Phaser — GPL/AGPL, so our own code written from its behaviour, as with the chorus. DecenTron's phaser is unreachable (its knobs are wired to the chorus), so no current preset needs it |
| 24 | **Tempo-synced times** (`musical_time` delay and LFO rates) | S | The value → note-length mapping is undocumented; the host tempo is available |
| 25 | **Wave folder** | S | Curve undocumented |
| 26 | **Pitch shifter** ("old-school") | M | Algorithm undocumented |
| 27 | **Stereo simulator** (adt, lauridsen, schroeder) | M | Three algorithms named, none described |
| 28 | **Reverb dry level** — confirm DecentSampler passes dry at unity | S | One A/B listen |

## Tier 4 — major feature work

| # | Item | Effort | Note |
|---|---|---|---|
| 29 | **Synth oscillators** (`<oscillator>`: basic waveforms, wavetable, harmonic/additive, pluck, 6-operator FM) with their ~200 binding targets | L (several) | A second sound engine. No installed preset uses it. Worth it only if Josh collects libraries built on it |
| 30 | **Buses** (`<buses>`, per-group output routing, bus effects, BUS_VOLUME) | M–L | The Move has one stereo out, so buses fold back into it; what matters is the bus EFFECTS a preset routes through |
| 31 | **Group-level reverb / chorus / delay** (one per note) | M | CPU is the question: a reverb per note is heavy |

## Dropped (Josh, 2026-09-19)

Not to be built for this platform: arpeggiator, note sequences, aux outputs beyond main, MPE
pressure/timbre modulators, UI visuals (images, animations, skins, oscilloscope, keyboard
colours, labels, text/colour bindings), `playbackMode`. The Move sequences, arpeggiates and draws
its own pages; it has one stereo output and no MPE input path.

## Decision (Josh, 2026-09-19)

**Go:** every item in Tiers 1 and 2 that needs no recording — items 3–8 and 10–22.
**Waiting on recordings from DecentSampler:** items 1, 2 and 9 (and all of Tier 3).
**Not now:** Tier 4. **Dropped:** the list above.

## Suggested order

1. Tier 1 items 3–7 (small, no reference needed) — every installed instrument then fully covered
   except the three that need a recording.
2. Items 1, 2, 9 as soon as the recordings exist; until then, a documented best guess can ship
   behind the same tests.
3. Tier 2 items 10–15 (choke, keyswitches, legato, glide, release decay, velocity) — what
   drum, orchestral and guitar libraries need.
4. Item 8 (convolution) and the rest of Tier 2.
5. Tier 3 as recordings arrive; Tier 4 only on demand.

Each feature gets its own plan before it is built.
