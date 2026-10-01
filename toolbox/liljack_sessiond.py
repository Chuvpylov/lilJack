#!/usr/bin/env python3
"""liljack_sessiond — the native session supervisor (tmux replacement, opt-in).

Owns every agent pty master fd so an agent's session OUTLIVES the lilJack UI.
The UI (or `liljack attach`) connects to one unix socket, and the supervisor is
the sole reader of each pty master: it appends every byte to a per-session
scrollback ring and forwards the same bytes to every attached client. A client
sends input back; the supervisor writes it to the master. This is the one shape
where (a) the UI gets live bytes, (b) scrollback survives UI death, and (c) a
second client can attach without missing a byte.

Frame: 4-byte big-endian length, then a 1-byte tag + body.
  C  control (JSON)   O  output bytes (supervisor -> client)
  I  input bytes      F  SCM_RIGHTS fd (JSON body names the fd)
Control ops: create list attach detach resize kill scrollback hello.
Stdlib only. Run as a user systemd unit (linger) so it outlives the UI.
"""
from __future__ import annotations

import argparse
import errno
import json
import os
import select
import signal
import socket
import sqlite3
import struct
import time
import uuid
from pathlib import Path

RING_CAP = 1024 * 1024            # 1 MiB of scrollback bytes per session

DEFAULT_SOCK = str(Path(os.environ.get("XDG_RUNTIME_DIR",
                                       str(Path.home() / ".cache/liljack"))) / "sessiond.sock")

DEFAULT_JOURNAL = str(Path(os.environ.get("LILJACK_CACHE",
                                          str(Path.home() / ".cache/liljack"))) / "sessiond-journal.db")


class Journal:
    """Durable intent journal: the last-known set after a reboot.

    A pty does not survive a reboot and neither does the supervisor's in-memory
    registry. This SQLite sidecar records each `create` (argv/env/cwd) BEFORE the
    fork, so after a reboot the lilJack app can offer "resume the sessions that
    were running" — the same idempotency reserve_room_start already gives rooms.
    Enumeration is then two layers: the LIVE set (the daemon's `list`) and the
    LAST-KNOWN set (this journal), which is what the UI shows after a reboot.
    """

    def __init__(self, path):
        self.path = path
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, timeout=5)
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS intents ("
            "id TEXT PRIMARY KEY, name TEXT, cmd TEXT, env TEXT, cwd TEXT,"
            "cols INTEGER, rows INTEGER, created_at TEXT,"
            "state TEXT, exit_status INTEGER)")
        self.db.commit()

    def record(self, sid, name, cmd, env, cwd, cols, rows):
        self.db.execute(
            "INSERT OR REPLACE INTO intents VALUES(?,?,?,?,?,?,?,?,?,?)",
            (sid, name, json.dumps(cmd), json.dumps(env), cwd, cols, rows,
             time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "running", None))
        self.db.commit()

    def mark_exited(self, sid, status):
        self.db.execute("UPDATE intents SET state='exited', exit_status=? WHERE id=?",
                        (status, sid))
        self.db.commit()

    def last_known(self):
        rows = self.db.execute(
            "SELECT id,name,cmd,env,cwd,cols,rows,state,exit_status FROM intents "
            "ORDER BY created_at").fetchall()
        return [{"id": r[0], "name": r[1], "cmd": json.loads(r[2]), "env": json.loads(r[3]),
                 "cwd": r[4], "cols": r[5], "rows": r[6], "state": r[7],
                 "exit_status": r[8]} for r in rows]

    def close(self):
        self.db.close()


class Session:
    def __init__(self, sid, pid, fd, cmd, cwd, cols, rows, env, meta):
        self.sid = sid
        self.pid = pid
        self.fd = fd
        self.cmd = cmd
        self.cwd = cwd
        self.cols = cols
        self.rows = rows
        self.env = env
        self.meta = meta            # agent / project / harness / name, for the UI
        self.ring = bytearray()
        self.clients = set()          # attached client sockets
        self.exited = False
        self.exit_status = None

    def append(self, data):
        self.ring += data
        if len(self.ring) > RING_CAP:
            del self.ring[:len(self.ring) - RING_CAP]

    def describe(self):
        return {"id": self.sid, "pid": self.pid, "cmd": self.cmd, "cwd": self.cwd,
                "cols": self.cols, "rows": self.rows, "state": "exited" if self.exited else "running",
                "exited": self.exited, "exit_status": self.exit_status,
                "agent": self.meta.get("agent", ""), "project": self.meta.get("project", ""),
                "harness": self.meta.get("harness", ""), "name": self.meta.get("name", "")}


class Sessiond:
    def __init__(self, sock_path, journal_path=None):
        self.sock_path = sock_path
        self.journal = Journal(journal_path or DEFAULT_JOURNAL)
        self.sessions = {}
        self.client_session = {}       # client socket -> session id it attached to
        self.conns = set()             # every connected client socket
        self.listen = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass
        self.listen.bind(sock_path)
        self.listen.listen(16)
        self.running = True

    # ── framing ────────────────────────────────────────────────────────────
    @staticmethod
    def _recv_exact(sock, n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    @staticmethod
    def send_frame(sock, tag, body):
        payload = tag + body
        sock.sendall(struct.pack(">I", len(payload)) + payload)

    @staticmethod
    def send_fd(sock, fd, tag, body):
        payload = tag + body
        hdr = struct.pack(">I", len(payload))
        sock.sendmsg([hdr + payload],
                     [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))])

    @staticmethod
    def recv_frame(sock):
        n = struct.unpack(">I", Sessiond._recv_exact(sock, 4))[0]
        payload = Sessiond._recv_exact(sock, n)
        return payload[:1], payload[1:]

    # ── operations ─────────────────────────────────────────────────────────
    def op_create(self, req):
        cmd = req.get("cmd")
        if not isinstance(cmd, list) or not cmd or not all(isinstance(c, str) for c in cmd):
            return {"error": "create needs a non-empty cmd list"}
        cwd = str(req.get("cwd") or ".")
        cols = int(req.get("cols") or 80)
        rows = int(req.get("rows") or 24)
        env = dict(req.get("env") or {})
        meta = {k: str(req.get(k) or "") for k in ("agent", "project", "harness", "name")}
        sid = "s-" + uuid.uuid4().hex[:12]
        # Record the intent BEFORE the fork so a reboot cannot strand the argv.
        self.journal.record(sid, meta["name"] or sid, cmd, env, cwd, cols, rows)
        pid, fd = os.forkpty()
        if pid == 0:
            # child: the agent, on the pty slave as its controlling terminal
            os.environ.clear()
            os.environ.update(env)
            os.chdir(cwd)
            try:
                os.execvpe(cmd[0], cmd, os.environ)
            except OSError as exc:
                os.write(2, f"sessiond: exec failed: {exc}\n".encode())
            os._exit(127)
        os.set_blocking(fd, False)
        self.sessions[sid] = Session(sid, pid, fd, cmd, cwd, cols, rows, env, meta)
        self._winsize(fd, cols, rows)
        return {"session_id": sid}

    @staticmethod
    def _winsize(fd, cols, rows):
        import fcntl, termios
        try:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        except OSError:
            pass

    def op_list(self, req):
        return {"sessions": [s.describe() for s in self.sessions.values()]}

    def op_attach(self, req, client):
        s = self.sessions.get(req.get("session"))
        if s is None:
            return {"error": "no such session"}
        self.send_fd(client, s.fd, b"F", json.dumps({"session": s.sid, "cols": s.cols,
                                                      "rows": s.rows, "ring": len(s.ring)}).encode())
        # Replay the ring tail, then this client goes live.
        if s.ring:
            self.send_frame(client, b"O", bytes(s.ring))
        s.clients.add(client)
        self.client_session[client] = s.sid
        return None                     # handled inline; no JSON response

    def op_detach(self, req, client):
        s = self.sessions.get(req.get("session"))
        if s is not None:
            s.clients.discard(client)
        self.client_session.pop(client, None)
        return {"ok": True}

    def op_resize(self, req):
        s = self.sessions.get(req.get("session"))
        if s is None:
            return {"error": "no such session"}
        s.cols = int(req.get("cols") or s.cols)
        s.rows = int(req.get("rows") or s.rows)
        self._winsize(s.fd, s.cols, s.rows)
        return {"ok": True}

    def op_kill(self, req):
        s = self.sessions.get(req.get("session"))
        if s is None:
            return {"error": "no such session"}
        sig = int(req.get("signal") or signal.SIGTERM)
        try:
            os.kill(s.pid, sig)
        except ProcessLookupError:
            pass
        return {"ok": True}

    def op_scrollback(self, req):
        s = self.sessions.get(req.get("session"))
        if s is None:
            return {"error": "no such session"}
        lines = int(req.get("lines") or 500)
        return {"session": s.sid, "scrollback": bytes(s.ring[-lines * 200:]).decode("utf-8", "replace")}

    def op_send(self, req):
        """Inject bytes into a session's pty master — the supervisor-side answer
        to tmux send-keys, so the operator can press Enter / Ctrl-C in a native
        session that has no attached client."""
        s = self.sessions.get(req.get("session"))
        if s is None:
            return {"error": "no such session"}
        data = req.get("data")
        if isinstance(data, str):
            data = data.encode()
        if not isinstance(data, (bytes, bytearray)):
            return {"error": "send needs a data string"}
        try:
            os.write(s.fd, bytes(data))
        except OSError:
            pass
        return {"ok": True}

    def op_resume(self, req):
        return {"intents": self.journal.last_known()}

    def handle_control(self, client, body):
        try:
            req = json.loads(body.decode("utf-8", "replace"))
        except (ValueError, UnicodeDecodeError):
            self.send_frame(client, b"C", json.dumps({"error": "bad json"}).encode())
            return
        op = req.get("op")
        if op == "create":
            resp = self.op_create(req)
        elif op == "list":
            resp = self.op_list(req)
        elif op == "attach":
            resp = self.op_attach(req, client)
        elif op == "detach":
            resp = self.op_detach(req, client)
        elif op == "resize":
            resp = self.op_resize(req)
        elif op == "kill":
            resp = self.op_kill(req)
        elif op == "scrollback":
            resp = self.op_scrollback(req)
        elif op == "send":
            resp = self.op_send(req)
        elif op == "resume":
            resp = self.op_resume(req)
        elif op == "hello":
            resp = {"daemon": True, "version": 1}
        else:
            resp = {"error": f"unknown op {op!r}"}
        if resp is not None:
            self.send_frame(client, b"C", json.dumps(resp).encode())

    def reap(self):
        for s in list(self.sessions.values()):
            if s.exited:
                continue
            try:
                pid, status = os.waitpid(s.pid, os.WNOHANG)
            except ChildProcessError:
                pid, status = 0, 0
            if pid == 0:
                continue
            s.exited = True
            s.exit_status = status
            self.journal.mark_exited(s.sid, status)
            note = json.dumps({"op": "exited", "session": s.sid,
                               "exit_status": status}).encode()
            for c in list(s.clients):
                try:
                    self.send_frame(c, b"C", note)
                except OSError:
                    s.clients.discard(c)

    def pump(self, client):
        """One readable client socket: raw input bytes or a control frame."""
        try:
            tag, body = self.recv_frame(client)
        except (EOFError, OSError):
            # A client died. Detach it from every session it was attached to.
            self.client_session.pop(client, None)
            for s in self.sessions.values():
                s.clients.discard(client)
            return False
        if tag == b"I":
            sid = self.client_session.get(client)
            s = self.sessions.get(sid)
            if s is not None:
                try:
                    os.write(s.fd, body)
                except OSError:
                    pass
            return True
        if tag == b"C":
            try:
                self.handle_control(client, body)
            except OSError:
                # The client died while we were replying (e.g. it sent detach and
                # closed its socket before the response). Never let a broken pipe
                # kill the supervisor — detach and let the caller reap the client.
                return False
            return True
        return True

    def run(self):
        while self.running:
            r, _, _ = select.select([self.listen]
                                    + [s.fd for s in self.sessions.values() if not s.exited]
                                    + list(self.conns),
                                    [], [], 1.0)
            for obj in r:
                if obj is self.listen:
                    conn, _ = self.listen.accept()
                    self.conns.add(conn)
                elif isinstance(obj, socket.socket):
                    if not self.pump(obj):
                        self.conns.discard(obj)
                        self.client_session.pop(obj, None)
                        for s in self.sessions.values():
                            s.clients.discard(obj)
                        try:
                            obj.close()
                        except OSError:
                            pass
                else:
                    # a session master fd: agent wrote output
                    s = next((x for x in self.sessions.values() if x.fd == obj), None)
                    if s is None:
                        continue
                    try:
                        data = os.read(obj, 65536)
                    except (BlockingIOError, OSError):
                        continue
                    if not data:
                        continue          # EOF; waitpid handles the exit
                    s.append(data)
                    for c in list(s.clients):
                        try:
                            self.send_frame(c, b"O", data)
                        except OSError:
                            s.clients.discard(c)
            self.reap()


def client_attach(sock_path, session=None):
    """`liljack attach` — reattach to a native session from a plain terminal.

    Connects to the supervisor, attaches to `session` (or lists when omitted),
    puts stdin in raw mode, and shuttles bytes both ways until Ctrl-C. This is
    the same protocol the UI uses, so it doubles as the supervisor's debug tool.
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)

    def control(obj):
        body = json.dumps(obj).encode()
        s.sendall(struct.pack(">I", 1 + len(body)) + b"C" + body)

    if session is None:
        control({"op": "list"})
        tag, body = Sessiond.recv_frame(s)
        if tag != b"C":
            raise SystemExit("unexpected response")
        sessions = json.loads(body.decode()).get("sessions", [])
        if not sessions:
            raise SystemExit("no native sessions running")
        print("\n".join(f"{x['id']}  {x.get('agent') or x['cmd'][0]}  {x['state']}  {x['cwd']}"
                        for x in sessions))
        return

    control({"op": "attach", "session": session})
    # drain the SCM_RIGHTS fd (ignore it; the byte stream is what we render)
    tag, body = Sessiond.recv_frame(s)
    if tag == b"F":
        pass

    import termios as _termios
    old = _termios.tcgetattr(0)
    try:
        raw = _termios.tcgetattr(0)
        raw[3] &= ~(_termios.ICANON | _termios.ECHO | _termios.ISIG)
        raw[6][_termios.VMIN] = 1
        raw[6][_termios.VTIME] = 0
        _termios.tcsetattr(0, _termios.TCSANOW, raw)
        while True:
            r, _, _ = select.select([s, 0], [], [])
            if s in r:
                tag, body = Sessiond.recv_frame(s)
                if tag == b"O":
                    os.write(1, body)
                elif tag == b"C":
                    msg = json.loads(body.decode())
                    if msg.get("op") == "exited":
                        break
            if 0 in r:
                data = os.read(0, 4096)
                if not data:
                    break
                s.sendall(struct.pack(">I", 1 + len(data)) + b"I" + data)
    except (EOFError, OSError, KeyboardInterrupt):
        pass
    finally:
        _termios.tcsetattr(0, _termios.TCSANOW, old)
        s.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description="lilJack native session supervisor")
    ap.add_argument("--sock", default=DEFAULT_SOCK, help="unix socket path")
    ap.add_argument("--journal", default=None, help="intent journal path (SQLite)")
    ap.add_argument("--attach", metavar="SESSION", nargs="?", const="",
                    help="attach to a native session (list when omitted) instead of serving")
    a = ap.parse_args(argv)
    if a.attach is not None:
        client_attach(a.sock, a.attach or None)
        return
    Sessiond(a.sock, journal_path=a.journal).run()


if __name__ == "__main__":
    main()
