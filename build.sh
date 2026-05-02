#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Prefer local venv cmake if available
if [ -x "$SCRIPT_DIR/.venv/bin/cmake" ]; then
    CMAKE="$SCRIPT_DIR/.venv/bin/cmake"
else
    CMAKE="cmake"
fi

"$CMAKE" ..
make -j"$(nproc)"

echo ""
echo "========================================"
echo "  Build completed successfully!"
echo "========================================"
echo ""
echo "Run the application with:"
echo "  ./Wasteland"
echo ""
