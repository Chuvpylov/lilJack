#!/usr/bin/env python3
"""worker_wake.json is re-read and re-written on every heartbeat call; it grew
forever (1376 keys on 2026-10-15) because nothing ever removed a nudge memo.
Memos whose last attempt is older than a week are dropped with their :n and
:reason companions; recent memos and unavailable/blocked records stay."""
import json
import os
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
import worker_wake as W

with tempfile.TemporaryDirectory() as tmp:
    W.STATE = Path(tmp) / "wake.json"
    now = time.time()
    old = now - 8 * 86400
    W.STATE.write_text(json.dumps({
        "s-old:task": old, "s-old:task:n": 3, "s-old:task:reason": "x",
        "s-new:task": now - 60, "s-new:task:n": 1,
        "unavail:s-old": {"reason": "gone", "at": old},
    }))
    W.record("s-new", "other", now=now)
    s = json.loads(W.STATE.read_text())
    bad = [k for k in ("s-old:task", "s-old:task:n", "s-old:task:reason") if k in s]
    keep = [k for k in ("s-new:task", "s-new:task:n", "s-new:other", "unavail:s-old") if k not in s]
    if bad or keep:
        print(f"FAIL prune: stale kept {bad}, live lost {keep}")
        sys.exit(1)
print("PASS worker_wake state drops week-old nudge memos")
