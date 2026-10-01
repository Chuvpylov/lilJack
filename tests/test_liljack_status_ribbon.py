#!/usr/bin/env python3
"""Planted tests for the status ribbon adapter.

The ones that matter: fields() must NEVER block the UI thread (the script costs
~350ms and the app paints every 25ms), must never raise, and must never return
an empty bar — a blank ribbon reads as "nothing is wrong".
"""
import sys, time, tempfile, os
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolbox" / "liljack_app"))
import status_ribbon as SR

ok = True
def check(name, cond, extra=""):
    global ok
    print(("  ok   " if cond else "  FAIL ") + name + (("  " + extra) if extra else ""))
    ok = ok and bool(cond)

tmp = Path(tempfile.mkdtemp())

def script(body, name="s.sh"):
    p = tmp / name
    p.write_text("#!/bin/bash\n" + body + "\n")
    p.chmod(0o755)
    return p

# ── 1. the multi-byte separator trap ──────────────────────────────────────
s = script('echo "a │ b │ c"')
r = SR.Ribbon(s, autostart=False); r.refresh()
f = r.fields()
check("splits on the multi-byte U+2502, not bytes", [x["text"] for x in f] == ["a","b","c"],
      str([x["text"] for x in f]))

# ── 2. classification is by CONTENT, not position ─────────────────────────
s = script('echo "☑277/438 │ lilJack (x)[Y] │ ⎇ main │ ctx[a] │ ⬡R1 │ ⚡n:1 │ 📦9"')
r = SR.Ribbon(s, autostart=False); r.refresh()
kinds = [x["kind"] for x in r.fields()]
check("classifies by content even when the order is scrambled",
      kinds == ["todo","identity","git","context","ring","node","archive"], str(kinds))

# ── 3. fields() NEVER BLOCKS — the whole reason this module exists ────────
s = script('sleep 3; echo "slow │ bar"')
r = SR.Ribbon(s, refresh_s=0.05, autostart=True)
t0 = time.time()
for _ in range(200):            # 200 "frames"
    r.fields()
elapsed = time.time() - t0
check("200 fields() calls while the script sleeps 3s stay non-blocking",
      elapsed < 0.5, f"{elapsed*1000:.0f}ms for 200 calls")
check("  ...and it reports honestly before the first result",
      r.fields()[0]["stale"] is True)
r.stop()

# ── 4. never empty, never raises ──────────────────────────────────────────
r = SR.Ribbon(tmp / "does-not-exist.sh", autostart=False)
check("missing script does not raise", r.refresh() is False)
f = r.fields()
check("  ...and returns an honest placeholder, NOT an empty bar", len(f) == 1 and f[0]["text"])
check("  ...marked as an error", f[0]["kind"] == "error", f[0]["text"][:40])

s = script('exit 7')
r = SR.Ribbon(s, autostart=False)
check("failing script does not raise", r.refresh() is False)
check("  ...bar is still non-empty", len(r.fields()) >= 1)

s = script('echo "" ')
r = SR.Ribbon(s, autostart=False)
check("empty output is treated as failure", r.refresh() is False)

# ── 5. a good value survives a later failure ──────────────────────────────
good = script('echo "alpha │ beta"', "g.sh")
r = SR.Ribbon(good, autostart=False); r.refresh()
r.script = tmp / "gone.sh"
r.refresh()
check("last good value is kept when a later refresh fails",
      [x["text"] for x in r.fields()] == ["alpha","beta"])
check("  ...and error is surfaced rather than hidden", r.error is not None)

# ── 6. staleness is reported, not hidden ──────────────────────────────────
r = SR.Ribbon(good, autostart=False); r.refresh()
check("fresh value is not stale", r.fields()[0]["stale"] is False)
r._at = time.time() - (SR.STALE_AFTER_S + 5)
check("an old value is MARKED stale rather than shown as current",
      r.fields()[0]["stale"] is True)

# ── 7. the real script, if present ────────────────────────────────────────
if SR.SCRIPT.exists():
    r = SR.Ribbon(autostart=False)
    if r.refresh():
        kinds = {x["kind"] for x in r.fields()}
        check("real liljack_status.sh yields the identity field", "identity" in kinds, str(sorted(kinds)))
        check("  ...and is not classified as all-other", kinds != {"other"})
    else:
        print("  skip  real script present but did not produce output here")

print("\n" + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
