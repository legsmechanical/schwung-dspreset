#!/usr/bin/env bash
# Builds and runs every test natively against the SAME source list the ARM
# build ships (scripts/dsp_sources.txt).
#
# Real-library fixtures are not committed. Point at them with:
#   DSPRESET_CAPTURE=/path/Capture GO-TO Bass.dspreset
#   DSPRESET_ASIMOV_DIR=/path/ASIMOV v1.0          (folder of 15 .dspreset)
# A missing fixture FAILS the run. DSPRESET_ALLOW_MISSING_FIXTURES=1 turns that
# into a named SKIP, which is reported beside the count.
set -u
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/dspreset-tests.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
export TEST_TMP="$BUILD/tmp"; mkdir -p "$TEST_TMP"

for tool in "$CC" python3; do
    command -v "$tool" >/dev/null || { echo "FAIL: required tool '$tool' not found"; exit 1; }
done

SOURCES=$(cat scripts/dsp_sources.txt)
CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc/dsp"
# AddressSanitizer + UBSan on by default: an out-of-bounds read that happens to
# land in mapped memory passes silently otherwise (it did, on macOS, while the
# same test crashed on Linux). DSPRESET_NO_SANITIZE=1 is the named way out.
if [ "${DSPRESET_NO_SANITIZE:-}" != 1 ]; then
    CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
else
    echo "NOTE: sanitizers OFF (DSPRESET_NO_SANITIZE=1)"
fi
if ! $CC $CFLAGS -c $SOURCES 2>"$BUILD/warnings.txt"; then cat "$BUILD/warnings.txt"; echo "FAIL: engine does not compile"; exit 1; fi
mv ./*.o "$BUILD/"
if [ -s "$BUILD/warnings.txt" ]; then cat "$BUILD/warnings.txt"; echo "FAIL: engine compiles with warnings"; exit 1; fi

pass=0; fail=0; skipped=()
run() {  # name, then the command
    local name="$1"; shift
    echo "--- $name"
    if "$@"; then pass=$((pass + 1)); else echo "FAILED: $name"; fail=$((fail + 1)); fi
}

# A small Capture-shaped .dslibrary for the archive tests: one preset, a
# deflated sample, and macOS resource-fork noise the importer must ignore.
ARCHIVE="$BUILD/archive/Capture.dslibrary"
mkdir -p "$BUILD/archive/src/Capture GO-TO Bass/Samples" "$BUILD/archive/src/__MACOSX"
printf '<DecentSampler><groups><group><sample path="Samples/a.wav" rootNote="60"/></group></groups></DecentSampler>' \
    > "$BUILD/archive/src/Capture GO-TO Bass/Capture GO-TO Bass.dspreset"
head -c 200000 /dev/zero > "$BUILD/archive/src/Capture GO-TO Bass/Samples/a.wav"
printf x > "$BUILD/archive/src/__MACOSX/._junk.dspreset"
(cd "$BUILD/archive/src" && python3 -m zipfile -c "$ARCHIVE" "Capture GO-TO Bass" __MACOSX) || { echo "FAIL: cannot build archive fixture"; exit 1; }

tests=$(ls tests/test_*.c)
[ -n "$tests" ] || { echo "FAIL: no tests collected"; exit 1; }
for t in $tests; do
    name=$(basename "$t" .c)
    if ! $CC $CFLAGS -o "$BUILD/$name" "$t" "$BUILD"/*.o ${ZLIB_LINK:--lz} -lpthread -lm; then
        echo "FAILED: $name does not compile"; fail=$((fail + 1)); continue
    fi
    case "$name" in
        test_real_libraries)
            if [ ! -f "${DSPRESET_CAPTURE:-}" ] || [ ! -d "${DSPRESET_ASIMOV_DIR:-}" ]; then
                if [ "${DSPRESET_ALLOW_MISSING_FIXTURES:-}" = 1 ]; then skipped+=("$name"); continue; fi
                echo "FAILED: $name needs DSPRESET_CAPTURE and DSPRESET_ASIMOV_DIR (or DSPRESET_ALLOW_MISSING_FIXTURES=1)"
                fail=$((fail + 1)); continue
            fi ;;
        test_zip_index) run "$name" "$BUILD/$name" "$ARCHIVE"; continue ;;
        test_zip_extract|test_library_preparer)
            run "$name" "$BUILD/$name" "$ARCHIVE" "$BUILD/archive/out-$name"; continue ;;
    esac
    run "$name" "$BUILD/$name"
done

echo
echo "tests: $pass passed, $fail failed, ${#skipped[@]} skipped${DSPRESET_NO_SANITIZE:+ (sanitizers OFF)}"
for s in "${skipped[@]+"${skipped[@]}"}"; do echo "  SKIPPED: $s"; done
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
