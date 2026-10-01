#!/usr/bin/env python3
"""The sixel TRANSPORT: tmux must stop eating the DCS.

tmux gates sixel on the ATTACH CLIENT's TERM carrying the `Sxl` boolean
capability (tmux 3.7 tty-features.c: tty_feature_sixel_capabilities={"Sxl"},
read via tigetflag) AND on the client reporting its PIXEL size (ws_xpixel /
ws_ypixel — zero pixels forces the "SIXEL IMAGE (WxH)" placeholder). This file
pins both, so a revert of either half fails the suite.

Evidence-backed route choice: allow-passthrough is NOT the fix — tmux parses
sixel natively (input.c) and only the Sxl + pixel-size gate controls it;
allow-passthrough only covers the `tmux;`-prefixed escape.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

SRC = ROOT / "toolbox" / "liljack_app" / "terminfo" / "xterm-256color-sixel.src"
TERM = "xterm-256color-sixel"

print("terminfo: the Sxl entry compiles and is otherwise xterm-256color")
check(SRC.exists(), "the sixel terminfo source ships with the app")
tic = shutil.which("tic")
check(tic is not None, "tic is available")

d = Path(tempfile.mkdtemp(prefix="lj-term-"))
if tic and SRC.exists():
    r = subprocess.run([tic, "-x", "-o", str(d), str(SRC)],
                       capture_output=True, text=True)
    check(r.returncode == 0, "tic -x compiles xterm-256color-sixel")
    env = {**os.environ, "TERMINFO": str(d)}
    out = subprocess.run(["infocmp", "-x", "-1", TERM],
                         capture_output=True, text=True, env=env).stdout
    check("Sxl" in out, "the compiled entry carries the Sxl sixel flag")
    base = subprocess.run(["infocmp", "-x", "-1", "xterm-256color"],
                          capture_output=True, text=True).stdout
    def norm(s):
        # keep only capability lines (indented), dropping the name line, any
        # header comment, and the one Sxl flag we added.
        return "\n".join(l for l in s.splitlines()
                         if l.startswith("\t") and l.strip() != "Sxl,")
    check(norm(base) == norm(out),
          "every non-sixel capability is byte-identical to xterm-256color")

print("runtime: the attach client picks the Sxl entry and reports pixel size")
import liljack_app.runtime as R
check(R.CELL_W == 10 and R.CELL_H == 20, "cell geometry matches c_render.h (10x20)")
check(hasattr(R, "sixel_terminfo_dir"), "the terminfo compiler helper exists")
cache = Path(tempfile.mkdtemp(prefix="lj-cache-"))
os.environ["LILJACK_CACHE"] = str(cache)
tdir = R.sixel_terminfo_dir()
check(tdir is not None and (Path(tdir) / "x" / TERM).exists(),
      "sixel_terminfo_dir() compiles the entry into the cache, idempotently")
os.environ.pop("LILJACK_CACHE", None)

# The resize packs ws_xpixel/ws_ypixel = cols*CELL_W / rows*CELL_H, never zero.
import fcntl, pty, termios  # noqa: E402
pid, fd = pty.fork()
if pid == 0:
    os._exit(0)
try:
    R_ = struct.pack("HHHH", 24, 80, 800, 480)  # 80x24 @ 10x20 px
    fcntl.ioctl(fd, termios.TIOCSWINSZ, R_)
    got = fcntl.ioctl(fd, termios.TIOCGWINSZ, b"\0" * 8)
    row, col, xp, yp = struct.unpack("HHHH", got)
    check((col, row, xp, yp) == (80, 24, 800, 480),
          "the attach pty carries the pixel size (800x480), not zero")
finally:
    os.close(fd)

# ── end-to-end through a real tmux, when one is available ───────────────────
TMUX = shutil.which("tmux") or (
    str(Path.home() / ".cache/liljack/build/usr/bin/tmux")
    if (Path.home() / ".cache/liljack/build/usr/bin/tmux").is_file() else None)
if TMUX and os.access(TMUX, os.X_OK):
    print("end-to-end: tmux re-encodes sixel with Sxl + pixels, placeholder without")
    import select, signal  # noqa: E402
    SOCK = "sxtest-reg"
    SIXEL = b"\x1bP0;0;0q#0;2;100;0;0!2~\x1b\\"
    def tm(args):
        return subprocess.run([TMUX, "-L", SOCK, "-f", "/dev/null"] + args,
                              capture_output=True, text=True, timeout=10)
    def attach(term, pixels):
        env = dict(os.environ); env["TERM"] = term; env.pop("TMUX", None)
        if pixels: env["TERMINFO"] = str(d)
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(TMUX, [TMUX, "-L", SOCK, "-f", "/dev/null",
                             "attach-session", "-t", "test"], env)
        if pixels:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480))
        data = b""; dl = time.time() + 4
        while time.time() < dl:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: c = os.read(fd, 65536)
                except OSError: break
                if not c: break
                data += c
        try: os.kill(pid, signal.SIGHUP)
        except Exception: pass
        try: os.close(fd)
        except Exception: pass
        return data
    f = tempfile.NamedTemporaryFile(delete=False); f.write(SIXEL); f.close()
    tm(["start-server"])
    tm(["new-session", "-d", "-s", "test", "sh", "-c", f"sleep 1; cat {f.name}; sleep 60"])
    time.sleep(0.2)
    base = attach("xterm-256color", pixels=True)
    six  = attach(TERM, pixels=True)
    check(b"SIXEL IMAGE" in base and b"\x1bP" not in base,
          "without Sxl the placeholder appears (the regression this task fixes)")
    check(b"SIXEL IMAGE" not in six and b"\x1bP" in six and b"\x1b\\" in six,
          "with Sxl + pixels tmux re-encodes the sixel instead of the placeholder")
    tm(["kill-server"]); os.unlink(f.name)
else:
    print("end-to-end: skipped — no usable tmux binary on this box")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
