#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build/windows"
VCPKG="${VCPKG_DIR:-$HOME/vcpkg}"

cmake -S "$ROOT" -B "$BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_TOOLCHAIN_FILE_EXTRA="$ROOT/cmake/toolchain-mingw.cmake" \
    -DVCPKG_TARGET_TRIPLET=x64-mingw-static

cmake --build "$BUILD" --parallel "$(nproc)"
