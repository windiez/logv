#!/usr/bin/env bash
# build-wasm.sh — Build the self-contained logv.html WebAssembly viewer.
#
# Output: wasm/dist/logv.html  (single, fully offline file — open in any browser)
#
# USAGE
#   ./build-wasm.sh            # build with emcc already on PATH
#   ./build-wasm.sh --install  # install emsdk first, then build
#
# Requires: cmake, python3, git (for emsdk), node (optional, for emsdk checks)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WASM_DIR="$SCRIPT_DIR/wasm"
DIST_DIR="$WASM_DIR/dist"

# ── Install emsdk if requested ────────────────────────────────────────────────
if [[ "${1:-}" == "--install" ]]; then
  EMSDK_DIR="$SCRIPT_DIR/emsdk"
  if [[ ! -d "$EMSDK_DIR" ]]; then
    echo "[emsdk] Cloning Emscripten SDK..."
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
  fi
  cd "$EMSDK_DIR"
  ./emsdk install  latest
  ./emsdk activate latest
  # shellcheck source=/dev/null
  source "$EMSDK_DIR/emsdk_env.sh"
  cd "$SCRIPT_DIR"
fi

# ── Check for emcc ────────────────────────────────────────────────────────────
if ! command -v emcc &>/dev/null; then
  # Try activating a local emsdk if present
  EMSDK_DIR="$SCRIPT_DIR/emsdk"
  if [[ -f "$EMSDK_DIR/emsdk_env.sh" ]]; then
    # shellcheck source=/dev/null
    source "$EMSDK_DIR/emsdk_env.sh"
  fi
  if ! command -v emcc &>/dev/null; then
    echo ""
    echo "ERROR: emcc not found."
    echo ""
    echo "Install Emscripten first:"
    echo "  Run:  ./build-wasm.sh --install"
    echo "  Or:   https://emscripten.org/docs/getting_started/downloads.html"
    echo ""
    exit 1
  fi
fi

echo "[logv-wasm] Using: $(emcc --version | head -1)"

mkdir -p "$DIST_DIR"

# ── Compile ───────────────────────────────────────────────────────────────────
emcc \
  "$WASM_DIR/src/logv_wasm.cpp" \
  "$SCRIPT_DIR/logvcore/src/log_parser.cpp" \
  -I"$SCRIPT_DIR/logvcore/include" \
  -std=c++17 \
  -O2 \
  -lembind \
  -sSINGLE_FILE=1 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORT_ES6=0 \
  -sMODULARIZE=0 \
  --shell-file "$WASM_DIR/shell.html" \
  -o "$DIST_DIR/logv.html"

echo ""
echo "Done! Output: $DIST_DIR/logv.html"
echo "Open it directly in any browser (no server needed)."
