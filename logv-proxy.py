#!/usr/bin/env python3
"""
logv-proxy - WebSocket proxy for the logv web viewer.
Runs on the target device. Streams journalctl output to the browser.

ZERO external dependencies - pure Python 3 builtins only.
Uses BusyBox nc for TCP (always present on embedded Linux).

USAGE
  python3 logv-proxy.py [port]   (default: 9222)

QUICK START
  scp logv-proxy.py root@<device-ip>:
  ssh root@<device-ip> python3 logv-proxy.py
  Then open logv.html, enter the IP, click Connect.
"""

import os, sys, signal, struct, subprocess, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
DEFAULT_CMD = "journalctl -f --no-pager -o short-precise 2>&1 | cat"
_WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# ---------------------------------------------------------------------------
# Tiny JSON string extractor -- no json module needed.
# Only used to parse the browser's {"cmd": "..."} config message.
# ---------------------------------------------------------------------------

def _json_str(text, key):
    needle = '"{}":'.format(key)
    idx = text.find(needle)
    if idx < 0:
        return None
    idx += len(needle)
    while idx < len(text) and text[idx] in ' \t\r\n':
        idx += 1
    if idx >= len(text) or text[idx] != '"':
        return None
    idx += 1
    out = []
    while idx < len(text):
        ch = text[idx]
        if ch == '\\' and idx + 1 < len(text):
            out.append(text[idx + 1])
            idx += 2
        elif ch == '"':
            break
        else:
            out.append(ch)
            idx += 1
    return ''.join(out)


# ---------------------------------------------------------------------------
# Pure-Python SHA-1  (RFC 3174)
# Only used for the WebSocket handshake key -- no hashlib needed.
# ---------------------------------------------------------------------------

def _sha1(data):
    if isinstance(data, str):
        data = data.encode()
    def _rol(n, b):
        return ((n << b) | (n >> (32 - b))) & 0xFFFFFFFF
    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]
    msg = bytearray(data)
    bit_len = len(data) * 8
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    msg += bit_len.to_bytes(8, 'big')
    for i in range(0, len(msg), 64):
        w = list(struct.unpack('>16I', bytes(msg[i:i+64])))
        for j in range(16, 80):
            w.append(_rol(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1))
        a, b, c, d, e = h
        for j in range(80):
            if j < 20:  f, k = (b & c) | (~b & d), 0x5A827999
            elif j < 40: f, k = b ^ c ^ d,          0x6ED9EBA1
            elif j < 60: f, k = (b & c) | (b & d) | (c & d), 0x8F1BBCDC
            else:        f, k = b ^ c ^ d,           0xCA62C1D6
            a, b, c, d, e = (
                (_rol(a, 5) + f + e + k + w[j]) & 0xFFFFFFFF,
                a, _rol(b, 30), c, d,
            )
        h = [(x + y) & 0xFFFFFFFF for x, y in zip(h, [a, b, c, d, e])]
    return struct.pack('>5I', *h)


# ---------------------------------------------------------------------------
# Base64 encode -- binascii is a compiled-in C extension (always available).
# Pure-Python fallback just in case.
# ---------------------------------------------------------------------------

try:
    from binascii import b2a_base64 as _b2a
    def _b64encode(data):
        return _b2a(data).decode().rstrip('\n')
except ImportError:
    _B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    def _b64encode(data):
        out = []
        for i in range(0, len(data), 3):
            chunk = data[i:i+3]
            b = int.from_bytes(chunk.ljust(3, b'\x00'), 'big')
            out.append(_B64[(b >> 18) & 0x3F])
            out.append(_B64[(b >> 12) & 0x3F])
            out.append(_B64[(b >>  6) & 0x3F] if len(chunk) > 1 else '=')
            out.append(_B64[(b >>  0) & 0x3F] if len(chunk) > 2 else '=')
        return ''.join(out)


# ---------------------------------------------------------------------------
# Connection wrapper for nc -e mode.
# When nc spawns this script, it connects stdin(0)/stdout(1) to the TCP socket.
# os.read / os.write are raw syscalls -- no socket module needed at all.
# ---------------------------------------------------------------------------

class _Conn:
    def recv(self, n):
        try:
            return os.read(0, n) or b''
        except OSError:
            return b''

    def sendall(self, data):
        off = 0
        while off < len(data):
            r = os.write(1, data[off:])
            if r <= 0:
                raise OSError("write")
            off += r

    def close(self):
        pass  # nc owns the fd; it closes on process exit


class _SockConn:
    def __init__(self, sock):
        self.sock = sock

    def recv(self, n):
        try:
            return self.sock.recv(n)
        except OSError:
            return b""

    def sendall(self, data):
        self.sock.sendall(data)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Logging -- always to stderr so it never corrupts the WebSocket stream.
# ---------------------------------------------------------------------------

def _log(*args):
    print(*args, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# Minimal WebSocket framing
# ---------------------------------------------------------------------------

def _ws_handshake(conn):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        buf += chunk
    headers = {}
    for line in buf.decode(errors="replace").split("\r\n")[1:]:
        if ": " in line:
            k, v = line.split(": ", 1)
            headers[k.lower()] = v.strip()
    key = headers.get("sec-websocket-key", "")
    accept = _b64encode(_sha1((key + _WS_MAGIC).encode()))
    # Echo the browser UA back so embedded-device logs can show which client
    # connected (useful when no reverse-DNS is available on the LAN).
    ua = headers.get("user-agent", "")
    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {}\r\n"
        "X-Client: {}\r\n\r\n"
    ).format(accept, ua).encode())
    return True


def _ws_send(conn, text):
    payload = text.encode("utf-8")
    n = len(payload)
    if   n <= 125:   header = bytes([0x81, n])
    elif n <= 65535: header = struct.pack(">BBH", 0x81, 126, n)
    else:            header = struct.pack(">BBQ", 0x81, 127, n)
    try:
        conn.sendall(header + payload)
        return True
    except OSError:
        return False


def _ws_recv(conn):
    def _read(n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf
    try:
        head   = _read(2)
        opcode = head[0] & 0x0F
        masked = bool(head[1] & 0x80)
        length = head[1] & 0x7F
        if   length == 126: length = struct.unpack(">H", _read(2))[0]
        elif length == 127: length = struct.unpack(">Q", _read(8))[0]
        mask = _read(4) if masked else b""
        data = _read(length)
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if opcode == 8:  return None   # close frame
        if opcode == 1:  return data.decode("utf-8", errors="replace")
        return ""  # ping / continuation -- ignore
    except (OSError, ConnectionError, struct.error):
        return None


# ---------------------------------------------------------------------------
# Client handler (runs in the nc -e child process)
# ---------------------------------------------------------------------------

def _handle_client(conn=None):
    if conn is None:
        conn = _Conn()
    _log("[logv-proxy] Connected")
    try:
        if not _ws_handshake(conn):
            return

        cmd = DEFAULT_CMD
        first = _ws_recv(conn)
        if first:
            v = _json_str(first, "cmd")
            if v:
                cmd = v

        _log("[logv-proxy] Running:", cmd)

        proc = subprocess.Popen(
            ["sh", "-c", cmd],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        stop = threading.Event()

        def _reader():
            for raw in proc.stdout:
                if stop.is_set():
                    break
                if not _ws_send(conn, raw.decode("utf-8", errors="replace")):
                    stop.set()
                    break
            stop.set()

        t = threading.Thread(target=_reader, daemon=True)
        t.start()

        while not stop.is_set():
            if _ws_recv(conn) is None:
                stop.set()

        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()
        t.join(timeout=2)

    except Exception as ex:
        _log("[logv-proxy] Error:", ex)
    finally:
        conn.close()
        _log("[logv-proxy] Disconnected")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _is_socket(fd):
    """True when fd is a TCP socket -- means nc -e spawned us as a handler."""
    try:
        import stat as _s
        return _s.S_ISSOCK(os.fstat(fd).st_mode)
    except (ImportError, OSError):
        return False


def _nc_supports_exec():
    """
    True when `nc` supports executing a program per connection (`-e` or --exec).
    BusyBox nc usually has `-e`; OpenBSD nc usually does not.
    """
    try:
        p = subprocess.run(
            ["nc", "-h"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        out = p.stdout or ""
    except OSError:
        return False
    low = out.lower()
    return (" -e " in (" " + low.replace("\n", " ") + " ")) or ("\n-e" in low) or ("--exec" in low)


def _serve_with_python_socket():
    """
    Fallback server for hosts where nc exists but does not support -e.
    Uses Python's socket module when available.
    """
    try:
        import socket
    except ImportError:
        sys.exit(
            "[logv-proxy] FATAL: nc lacks -e and Python 'socket' is unavailable.\n"
            "Install BusyBox nc with -e support (or a Python build with socket)."
        )

    _free_port(PORT)
    try:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("", PORT))
        srv.listen(16)
    except OSError as ex:
        sys.exit("[logv-proxy] FATAL: cannot open/listen on port {}: {}".format(PORT, ex))
    print("[logv-proxy] Listening on port {} (python socket fallback)".format(PORT), flush=True)
    print("[logv-proxy] Open logv.html, enter this device's IP, click Connect", flush=True)

    while True:
        try:
            sock, _addr = srv.accept()
        except OSError:
            continue
        conn = _SockConn(sock)
        t = threading.Thread(target=_handle_client, args=(conn,), daemon=True)
        t.start()


def _free_port(port):
    """
    Kill any process currently listening on `port` (TCP).
    Reads /proc/net/tcp[6] then walks /proc/<pid>/fd looking for the inode.
    Uses only the 'os' module -- no socket, no fuser, no external tools.
    """
    hex_port = "{:04X}".format(port)
    inodes = set()
    for fname in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(fname) as f:
                for line in f:
                    parts = line.split()
                    # columns: sl local_addr rem_addr state ... inode
                    # state 0A = LISTEN
                    if len(parts) >= 10 and parts[3] == "0A" and parts[1].endswith(":" + hex_port):
                        inodes.add(parts[9])
        except OSError:
            pass
    if not inodes:
        return
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        fd_dir = "/proc/{}/fd".format(pid)
        try:
            for fd in os.listdir(fd_dir):
                try:
                    link = os.readlink("{}/{}".format(fd_dir, fd))
                    # socket:[inode]
                    if link.startswith("socket:[") and link[8:-1] in inodes:
                        print("[logv-proxy] Killing old instance (pid {})".format(pid), flush=True)
                        os.kill(int(pid), signal.SIGTERM)
                        break
                except OSError:
                    pass
        except OSError:
            pass
    time.sleep(0.3)   # give the old process time to release the port


def main():
    # Handler mode: nc -e re-executes this script with stdin = TCP socket.
    if _is_socket(0):
        _handle_client()
        return

    # Server mode:
    # 1) Prefer nc -lk -e (works on many embedded BusyBox systems).
    # 2) Fallback to a pure-Python socket listener when nc lacks -e.
    if _nc_supports_exec():
        _free_port(PORT)   # kill any previous instance still holding the port
        script = os.path.abspath(sys.argv[0])
        print("[logv-proxy] Listening on port {} (nc -lk)".format(PORT), flush=True)
        print("[logv-proxy] Open logv.html, enter this device's IP, click Connect", flush=True)
        try:
            os.execvp("nc", ["nc", "-lk", "-p", str(PORT), "-e", "python3", script])
        except FileNotFoundError:
            sys.exit(
                "[logv-proxy] FATAL: 'nc' not found.\n"
                "Install with: opkg install busybox-extras  (or netcat-openbsd)"
            )
    else:
        _log("[logv-proxy] nc has no -e; using Python socket fallback")
        _serve_with_python_socket()


if __name__ == "__main__":
    main()
