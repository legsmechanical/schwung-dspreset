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
- **`.dslibrary` packages** are unpacked on the Move the first time you pick one.

Not yet: DecentSampler's effects, on-screen knobs and buttons, and modulators.

## Using it

Put libraries in `/data/UserData/schwung/modules/sound_generators/dspreset/instruments/` (the
browser opens there), add DSPreset to a track, and pick a file with **Library**. A `.dslibrary`
loads its first preset; to pick a different one, browse into `<name>.dslibrary.unpacked/` and
choose its `.dspreset`.

## Building

See [`CLAUDE.md`](CLAUDE.md) for the build, the tests and how playback works.

## License

LGPL-3.0.
