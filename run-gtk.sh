#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/gtk_native/build/logv-gtk"

if [[ ! -x "$BIN" ]]; then
  echo "Binary not found. Building first..."
  "$SCRIPT_DIR/build-gtk.sh"
fi

exec "$BIN"
