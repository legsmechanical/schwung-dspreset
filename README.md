# DSPreset

A DecentSampler player for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move.
Load a `.dspreset` preset or a `.dslibrary` package and play it from the pads or any MIDI
source.

- **Direct, not converted.** The preset's XML is read as it is — no SFZ step.
- **Big libraries stream from disk.** The start of every sample is kept in memory so notes speak
  instantly; the rest streams in the background.
- **Plays what most sample libraries use:** key and velocity zones, round robin, volume, tuning,
  pan, amp envelope, loops (including loop points stored in the WAV files), release samples,
  sustain pedal and pitch bend.
- **Missing samples don't stop a preset loading** — the rest of it plays.
- **`.dslibrary` packages** are unpacked on the Move the first time you pick one; a **`.dsbundle`**
  (a folder) is a bank as it is.
- **WAV and AIFF samples.**

- **The preset's own controls** — its knobs, buttons and menus — are on the module's knobs,
  named after the preset (or after what they drive, when the preset draws its names in a
  picture). Layers, mic mixes, envelopes, tuning and volume respond now.

- **Filters, EQ and gain** from the preset's effects, with its knobs driving them.
- **Reverb:** the same reverb DecentSampler uses (JUCE's), so a preset's reverb sounds as its
  author set it.
- **Chorus and delay**, with the preset's knobs driving them. The chorus works the way
  DecentSampler's does; the delay echoes at the preset's times, left and right apart by its
  stereo offset.

- **An amp envelope for every preset** on its own page, drawn as an envelope: Attack, Decay,
  Sustain and Release, and an Override switch. Off, they show the preset's own envelope; on,
  they replace it. Beside them, **Polyphony**: "Preset" or 1–64 notes (1 = mono).
- **Modulators:** envelopes and LFOs sweep filters, pitch, volume and pan, per note or shared,
  with the preset's knobs setting their depth, rate and times.

Not yet: phaser and the other remaining effects, and tempo-synced delay (their knobs show
up but do nothing yet).

## Using it

Put libraries in `/data/UserData/schwung/modules/sound_generators/dspreset/instruments/`. Each
folder, `.dslibrary` or loose `.dspreset` there is a **bank**.

In the module's pages, **Presets** lists the current bank's presets — scroll and stop, and the one
you stop on loads. **Banks** lists every bank; choose one and you land back on its presets. A
`.dslibrary` is unpacked the first time you choose it (its name reads "Unpacking..." meanwhile).
Your bank and preset are saved with the project.

## Building

See [`CLAUDE.md`](CLAUDE.md) for the build, the tests and how playback works.

## License

MIT — see [`LICENSE`](LICENSE). Code under `src/dsp/third_party/` (unused by this module, kept from
the Multisampler fork) carries its own licenses.
