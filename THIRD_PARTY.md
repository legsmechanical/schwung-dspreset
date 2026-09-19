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

## dr_flac (`src/dsp/dspreset/third_party/dr_flac.h`)

FLAC decoding: dr_flac v0.13.4 by David Reid (github.com/mackron/dr_libs, commit
`dfe8377`), used unmodified. Its author offers it as public domain (Unlicense) or under MIT No
Attribution; we take it under MIT-0:

```
Copyright 2023 David Reid

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Not third-party code

The chorus (`chorus.c`) behaves as `juce::dsp::Chorus` does with its default centre delay and
feedback, but is our own code written from that description: JUCE's `juce_dsp` module is GPL/AGPL
and none of it is used. The delay (`delay.c`) is our own design.
