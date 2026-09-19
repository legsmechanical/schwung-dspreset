# Third-party code

## JUCE — `juce::Reverb` (`src/dsp/dspreset/reverb.c`)

The reverb is a C port of `juce_Reverb.h` from JUCE's `juce_audio_basics` module as released in
**JUCE 7 (7.0.12 and earlier), under the ISC licence** below. JUCE 8 relicensed that module
(AGPLv3 or commercial); nothing here is taken from JUCE 8 or later. The algorithm is itself Jezar
at Dreampoint's Freeverb (public domain).

```
Copyright (c) Raw Material Software Limited

Permission to use, copy, modify, and/or distribute this software for any purpose with or
without fee is hereby granted, provided that the above copyright notice and this permission
notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
OR PERFORMANCE OF THIS SOFTWARE.
```

## Not third-party code

The chorus (`chorus.c`) behaves as `juce::dsp::Chorus` does with its default centre delay and
feedback, but is our own code written from that description: JUCE's `juce_dsp` module is GPL/AGPL
and none of it is used. The delay (`delay.c`) is our own design.
