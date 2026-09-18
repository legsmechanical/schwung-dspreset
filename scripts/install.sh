#!/bin/bash
# Install the native DSPreset module to Move.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/dspreset" ]; then
    echo "Error: dist/dspreset not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing DSPreset ==="

# Deploy to Move - sound_generators subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/dspreset"
scp -r dist/dspreset/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/dspreset/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/schwung/patches/
fi

# Create instruments directory for user sample libraries
echo "Creating instruments directory..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/dspreset/instruments"

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/dspreset"

echo "Restarting Schwung so the native plugin is loaded..."
"$REPO_ROOT/../scripts/restart_move.sh"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/dspreset/"
echo ""
echo "Load DSPreset files or DSLibrary packages from the Library parameter."
