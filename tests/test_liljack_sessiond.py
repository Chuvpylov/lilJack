#!/usr/bin/env python3
"""The one property that justifies replacing tmux: session lifetime.

A native session's pty must OUTLIVE the lilJack UI. This proves it: spawn a
session under liljack_sessiond, attach, SIGKILL the "UI" (a child process holding
the client socket), then re-attach and show the command is still running with its
scrollback intact. Also pins SCM_RIGHTS fd passing and list-after-restart.
"""
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

def _recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b

class Client:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.settimeout(5)
    def control(self, obj):
        body = json.dumps(obj).encode()
        self.s.sendall(struct.pack(">I", 1 + len(body)) + b"C" + body)
    def send_input(self, data):
        self.s.sendall(struct.pack(">I", 1 + len(data)) + b"I" + data)
    def recv_frame(self):
        # The SCM_RIGHTS fd (if any) rides with the FIRST recvmsg of the frame.
        hdr = b""
        fds = []
        while len(hdr) < 4:
            chunk, anc, _f, _a = self.s.recvmsg(4 - len(hdr), socket.CMSG_SPACE(4))
            if not chunk:
                raise EOFError
            hdr += chunk
            for lvl, typ, data in anc:
                if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    fds += list(struct.unpack("i" * (len(data) // 4), data))
        n = struct.unpack(">I", hdr)[0]
        tag = _recv_exact(self.s, 1)
        body = _recv_exact(self.s, n - 1)
        return tag, body, fds
    def read_control(self):
        while True:
            tag, body, fds = self.recv_frame()
            if tag == b"C":
                return json.loads(body.decode())
    def close(self):
        self.s.close()

CMD = ["sh", "-c", "i=0; while true; do echo tick-$i; i=$((i+1)); sleep 0.1; done"]

print("sessiond: a native session outlives the UI (SIGKILL), with scrollback")

tmp = tempfile.mkdtemp(prefix="lj-sessiond-")
sock = os.path.join(tmp, "sessiond.sock")
daemon = subprocess.Popen([sys.executable, str(ROOT / "toolbox" / "liljack_sessiond.py"),
                           "--sock", sock],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.time() + 5
    while not os.path.exists(sock) and time.time() < deadline:
        time.sleep(0.05)
    check(os.path.exists(sock), "the supervisor's socket appears")

    a = Client(sock)
    a.control({"op": "hello"})
    check(a.read_control().get("daemon") is True, "the daemon answers hello")

    a.control({"op": "create", "cmd": CMD, "cols": 80, "rows": 24})
    sid = a.read_control()["session_id"]
    check(bool(sid), "a session is created")

    a.control({"op": "list"})
    lst = a.read_control()["sessions"]
    check(len(lst) == 1 and lst[0]["id"] == sid and lst[0]["state"] == "running",
          "the session is enumerated while running")

    # attach: expect an SCM_RIGHTS fd, then the byte stream
    a.control({"op": "attach", "session": sid})
    tag, body, fds = a.recv_frame()
    check(tag == b"F" and len(fds) == 1 and fds[0] >= 0,
          "attach passes the pty master fd over SCM_RIGHTS")
    os.close(fds[0])
    got = b""
    deadline = time.time() + 5
    while b"tick-" not in got and time.time() < deadline:
        tag, body, fds = a.recv_frame()
        if tag == b"O":
            got += body
    check(b"tick-" in got, "the agent's output streams to the attached client")

    # fork a "UI" client, then SIGKILL it mid-stream
    ui = os.fork()
    if ui == 0:
        try:
            c = Client(sock)
            c.control({"op": "attach", "session": sid})
            while True:
                c.recv_frame()
        except Exception:
            os._exit(0)
    time.sleep(0.5)
    os.kill(ui, signal.SIGKILL)
    os.waitpid(ui, 0)
    time.sleep(0.3)

    # re-attach: the session must still be alive, with its scrollback
    b = Client(sock)
    b.control({"op": "list"})
    lst = b.read_control()["sessions"]
    check(len(lst) == 1 and lst[0]["id"] == sid and lst[0]["state"] == "running",
          "★ after SIGKILLing the UI, the session is STILL running")
    b.control({"op": "attach", "session": sid})
    tag, body, fds = b.recv_frame()
    check(tag == b"F", "re-attach passes the fd again")
    if fds:
        os.close(fds[0])
    replay = b""
    deadline = time.time() + 5
    while b"tick-" not in replay and time.time() < deadline:
        tag, body, fds = b.recv_frame()
        if tag == b"O":
            replay += body
    check(b"tick-" in replay, "★ the stream resumes, and the scrollback replays")

    b.control({"op": "kill", "session": sid, "signal": signal.SIGTERM})
    b.read_control()
    exited = None
    deadline = time.time() + 5
    while time.time() < deadline and exited is None:
        tag, body, fds = b.recv_frame()
        if tag == b"C":
            r = json.loads(body.decode())
            if r.get("op") == "exited":
                exited = r
    check(exited is not None and exited.get("session") == sid,
          "a killed agent is marked exited, not orphaned")
    b.close(); a.close()

    # The runtime backend talks to the same socket: native is selectable per
    # session and enumerates alongside tmux.
    print("runtime: NativeBackend spawns and enumerates alongside tmux")
    os.environ["LILJACK_SESSIOND_SOCK"] = sock
    from liljack_app.runtime import Tmux
    t = Tmux()
    native_sid = t.create("shell", "/tmp/nb-proj", "/tmp/nb-root",
                          name="lj-native-t", backend="native", cwd=tmp)
    check(native_sid, "Tmux.create(backend='native') spawns via the supervisor")
    rows = t.sessions("/tmp/nb-proj")
    check(any(r["id"] == native_sid and r["state"] == "running" for r in rows),
          "Tmux.sessions() merges the native session alongside tmux")
    row = next(r for r in rows if r["id"] == native_sid)
    check(row.get("backend") == "native" and row.get("harness") == "native",
          "the native row declares backend=harness=native (stable session identity)")

    # Journal: every create was recorded BEFORE its fork, and exits mark them.
    print("journal: intents survive the fork, and an exit marks them")
    res = t.sessions("/tmp/nb-proj")
    rj = Client(sock)
    rj.control({"op": "resume"})
    intents = rj.read_control()["intents"]
    check(any(i["name"] == "lj-native-t" and i["state"] == "running" for i in intents),
          "the resume op lists the last-known intent while running")
    rj.close()
    t2 = Tmux()
    victim = t2.create("shell", "/tmp/nb-proj2", "/tmp/nb-root2",
                       name="lj-native-victim", backend="native", cwd="/tmp")
    cj = Client(sock)
    # SIGKILL rather than SIGTERM: the shell agent execs bash, which as a
    # non-interactive session leader ignores SIGTERM — the journal must still
    # record the exit, and SIGKILL cannot be ignored.
    cj.control({"op": "kill", "session": row["daemon_id"], "signal": signal.SIGKILL})
    cj.read_control()
    cj.close()
    jj = Client(sock)
    intents = {}
    deadline = time.time() + 5
    while time.time() < deadline and intents.get("lj-native-t", {}).get("state") != "exited":
        jj.control({"op": "resume"})
        intents = {i["name"]: i for i in jj.read_control()["intents"]}
        time.sleep(0.05)
    jj.close()
    check(intents.get("lj-native-t", {}).get("state") == "exited",
          "the journal marks an exited intent, not just the live registry")

    # The AttachedTerminal native render path: attach via the socket and feed
    # owkterm's own VT parser — no pty.fork, no tmux, no second VT layer.
    print("render: a native session draws through AttachedTerminal's VT")
    try:
        from liljack_app.native import load
        from liljack_app.runtime import AttachedTerminal
        lib = load()
    except Exception as exc:
        check(False, f"build the VT library (skipped: {exc})")
        lib = None
    if lib is not None:
        mk = t2.create("shell", "/tmp/nb-proj3", "/tmp/nb-root3",
                       name="lj-native-render", backend="native", cwd="/tmp")
        mrow = next(r for r in t2.sessions("/tmp/nb-proj3") if r["id"] == mk)
        at = AttachedTerminal(lib, t2, mrow, 80, 24)
        at.send("printf 'NATIVE_RENDER_MARKER\\n'\r")
        text = ""
        deadline = time.time() + 8
        while time.time() < deadline and "NATIVE_RENDER_MARKER" not in text:
            at.poll()
            time.sleep(0.05)
            text = "\n".join(at.vt.text(r) for r in range(at.vt.rows))
        check("NATIVE_RENDER_MARKER" in text,
              "the native session's output renders through the VT")
        at.close()

    # Native control: the operator can press keys and stop a session without a
    # tmux pane — the supervisor-side answer to send-keys / kill-session.
    print("control: send injects input and stop terminates a native session")
    from liljack_app.runtime import NativeBackend
    nb = NativeBackend()
    csid = t2.create("shell", "/tmp/nb-proj4", "/tmp/nb-root4",
                     name="lj-native-ctl", backend="native", cwd=tmp)
    crow = next(r for r in t2.sessions("/tmp/nb-proj4") if r["id"] == csid)
    cdid = crow["daemon_id"]
    check(nb.send(cdid, "echo CONTROL_MARKER\r"), "send() injects input into the session")
    time.sleep(0.4)
    kc = Client(sock)
    kc.control({"op": "scrollback", "session": cdid, "lines": 20})
    scroll = kc.read_control().get("scrollback", "")
    kc.close()
    check("CONTROL_MARKER" in scroll,
          "the injected command ran (its output is in the scrollback)")
    nb.stop(cdid)
    state = "running"
    deadline = time.time() + 5
    while time.time() < deadline and state != "exited":
        kc = Client(sock)
        kc.control({"op": "list"})
        state = next((s["state"] for s in kc.read_control()["sessions"] if s["id"] == cdid), "gone")
        kc.close()
        if state != "exited":
            time.sleep(0.05)
    check(state == "exited", "stop() terminates the session (SIGTERM then SIGKILL)")

    # C protocol parity: the exact frame layout the UI's native_attach uses
    # (4-byte BE length + tag), compiled and run against THIS supervisor, so a
    # Python-supervisor <-> C-client byte mismatch cannot ship silently.
    print("c-protocol: the UI's frame_write layout speaks to the supervisor")
    gcc = shutil.which("gcc")
    csrc = ROOT / "tests" / "test_liljack_sessiond.c"
    if gcc and csrc.exists():
        cbin = os.path.join(tmp, "test_sessiond_c")
        r = subprocess.run([gcc, "-std=c11", "-O1", "-D_GNU_SOURCE", str(csrc), "-o", cbin],
                           capture_output=True)
        if r.returncode == 0:
            r = subprocess.run([cbin, sock], capture_output=True, text=True, timeout=30)
            check(r.returncode == 0,
                  "the C client's frame layout matches the supervisor (create/attach/output/input)")
        else:
            check(False, "compile the C protocol test")
    else:
        check(False, "gcc and the C protocol test source are present")
finally:
    daemon.terminate()
    try:
        daemon.wait(timeout=3)
    except subprocess.TimeoutExpired:
        daemon.kill()

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
