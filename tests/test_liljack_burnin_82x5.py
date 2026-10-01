#!/usr/bin/env python3
"""82x5 proof — the tmux behaviour that squeezed the operator's live agents must be
structurally impossible on the native backend.

tmux sizes a window to its SMALLEST attached client, so one 82x5 view resized
every other view's pty. The native supervisor's answer: the pty size is a single
authoritative value, changed ONLY by an explicit `resize` op. `attach` and
`detach` carry no size, so merely connecting a smaller client cannot shrink the
pty. This script shows that, empirically, by reading the size back from INSIDE
the session (`stty size`) — not from the supervisor's own bookkeeping.

Usage: python3 tests/test_liljack_burnin_82x5.py   (spawns its own supervisor)
"""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def connect(sock):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    s.settimeout(5)
    return s


def ctl(s, o):
    b = json.dumps(o).encode()
    s.sendall(struct.pack(">I", 1 + len(b)) + b"C" + b)


def read_frame(s):
    hdr = b""
    while len(hdr) < 4:
        c = s.recv(4 - len(hdr))
        if not c:
            raise EOFError
        hdr += c
    n = struct.unpack(">I", hdr)[0]
    tag = s.recv(1)
    body = b""
    while len(body) < n - 1:
        c = s.recv(n - 1 - len(body))
        if not c:
            break
        body += c
    return tag, body


def read_control(s):
    while True:
        t, b = read_frame(s)
        if t == b"C":
            return json.loads(b.decode())


def pty_size(sock, sid):
    """Read the real pty size from inside the session. The shell echoes the typed
    command, so there is no echo-immune marker; instead take the LAST 'rows cols'
    line of the stream — the replay ring carries stale sizes, and the freshest
    line is the one written by this stty call."""
    sc = connect(sock)
    ctl(sc, {"op": "attach", "session": sid})
    read_frame(sc)                      # F frame (fd discarded)
    ctl(sc, {"op": "send", "session": sid, "data": "stty size\n"})
    got = b""
    deadline = time.time() + 2
    while time.time() < deadline:
        try:
            tag, body = read_frame(sc)
            if tag == b"O":
                got += body
        except Exception:
            break
    sc.close()
    matches = re.findall(r"(?m)^\s*(\d+)\s+(\d+)\s*$", got.decode("utf-8", "replace"))
    return (int(matches[-1][0]), int(matches[-1][1])) if matches else None


def main():
    tmp = tempfile.mkdtemp(prefix="lj-burnin-")
    sock = os.path.join(tmp, "s.sock")
    daemon = subprocess.Popen(
        [sys.executable, str(ROOT / "toolbox" / "liljack_sessiond.py"),
         "--sock", sock, "--journal", os.path.join(tmp, "j.db")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 5
        while not os.path.exists(sock) and time.time() < deadline:
            time.sleep(0.05)
        c = connect(sock)
        ctl(c, {"op": "create", "cmd": ["sh"], "cwd": tmp, "cols": 120, "rows": 40})
        sid = read_control(c)["session_id"]
        time.sleep(0.5)

        rows, cols = pty_size(sock, sid)
        print(f"1. created 120x40                  -> pty rows x cols = {rows} x {cols}")
        assert (rows, cols) == (40, 120), "create size not honoured"

        a = connect(sock)
        ctl(a, {"op": "attach", "session": sid}); read_frame(a)
        b = connect(sock)
        ctl(b, {"op": "attach", "session": sid}); read_frame(b)
        rows, cols = pty_size(sock, sid)
        print(f"2. two clients attached (no resize) -> pty = {rows} x {cols}")
        assert (rows, cols) == (40, 120), "attach must not resize the pty"

        # The tmux trap: B is the SMALLER view. Merely attaching it must not shrink.
        ctl(a, {"op": "resize", "session": sid, "cols": 120, "rows": 40}); read_control(a)
        ctl(b, {"op": "resize", "session": sid, "cols": 80, "rows": 24}); read_control(b)
        rows, cols = pty_size(sock, sid)
        print(f"3. A=120x40, B=80x24 (B smaller)   -> pty = {rows} x {cols}  (last explicit resize wins)")

        ctl(a, {"op": "resize", "session": sid, "cols": 120, "rows": 40}); read_control(a)
        rows, cols = pty_size(sock, sid)
        print(f"4. A back to 120x40, both attached -> pty = {rows} x {cols}")

        ctl(b, {"op": "detach", "session": sid}); read_control(b)
        rows, cols = pty_size(sock, sid)
        print(f"5. B detached (A stays)            -> pty = {rows} x {cols}")
        assert (rows, cols) == (40, 120), "detach must not resize the pty"

        print("\nSTRUCTURAL GUARANTEE: attach/detach never carry a size; the pty")
        print("size is one authoritative value changed only by an explicit resize op.")
        print("A smaller client attaching cannot shrink the pty — the tmux 82x5 trap")
        print("is absent by construction, not by avoidance.")
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=3)
        except subprocess.TimeoutExpired:
            daemon.kill()


if __name__ == "__main__":
    main()
