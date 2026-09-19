# FLAC fixtures

Real FLAC encodes (ffmpeg's encoder, LPC and all — not verbatim frames) of the
tests' own signal, so `tests/test_flac.c` can check the decoded frames against
`test_signal24` sample for sample. Committed because the test machines have
no FLAC encoder; regenerate with:

```sh
cc -I tests -o /tmp/gen tests/fixtures/flac/gen.c
/tmp/gen /tmp/m16.wav /tmp/s24.wav
ffmpeg -y -i /tmp/m16.wav -c:a flac -compression_level 8 tests/fixtures/flac/mono16.flac
ffmpeg -y -i /tmp/s24.wav -c:a flac -compression_level 8 tests/fixtures/flac/stereo24.flac
```

- `mono16.flac`: 40000 frames, 16-bit mono, `test_signal24(i, 0) >> 8`
- `stereo24.flac`: 40000 frames, 24-bit stereo, `test_signal24(i, ch)`
