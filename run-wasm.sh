#!/usr/bin/env bash
# run_wasm.sh - Deploy logv-proxy, start it on the device, open the web viewer.
#
# USAGE
#   bash run_wasm.sh root@<device-ip> [port]
#
# EXAMPLES
#   bash run_wasm.sh root@192.168.130.81
#   bash run_wasm.sh root@192.168.130.81 9222

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Arguments ──────────────────────────────────────────────────────────────────
if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
  echo ""
  echo "  USAGE: bash run_wasm.sh root@<device-ip> [port]"
  echo ""
  echo "  EXAMPLES:"
  echo "    bash run_wasm.sh root@192.168.130.81"
  echo "    bash run_wasm.sh root@192.168.130.81 9222"
  echo ""
  exit 1
fi

TARGET="$1"
PORT="${2:-9222}"
PROXY="$SCRIPT_DIR/logv-proxy.py"
HTML="$SCRIPT_DIR/wasm/dist/logv.html"

# Extract just the IP/hostname from "user@host" (handles bare "host" too)
HOST="${TARGET##*@}"

# ── Validate port ──────────────────────────────────────────────────────────────
if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
  echo "[run_wasm] ERROR: invalid port '$PORT' (must be 1-65535)"
  exit 1
fi

# ── Sanity checks ──────────────────────────────────────────────────────────────
if [[ ! -f "$PROXY" ]]; then
  echo "[run_wasm] ERROR: logv-proxy.py not found at $PROXY"
  exit 1
fi

if [[ ! -f "$HTML" ]]; then
  echo "[run_wasm] logv.html not found - building first..."
  bash "$SCRIPT_DIR/build-wasm.sh"
fi

# ── Check SSH connectivity before doing anything ──────────────────────────────
echo "[run_wasm] Checking SSH connection to $TARGET ..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$TARGET" true 2>/dev/null; then
  echo "[run_wasm] ERROR: cannot SSH into $TARGET"
  echo "  - Check the IP address and that the device is powered on"
  echo "  - Make sure your SSH key is authorized: ssh-copy-id $TARGET"
  exit 1
fi

# ── Version check: skip deploy if device already has the current proxy ─────────
# Version = last git commit hash that touched logv-proxy.py.
# Stored on device as ~/logv-proxy.ver   (one line, the hash).
LOCAL_VER="$(git -C "$SCRIPT_DIR" log -1 --format=%H -- logv-proxy.py 2>/dev/null || true)"
if [[ -z "$LOCAL_VER" ]]; then
  # Not in a git repo or file never committed — fall back to md5 of the file
  LOCAL_VER="$(md5sum "$PROXY" | cut -d' ' -f1)"
fi

DEVICE_VER="$(ssh "$TARGET" "cat ~/logv-proxy.ver 2>/dev/null || true" 2>/dev/null || true)"

PROXY_RUNNING="$(ssh "$TARGET" \
  "ps aux 2>/dev/null | grep '[l]ogv-proxy\|nc.*-lk.*-p.*$PORT' | head -1" 2>/dev/null || true)"

if [[ "$LOCAL_VER" == "$DEVICE_VER" && -n "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Proxy up-to-date and running (${LOCAL_VER:0:8}) -- reusing."
  SKIP_DEPLOY=1
elif [[ "$LOCAL_VER" == "$DEVICE_VER" && -z "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Proxy up-to-date (${LOCAL_VER:0:8}) but not running -- will start it."
  SKIP_DEPLOY=1
else
  echo "[run_wasm] New version (${LOCAL_VER:0:8} vs device ${DEVICE_VER:0:8}) -- deploying."
  SKIP_DEPLOY=0
fi

# ── Deploy proxy (only if needed) ─────────────────────────────────────────────
if [[ "$SKIP_DEPLOY" -eq 0 ]]; then
  echo "[run_wasm] Copying logv-proxy.py -> $TARGET:~/ ..."
  if ! scp -q "$PROXY" "$TARGET":~/logv-proxy.py; then
    echo "[run_wasm] ERROR: scp failed"
    exit 1
  fi
  # Write version stamp so next run can skip this step
  ssh "$TARGET" "echo '$LOCAL_VER' > ~/logv-proxy.ver" 2>/dev/null || true
fi

# ── Kill existing proxy on device (always restart to pick up new session) ──────
if [[ -n "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Stopping existing proxy ..."
  ssh "$TARGET" "
    PIDS=\$(ps aux 2>/dev/null | grep '[l]ogv-proxy\|nc.*-lk.*-p.*$PORT' | awk '{print \$1}' | sort -u)
    if [ -n \"\$PIDS\" ]; then
      echo \$PIDS | xargs kill -TERM 2>/dev/null || true
      sleep 0.4
    fi
    true
  " 2>/dev/null || true
fi

# ── Start proxy on device (background, output to /tmp/logv-proxy.log) ─────────
echo "[run_wasm] Starting proxy on $TARGET port $PORT ..."
# Use 'bash -c' so nohup+background works reliably with ssh -f
ssh -f "$TARGET" "bash -c 'nohup python3 \$HOME/logv-proxy.py $PORT > /tmp/logv-proxy.log 2>&1 &'"
sleep 1.0   # give nc time to start listening

# Confirm proxy is actually listening
if ssh -o ConnectTimeout=3 "$TARGET" \
     "grep -q 'Listening on port' /tmp/logv-proxy.log 2>/dev/null || \
      cat /tmp/logv-proxy.log 2>/dev/null | grep -qi 'error\|fatal\|traceback'" 2>/dev/null; then
  # Check for errors in the log
  PROXY_ERR=$(ssh "$TARGET" "grep -i 'error\|fatal\|traceback' /tmp/logv-proxy.log 2>/dev/null | head -3" 2>/dev/null || true)
  if [[ -n "$PROXY_ERR" ]]; then
    echo "[run_wasm] WARNING: proxy may have errors:"
    echo "$PROXY_ERR"
    echo "  Full log: ssh $TARGET 'cat /tmp/logv-proxy.log'"
  fi
fi

# ── Open the viewer in the browser ────────────────────────────────────────────
FILE_URL="file://${HTML}?host=${HOST}&port=${PORT}"
echo "[run_wasm] Opening viewer ..."

# Detect runtime environment and open in the appropriate browser.
HTTP_PIDFILE="/tmp/logv-wasm-http.pid"

# Detect runtime environment and open in the appropriate browser.
if grep -qi microsoft /proc/version 2>/dev/null; then
  # WSL: file:// with query strings is unreliable across Windows browsers.
  # Start a temporary Python HTTP server in WSL -- WSL2 auto-forwards localhost
  # to the Windows host, so the browser opens a clean http:// URL instead.

  # Kill any HTTP server left over from a previous run of this script
  if [[ -f "$HTTP_PIDFILE" ]]; then
    OLD_PID="$(cat "$HTTP_PIDFILE" 2>/dev/null || true)"
    if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
      echo "[run_wasm] Stopping previous HTTP server (pid $OLD_PID)..."
      kill "$OLD_PID" 2>/dev/null || true
    fi
    rm -f "$HTTP_PIDFILE"
  fi

  # Find a free port for the local HTTP server
  HTTP_PORT=$((8750 + RANDOM % 200))
  SERVE_DIR="$(dirname "$HTML")"
  HTML_NAME="$(basename "$HTML")"

  python3 -m http.server "$HTTP_PORT" --directory "$SERVE_DIR" --bind 127.0.0.1 \
    >/dev/null 2>&1 &
  HTTP_PID=$!
  echo "$HTTP_PID" > "$HTTP_PIDFILE"

  # Verify the HTTP server started
  sleep 0.4
  if ! kill -0 "$HTTP_PID" 2>/dev/null; then
    echo "[run_wasm] WARNING: HTTP server failed to start. Open manually: $HTML"
    rm -f "$HTTP_PIDFILE"
  else
    HTTP_URL="http://localhost:${HTTP_PORT}/${HTML_NAME}?host=${HOST}&port=${PORT}"
    echo "[run_wasm] $HTTP_URL"
    powershell.exe -NoProfile -Command "Start-Process '$HTTP_URL'" 2>/dev/null || \
      cmd.exe /c start "" "$HTTP_URL" 2>/dev/null || \
      echo "[run_wasm] Could not open browser. Open manually: $HTTP_URL"
    # Kill HTTP server after 60 s (enough time for browser to fully load the file)
    (sleep 60 && kill "$HTTP_PID" 2>/dev/null && rm -f "$HTTP_PIDFILE") &
  fi

elif command -v xdg-open &>/dev/null && xdg-open "$FILE_URL" 2>/dev/null; then
  :  # native Linux with desktop environment
elif command -v open &>/dev/null; then
  open "$FILE_URL"  # macOS
else
  for B in google-chrome google-chrome-stable chromium chromium-browser firefox; do
    if command -v "$B" &>/dev/null; then "$B" "$FILE_URL" & break; fi
  done || echo "[run_wasm] Could not open browser. Open manually: $HTML"
fi

echo "[run_wasm] Done.  Proxy log: ssh $TARGET 'tail -f /tmp/logv-proxy.log'"
