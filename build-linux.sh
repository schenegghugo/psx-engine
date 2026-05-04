#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build/linux"
VCPKG="${VCPKG_DIR:-$HOME/vcpkg}"

cmake -S "$ROOT" -B "$BUILD" \
    -G "Ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG/scripts/buildsystems/vcpkg.cmake"

cmake --build "$BUILD" --parallel "$(nproc)"

echo ""
echo "  engine:  $BUILD/engine/psx_engine"
echo "  editor:  $BUILD/editor/psx_editor"
