#!/usr/bin/env python3
"""actionable-dms-backend: DMs to the operator are per-room actionable rows, and a
reply reaches the asking SESSION.

the operator 2026-09-12 07:01: "lets add DMs in Actionable room tile tab so agents can
ask me questions". Rules planted here:
  1. a room shows only its own DMs (mail carries a room since mailbox-session-scoping);
  2. an untagged legacy row shows in every room rather than being lost;
  3. unread_dms counts what the operator has not read yet — the tab badge;
  4. a reply is sent as operator to the ASKER'S SESSION and stamped with the room;
  5. replying marks the original read, so it leaves the badge;
  6. replying to a message of another room, or with empty text, is refused.
"""
import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT))
import liljack_mail as M  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

root = Path(tempfile.mkdtemp(prefix="lj-dms-"))
ws = root / "workspace"; ws.mkdir()
con = sqlite3.connect(ws / "workspace.sqlite3")
con.executescript("CREATE TABLE room_members(session_id TEXT PRIMARY KEY, room_id TEXT);"
                  "CREATE TABLE sessions(id TEXT PRIMARY KEY, project TEXT, data TEXT);")
con.executemany("INSERT INTO room_members VALUES(?,?)",
                [("s-cx-a", "r-a"), ("s-ds-a", "r-a"), ("s-cx-b", "r-b")])
for sid, agent in [("s-cx-a", "codex"), ("s-ds-a", "deepseek"), ("s-cx-b", "codex")]:
    con.execute("INSERT INTO sessions VALUES(?,?,?)", (sid, "/p", json.dumps({"agent": agent})))
con.commit(); con.close()

keep = {k: os.environ.get(k) for k in ("LILJACK_WORKSPACE_ROOT", "LILJACK_WORKSPACE_SESSION", "LILJACK_CACHE")}
os.environ["LILJACK_WORKSPACE_ROOT"] = str(ws)
os.environ["LILJACK_CACHE"] = str(root)          # mailbox lives here
try:
    # Reload so the module picks up the CACHE override for its default paths.
    import importlib
    M = importlib.reload(M)

    os.environ["LILJACK_WORKSPACE_SESSION"] = "s-cx-a"
    M.send("the operator, which corpus should I build first?", "operator", frm="codex", subject="corpus order")
    os.environ["LILJACK_WORKSPACE_SESSION"] = "s-ds-a"
    M.send("the operator, may I read the PNGs?", "operator", frm="deepseek", subject="vision ok?")
    os.environ["LILJACK_WORKSPACE_SESSION"] = "s-cx-b"
    M.send("the operator, trio launch go?", "operator", frm="codex", subject="trio")
    M._append(root / "mailbox.jsonl", {"id": "legacy-1", "ts": "2026-09-12T00:00:00+00:00",
              "frm": "codex", "to": "operator", "subject": "old", "text": "untagged legacy"})

    from liljack_app.backend import Backend
    b = Backend.__new__(Backend)          # no tmux/store needed for these methods
    b.project = "/p"

    a = b.room_dms("r-a")
    texts = [d["text"] for d in a]
    check("the operator, which corpus should I build first?" in texts
          and "the operator, may I read the PNGs?" in texts
          and "the operator, trio launch go?" not in texts,
          "room r-a shows its own two DMs and not room r-b's")
    check("untagged legacy" in texts, "an untagged legacy row is shown rather than lost")
    check([d["text"] for d in b.room_dms("r-b")].count("the operator, trio launch go?") == 1,
          "room r-b shows only its own DM")
    row = next(d for d in a if d["agent"] == "deepseek")
    check(row["session"] == "s-ds-a" and row["subject"] == "vision ok?" and not row["read"],
          "a DM carries the asking session, subject and unread state")
    check(sum(1 for d in a if not d["read"]) == 3, "unread count is the tab badge (3 unread in r-a: two DMs + legacy)")

    res = b.room_dm_reply("r-a", row["id"], "Yes — vision is on, read one capture per verdict.")
    check(res["to"] == "deepseek" and res["to_session"] == "s-ds-a" and res["room"] == "r-a",
          "the reply is session-addressed to the asker and stamped with the room")
    got = [m["text"] for m in M.inbox("deepseek", session="s-ds-a")]
    check(got == ["Yes — vision is on, read one capture per verdict."],
          "the asking session receives the reply")
    check(M.inbox("deepseek", session="s-cx-a") == [] if False else
          not any(m["text"].startswith("Yes — vision") for m in M.inbox("codex", session="s-cx-a")),
          "no other session of any agent receives it")
    after = b.room_dms("r-a")
    answered = next(d for d in after if d["id"] == row["id"])
    check(answered["read"] and sum(1 for d in after if not d["read"]) == 2,
          "replying marks the original read, so it leaves the badge")

    for bad, label in ((("r-b", row["id"], "x"), "a DM from another room"),
                       (("r-a", row["id"], "   "), "an empty reply")):
        try:
            b.room_dm_reply(*bad); check(False, f"{label} is refused")
        except ValueError:
            check(True, f"{label} is refused")
finally:
    for k, v in keep.items():
        if v is None: os.environ.pop(k, None)
        else: os.environ[k] = v

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
