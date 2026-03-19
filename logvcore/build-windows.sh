#!/usr/bin/env bash
# Cross-compile logvcore for Windows x86_64 using MinGW.
# Produces: build-windows/logvcore.dll
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install mingw-w64
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR=build-windows
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake \
    -DCMAKE_BUILD_TYPE=Release

cmake --build . -j"$(nproc)"

echo ""
echo "Built:"
ls -lh logvcore.dll 2>/dev/null || ls -lh liblogvcore.dll 2>/dev/null
file logvcore.dll 2>/dev/null || file liblogvcore.dll 2>/dev/null
