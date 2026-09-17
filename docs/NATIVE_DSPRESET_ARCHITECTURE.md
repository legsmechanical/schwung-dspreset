# Native DSPreset architecture

## Module package contract

The authoritative package contract is Schwung upstream's
[`docs/MODULES.md`](https://github.com/charlesvestal/schwung/blob/main/docs/MODULES.md),
not Multisampler's existing `module.json`. This project is a
`sound_generator` using API version 2, so its eventual package must use the
module id as its directory name and ship the DSP binary as `dsp.so`.

`module.json` will remain a small, standards-compliant declaration. It will
advertise only the capabilities actually implemented by the native engine;
compatibility features must not be implied through inherited Multisampler
metadata.

## Contract

`schwung-dspreset` accepts `.dspreset` directly. It does not generate an SFZ
file, invoke an SFZ parser, or represent DecentSampler controls as synthetic
SFZ opcodes.

The target is functional DSPreset compatibility, measured against a pinned
DecentSampler release and a corpus of reference libraries. Exact audio parity
is a separately measured claim, not an assumption made from the XML guide.

## Layers

```
DSPreset XML
    -> validating SAX parser
    -> native document / inheritance resolver
    -> native region, binding, modulation, bus, and UI graph
    -> voice scheduler + effect graph
    -> sample source (memory or streamed)
    -> Move plugin adapter
```

Only the bottom sample-source layer may reuse code from xsynth. Its streamed
source, resident attack head, background I/O worker, source-rate conversion,
and optional `.x44c` cache are useful primitives. SFZ region parsing,
synthetic MIDI-CC mappings, SFZ effect semantics, preset scanning, automatic
gain, and its user interface are not part of this module's design.

## Playback policy

The DSPreset guide defines `playbackMode` as `memory`, `disk_streaming`, or
`auto`, at instrument, group, and sample scope. The resolved setting travels
with every native sample region.

`auto` is a device preference, not an arbitrary size threshold hidden in the
parser. The Move preference can choose streaming for large libraries, while
an author can require either mode for an individual region.

If a binding can alter `start`, `end`, `loopStart`, or `loopEnd` after load,
the region is forced into memory playback. This preserves the documented
DecentSampler constraint and prevents a windowed stream from playing stale or
unavailable frames. The engine records that fallback so the UI can explain
why a sample is resident.

## Disk and cache rules

- Original WAV/FLAC/AIFF files are authoritative and never rewritten.
- Disk streams run from a preloaded attack head; all later I/O happens on a
  worker, never in Move's real-time render callback.
- `.x44c` is an optional, disposable decoded/resampled cache, keyed by source
  fingerprint and output rate. It trades disk space for CPU; it is never a
  required conversion format.
- Cache creation and eviction are background/off-device work. A bounded cache
  budget must evict only derived files, never user assets.

## First vertical slice

1. Parse the root, `groups`, `group`, `sample`, and `binding` elements with
   correct inheritance and path resolution.
2. Resolve playback policy, key/velocity zones, loop points, and round robin
   into native regions.
3. Feed those regions to a direct xsynth streaming-source adapter.
4. Validate native render output with the existing fixture corpus and Move
   on-device tests.
