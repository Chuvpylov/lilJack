#!/usr/bin/env python3
"""status_ribbon.py — the lilJack status ribbon, for the app's bottom bar.

Owner: claude (2026-09-08, ownership agreed on thread liljack-app-rooms).

⚠ THIS DOES NOT COMPUTE THE RIBBON. `toolbox/liljack_status.sh` already does,
and it is already wired into ~/.claude/settings.json as Claude Code's
statusLine, so it is the live source of truth the operator reads all day. This module
RUNS it and shapes the output for drawing. A second implementation of that
string would drift from the one the operator actually sees, and then the app would be
confidently wrong about his own machine.

    lilJack (￣ω￣)[RUNNING·CMDS] │ ⬡R1▰▰▰▰▰ │ ctx[KB:165·m:14·⚙:2·p:2126]
      │ ⎇ work/2026-09-08-night(54✎) │ 📦145 ~3000msg ⚠43h46m
      │ ⚡node1:0.97 │ ☑277/438

TWO THINGS THAT DRIVE THE DESIGN:

1. ⚠ THE SCRIPT COSTS ~350 ms. The app paints every 25 ms. Calling it inline
   would stall the UI for fourteen frames, so it is refreshed on a BACKGROUND
   thread and `fields()` returns the last known value immediately, always.
   `fields()` never blocks, never raises, never runs a subprocess.

2. ⚠ SPLIT ON THE STRING, NOT WITH `tr`. The separator U+2502 is multi-byte and
   `tr` operates on bytes — piping this through `tr '│' '\n'` shreds every
   other glyph in the line. Python's str.split is codepoint-aware; use it.

Fields are classified by CONTENT, not by position, so the renderer can colour
them semantically and a change to the script's field order does not silently
recolour the bar.
"""
from __future__ import annotations

import os
import subprocess
import threading
import time
from pathlib import Path

SEP = "│"                                   # │
REPO = Path(os.environ.get("LILJACK_TOOLBOX_ROOT", str(Path(__file__).resolve().parents[1])))
PROJECT_ROOT = REPO                                   # compatibility alias
SCRIPT = REPO / "toolbox" / "liljack_status.sh"
REFRESH_S = 5.0                                  # script is ~350ms; 5s is ample
TIMEOUT_S = 8.0
STALE_AFTER_S = 30.0                             # older than this is reported stale


def classify(text: str) -> str:
    """Semantic kind of one field, from its content. Position is not load-bearing:
    the script may gain or reorder fields and colours must not follow blindly."""
    t = text.strip()
    if t.startswith("lilJack"):     return "identity"
    if t.startswith("⬡"):      return "ring"        # ⬡
    if t.startswith("ctx["):        return "context"
    if t.startswith("⎇"):      return "git"         # ⎇
    if t.startswith("\U0001F4E6"):  return "archive"     # 📦
    if t.startswith("⚡"):      return "node"        # ⚡
    if t.startswith("☑"):      return "todo"        # ☑
    return "other"


class Ribbon:
    """Background-refreshed status ribbon. Construct once; poll fields()."""

    def __init__(self, script: Path | None = None, refresh_s: float = REFRESH_S,
                 autostart: bool = True):
        self.script = Path(script) if script else SCRIPT
        self.refresh_s = float(refresh_s)
        self._lock = threading.Lock()
        self._raw = ""
        self._at = 0.0
        self._err: str | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        if autostart:
            self.start()

    # ── lifecycle ─────────────────────────────────────────────────────────
    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True,
                                        name="lj-ribbon")
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)

    def _loop(self):
        while not self._stop.is_set():
            self.refresh()
            self._stop.wait(self.refresh_s)

    # ── the one place a subprocess runs ───────────────────────────────────
    def refresh(self) -> bool:
        """Run the script once. Returns True on success. Never raises."""
        if not self.script.exists():
            with self._lock:
                self._err = f"status script not found: {self.script}"
            return False
        try:
            # stdin=DEVNULL is load-bearing, not tidiness: capture_output only
            # redirects stdout/stderr, so without this the status script inherits
            # the app's stdin — which in --tui is the RAW TERMINAL — and anything
            # it spawns can swallow keystrokes meant for the workspace.
            r = subprocess.run(["bash", str(self.script)], capture_output=True,
                               stdin=subprocess.DEVNULL,
                               text=True, timeout=TIMEOUT_S,
                               cwd=str(PROJECT_ROOT), env=dict(os.environ))
            out = (r.stdout or "").strip()
            if not out:
                with self._lock:
                    self._err = (r.stderr or "").strip()[:200] or "empty output"
                return False
            with self._lock:
                self._raw, self._at, self._err = out, time.time(), None
            return True
        except subprocess.TimeoutExpired:
            with self._lock:
                self._err = f"status script exceeded {TIMEOUT_S:.0f}s"
        except Exception as e:                                   # noqa: BLE001
            with self._lock:
                self._err = f"{type(e).__name__}: {e}"
        return False

    # ── what the renderer calls, every frame ──────────────────────────────
    def fields(self) -> list:
        """[{text, kind, stale}] — instant, non-blocking, never raises.

        Before the first successful refresh this returns a single honest
        placeholder rather than an empty bar: a blank ribbon reads as "nothing
        is wrong", which is exactly what a missing status must not say."""
        with self._lock:
            raw, at, err = self._raw, self._at, self._err
        if not raw:
            return [{"text": err or "status: starting…",
                     "kind": "error" if err else "other", "stale": True}]
        stale = (time.time() - at) > STALE_AFTER_S
        return [{"text": f.strip(), "kind": classify(f), "stale": stale}
                for f in raw.split(SEP) if f.strip()]

    def line(self) -> str:
        """The ribbon as one string, for a plain/ANSI bar."""
        return f" {SEP} ".join(f["text"] for f in self.fields())

    @property
    def age_s(self) -> float:
        with self._lock:
            return time.time() - self._at if self._at else float("inf")

    @property
    def error(self):
        with self._lock:
            return self._err


_default: Ribbon | None = None


def ribbon() -> Ribbon:
    """Process-wide instance, started on first use."""
    global _default
    if _default is None:
        _default = Ribbon()
    return _default


if __name__ == "__main__":
    import json, sys
    r = Ribbon(autostart=False)
    ok = r.refresh()
    if "--json" in sys.argv:
        print(json.dumps({"ok": ok, "age_s": round(r.age_s, 2),
                          "error": r.error, "fields": r.fields()},
                         ensure_ascii=False, indent=1))
    else:
        print(r.line())
        for f in r.fields():
            print(f"  {f['kind']:<9} {f['text']}")
    sys.exit(0 if ok else 1)
