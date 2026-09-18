#!/usr/bin/env bash
# Build the native DSPreset module for Schwung (ARM64).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-sfz-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Multisampler Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo ""
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building DSPreset ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build dist/dspreset

# Compile the direct XML/region/WAV/cache engine and its V2 wrapper.
echo ""
echo "=== Compiling DSP plugin ==="
for src in src/dsp/dspreset_plugin.c \
           src/dsp/dspreset/native_engine.c src/dsp/dspreset/region_map.c \
           src/dsp/dspreset/dspreset_parser.c src/dsp/dspreset/library_input.c \
           src/dsp/dspreset/library_preparer.c src/dsp/dspreset/zip_index.c \
           src/dsp/dspreset/zip_extract.c \
           src/dsp/dspreset/voice.c src/dsp/dspreset/page_cache.c \
           src/dsp/dspreset/wav_source.c; do
    obj="build/$(basename "$src" .c).o"
    ${CROSS_PREFIX}gcc -O3 -fPIC \
        -march=armv8-a -mtune=cortex-a72 \
        -DNDEBUG \
        -c "$src" \
        -o "$obj" \
        -Isrc/dsp
done

echo "=== Linking dsp.so ==="
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    build/dspreset_plugin.o build/native_engine.o build/region_map.o \
    build/dspreset_parser.o build/library_input.o build/library_preparer.o \
    build/zip_index.o build/zip_extract.o build/voice.o \
    build/page_cache.o build/wav_source.o \
    -o build/dsp.so \
    -lm -lpthread -lz

echo "DSP plugin linked"

# --- Step 3: Package ---
echo ""
echo "=== Packaging ==="
cat src/module.json > dist/dspreset/module.json
cat build/dsp.so > dist/dspreset/dsp.so
chmod +x dist/dspreset/dsp.so
mkdir -p dist/dspreset/instruments

cd dist
tar -czvf dspreset-module.tar.gz dspreset/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/dspreset/"
echo "Tarball: dist/dspreset-module.tar.gz"
echo ""
echo "To install on Move:  ./scripts/install.sh"
