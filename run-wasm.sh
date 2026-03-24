#!/usr/bin/env bash
# run-wasm.sh - Deploy logv-proxy, start it, open the web viewer.
#
# USAGE
#   bash run-wasm.sh <target|local> [port] [mode]
#
# MODES
#   auto  (default) -> local when target is a local host/IP, else SSH
#   ssh             -> force SSH
#   local           -> force local execution
#
# EXAMPLES
#   bash run-wasm.sh root@192.168.130.81
#   bash run-wasm.sh root@192.168.130.81 9222 ssh
#   bash run-wasm.sh 192.168.1.99 9222 auto
#   bash run-wasm.sh local 9222 local
#
# NOTES
# - For SSH password auth, install sshpass and export LOGV_SSH_PASSWORD,
#   or the script will prompt interactively.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
  echo ""
  echo "  USAGE: bash run-wasm.sh <target|local> [port] [mode]"
  echo ""
  echo "  MODES: auto (default), ssh, local"
  echo ""
  echo "  EXAMPLES:"
  echo "    bash run-wasm.sh root@192.168.130.81"
  echo "    bash run-wasm.sh 192.168.1.99 9222 auto"
  echo "    bash run-wasm.sh local 9222 local"
  echo ""
}

# ── Arguments ──────────────────────────────────────────────────────────────────
if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 1
fi

TARGET_RAW="$1"
PORT="${2:-9222}"
MODE="${3:-${LOGV_CONN_MODE:-auto}}"

PROXY="$SCRIPT_DIR/logv-proxy.py"
HTML="$SCRIPT_DIR/wasm/dist/logv.html"

if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
  echo "[run_wasm] ERROR: invalid port '$PORT' (must be 1-65535)"
  exit 1
fi

case "$MODE" in
  auto|ssh|local) ;;
  *)
    echo "[run_wasm] ERROR: invalid mode '$MODE' (expected: auto|ssh|local)"
    exit 1
    ;;
esac

if [[ "$TARGET_RAW" == "local" ]]; then
  TARGET=""
  HOST="127.0.0.1"
else
  TARGET="$TARGET_RAW"
  HOST="${TARGET_RAW##*@}"
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

is_local_host() {
  local h="$1"
  local h_low
  h_low="$(printf '%s' "$h" | tr '[:upper:]' '[:lower:]')"
  if [[ -z "$h_low" || "$h_low" == "localhost" || "$h_low" == "127.0.0.1" || "$h_low" == "::1" ]]; then
    return 0
  fi

  local ips=""
  if command -v ip >/dev/null 2>&1; then
    ips="$(
      {
        ip -o -4 addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true
        ip -o -6 addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true
      } | sort -u
    )"
  elif command -v ifconfig >/dev/null 2>&1; then
    ips="$(
      {
        ifconfig 2>/dev/null | awk '/inet /{print $2}' || true
        ifconfig 2>/dev/null | awk '/inet6 /{print $2}' | sed 's/%.*$//' || true
      } | sort -u
    )"
  elif command -v hostname >/dev/null 2>&1; then
    ips="$(hostname -i 2>/dev/null | tr ' ' '\n' || true)"
  fi

  [[ -n "$ips" ]] && grep -Fxq "$h_low" <<<"$ips"
}

hash_file_sha256() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$f" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$f" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$f" | awk '{print $NF}'
  elif command -v md5sum >/dev/null 2>&1; then
    md5sum "$f" | awk '{print $1}'
  elif command -v md5 >/dev/null 2>&1; then
    md5 -q "$f"
  else
    # Last resort: nanoseconds since epoch to force deploy.
    date +%s%N
  fi
}

# Determine runtime connection mode.
if [[ "$MODE" == "local" ]]; then
  EXEC_MODE="local"
elif [[ "$MODE" == "ssh" ]]; then
  EXEC_MODE="ssh"
elif is_local_host "$HOST"; then
  EXEC_MODE="local"
else
  EXEC_MODE="ssh"
fi

echo "[run_wasm] Mode: $EXEC_MODE (requested: $MODE, host: $HOST)"

# In forced/local-exec mode, the viewer must target this machine.
if [[ "$EXEC_MODE" == "local" ]] && ! is_local_host "$HOST"; then
  echo "[run_wasm] NOTE: overriding viewer host '$HOST' -> 127.0.0.1 for local mode"
  HOST="127.0.0.1"
fi

SSH_CMD=()
SCP_CMD=()
if [[ "$EXEC_MODE" == "ssh" ]]; then
  if [[ -z "$TARGET" ]]; then
    echo "[run_wasm] ERROR: SSH mode requires a target like user@host"
    exit 1
  fi

  echo "[run_wasm] Probing SSH auth for $TARGET ..."
  set +e
  SSH_PROBE_OUT="$(ssh -o ConnectTimeout=5 -o BatchMode=yes -o NumberOfPasswordPrompts=0 "$TARGET" true 2>&1)"
  SSH_PROBE_RC=$?
  set -e

  if [[ "$SSH_PROBE_RC" -eq 0 ]]; then
    SSH_CMD=(ssh -o BatchMode=yes)
    SCP_CMD=(scp)
    echo "[run_wasm] SSH auth: key/cert"
  else
    SSH_PROBE_LOW="$(printf '%s' "$SSH_PROBE_OUT" | tr '[:upper:]' '[:lower:]')"
    if [[ "$SSH_PROBE_LOW" == *"permission denied"* ||
          "$SSH_PROBE_LOW" == *"publickey"* ||
          "$SSH_PROBE_LOW" == *"keyboard-interactive"* ||
          "$SSH_PROBE_LOW" == *"password"* ||
          "$SSH_PROBE_LOW" == *"authentication failed"* ]]; then
      if ! command -v sshpass >/dev/null 2>&1; then
        echo "[run_wasm] ERROR: SSH password auth needed but 'sshpass' is not installed."
        exit 1
      fi
      if [[ -z "${LOGV_SSH_PASSWORD:-}" ]]; then
        read -r -s -p "[run_wasm] SSH password for $TARGET: " LOGV_SSH_PASSWORD
        echo ""
      fi
      export SSHPASS="${LOGV_SSH_PASSWORD}"
      SSH_CMD=(sshpass -e ssh -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no)
      SCP_CMD=(sshpass -e scp -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no)
      if ! "${SSH_CMD[@]}" -o ConnectTimeout=5 "$TARGET" true 2>/dev/null; then
        echo "[run_wasm] ERROR: SSH password auth failed for $TARGET"
        exit 1
      fi
      echo "[run_wasm] SSH auth: password"
    else
      echo "[run_wasm] ERROR: cannot SSH into $TARGET"
      echo "  ${SSH_PROBE_OUT%%$'\n'*}"
      exit 1
    fi
  fi
fi

target_exec() {
  local cmd="$1"
  if [[ "$EXEC_MODE" == "ssh" ]]; then
    "${SSH_CMD[@]}" -o ConnectTimeout=5 "$TARGET" "$cmd"
  else
    bash -lc "$cmd"
  fi
}

# Use user-scoped files in /tmp to avoid permission collisions between
# different users (e.g. prior root-run artifacts).
REMOTE_USER_RAW="$(target_exec "id -un 2>/dev/null || whoami 2>/dev/null || echo user" 2>/dev/null || echo user)"
REMOTE_USER_TAG="$(printf '%s' "$REMOTE_USER_RAW" | tr -c '[:alnum:]_.-' '_' )"
if [[ -z "$REMOTE_USER_TAG" ]]; then
  REMOTE_USER_TAG="user"
fi
REMOTE_PREFIX="/tmp/logv-proxy-${REMOTE_USER_TAG}"
REMOTE_PROXY="${REMOTE_PREFIX}.py"
REMOTE_VER="${REMOTE_PREFIX}.ver"
REMOTE_LOG="${REMOTE_PREFIX}.log"

target_copy_proxy() {
  if [[ "$EXEC_MODE" == "ssh" ]]; then
    "${SCP_CMD[@]}" -q "$PROXY" "$TARGET":"$REMOTE_PROXY"
  else
    cp "$PROXY" "$REMOTE_PROXY"
  fi
}

# ── Check runtime prerequisites ────────────────────────────────────────────────
if ! target_exec "command -v nc >/dev/null 2>&1" 2>/dev/null; then
  if [[ "$EXEC_MODE" == "ssh" ]]; then
    echo "[run_wasm] ERROR: 'nc' not found on $TARGET"
  else
    echo "[run_wasm] ERROR: 'nc' not found on local machine"
  fi
  echo "  - Install BusyBox extras or a netcat package"
  exit 1
fi

# ── Version check / deploy decision ───────────────────────────────────────────
# Use file-content hash (not git commit) so uncommitted local edits are deployed.
LOCAL_VER="$(hash_file_sha256 "$PROXY")"

REMOTE_PROXY_EXISTS="$(target_exec "test -f '$REMOTE_PROXY' && echo yes || echo no" 2>/dev/null || true)"
DEVICE_VER="$(target_exec "cat '$REMOTE_VER' 2>/dev/null || true" 2>/dev/null || true)"
PROXY_RUNNING="$(target_exec "ps aux 2>/dev/null | grep '[l]ogv-proxy\\|nc.*-lk.*-p.*$PORT' | head -1" 2>/dev/null || true)"

if [[ "$REMOTE_PROXY_EXISTS" != "yes" ]]; then
  echo "[run_wasm] Proxy missing on target (/tmp may have been cleared) -- deploying."
  SKIP_DEPLOY=0
elif [[ "$LOCAL_VER" == "$DEVICE_VER" && -n "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Proxy up-to-date and running (${LOCAL_VER:0:8}) -- reusing."
  SKIP_DEPLOY=1
elif [[ "$LOCAL_VER" == "$DEVICE_VER" && -z "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Proxy up-to-date (${LOCAL_VER:0:8}) but not running -- will start it."
  SKIP_DEPLOY=1
else
  echo "[run_wasm] New version (${LOCAL_VER:0:8} vs target ${DEVICE_VER:0:8}) -- deploying."
  SKIP_DEPLOY=0
fi

if [[ "$SKIP_DEPLOY" -eq 0 ]]; then
  if [[ "$EXEC_MODE" == "ssh" ]]; then
    echo "[run_wasm] Copying logv-proxy.py -> $TARGET:$REMOTE_PROXY ..."
  else
    echo "[run_wasm] Copying logv-proxy.py -> $REMOTE_PROXY ..."
  fi
  if ! target_copy_proxy; then
    echo "[run_wasm] ERROR: proxy copy failed"
    exit 1
  fi
  target_exec "echo '$LOCAL_VER' > '$REMOTE_VER'" >/dev/null 2>&1 || true
fi

# ── Restart proxy ──────────────────────────────────────────────────────────────
if [[ -n "$PROXY_RUNNING" ]]; then
  echo "[run_wasm] Stopping existing proxy ..."
  target_exec "
    PIDS=\$(ps aux 2>/dev/null | grep '[l]ogv-proxy\|nc.*-lk.*-p.*$PORT' | awk '{print \$2}' | sort -u)
    if [ -n \"\$PIDS\" ]; then
      echo \$PIDS | xargs kill -TERM 2>/dev/null || true
      sleep 0.4
    fi
    true
  " 2>/dev/null || true
fi

echo "[run_wasm] Starting proxy on $HOST port $PORT ..."
if [[ "$EXEC_MODE" == "ssh" ]]; then
  "${SSH_CMD[@]}" -f -o ConnectTimeout=5 "$TARGET" "bash -c 'nohup python3 $REMOTE_PROXY $PORT > $REMOTE_LOG 2>&1 &'"
else
  bash -c "nohup python3 '$REMOTE_PROXY' '$PORT' > '$REMOTE_LOG' 2>&1 &"
fi
sleep 1.0

if target_exec "grep -q 'Listening on port' '$REMOTE_LOG' 2>/dev/null || cat '$REMOTE_LOG' 2>/dev/null | grep -qi 'error\\|fatal\\|traceback'" 2>/dev/null; then
  PROXY_ERR="$(target_exec "grep -i 'error\\|fatal\\|traceback' '$REMOTE_LOG' 2>/dev/null | head -3" 2>/dev/null || true)"
  if [[ -n "$PROXY_ERR" ]]; then
    echo "[run_wasm] WARNING: proxy may have errors:"
    echo "$PROXY_ERR"
    if [[ "$EXEC_MODE" == "ssh" ]]; then
      echo "  Full log: ssh $TARGET 'cat $REMOTE_LOG'"
    else
      echo "  Full log: cat $REMOTE_LOG"
    fi
  fi
fi

# ── Open the viewer in the browser ────────────────────────────────────────────
FILE_URL="file://${HTML}?host=${HOST}&port=${PORT}"
echo "[run_wasm] Opening viewer ..."

HTTP_PIDFILE="/tmp/logv-wasm-http.pid"

if grep -qi microsoft /proc/version 2>/dev/null; then
  if [[ -f "$HTTP_PIDFILE" ]]; then
    OLD_PID="$(cat "$HTTP_PIDFILE" 2>/dev/null || true)"
    if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
      echo "[run_wasm] Stopping previous HTTP server (pid $OLD_PID)..."
      kill "$OLD_PID" 2>/dev/null || true
    fi
    rm -f "$HTTP_PIDFILE"
  fi

  HTTP_PORT=$((8750 + RANDOM % 200))
  SERVE_DIR="$(dirname "$HTML")"
  HTML_NAME="$(basename "$HTML")"

  python3 -m http.server "$HTTP_PORT" --directory "$SERVE_DIR" --bind 127.0.0.1 >/dev/null 2>&1 &
  HTTP_PID=$!
  echo "$HTTP_PID" > "$HTTP_PIDFILE"

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
    (sleep 60 && kill "$HTTP_PID" 2>/dev/null && rm -f "$HTTP_PIDFILE") &
  fi
elif command -v xdg-open &>/dev/null && xdg-open "$FILE_URL" 2>/dev/null; then
  :
elif command -v open &>/dev/null; then
  open "$FILE_URL"
else
  for B in google-chrome google-chrome-stable chromium chromium-browser firefox; do
    if command -v "$B" &>/dev/null; then "$B" "$FILE_URL" & break; fi
  done || echo "[run_wasm] Could not open browser. Open manually: $HTML"
fi

if [[ "$EXEC_MODE" == "ssh" ]]; then
  echo "[run_wasm] Done.  Proxy log: ssh $TARGET 'tail -f $REMOTE_LOG'"
else
  echo "[run_wasm] Done.  Proxy log: tail -f $REMOTE_LOG"
fi
