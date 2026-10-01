#!/usr/bin/env python3
"""liljack_agentd.py — the agent runner daemon. SPAWNS, OWNS and CONTROLS
lilJack's three agents over a user-only unix socket.

    socket   ~/.cache/liljack/agentd.sock   (mode 0600; no TCP, no ring token —
             the socket limits access to the user; it does not identify agents)
    wire     newline-delimited JSON, one request → one response line, except
             `events`, which keeps the connection and streams one JSON line per
             event until the client hangs up.

Mutations bind identity once per connection using Linux SO_PEERCRED and the
peer's launch environment. Set LILJACK_AGENT=operator when launching the operator
client; harness clients launch with their own LILJACK_AGENT. Missing identity
is read-only. `from`/`who` are optional assertions, never authentication.
Spawn/stop/kill/take_control/shutdown are operator-only. Same-UID hostile code
can choose its launch environment: this is not OS isolation between agents.

Ops (THE CONTRACT the chat UI and liljack_roles are built against):

    {"op":"list"}                       → {"ok":true,"agents":[{name,role,state,pid,backend,control,transcript}]}
    {"op":"spawn","agent":A,"args":{}}  → {"ok":true,"agent":A,"pid":N,"backend":B,"cmd":[...]}
    {"op":"stop","agent":A}             → {"ok":true,...how it died...}      SIGTERM → SIGKILL after grace
    {"op":"kill","agent":A}             → {"ok":true,...}                    SIGKILL now
    {"op":"send","agent":A,"text":T}    → {"ok":true,"event":{ts,dir:"in",text}}
                                          REFUSED unless `from` is the operator
                                          or the current control holder.
    {"op":"tail","agent":A,"n":50}      → {"ok":true,"events":[{ts,dir,text}]}
    {"op":"control"}                    → {"ok":true,"holder":H}
    {"op":"take_control","who":W}       → {"ok":true,"holder":W}            operator only
    {"op":"give_control","from":F,"to":T}→{"ok":true,"holder":T}            F must hold it AND may_control(F,T)
    {"op":"events"}                     → stream: {"event":"spawn|exit|state|control|output|send",...}

Extras (not in the contract, harmless): `ping`, `matrix` (backend matrix),
`shutdown` (operator only).

Control is ONE variable behind ONE lock (`Daemon._ctl_lock`); every mutation
goes through `_set_holder`, so "exactly one holder" is structural, not a
convention. Who may hand control to whom comes from `liljack_roles.may_control`
when that module exists; until it does, `_FALLBACK_MAY_CONTROL` applies:
operator→any, claude→codex|deepseek, codex→claude, deepseek→nobody.

Every running agent is registered with `liljack_sessions.register_source` (the
`agentd` harness, rows built from `agentd.state.json` so a heartbeat in ANOTHER
process sees them too via `LILJACK_SESSION_SOURCES=liljack_agentd`), and every
control change is recorded on the board as `set_note("control", holder, …)`.
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import signal
import socket
import struct
import sys
import threading
import time
from pathlib import Path

TOOLBOX = Path(__file__).resolve().parent
PROJECT_ROOT = TOOLBOX.parent
if str(TOOLBOX) not in sys.path:
    sys.path.insert(0, str(TOOLBOX))

import liljack_agents as A            # noqa: E402

OPERATOR = "operator"
STATE_FILE = "agentd.state.json"
SOCK_NAME = "agentd.sock"
PID_NAME = "agentd.pid"
_NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")

_FALLBACK_MAY_CONTROL = {
    OPERATOR: None,                      # None = anyone
    "claude": {"codex", "deepseek"},
    "codex": {"claude"},
    "deepseek": set(),
}


def cache_root() -> Path:
    return Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))


def socket_path(root=None) -> Path:
    return Path(root or cache_root()) / SOCK_NAME


def may_control(frm: str, to: str) -> bool:
    """Roles module first; hard-coded table until it lands."""
    try:
        import liljack_roles                              # being written now
        fn = getattr(liljack_roles, "may_control", None)
        if callable(fn):
            return bool(fn(frm, to))
    except ImportError:
        pass
    except Exception as exc:                              # a broken roles module is not a licence
        sys.stderr.write(f"[agentd] liljack_roles.may_control raised {exc!r}; using fallback\n")
    if frm == OPERATOR:
        return True
    allowed = _FALLBACK_MAY_CONTROL.get(frm)
    return bool(allowed) and to in allowed


class Refused(Exception):
    """A request that is well-formed but not permitted."""


def peer_identity(conn: socket.socket) -> str:
    """Bind a Linux local peer's launch identity, never a JSON claim.

    This protects cooperating harnesses from request-level impersonation, not
    hostile code running under the same UID (which can choose its launch env).
    Unavailable credentials/environment fail closed for mutations. Environment
    contents must never be logged: they can contain credentials.
    """
    try:
        pid, uid, _ = struct.unpack("3i", conn.getsockopt(
            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
        if uid != os.getuid():
            return ""
        raw = Path(f"/proc/{pid}/environ").read_bytes()
        env = dict(part.split(b"=", 1) for part in raw.split(b"\0") if b"=" in part)
        named = env.get(b"LILJACK_AGENT", b"").decode(errors="replace").strip().lower()
        if named:
            return named if named in {*A.AGENTS, OPERATOR} else ""
        if env.get(b"CLAUDECODE") or env.get(b"CLAUDE_CODE_ENTRYPOINT"):
            return "claude"
        if any(key.startswith(b"CODEX_") for key in env):
            return "codex"
    except (OSError, ValueError, AttributeError):
        pass
    return ""


# ══════════════════════════════════════════════════════════════════════════
# the daemon
# ══════════════════════════════════════════════════════════════════════════

class Daemon:
    def __init__(self, root=None, grace: float = A.GRACE_DEFAULT, board: bool = True,
                 sessions: bool = True):
        self.root = Path(root or cache_root())
        self.root.mkdir(parents=True, exist_ok=True)
        self.sock_path = socket_path(self.root)
        self.state_path = self.root / STATE_FILE
        self.grace = float(grace)
        # ⚠ Only a daemon on the DEFAULT root may touch SHARED state — the board
        # and the sessions registry. On 2026-09-05 a temp-root test daemon wrote
        # control=claude to the live board while the real daemon's holder was
        # operator: a side effect that escaped its own test root. Anything but the
        # default root is a test or a sandbox; it keeps its own state only.
        shared_ok = self.root.resolve() == cache_root().resolve()
        self.use_board, self.use_sessions = (bool(board) and shared_ok), (bool(sessions) and shared_ok)
        self.agents: dict = {}
        self._agents_lock = threading.Lock()
        self._ctl_lock = threading.Lock()
        self._holder = OPERATOR
        self._subs: list = []
        self._subs_lock = threading.Lock()
        self._srv = None
        self._stop = threading.Event()
        self._threads: list = []
        self.started_at = A.utcnow()
        for name in A.AGENTS:
            self._transcript_for(name)
        if self.use_sessions:
            self._register_sessions()
        self._write_state()

    # ── transcripts / agents ──────────────────────────────────────────────
    def transcript_path(self, name: str) -> Path:
        return self.root / "agents" / f"{name}.jsonl"

    _transcripts: dict = None

    def _transcript_for(self, name: str) -> A.Transcript:
        if self._transcripts is None:
            self._transcripts = {}
        t = self._transcripts.get(name)
        if t is None:
            t = A.Transcript(self.transcript_path(name),
                             on_event=lambda ev, n=name: self._emit("output" if ev["dir"] == "out" else "send",
                                                                   agent=n, **ev))
            self._transcripts[name] = t
        return t

    def _agent(self, name: str):
        if not isinstance(name, str) or not _NAME_RE.match(name):
            raise Refused(f"bad agent name {name!r}")
        if name not in A.AGENTS:
            raise Refused(f"unknown agent {name!r}; known: {sorted(A.AGENTS)}")
        with self._agents_lock:
            return self.agents.get(name)

    # ── events ───────────────────────────────────────────────────────────
    def _emit(self, kind: str, **payload):
        ev = {"event": kind, "ts": payload.pop("ts", None) or A.utcnow(), **payload}
        with self._subs_lock:
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(ev)
            except queue.Full:
                pass
        if kind in ("spawn", "exit", "state", "control"):
            self._write_state()

    def subscribe(self) -> queue.Queue:
        q = queue.Queue(maxsize=10000)
        with self._subs_lock:
            self._subs.append(q)
        return q

    def unsubscribe(self, q):
        with self._subs_lock:
            if q in self._subs:
                self._subs.remove(q)

    def _on_state(self, agent, state, info):
        kind = {"running": "spawn", "exited": "exit"}.get(state, "state")
        if kind == "spawn" and getattr(agent, "_announced", False):
            kind = "state"                     # blocked→running is a state change, not a spawn
        if state == "running":
            agent._announced = True
        payload = dict(info or {})
        payload.update(agent=agent.name, state=state, pid=agent.pid)
        self._emit(kind, **payload)

    # ── control (the invariant) ───────────────────────────────────────────
    def holder(self) -> str:
        with self._ctl_lock:
            return self._holder

    def _set_holder(self, who: str, by: str, reason: str) -> str:
        """The ONLY writer of `_holder`. Called with `_ctl_lock` held."""
        prev, self._holder = self._holder, who
        self._emit("control", holder=who, previous=prev, by=by, reason=reason)
        self._board_note(who, f"{by}: {reason}")
        return who

    def take_control(self, who: str) -> str:
        if who != OPERATOR:
            raise Refused(f"only the operator ({OPERATOR!r}) may take control; {who!r} must be given it")
        with self._ctl_lock:
            if self._holder == who:
                return who
            return self._set_holder(who, by=who, reason="operator took control")

    def give_control(self, frm: str, to: str) -> str:
        if not isinstance(to, str) or not (to == OPERATOR or to in A.AGENTS):
            raise Refused(f"cannot give control to {to!r}")
        with self._ctl_lock:
            if self._holder != frm:
                raise Refused(f"{frm!r} does not hold control ({self._holder!r} does)")
            if not may_control(frm, to):
                raise Refused(f"roles forbid {frm!r} → {to!r}")
            if to == frm:
                return frm
            return self._set_holder(to, by=frm, reason=f"{frm} gave control to {to}")

    def _board_note(self, holder: str, reason: str):
        if not self.use_board:
            return
        try:
            import liljack_board
            liljack_board.set_note("control", holder, reason=reason)
        except Exception as exc:
            sys.stderr.write(f"[agentd] board note failed: {type(exc).__name__}: {exc}\n")

    # ── ops ──────────────────────────────────────────────────────────────
    def op_list(self, req) -> dict:
        holder = self.holder()
        out = []
        for name, meta in A.AGENTS.items():
            ag = self._agent(name)
            info = ag.info() if ag else {"pid": None, "state": "stopped", "backend": meta["backend"]}
            out.append({"name": name, "role": meta["role"], "state": info["state"],
                        "pid": info.get("pid"), "backend": info.get("backend"),
                        "control": holder == name,
                        "transcript": str(self.transcript_path(name))})
        return {"agents": out, "holder": holder}

    def op_spawn(self, req) -> dict:
        name = req.get("agent")
        self._agent(name)                                  # validates the name
        args = dict(req.get("args") or {})
        meta = A.AGENTS[name]
        backend = args.get("backend") or meta["backend"]
        with self._agents_lock:
            cur = self.agents.get(name)
            if cur and cur.alive():
                raise Refused(f"{name} already running (pid {cur.pid})")
            try:
                ag = A.make_agent(name, backend, self._transcript_for(name),
                                  on_state=self._on_state, grace=self.grace)
            except A.BackendUnavailable as exc:
                raise Refused(f"backend {backend!r} unavailable: {exc}")
            self.agents[name] = ag
        cwd = args.get("cwd") or str(PROJECT_ROOT)
        if backend == "inproc":
            ag.spawn(args=args)
            return {"agent": name, "pid": None, "backend": backend, "cmd": ag.cmd}
        cmd = A.command_for(name, args)
        try:
            pid = ag.spawn(cmd, cwd=cwd, env_extra=args.get("env"))
        except FileNotFoundError as exc:
            raise Refused(f"cannot exec {cmd[0]!r}: {exc}")
        return {"agent": name, "pid": pid, "backend": backend, "cmd": cmd}

    def op_stop(self, req) -> dict:
        ag = self._agent(req.get("agent"))
        if ag is None:
            return {"stopped": False, "reason": "never spawned", "state": "stopped"}
        return ag.stop(grace=req.get("grace"))

    def op_kill(self, req) -> dict:
        ag = self._agent(req.get("agent"))
        if ag is None:
            return {"killed": False, "reason": "never spawned", "state": "stopped"}
        return ag.kill()

    def op_send(self, req) -> dict:
        sender = req["from"]
        holder = self.holder()
        if sender != OPERATOR and sender != holder:
            raise Refused(f"send refused: {sender!r} is neither the operator nor the control holder ({holder!r})")
        ag = self._agent(req.get("agent"))
        text = req.get("text")
        if not isinstance(text, str) or not text:
            raise Refused("send needs non-empty text")
        if ag is None or not ag.alive():
            raise Refused(f"{req.get('agent')} is not running")
        return {"event": ag.send(text), "from": sender}

    def op_tail(self, req) -> dict:
        self._agent(req.get("agent"))
        n = req.get("n", 50)
        try:
            n = int(n)
        except (TypeError, ValueError):
            raise Refused("n must be an integer")
        return {"agent": req["agent"], "events": self._transcript_for(req["agent"]).tail(n)}

    def op_control(self, req) -> dict:
        return {"holder": self.holder()}

    def op_take_control(self, req) -> dict:
        return {"holder": self.take_control(req["who"])}

    def op_give_control(self, req) -> dict:
        return {"holder": self.give_control(req.get("from"), req.get("to"))}

    def op_ping(self, req) -> dict:
        return {"pong": True, "pid": os.getpid(), "started": self.started_at,
                "socket": str(self.sock_path)}

    def op_matrix(self, req) -> dict:
        return {"matrix": A.backend_matrix(), "tmux": A.tmux_path()}

    def op_shutdown(self, req) -> dict:
        if req.get("from") != OPERATOR:
            raise Refused("only the operator may shut the daemon down")
        threading.Thread(target=self.shutdown, daemon=True).start()
        return {"shutting_down": True}

    def handle(self, req: dict, *, identity: str = "") -> dict:
        op = req.get("op") if isinstance(req, dict) else None
        fn = getattr(self, f"op_{op}", None) if isinstance(op, str) and not op.startswith("_") else None
        if fn is None:
            return {"ok": False, "error": f"unknown op {op!r}"}
        try:
            if op not in {"list", "tail", "control", "ping", "matrix"}:
                if identity not in {*A.AGENTS, OPERATOR}:
                    raise Refused("caller identity unavailable; launch client with LILJACK_AGENT set")
                for field in ("from", "who"):
                    if field in req and req[field] != identity:
                        raise Refused(f"claimed {field} conflicts with bound caller {identity!r}")
                if op in {"spawn", "stop", "kill", "take_control", "shutdown"} and identity != OPERATOR:
                    raise Refused(f"only the operator ({OPERATOR!r}) may {op}")
                req = dict(req, **{"from": identity, "who": identity})
            out = fn(req)
            return {"ok": True, **out}
        except Refused as exc:
            return {"ok": False, "refused": True, "error": str(exc)}
        except Exception as exc:
            return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}

    # ── state file + sessions ────────────────────────────────────────────
    def _write_state(self):
        try:
            agents = {}
            for name in A.AGENTS:
                ag = self.agents.get(name)
                agents[name] = ag.info() if ag else {"pid": None, "state": "stopped"}
            blob = {"updated": A.utcnow(), "daemon_pid": os.getpid(), "holder": self._holder,
                    "agents": agents, "socket": str(self.sock_path)}
            tmp = self.state_path.with_suffix(".tmp")
            tmp.write_text(json.dumps(blob, indent=1))
            os.replace(tmp, self.state_path)
        except OSError as exc:
            sys.stderr.write(f"[agentd] state write failed: {exc}\n")

    def _register_sessions(self):
        try:
            import liljack_sessions
            register(liljack_sessions)
        except Exception as exc:
            sys.stderr.write(f"[agentd] sessions registration failed: {exc}\n")

    # ── server ───────────────────────────────────────────────────────────
    def serve(self, block: bool = True):
        if len(str(self.sock_path).encode()) > 100:      # AF_UNIX sun_path limit
            raise RuntimeError(f"socket path too long for AF_UNIX ({len(str(self.sock_path))} bytes): "
                               f"{self.sock_path} — use a shorter LILJACK_CACHE")
        if self.sock_path.exists():
            if _alive(self.sock_path):
                raise RuntimeError(f"another agentd answers on {self.sock_path}")
            self.sock_path.unlink()
        old = os.umask(0o177)                 # the socket is born 0600, no window
        try:
            self._srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._srv.bind(str(self.sock_path))
        finally:
            os.umask(old)
        os.chmod(self.sock_path, 0o600)
        self._srv.listen(32)
        self._srv.settimeout(0.5)
        (self.root / PID_NAME).write_text(str(os.getpid()))
        t = threading.Thread(target=self._accept_loop, name="agentd-accept", daemon=True)
        t.start()
        self._threads.append(t)
        if block:
            try:
                while not self._stop.is_set():
                    time.sleep(0.5)
            except KeyboardInterrupt:
                pass
            self.shutdown()
        return self

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            th = threading.Thread(target=self._client, args=(conn,), daemon=True)
            th.start()

    def _client(self, conn: socket.socket):
        identity = peer_identity(conn)
        fh = conn.makefile("rwb", buffering=0)
        try:
            while not self._stop.is_set():
                line = fh.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    continue
                try:
                    req = json.loads(line.decode("utf-8"))
                except (ValueError, UnicodeDecodeError) as exc:
                    fh.write((json.dumps({"ok": False, "error": f"bad json: {exc}"}) + "\n").encode())
                    continue
                if isinstance(req, dict) and req.get("op") == "events":
                    self._stream_events(conn, fh, req)
                    return
                fh.write((json.dumps(self.handle(req, identity=identity), ensure_ascii=False) + "\n").encode("utf-8"))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            try:
                fh.close()
                conn.close()
            except OSError:
                pass

    def _stream_events(self, conn, fh, req):
        q = self.subscribe()
        try:
            fh.write((json.dumps({"ok": True, "event": "subscribed", "ts": A.utcnow(),
                                  "holder": self.holder()}) + "\n").encode())
            while not self._stop.is_set():
                try:
                    ev = q.get(timeout=0.5)
                except queue.Empty:
                    continue
                fh.write((json.dumps(ev, ensure_ascii=False) + "\n").encode("utf-8"))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.unsubscribe(q)

    def shutdown(self, stop_agents: bool = True):
        if self._stop.is_set():
            return
        self._stop.set()
        if stop_agents:
            for ag in list(self.agents.values()):
                try:
                    ag.stop(grace=min(self.grace, 2.0))
                except Exception:
                    pass
        try:
            if self._srv:
                self._srv.close()
        except OSError:
            pass
        for p in (self.sock_path, self.root / PID_NAME):
            try:
                p.unlink()
            except OSError:
                pass
        self._write_state()


# ══════════════════════════════════════════════════════════════════════════
# sessions adapter — importable by a heartbeat in another process
# ══════════════════════════════════════════════════════════════════════════

def session_source(root=None, now=None, **_) -> dict:
    """`liljack_sessions` rows for agentd's running agents, read from the
    state file. Event `Available` (like DeepSeek): these terminals are driven
    by agentd, not by the heartbeat's injector, so `idle` would be a lie."""
    p = Path(root or cache_root()) / STATE_FILE
    if not p.exists():
        return {"ok": False, "error": f"no agentd state at {p}", "rows": []}
    try:
        st = json.loads(p.read_text())
    except (OSError, ValueError) as exc:
        return {"ok": False, "error": f"unreadable agentd state: {exc}", "rows": []}
    try:
        import liljack_sessions as S
        avail, end = S.AVAILABLE_EVENT, S.END_EVENT
    except Exception:
        avail, end = "Available", "SessionEnd"
    rows, seq = [], 0
    for name, info in (st.get("agents") or {}).items():
        state = info.get("state")
        if state in ("running", "blocked", "starting"):
            ev = avail
        elif state == "exited":
            ev = end
        else:
            continue
        seq += 1
        rows.append({"seq": seq, "at": st.get("updated"), "session": f"agentd-{name}",
                     "cwd": str(PROJECT_ROOT), "event": ev, "agent": name, "harness": "agentd",
                     "pid": info.get("pid"), "control": st.get("holder") == name})
    return {"ok": True, "rows": rows}


def register(sessions=None) -> None:
    """`liljack_sessions.autoload` entry point (`LILJACK_SESSION_SOURCES=liljack_agentd`)."""
    if sessions is None:
        import liljack_sessions as sessions
    sessions.register_source("agentd", session_source)


# ══════════════════════════════════════════════════════════════════════════
# client
# ══════════════════════════════════════════════════════════════════════════

def _alive(path) -> bool:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(str(path))
        return True
    except OSError:
        return False
    finally:
        s.close()


def request(req: dict, root=None, timeout: float = 30.0) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(str(socket_path(root)))
        fh = s.makefile("rwb", buffering=0)
        fh.write((json.dumps(req) + "\n").encode("utf-8"))
        line = fh.readline()
        if not line:
            return {"ok": False, "error": "daemon closed the connection"}
        return json.loads(line.decode("utf-8"))
    finally:
        s.close()


def events(root=None):
    """Generator over the event stream. Caller closes by breaking out."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(str(socket_path(root)))
        fh = s.makefile("rwb", buffering=0)
        fh.write(b'{"op":"events"}\n')
        for line in fh:
            line = line.strip()
            if line:
                yield json.loads(line.decode("utf-8"))
    finally:
        s.close()


# ══════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="liljack_agentd", description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", help="cache root (default $LILJACK_CACHE or ~/.cache/liljack)")
    ap.add_argument("--from", dest="frm", default=None, help="optional sender assertion; must match launch identity")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve"); s.add_argument("--grace", type=float, default=A.GRACE_DEFAULT)
    s.add_argument("--no-board", action="store_true"); s.add_argument("--no-sessions", action="store_true")
    sub.add_parser("list"); sub.add_parser("control"); sub.add_parser("ping"); sub.add_parser("matrix")
    sub.add_parser("shutdown")
    p = sub.add_parser("spawn"); p.add_argument("agent"); p.add_argument("--args", default="{}",
                                                                          help="JSON args (cmd, task, resume, model, backend, cwd, ...)")
    for c in ("stop", "kill"):
        p = sub.add_parser(c); p.add_argument("agent")
    p = sub.add_parser("send"); p.add_argument("agent"); p.add_argument("text")
    p = sub.add_parser("tail"); p.add_argument("agent"); p.add_argument("-n", type=int, default=50)
    p = sub.add_parser("take"); p.add_argument("--who", default=None)
    p = sub.add_parser("give"); p.add_argument("to")
    sub.add_parser("events")
    ns = ap.parse_args(argv)
    root = ns.root

    if ns.cmd == "serve":
        d = Daemon(root=root, grace=ns.grace, board=not ns.no_board, sessions=not ns.no_sessions)
        signal.signal(signal.SIGTERM, lambda *_: d.shutdown())
        print(f"[agentd] pid {os.getpid()} socket {d.sock_path} tmux={'yes' if A.tmux_path() else 'absent'}",
              file=sys.stderr)
        d.serve(block=True)
        return 0
    if ns.cmd == "events":
        try:
            for ev in events(root):
                print(json.dumps(ev, ensure_ascii=False), flush=True)
        except KeyboardInterrupt:
            pass
        return 0
    req = {"op": {"take": "take_control", "give": "give_control"}.get(ns.cmd, ns.cmd)}
    if ns.cmd == "spawn":
        try:
            req.update(agent=ns.agent, args=json.loads(ns.args))
        except ValueError as exc:
            print(f"bad --args JSON: {exc}", file=sys.stderr); return 2
    elif ns.cmd in ("stop", "kill"):
        req["agent"] = ns.agent
    elif ns.cmd == "send":
        req.update(agent=ns.agent, text=ns.text, **{"from": ns.frm})
    elif ns.cmd == "tail":
        req.update(agent=ns.agent, n=ns.n)
    elif ns.cmd == "take":
        req["who"] = ns.who
    elif ns.cmd == "give":
        req.update(to=ns.to, **{"from": ns.frm})
    elif ns.cmd == "shutdown":
        req["from"] = ns.frm
    req = {k: v for k, v in req.items() if v is not None}
    try:
        out = request(req, root)
    except OSError as exc:
        print(json.dumps({"ok": False, "error": f"no daemon at {socket_path(root)}: {exc}"}))
        return 1
    print(json.dumps(out, ensure_ascii=False, indent=1))
    return 0 if out.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
