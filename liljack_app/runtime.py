"""App-owned tmux sessions and foreground PTY attachment clients."""
from __future__ import annotations

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import uuid
import unicodedata

from liljack_workspace import project_key, session_key, utcnow
from .native import Terminal, PROJECT_ROOT


def app_child_env():
    """Keep the shared allowlist while preserving the operator CUDA setting."""
    from liljack_agents import minimal_env
    env = minimal_env()
    env.pop("CUDA_VISIBLE_DEVICES", None)
    if "CUDA_VISIBLE_DEVICES" in os.environ:
        env["CUDA_VISIBLE_DEVICES"] = os.environ["CUDA_VISIBLE_DEVICES"]
    return env


def tmux_binary():
    candidates = [os.environ.get("LILJACK_TMUX"), shutil.which("tmux"),
                  str(Path.home() / ".cache/liljack/build/usr/bin/tmux")]
    return next((p for p in candidates if p and os.access(p, os.X_OK)), None)


# ── sixel transport ─────────────────────────────────────────────────────────
# The terminal cell geometry, in pixels — must match LJ_CELL_W / LJ_LINE_H in
# c_render.h. tmux needs the attach client's PIXEL size (ws_xpixel/ws_ypixel) to
# scale sixel images; without it (xpixel==0) it falls back to the placeholder
# even when the Sxl capability is present.
CELL_W = 10
CELL_H = 20
_SIXEL_TERM = "xterm-256color-sixel"


def sixel_terminfo_dir():
    """Compile the Sxl-capable terminfo entry into the lilJack cache, once.

    Returns the TERMINFO directory to point the attach client at, or None when
    `tic` is missing or the entry cannot be built — the caller then falls back to
    plain xterm-256color (no sixel, everything else byte-identical). Idempotent:
    the compile runs only when the compiled entry is absent.
    """
    base = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache/liljack"))) / "terminfo"
    if (base / "x" / _SIXEL_TERM).exists():
        return str(base)
    tic = shutil.which("tic")
    src = Path(__file__).resolve().parent / "terminfo" / (_SIXEL_TERM + ".src")
    if not tic or not src.exists():
        return None
    try:
        base.mkdir(parents=True, exist_ok=True)
        subprocess.run([tic, "-x", "-o", str(base), str(src)], check=True,
                       capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return None
    return str(base) if (base / "x" / _SIXEL_TERM).exists() else None


class Tmux:
    def __init__(self, socket_name=None):
        self.binary = tmux_binary()
        self.socket = socket_name or os.environ.get("LILJACK_TMUX_SOCKET", "liljack-app")

    def call(self, *args, check=True, input_text=None):
        if not self.binary:
            raise RuntimeError("tmux missing: install tmux or set LILJACK_TMUX")
        env = app_child_env()
        for key in ("LC_CTYPE", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            if key in os.environ:
                env[key] = os.environ[key]
        r = subprocess.run([self.binary, "-L", self.socket, "-f", "/dev/null", *args],
                           text=True, capture_output=True, timeout=5, input=input_text, env=env)
        if check and r.returncode:
            raise RuntimeError("tmux: " + r.stderr.strip()[:200])
        return r

    def sessions(self, project):
        if not self.binary:
            return []
        fmt = "#{session_name}\t#{pane_id}\t#{@liljack_agent}\t#{session_path}\t#{pane_dead}\t#{@liljack_project}"
        r = self.call("list-panes", "-a", "-F", fmt, check=False)
        if r.returncode:
            if "no server running" in r.stderr or "No such file" in r.stderr:
                return []
            raise RuntimeError("tmux discovery failed: " + r.stderr.strip()[:180])
        out = []
        for line in r.stdout.splitlines():
            fields = line.split("\t")
            if len(fields) not in (5, 6):
                continue
            name, pane, agent, cwd, dead = fields[:5]
            workspace_project = fields[5] if len(fields) == 6 and fields[5] else cwd
            if not name.startswith("lj-") or not cwd or project_key(workspace_project) != project_key(project):
                continue
            native = f"{self.socket}/{name}"
            out.append({"session": native, "harness": "tmux", "backend": "tmux",
                        "agent": agent or "shell",
                        "cwd": cwd, "project": project_key(workspace_project),
                        "event": "Exit" if dead == "1" else "Attached",
                        "state": "exited" if dead == "1" else "running", "at": utcnow(),
                        "tmux_name": name, "pane": pane,
                        "id": session_key("tmux", native)})
        # Native (supervisor) sessions live alongside tmux; absent daemon -> [].
        try:
            out += NativeBackend().sessions(project)
        except (OSError, ValueError):
            pass
        return out

    @staticmethod
    def command(agent):
        # the operator explicitly requested approval-free app launches (2026-09-09).
        commands = {"shell": [os.environ.get("SHELL", "/bin/bash")],
                    "codex": ["codex", "--dangerously-bypass-approvals-and-sandbox"],
                    "claude": ["claude", "--dangerously-skip-permissions"],
                    "deepseek": ["opencode", "--model",
                                 os.environ.get("LILJACK_DEEPSEEK_MODEL", "deepseek/deepseek-v4-pro")]}
        argv = commands.get(agent)
        if not argv or not shutil.which(argv[0]):
            raise RuntimeError(f"{agent} executable is not available")
        return argv

    def preflight(self, agent, cwd, backend="tmux"):
        if backend != "native" and not self.binary:
            raise RuntimeError('tmux missing: install tmux or set LILJACK_TMUX')
        if not Path(cwd).is_dir():
            raise ValueError('room folder must be an existing directory')
        self.command(agent)

    def create(self, agent, project, root, *, cwd=None, intro=None, name=None,
               backend="tmux", room_id=''):
        if backend == "native":
            return NativeBackend().create(agent, project, root, cwd=cwd, intro=intro,
                                          name=name, room_id=room_id)
        cwd = project_key(cwd or project)
        self.preflight(agent, cwd)
        argv = self.command(agent)
        if intro is not None:
            from liljack_workspace import clean
            intro = clean(intro, 48000)
            if agent in ('codex', 'claude'):
                argv += ['--', intro]
            elif agent == 'deepseek':
                argv += ['--prompt', intro]
            else:
                # Never hand prose to a shell parser. This foreground wrapper
                # prints an argv value, then replaces itself with the shell.
                argv = [sys.executable, '-c',
                        'import os,sys; print(sys.argv[1], flush=True); os.execvp(sys.argv[2],sys.argv[2:])',
                        intro, *argv]
        if agent == "deepseek":
            argv = ["env", 'OPENCODE_PERMISSION={"*":"allow"}', *argv]
        name = name or "lj-" + uuid.uuid4().hex[:12]
        if not name.startswith('lj-') or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in name):
            raise ValueError('invalid managed session name')
        sid = session_key("tmux", f"{self.socket}/{name}")
        argv = _with_system_prompt(argv, agent, project, root, sid, intro, room_id)
        harness = {"deepseek": "opencode", "codex": "codex", "claude": "claude"}.get(agent)
        cuda_args = ["-e", "CUDA_VISIBLE_DEVICES=" + os.environ["CUDA_VISIBLE_DEVICES"]] if "CUDA_VISIBLE_DEVICES" in os.environ else []
        harness_args = ["-e", f"LILJACK_HARNESS={harness}"] if harness else []
        if not harness:
            argv = ["env", "-u", "LILJACK_HARNESS", *argv]
        if "CUDA_VISIBLE_DEVICES" not in os.environ:
            argv = ["env", "-u", "CUDA_VISIBLE_DEVICES", *argv]
        self.call("new-session", "-d", "-s", name, "-c", cwd,
                  *cuda_args, "-e", f"LILJACK_WORKSPACE_SESSION={sid}",
                  "-e", f"LILJACK_WORKSPACE_ROOT={root}",
                  "-e", f"LILJACK_WORKSPACE_PROJECT={project_key(project)}",
                  "-e", f"LILJACK_AGENT={'operator' if agent == 'shell' else agent}",
                  *harness_args, *argv)
        self.call('set-option', '-t', name, '@liljack_project', project_key(project))
        self.call("set-option", "-t", name, "@liljack_agent", agent)
        self.call("set-option", "-t", name, "status", "off")
        self.call("set-option", "-t", name, "mouse", "on")
        self.call("set-window-option", "-t", name, "remain-on-exit", "on")
        return sid


def _with_system_prompt(argv, agent, project, root, sid, intro, room_id=''):
    """T2: for claude, the standing set + room id go in as a SYSTEM prompt file
    (prompts/<sid>.md under the workspace root) so they survive /compact and a
    restart; the intro alone is one-shot. codex/opencode read the room-folder
    AGENTS.md fragment instead (room_instructions.py) — their argv is untouched."""
    if agent != 'claude' or intro is None:
        return argv
    from .room_start import system_prompt_text
    folder = Path(root) / 'prompts'
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f'{sid}.md'
    path.write_text(system_prompt_text(project, room_id), encoding='utf-8')
    argv = list(argv)
    at = argv.index('--dangerously-skip-permissions') + 1
    return argv[:at] + ['--append-system-prompt-file', str(path)] + argv[at:]


class NativeBackend:
    """Spawn and enumerate sessions via liljack-sessiond (opt-in; no tmux).

    This is the other half of owkterm-native-sessions: the same agent argv/env
    that `Tmux.create` hands to tmux is handed to the supervisor instead, so a
    session can be created on EITHER backend. The supervisor owns the pty, so the
    session outlives the UI — the property tmux exists for.
    """

    def __init__(self):
        self.sock = os.environ.get(
            "LILJACK_SESSIOND_SOCK",
            str(Path(os.environ.get("XDG_RUNTIME_DIR",
                                    str(Path.home() / ".cache/liljack"))) / "sessiond.sock"))

    def _call(self, obj):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(5)
            s.connect(self.sock)
            body = json.dumps(obj).encode()
            s.sendall(struct.pack(">I", 1 + len(body)) + b"C" + body)
            hdr = b""
            while len(hdr) < 4:
                chunk = s.recv(4 - len(hdr))
                if not chunk:
                    raise OSError("sessiond closed the connection")
                hdr += chunk
            n = struct.unpack(">I", hdr)[0]
            tag = s.recv(1)
            body = b""
            while len(body) < n - 1:
                chunk = s.recv(n - 1 - len(body))
                if not chunk:
                    break
                body += chunk
            return json.loads(body.decode("utf-8", "replace"))
        finally:
            s.close()

    def create(self, agent, project, root, *, cwd=None, intro=None, name=None,
               cols=80, rows=24, room_id=''):
        cwd = project_key(cwd or project)
        argv = Tmux.command(agent)
        if intro is not None:
            from liljack_workspace import clean
            intro = clean(intro, 48000)
            if agent in ("codex", "claude"):
                argv += ["--", intro]
            elif agent == "deepseek":
                argv += ["--prompt", intro]
            else:
                argv = [sys.executable, "-c",
                        "import os,sys; print(sys.argv[1], flush=True); os.execvp(sys.argv[2],sys.argv[2:])",
                        intro, *argv]
        if agent == "deepseek":
            argv = ["env", 'OPENCODE_PERMISSION={"*":"allow"}', *argv]
        name = name or "lj-" + uuid.uuid4().hex[:12]
        if not name.startswith("lj-") or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name):
            raise ValueError("invalid managed session name")
        sid = session_key("native", name)
        argv = _with_system_prompt(argv, agent, project, root, sid, intro, room_id)
        harness = {"deepseek": "opencode", "codex": "codex", "claude": "claude"}.get(agent)
        env = app_child_env()
        env["LILJACK_WORKSPACE_SESSION"] = sid
        env["LILJACK_WORKSPACE_ROOT"] = root
        env["LILJACK_WORKSPACE_PROJECT"] = project_key(project)
        env["LILJACK_AGENT"] = "operator" if agent == "shell" else agent
        if harness:
            env["LILJACK_HARNESS"] = harness
        resp = self._call({"op": "create", "cmd": argv, "cwd": cwd, "cols": cols, "rows": rows,
                           "env": env, "agent": agent, "project": project_key(project),
                           "harness": harness or "", "name": name})
        if "session_id" not in resp:
            raise RuntimeError("sessiond create failed: " + json.dumps(resp)[:200])
        return sid

    def sessions(self, project):
        try:
            resp = self._call({"op": "list"})
        except (OSError, ValueError):
            return []
        out = []
        for s in resp.get("sessions", []):
            if s.get("project") != project_key(project):
                continue
            name = s.get("name") or s.get("id")
            out.append({"session": name,
                        "harness": "native", "backend": "native",
                        "agent": s.get("agent") or "shell",
                        "cwd": s.get("cwd") or project_key(project),
                        "project": s.get("project") or project_key(project),
                        "event": "Exit" if s.get("exited") else "Attached",
                        "state": "exited" if s.get("exited") else "running",
                        "at": utcnow(), "tmux_name": name,
                        "pane": "", "daemon_id": s.get("id"),
                        "id": session_key("native", name)})
        return out

    def kill(self, session, sig=signal.SIGTERM):
        resp = self._call({"op": "kill", "session": session, "signal": int(sig)})
        return resp.get("ok") is True

    def send(self, session, data):
        resp = self._call({"op": "send", "session": session, "data": data})
        return resp.get("ok") is True

    def stop(self, session):
        """Terminate a native session, escalating so a SIGTERM-ignoring process
        (e.g. a bash shell) still stops — the operator's answer to tmux kill-session."""
        self.kill(session, signal.SIGTERM)
        time.sleep(0.2)
        self.kill(session, signal.SIGKILL)


class _NativeStream:
    """The attach-client half of the sessiond protocol, non-blocking.

    Buffers partial frames; the SCM_RIGHTS master fd (if any) rides the first
    recvmsg of its "F" frame. The supervisor stays the sole reader of the pty
    master, so the byte stream (tag "O") is the only thing the VT is fed; the
    fd is kept only for out-of-band ioctls and closed on detach.
    """

    def __init__(self, sock, daemon_id):
        self.sock = sock
        self.daemon_id = daemon_id
        self.buf = b""
        self.fds = []
        self.closed = False

    def send_control(self, obj):
        body = json.dumps(obj).encode()
        self.sock.sendall(struct.pack(">I", 1 + len(body)) + b"C" + body)

    def send_input(self, data):
        self.sock.sendall(struct.pack(">I", 1 + len(data)) + b"I" + data)

    def drain(self):
        """Every complete frame now readable, as (tag, body, fds)."""
        frames = []
        while not self.closed:
            try:
                chunk, anc, _, _ = self.sock.recvmsg(65536, socket.CMSG_SPACE(4))
            except BlockingIOError:
                break
            except OSError:
                self.closed = True
                break
            if not chunk:
                self.closed = True
                break
            self.buf += chunk
            for lvl, typ, data in anc:
                if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    self.fds += list(struct.unpack("i" * (len(data) // 4), data))
        while len(self.buf) >= 4:
            n = struct.unpack(">I", self.buf[:4])[0]
            if len(self.buf) < 4 + n:
                break
            payload = self.buf[4:4 + n]
            self.buf = self.buf[4 + n:]
            tag, body = payload[:1], payload[1:]
            fds = [self.fds.pop(0)] if tag == b"F" and self.fds else []
            frames.append((tag, body, fds))
        return frames


class AttachedTerminal:
    """Only the ATTACH client belongs to the UI; the session survives it.

    tmux backend: a forked `tmux attach-session` child; the tmux server holds
    the pty. native backend: a socket to liljack-sessiond; the supervisor holds
    the pty and forwards the byte stream, so no tmux (and no DCS-eating second
    VT layer) sits between the agent and owkterm's own parser.
    """
    def __init__(self, lib, tmux, row, cols, rows):
        self.vt = Terminal(lib, cols, rows)
        self.session_id, self.name = row["id"], row["tmux_name"]
        self.pending = bytearray()
        self.connected = True
        self.backend = row.get("backend") or row.get("harness") or "tmux"
        self.fd = None
        self.pid = None
        self.sock = None
        self.stream = None
        if self.backend == "native":
            self._attach_native(row, cols, rows)
        else:
            self._attach_tmux(tmux, row, cols, rows)

    def _attach_native(self, row, cols, rows):
        sock_path = os.environ.get(
            "LILJACK_SESSIOND_SOCK",
            str(Path(os.environ.get("XDG_RUNTIME_DIR",
                                    str(Path.home() / ".cache/liljack"))) / "sessiond.sock"))
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(sock_path)
        self.sock.setblocking(False)
        daemon_id = row.get("daemon_id") or row["tmux_name"]
        self.stream = _NativeStream(self.sock, daemon_id)
        self.stream.send_control({"op": "attach", "session": daemon_id})
        self.resize(cols, rows)

    def _attach_tmux(self, tmux, row, cols, rows):
        # Build the sixel terminfo entry once, in the parent, before forking.
        self.terminfo = sixel_terminfo_dir()
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            child_env = app_child_env()
            os.environ.clear()
            os.environ.update(child_env)
            # ⚠ TERM here is the ATTACH CLIENT's, and tmux renders the pane into
            # it. Use the Sxl-capable entry only when it compiled; the fallback
            # is byte-identical to today's xterm-256color for non-sixel content.
            if self.terminfo:
                os.environ["TERM"] = _SIXEL_TERM
                os.environ["TERMINFO"] = self.terminfo
            else:
                os.environ["TERM"] = "xterm-256color"
            os.execv(tmux.binary, [tmux.binary, "-L", tmux.socket, "-f", "/dev/null",
                                  "attach-session", "-t", self.name])
        os.set_blocking(self.fd, False)
        self.resize(cols, rows)

    def resize(self, cols, rows):
        self.vt.resize(cols, rows)
        if self.backend == "native":
            self.stream.send_control({"op": "resize", "session": self.stream.daemon_id,
                                      "cols": cols, "rows": rows})
            return
        # ws_xpixel / ws_ypixel report the PIXEL size of the attach terminal.
        # tmux divides them by cols/rows to learn the per-cell geometry and scale
        # sixel images; zero pixels makes it substitute the placeholder instead.
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", self.vt.rows, self.vt.cols,
                                self.vt.cols * CELL_W, self.vt.rows * CELL_H))

    def send(self, data):
        if not self.connected:
            raise RuntimeError("terminal disconnected; reconnect before sending")
        if isinstance(data, str):
            data = data.encode()
        if len(self.pending) + len(data) > 1024 * 1024:
            raise RuntimeError("terminal input queue is full")
        self.vt.scroll(-100000)
        self.pending.extend(data)

    def paste(self, text):
        data = text.encode()
        if self.vt.state(6):
            data = b"\x1b[200~" + data.replace(b"\x1b[201~", b"") + b"\x1b[201~"
        self.send(data)

    def poll(self):
        changed = False
        if not self.connected:
            return changed
        if self.backend == "native":
            if self.pending:
                try:
                    self.stream.send_input(bytes(self.pending))
                    self.pending.clear()
                except OSError:
                    self.connected = False
                    return changed
            for tag, body, fds in self.stream.drain():
                if fds:
                    # The pty master fd, passed for ioctls only; the supervisor
                    # stays the sole reader. Hold it so TIOCSWINSZ works out of
                    # band; it is closed on detach.
                    if self.fd is not None:
                        os.close(self.fd)
                    self.fd = fds[0]
                    os.set_blocking(self.fd, False)
                if tag == b"O":
                    self.vt.feed(body)
                    reply = self.vt.reply()
                    if reply:
                        self.pending.extend(reply)
                    changed = True
                elif tag == b"C":
                    msg = json.loads(body.decode("utf-8", "replace"))
                    if msg.get("op") == "exited":
                        self.connected = False
            if self.stream.closed and not changed:
                self.connected = False
            return changed
        try:
            if self.pending:
                try:
                    n = os.write(self.fd, self.pending[:65536])
                    del self.pending[:n]
                except BlockingIOError:
                    pass
            # Bound a noisy pane's work so it cannot starve input/other panes.
            for _ in range(8):
                try:
                    data = os.read(self.fd, 32768)
                except BlockingIOError:
                    break
                if not data:
                    self.connected = False
                    break
                self.vt.feed(data)
                reply = self.vt.reply()
                if reply:
                    self.pending.extend(reply)
                changed = True
        except OSError as e:
            if e.errno != errno.EIO:
                raise
            self.connected = False
        return changed

    def close(self):
        if self.backend == "native":
            if self.stream is not None:
                try:
                    self.stream.send_control({"op": "detach", "session": self.stream.daemon_id})
                except OSError:
                    pass
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
            self.vt.close()
            return
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
            # SIGHUP goes to our attach client, not the persistent tmux server.
            try:
                os.kill(self.pid, signal.SIGHUP)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                try:
                    if os.waitpid(self.pid, os.WNOHANG)[0]:
                        break
                except ChildProcessError:
                    break
                time.sleep(.01)
            else:
                try:
                    os.kill(self.pid, signal.SIGKILL)
                    os.waitpid(self.pid, 0)
                except (ProcessLookupError, ChildProcessError):
                    pass
        self.vt.close()


def room_instruction(message, root):
    destination = message.get("destination", "")
    reply_to = destination if destination == "room" or destination.startswith("r-") else message["sender"]
    command = [str(PROJECT_ROOT / "liljack"), "room", "--root", str(root),
               "--project", message["project"], "--reply", message["id"],
               "--to", reply_to, "--text", "YOUR_REPLY"]
    text = "".join(ch for ch in message["text"] if (ch in "\n\t" or unicodedata.category(ch) != "Cc")
                   and ch not in "\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069")
    return (f"the operator via lilJack room ({message['id']}):\n{text}\n\n"
            "Reply in your preferred language using this command, replacing YOUR_REPLY: "
            + shlex.join(command) + "\n")
