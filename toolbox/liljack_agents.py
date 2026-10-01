#!/usr/bin/env python3
"""liljack_agents.py — the backends that SPAWN and OWN lilJack's agents.

Ownership is control. On 2026-09-05 lilJack could message Codex but had no
authority over the process, so the operator killed it by hand. This module is the
missing primitive: every agent runs under a pid this process owns, its output
is captured into a scrubbed JSONL transcript, and stop/kill are real signals
with a real grace period.

Three agents, three ways of being driven:

    claude    main brain    `claude -p --output-format stream-json
                             --input-format stream-json` (persistent, bidirectional)
    codex     second brain  `codex exec --json <task>` per task (bounded, killable)
                            or an interactive `codex` (args.interactive)
    deepseek  third opinion IN-PROCESS via liljack_deepseek.ask(); no pid to own.
                            UNTRUSTED: its reply is DATA in the transcript, never
                            executed, never fed to a shell.

Two process backends:

    pty   always available. `pty.fork`, one reader thread per agent, SIGTERM
          then SIGKILL after `grace` seconds. This is the control primitive
          and it must work with nothing installed.
    tmux  optional. `shutil.which("tmux")` gates it; absent → `BackendUnavailable`
          with the reason, never a crash. Session `liljack`, one window per
          agent plus a `control` window the operator can attach to from any terminal.
          ⚠ tmux is NOT installed on devbox today, so this backend is
          UNVERIFIED end to end — see the test, which asserts only the refusal.

Non-negotiables enforced here:

  * Every child gets `minimal_env()` — an ALLOWLIST (PATH/HOME/LANG/LC_ALL/
    TZ/TMPDIR) plus `CUDA_VISIBLE_DEVICES=""`. Never the parent's environment,
    so no DEEPSEEK_*/ANTHROPIC_*/OPENAI_* variable can leak into a child even
    when the parent has them set. DeepSeek reads its own credential file
    in-process; its key is in no env this module builds.
  * Transcripts are scrubbed AT WRITE with `liljack_chains._redact_live_secrets`
    under the `liljack_codex._capture_text` contract: scrubber unavailable →
    the text is OMITTED, never written raw.
"""
from __future__ import annotations

import fcntl
import json
import os
import pty
import shutil
import signal
import struct
import sys
import termios
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

TOOLBOX = Path(__file__).resolve().parent
PROJECT_ROOT = TOOLBOX.parent
if str(TOOLBOX) not in sys.path:
    sys.path.insert(0, str(TOOLBOX))

CACHE = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))

STATES = ("stopped", "starting", "running", "blocked", "exited")
GRACE_DEFAULT = 5.0            # seconds between SIGTERM and SIGKILL
TRANSCRIPT_KEEP = 4000         # in-memory tail per agent
TEXT_CAP = 8000                # per transcript event, same cap as _capture_text
OMITTED = "[text omitted: credential scrubber unavailable]"
TMUX_SESSION = "liljack"

# name → role. Roles themselves (who may control whom, who is trusted) live in
# liljack_roles; this is only the label shown by `list`.
AGENTS = {
    "claude":   {"role": "main",     "trusted": True,  "backend": "pty"},
    "codex":    {"role": "second",   "trusted": True,  "backend": "pty"},
    "deepseek": {"role": "untrusted", "trusted": False, "backend": "inproc"},
}


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class BackendUnavailable(RuntimeError):
    """The requested backend cannot run here (e.g. tmux not installed).
    Carries the reason; callers REPORT it, they do not fall over."""


# ══════════════════════════════════════════════════════════════════════════
# env + scrubbing
# ══════════════════════════════════════════════════════════════════════════

def minimal_env(extra=None, environ=None) -> dict:
    """Allowlisted child env. Delegates to liljack_deepseek.minimal_env so the
    two spawn paths cannot drift; the local copy is the fallback if that
    module cannot import (it must not be a reason to ship the parent env)."""
    try:
        from liljack_deepseek import minimal_env as _me
        return _me(extra=extra, environ=environ)
    except Exception:
        src = os.environ if environ is None else environ
        safe = ("PATH", "HOME", "LANG", "LC_ALL", "TZ", "TMPDIR")
        env = {k: str(src[k]) for k in safe if k in src and src[k] is not None}
        env.setdefault("PATH", "/usr/bin:/bin")
        env["CUDA_VISIBLE_DEVICES"] = ""
        for k, v in (extra or {}).items():
            k = str(k)
            if not k or "=" in k:
                raise ValueError(f"bad env name {k!r}")
            env[k] = str(v)
        return env


def scrub(text) -> str:
    """The `_capture_text` contract: known live literals → [REDACTED]; scrubber
    unavailable → omit the text entirely."""
    if not isinstance(text, str) or not text:
        return ""
    try:
        from liljack_chains import _redact_live_secrets
        cleaned, n = _redact_live_secrets(text)
        if n < 0:
            return OMITTED
        return cleaned[:TEXT_CAP]
    except Exception:
        return OMITTED


# ══════════════════════════════════════════════════════════════════════════
# transcript
# ══════════════════════════════════════════════════════════════════════════

class Transcript:
    """Append-only JSONL, one `{ts, dir, text}` per line, scrubbed at write.
    `dir` is `in` (what we wrote to the agent) or `out` (what it printed)."""

    def __init__(self, path: Path, on_event=None):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.tail_buf = deque(maxlen=TRANSCRIPT_KEEP)
        self._lock = threading.Lock()
        self._on_event = on_event

    def write(self, direction: str, text: str, raw=None) -> dict:
        ev = {"ts": utcnow(), "dir": direction, "text": scrub(text)}
        if raw is not None:
            ev["raw_len"] = len(raw)
        line = json.dumps(ev, ensure_ascii=False)
        with self._lock:
            with open(self.path, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
            self.tail_buf.append(ev)
        if self._on_event:
            self._on_event(ev)
        return ev

    def tail(self, n: int = 50) -> list:
        n = max(0, int(n))
        with self._lock:
            if len(self.tail_buf) >= n or not self.path.exists():
                return list(self.tail_buf)[-n:] if n else []
        # more than memory holds: read the file
        out = deque(maxlen=n)
        try:
            with open(self.path, encoding="utf-8") as fh:
                for ln in fh:
                    ln = ln.strip()
                    if ln:
                        try:
                            out.append(json.loads(ln))
                        except json.JSONDecodeError:
                            continue
        except OSError:
            pass
        return list(out)


# ══════════════════════════════════════════════════════════════════════════
# command lines
# ══════════════════════════════════════════════════════════════════════════

def command_for(agent: str, args: dict | None) -> list:
    """Argv for an agent. `args.cmd` overrides everything (tests, --version
    proofs). Read `claude --help` / `codex exec --help` for the live flags —
    these were checked against Claude Code 2.1.261 and codex-cli 0.153.4."""
    a = dict(args or {})
    if a.get("cmd"):
        cmd = a["cmd"]
        if isinstance(cmd, str):
            cmd = [cmd]
        return [str(c) for c in cmd]
    extra = [str(x) for x in (a.get("extra") or [])]
    if agent == "claude":
        cmd = ["claude", "-p", "--output-format", "stream-json",
               "--input-format", "stream-json", "--verbose"]
        if a.get("resume"):
            cmd += ["--resume", str(a["resume"])]
        elif a.get("session_id"):
            cmd += ["--session-id", str(a["session_id"])]
        if a.get("model"):
            cmd += ["--model", str(a["model"])]
        if a.get("permission_mode"):
            cmd += ["--permission-mode", str(a["permission_mode"])]
        return cmd + extra
    if agent == "codex":
        if a.get("interactive"):
            cmd = ["codex"]
            if a.get("model"):
                cmd += ["-m", str(a["model"])]
            return cmd + extra
        cmd = ["codex", "exec", "--json"]
        if a.get("model"):
            cmd += ["-m", str(a["model"])]
        if a.get("sandbox"):
            cmd += ["-s", str(a["sandbox"])]
        if a.get("ephemeral"):
            cmd += ["--ephemeral"]
        if a.get("cwd"):
            cmd += ["-C", str(a["cwd"])]
        cmd += extra
        if a.get("task"):
            cmd += [str(a["task"])]
        return cmd
    raise ValueError(f"no command line for agent {agent!r} — pass args.cmd")


# ══════════════════════════════════════════════════════════════════════════
# pty backend — the control primitive
# ══════════════════════════════════════════════════════════════════════════

class PtyAgent:
    """One agent under a pty this process owns."""
    backend = "pty"

    def __init__(self, name: str, transcript: Transcript, on_state=None,
                 grace: float = GRACE_DEFAULT):
        self.name = name
        self.transcript = transcript
        self._on_state = on_state
        self.grace = float(grace)
        self.pid = None
        self.fd = None
        self.state = "stopped"
        self.exit = None          # {"code": int|None, "signal": int|None}
        self.cmd = None
        self.started = None
        self._reader = None
        self._lock = threading.Lock()

    # ── state ────────────────────────────────────────────────────────────
    def _set(self, state: str, **info):
        assert state in STATES, state
        with self._lock:
            if self.state == state:
                return
            self.state = state
        if self._on_state:
            self._on_state(self, state, info)

    def alive(self) -> bool:
        return self.pid is not None and self.exit is None

    def proc_state(self) -> str:
        """Kernel state letter from /proc — 'T' means stopped (blocked)."""
        if not self.pid:
            return ""
        try:
            stat = Path(f"/proc/{self.pid}/stat").read_text()
            return stat.rsplit(")", 1)[1].split()[0]
        except (OSError, IndexError):
            return ""

    # ── spawn ────────────────────────────────────────────────────────────
    def spawn(self, cmd: list, cwd=None, env_extra=None, environ=None) -> int:
        if self.alive():
            raise RuntimeError(f"{self.name} already running (pid {self.pid})")
        env = minimal_env(extra=env_extra, environ=environ)
        self.cmd, self.exit, self.started = list(cmd), None, utcnow()
        self._set("starting")
        pid, fd = pty.fork()
        if pid == 0:                                   # child
            try:
                # no echo: the transcript must not carry every `in` twice
                attrs = termios.tcgetattr(0)
                attrs[3] &= ~termios.ECHO
                termios.tcsetattr(0, termios.TCSANOW, attrs)
            except Exception:
                pass
            try:
                if cwd:
                    os.chdir(str(cwd))
                os.execvpe(cmd[0], list(cmd), env)
            except Exception as exc:                   # pragma: no cover
                os.write(2, f"[agentd] exec failed: {exc}\n".encode())
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd
        try:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
        except OSError:
            pass
        self._reader = threading.Thread(target=self._read_loop,
                                        name=f"agent-{self.name}", daemon=True)
        self._reader.start()
        self._set("running", pid=pid, cmd=self.cmd)
        return pid

    def _read_loop(self):
        buf = b""
        last = time.monotonic()
        import select
        while True:
            try:
                r, _, _ = select.select([self.fd], [], [], 0.25)
            except (OSError, ValueError):
                break
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    break
                buf += chunk
                last = time.monotonic()
                *lines, buf = buf.split(b"\n")
                if lines:
                    self._emit_lines(lines)
            else:
                if buf and time.monotonic() - last > 0.3:
                    self._emit_lines([buf])            # prompt without newline
                    buf = b""
                if self.proc_state() == "T":
                    self._set("blocked")
                elif self.state == "blocked" and self.proc_state() in ("R", "S", "D"):
                    self._set("running")
        if buf:
            self._emit_lines([buf])
        self._reap()

    def _emit_lines(self, lines):
        for raw in lines:
            text = raw.decode("utf-8", "replace").rstrip("\r")
            if text:
                self.transcript.write("out", text)

    def _reap(self):
        if self.pid is None or self.exit is not None:
            return
        try:
            _, status = os.waitpid(self.pid, 0)
        except ChildProcessError:
            status = 0
        code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else None
        sig = os.WTERMSIG(status) if os.WIFSIGNALED(status) else None
        self.exit = {"code": code, "signal": sig, "at": utcnow()}
        try:
            os.close(self.fd)
        except OSError:
            pass
        self._set("exited", **self.exit)

    # ── I/O ──────────────────────────────────────────────────────────────
    def send(self, text: str) -> dict:
        if not self.alive():
            raise RuntimeError(f"{self.name} is not running")
        data = text if text.endswith("\n") else text + "\n"
        os.write(self.fd, data.encode("utf-8"))
        return self.transcript.write("in", text)

    # ── stop/kill ────────────────────────────────────────────────────────
    def _signal(self, sig) -> bool:
        if not self.alive():
            return False
        try:
            os.kill(self.pid, sig)
            return True
        except ProcessLookupError:
            return False

    def wait(self, timeout: float) -> bool:
        end = time.monotonic() + timeout
        while self.alive() and time.monotonic() < end:
            time.sleep(0.02)
        return not self.alive()

    def stop(self, grace=None) -> dict:
        """SIGTERM, then SIGKILL after the grace period. Returns how it died."""
        g = self.grace if grace is None else float(grace)
        if not self.alive():
            return {"stopped": False, "reason": "not running", "state": self.state}
        self._signal(signal.SIGTERM)
        if self.wait(g):
            return {"stopped": True, "how": "SIGTERM", "exit": self.exit}
        self._signal(signal.SIGKILL)
        self.wait(2.0)
        return {"stopped": not self.alive(), "how": "SIGKILL", "exit": self.exit}

    def kill(self) -> dict:
        if not self.alive():
            return {"killed": False, "reason": "not running", "state": self.state}
        self._signal(signal.SIGKILL)
        self.wait(2.0)
        return {"killed": not self.alive(), "how": "SIGKILL", "exit": self.exit}

    def info(self) -> dict:
        return {"pid": self.pid if self.alive() else None, "state": self.state,
                "backend": self.backend, "cmd": self.cmd, "started": self.started,
                "exit": self.exit}


# ══════════════════════════════════════════════════════════════════════════
# tmux backend — optional, capability-gated
# ══════════════════════════════════════════════════════════════════════════

def tmux_path():
    return shutil.which("tmux")


class TmuxAgent:
    """An agent in a window of the `liljack` tmux session, transcript via
    `pipe-pane`. ⚠ UNVERIFIED on this box (no tmux); the shape follows the
    documented tmux CLI and every call reports rather than raises when a
    tmux command fails."""
    backend = "tmux"

    def __init__(self, name: str, transcript: Transcript, on_state=None,
                 grace: float = GRACE_DEFAULT, session: str = TMUX_SESSION):
        self.name, self.transcript, self._on_state = name, transcript, on_state
        self.grace, self.session = float(grace), session
        self.pid, self.state, self.exit, self.cmd, self.started = None, "stopped", None, None, None
        self._raw_log = transcript.path.with_suffix(".tmux.raw")
        self._pump = None
        self._lock = threading.Lock()

    @property
    def target(self) -> str:
        return f"{self.session}:{self.name}"

    def _tmux(self, *args, check=True) -> str:
        import subprocess
        exe = tmux_path()
        if not exe:
            raise BackendUnavailable("tmux is not installed (shutil.which('tmux') is None)")
        p = subprocess.run([exe, *args], capture_output=True, text=True,
                           env=minimal_env())
        if check and p.returncode != 0:
            raise RuntimeError(f"tmux {' '.join(args)} failed: {p.stderr.strip()}")
        return p.stdout.strip()

    def _set(self, state, **info):
        with self._lock:
            if self.state == state:
                return
            self.state = state
        if self._on_state:
            self._on_state(self, state, info)

    def alive(self) -> bool:
        if not self.pid:
            return False
        try:
            os.kill(self.pid, 0)
            return Path(f"/proc/{self.pid}").exists() and \
                Path(f"/proc/{self.pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
        except (OSError, IndexError):
            return False

    def ensure_session(self):
        if not tmux_path():
            raise BackendUnavailable("tmux is not installed (shutil.which('tmux') is None)")
        if self._tmux("has-session", "-t", self.session, check=False) == "" and \
                self._tmux("list-sessions", "-F", "#{session_name}", check=False).split().count(self.session) == 0:
            self._tmux("new-session", "-d", "-s", self.session, "-n", "control")

    def spawn(self, cmd: list, cwd=None, env_extra=None, environ=None) -> int:
        import shlex
        if self.alive():
            raise RuntimeError(f"{self.name} already running (pid {self.pid})")
        self.ensure_session()
        env = minimal_env(extra=env_extra, environ=environ)
        self.cmd, self.exit, self.started = list(cmd), None, utcnow()
        self._set("starting")
        envp = " ".join(f"{k}={shlex.quote(v)}" for k, v in env.items())
        shell = f"cd {shlex.quote(str(cwd or PROJECT_ROOT))} && exec env -i {envp} {' '.join(shlex.quote(c) for c in cmd)}"
        self._tmux("kill-window", "-t", self.target, check=False)
        self._tmux("new-window", "-d", "-t", self.session, "-n", self.name, shell)
        self._tmux("set-option", "-t", self.target, "remain-on-exit", "on", check=False)
        self._raw_log.parent.mkdir(parents=True, exist_ok=True)
        # Raw logs are training data. Preserve earlier sessions, but start
        # this session's transcript pump at the previous end to avoid replay.
        with open(self._raw_log, "ab") as fh:
            start_pos = fh.tell()
        self._tmux("pipe-pane", "-t", self.target, "-o", f"cat >> {shlex.quote(str(self._raw_log))}")
        self.pid = int(self._tmux("display-message", "-p", "-t", self.target, "#{pane_pid}"))
        self._pump = threading.Thread(target=self._pump_loop, args=(start_pos,), daemon=True,
                                      name=f"tmux-{self.name}")
        self._pump.start()
        self._set("running", pid=self.pid, cmd=self.cmd)
        return self.pid

    def _pump_loop(self, pos=0):
        buf = b""
        while True:
            try:
                with open(self._raw_log, "rb") as fh:
                    fh.seek(pos)
                    chunk = fh.read()
                    pos = fh.tell()
            except OSError:
                chunk = b""
            if chunk:
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for raw in lines:
                    t = raw.decode("utf-8", "replace").rstrip("\r")
                    if t:
                        self.transcript.write("out", t)
            if not self.alive():
                if buf:
                    self.transcript.write("out", buf.decode("utf-8", "replace"))
                dead = self._tmux("display-message", "-p", "-t", self.target,
                                  "#{pane_dead_status}", check=False)
                self.exit = {"code": int(dead) if dead.isdigit() else None,
                             "signal": None, "at": utcnow()}
                self._set("exited", **self.exit)
                return
            time.sleep(0.25)

    def send(self, text: str) -> dict:
        if not self.alive():
            raise RuntimeError(f"{self.name} is not running")
        self._tmux("send-keys", "-t", self.target, "-l", text)
        self._tmux("send-keys", "-t", self.target, "Enter")
        return self.transcript.write("in", text)

    def wait(self, timeout: float) -> bool:
        end = time.monotonic() + timeout
        while self.alive() and time.monotonic() < end:
            time.sleep(0.05)
        return not self.alive()

    def stop(self, grace=None) -> dict:
        g = self.grace if grace is None else float(grace)
        if not self.alive():
            return {"stopped": False, "reason": "not running", "state": self.state}
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        if self.wait(g):
            return {"stopped": True, "how": "SIGTERM", "exit": self.exit}
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.wait(2.0)
        return {"stopped": not self.alive(), "how": "SIGKILL", "exit": self.exit}

    def kill(self) -> dict:
        if not self.alive():
            return {"killed": False, "reason": "not running", "state": self.state}
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.wait(2.0)
        self._tmux("kill-window", "-t", self.target, check=False)
        return {"killed": not self.alive(), "how": "SIGKILL", "exit": self.exit}

    def info(self) -> dict:
        return {"pid": self.pid if self.alive() else None, "state": self.state,
                "backend": self.backend, "cmd": self.cmd, "started": self.started,
                "exit": self.exit, "tmux_target": self.target}


# ══════════════════════════════════════════════════════════════════════════
# deepseek — in-process, untrusted, no pid
# ══════════════════════════════════════════════════════════════════════════

class DeepSeekAgent:
    """No process. `spawn` = verify a credential is configured; `send` = one
    `ask()` on a worker thread, reply appended to the transcript as DATA
    (`reasoning` is recorded separately, like `ask()` returns it). Nothing in
    the reply is ever executed by this module."""
    backend = "inproc"

    def __init__(self, name: str, transcript: Transcript, on_state=None, **_):
        self.name, self.transcript, self._on_state = name, transcript, on_state
        self.pid, self.state, self.exit, self.cmd, self.started = None, "stopped", None, None, None
        self.model = None
        self._busy = threading.Lock()

    def _set(self, state, **info):
        if self.state == state:
            return
        self.state = state
        if self._on_state:
            self._on_state(self, state, info)

    def alive(self) -> bool:
        return self.state in ("running", "blocked")

    def spawn(self, cmd=None, cwd=None, env_extra=None, environ=None, args=None) -> int:
        import liljack_deepseek as D
        a = dict(args or {})
        self.model = a.get("model") or D.DEFAULT_MODEL
        self.cmd = ["<in-process>", "liljack_deepseek.ask", self.model]
        self._set("starting")
        try:
            D.load_credentials()
        except Exception as exc:
            self.exit = {"code": None, "signal": None, "at": utcnow(),
                         "error": D.redact(str(exc)) if hasattr(D, "redact") else "credential error"}
            self._set("exited", **self.exit)
            raise RuntimeError(f"deepseek: {self.exit['error']}")
        self.started, self.exit = utcnow(), None
        self._set("running")
        return 0

    def send(self, text: str) -> dict:
        if not self.alive():
            raise RuntimeError("deepseek is not running")
        ev = self.transcript.write("in", text)
        threading.Thread(target=self._ask, args=(text,), daemon=True,
                         name="deepseek-ask").start()
        return ev

    def _ask(self, text: str):
        import liljack_deepseek as D
        with self._busy:
            self._set("blocked")          # a call is in flight
            try:
                r = D.ask(text, model=self.model)
                content = r.get("content") if isinstance(r, dict) else str(r)
                reasoning = r.get("reasoning") if isinstance(r, dict) else None
                if reasoning:
                    self.transcript.write("out", "[reasoning] " + str(reasoning))
                self.transcript.write("out", str(content or ""))
            except Exception as exc:
                self.transcript.write("out", f"[deepseek error] {type(exc).__name__}: "
                                             f"{D.redact(str(exc)) if hasattr(D, 'redact') else exc}")
            finally:
                if self.state == "blocked":
                    self._set("running")

    def stop(self, grace=None) -> dict:
        if not self.alive():
            return {"stopped": False, "reason": "not running", "state": self.state}
        self.exit = {"code": 0, "signal": None, "at": utcnow()}
        self._set("exited", **self.exit)
        return {"stopped": True, "how": "release", "exit": self.exit}

    kill = stop

    def wait(self, timeout: float) -> bool:
        return True

    def info(self) -> dict:
        return {"pid": None, "state": self.state, "backend": self.backend,
                "cmd": self.cmd, "started": self.started, "exit": self.exit,
                "model": self.model}


BACKENDS = {"pty": PtyAgent, "tmux": TmuxAgent, "inproc": DeepSeekAgent}


def backend_matrix() -> dict:
    """What can run where, right now, on this box."""
    tm = tmux_path()
    out = {}
    for name, meta in AGENTS.items():
        if meta["backend"] == "inproc":
            out[name] = {"pty": "n/a (in-process)", "tmux": "n/a (in-process)",
                         "inproc": "yes"}
        else:
            out[name] = {"pty": "yes",
                         "tmux": "yes" if tm else "unavailable: tmux not installed",
                         "inproc": "no"}
    return out


def make_agent(name: str, backend: str, transcript: Transcript, on_state=None,
               grace: float = GRACE_DEFAULT):
    if backend == "tmux" and not tmux_path():
        raise BackendUnavailable("tmux is not installed (shutil.which('tmux') is None)")
    cls = BACKENDS.get(backend)
    if cls is None:
        raise ValueError(f"unknown backend {backend!r}")
    return cls(name, transcript, on_state=on_state, grace=grace)
