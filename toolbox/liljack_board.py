#!/usr/bin/env python3
"""
liljack_board.py — the shared attention board, held as BMD claims.

the operator, 2026-09-05: "make sure lilJack always has context for each agent's task
summary — not after each message, but a general attention context, updatable,
with an easily markable TODO for agents, in BMD format".

The mailbox (`liljack_mail.py`) is chatter: a message is an event, it scrolls
away, and "what is codex doing right now" is only answerable by re-reading the
thread. This is the opposite object — ONE durable board, rewritten in place,
cheap enough to inject as standing context and markable in a single call.

⚠ FORMAT — the three traps, all of them already paid for in this project:

  1. Claims live in the DOCUMENT-LEVEL `[[footer]]`. `bmd.parse_assertion` is
     applied to footer lines ONLY; a claim written as prose inside a
     `[[pasem:]]` parses as text and the file yields ZERO assertions. The pasem
     here is therefore PROSE ONLY, and it is REGENERATED from the claims on
     every write so it can never drift out of agreement with them.
  2. `fact` takes (pred, subj[, obj]) — at most TWO args after the predicate.
     Everything else (state, progress, note, timestamps) is an attribute,
     inside the parens.
  3. Any value carrying `|`, `,`, `'` or `"` must go through `bmd.qval()`.

⚠ NO NEGATION-AS-FAILURE. The toolbox_root engine cannot read an absent claim as
false, so "claude is NOT doing X" is written explicitly with `neg(...)` —
`negate()` — and never left implied by omission.

⚠ CONCURRENCY. Two harnesses write this file and a board is a whole-file
rewrite, so unlike the mailbox it cannot be append-only. Every read-modify-write
runs under an exclusive lock file (`_lock`, O_CREAT|O_EXCL with a stale-lock
steal) and every write lands via `<name>.tmp` + `os.replace`, so a reader never
sees a half-written board and a crashed writer never wedges it permanently.

Stdlib only, plus `the trainer/corpus/bmd.py` loaded BY PATH — the Den imports the
lilJack modules and must not acquire a dependency. the trainer owns bmd.py; this
module reads it and never writes to it.
"""
from __future__ import annotations

import os
import random
import sys
import time
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

AGENTS = ("claude", "codex", "deepseek", "operator")
STATES = ("active", "blocked", "queued", "done")
# Display order on a line: what is moving, what is stuck, what is next, what is
# finished. A done item is the least useful thing on an attention board, so it
# is also the first thing dropped when the digest hits its budget.
_STATE_ORDER = {"active": 0, "blocked": 1, "queued": 2, "done": 3}
_MARK = {"active": "▶", "blocked": "⛔", "queued": "○", "done": "✓"}

PRED_TASK = "agent-task"
PRED_NOTE = "board-note"

MAX_NOTE = 200
MAX_PROGRESS = 32
MAX_TASK = 60
MAX_VALUE = 80             # a board note's value is a short state word, not prose
DIGEST_BUDGET = 900        # chars; this rides in every session's standing context

LOCK_WAIT = 10.0           # how long a writer waits for the board
LOCK_STALE = 60.0          # older than this and the holder is presumed dead


def _sibling() -> Path:
    """the trainer sits beside demo. Reuse lilJack's own root resolution so the
    two agree about which copy of the tree is live."""
    try:
        from liljack_common import PROJECT_ROOT
        return Path(PROJECT_ROOT).parent / "the trainer"
    except Exception:
        return Path(__file__).resolve().parents[2] / "the trainer"


def default_path() -> Path:
    """LILJACK_BOARD, else the sibling the trainer braid when it exists, else
    ~/.cache/liljack/board/agent_board.bmd (standalone checkout)."""
    env = os.environ.get("LILJACK_BOARD")
    if env:
        return Path(env)
    braid = _sibling() / "data" / "braid"
    if braid.is_dir():
        return braid / "agent_board.bmd"
    return Path.home() / ".cache" / "liljack" / "board" / "agent_board.bmd"


_BMD = None


def bmd():
    """Load the trainer's bmd.py by path — the format's ONE implementation.

    Importing it rather than re-deriving the grammar is deliberate: a second
    writer would be a second dialect, and the acceptance test for this board is
    that the trainer's own parser reads back exactly what we wrote.
    """
    global _BMD
    if _BMD is not None:
        return _BMD
    import importlib.util
    # LILJACK_BMD, else the vendored toolbox/bmd.py, else the trainer's copy.
    candidates = [Path(v) for v in [os.environ.get("LILJACK_BMD", "")] if v]
    candidates += [Path(__file__).resolve().parent / "bmd.py",
                   _sibling() / "corpus" / "bmd.py"]
    p = next((c for c in candidates if c.is_file()), candidates[-1])
    spec = importlib.util.spec_from_file_location("liljack_bmd", p)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load bmd.py from {p}")
    mod = importlib.util.module_from_spec(spec)
    # ⚠ Register BEFORE exec_module. bmd.py uses @dataclass, and dataclasses
    # resolves `sys.modules.get(cls.__module__).__dict__` while building the
    # class — for a module loaded by path and not yet registered that lookup
    # returns None and the import dies with an AttributeError inside the
    # stdlib, nowhere near the real cause.
    sys.modules.setdefault("liljack_bmd", mod)
    spec.loader.exec_module(mod)
    _BMD = mod
    return mod


def now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def slug(text: str) -> str:
    return bmd().slug(text, maxlen=MAX_TASK)


# ── locking + atomic write ───────────────────────────────────────────────────

@contextmanager
def _lock(path: Path):
    """Exclusive board lock. O_CREAT|O_EXCL is the atomic primitive; a lock older
    than LOCK_STALE is stolen, because a killed harness must not wedge the board
    for the other one."""
    lock = Path(str(path) + ".lock")
    lock.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + LOCK_WAIT
    fd = None
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            break
        except FileExistsError:
            try:
                age = time.time() - lock.stat().st_mtime
            except FileNotFoundError:
                continue                      # holder released it; retry at once
            if age > LOCK_STALE:
                try:
                    lock.unlink()
                except FileNotFoundError:
                    pass
                continue
            if time.time() > deadline:
                raise TimeoutError(f"board locked by another writer: {lock}")
            time.sleep(0.005 + random.random() * 0.02)
    try:
        os.write(fd, f"{os.getpid()}\n".encode())
        os.close(fd)
        fd = None
        yield
    finally:
        if fd is not None:
            os.close(fd)
        try:
            lock.unlink()
        except FileNotFoundError:
            pass


def _write_atomic(path: Path, text: str) -> None:
    """`.tmp` + os.replace — a reader sees the old board or the new one, never a
    truncated one. Called only under the lock, so one tmp name is enough."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


# ── model ────────────────────────────────────────────────────────────────────

def _item(a) -> dict:
    return {"agent": str(a.args[0]), "task": str(a.args[1]) if len(a.args) > 1 else "",
            "state": a.attrs.get("state", "queued"),
            "progress": a.attrs.get("progress", ""),
            "note": a.attrs.get("note", ""),
            "updated": a.attrs.get("updated", "")}


def _note_row(a) -> dict:
    return {"topic": str(a.args[0]), "value": str(a.args[1]) if len(a.args) > 1 else "",
            "reason": a.attrs.get("reason", ""),
            "updated": a.attrs.get("updated", "")}


def read(path=None) -> dict:
    """Parse the board.

    Strict first: a malformed claim is a real problem and is counted, not
    swallowed. But a board is standing context for two harnesses, so one bad
    hand-edited line must not blind everybody — on a BmdError we re-parse
    permissively and report `errors`, rather than raising into a prompt hook.
    """
    p = Path(path) if path else default_path()
    B = bmd()
    out = {"path": str(p), "exists": p.exists(), "items": [], "notes": [],
           "negs": [], "stray": 0, "errors": 0, "updated": "", "header": {}}
    if not p.exists():
        return out
    text = p.read_text(encoding="utf-8", errors="replace")
    try:
        doc = B.parse(text, strict=True)
    except B.BmdError:
        doc = B.parse(text, strict=False)
        out["errors"] = 1
    out["header"] = dict(doc.header)
    out["updated"] = doc.header.get("updated", "")
    # Document-level footer is the board. Assertions inside a pasem's own footer
    # are legal BMD but are NOT ours to rewrite, so they are counted and left
    # alone rather than silently absorbed or silently dropped.
    out["stray"] = sum(len(ps.assertions) for ps in doc.pasems)
    for a in doc.assertions:
        if a.form == "fact" and a.pred == PRED_TASK and len(a.args) >= 2:
            out["items"].append(_item(a))
        elif a.form == "fact" and a.pred == PRED_NOTE and len(a.args) >= 1:
            out["notes"].append(_note_row(a))
        elif a.form == "neg":
            out["negs"].append({"pred": a.pred,
                                "args": [str(x) for x in a.args],
                                "reason": a.attrs.get("reason", ""),
                                "updated": a.attrs.get("updated", "")})
    return out


def _qattr(v) -> str:
    """Quote an ATTRIBUTE value for the grammar.

    `bmd.qval` covers the separators it knows about (`,` `|` `"` `\'`). Brackets
    need the same treatment and it does not give it to them: the splitters track
    paren DEPTH, and `_CALL` binds greedily to the last `)`, so a value carrying
    an unbalanced bracket — a smiley, "(see note" — moves the end of the
    assertion and shreds every attribute after it. Attributes are safe to quote
    because `_take_attrs` unquotes them on the way back in.
    """
    B = bmd()
    s = str(v).replace("\n", " ").strip()
    q = B.qval(s)
    if q.startswith('"') or not any(c in s for c in "()[]"):
        return q
    return '"' + s.replace('"', "'") + '"'


# ⚠ ARGUMENTS are NOT quotable the way attributes are: `parse_assertion` runs
# `_split_top` over them and never unquotes, so a quoted arg reads back WITH its
# quotes and the value no longer round-trips. Predicate arguments therefore have
# to be separator-free by construction — tasks and topics are slugged, and a
# note's value is refused rather than silently mangled. Prose goes in `reason`.
_UNSAFE_ARG = set(',|"\'()[]')


def _check_arg(value: str, field: str) -> str:
    s = str(value).strip()
    bad = sorted(set(s) & _UNSAFE_ARG)
    if bad:
        raise ValueError(f"{field} may not contain {''.join(bad)!r} — it is a bare "
                         "BMD argument and cannot be quoted; put prose in reason=")
    return s


def _sanitize_value(value):
    """A note VALUE is a bare BMD argument and cannot be quoted (the parser never
    unquotes args), so prose carrying separators used to REFUSE the whole board
    update (item 7: the error arrived after the prose was composed, and the
    update was lost). Instead, sanitize the value to a separator-free short word
    and return the dropped prose so `set_note` can fold it into `reason`, which
    is an attribute and IS quotable. Returns (safe, dropped)."""
    s = str(value).strip()
    safe = " ".join("".join(" " if c in _UNSAFE_ARG else c for c in s).split())
    return safe, (s if safe != s else "")


def _attrs(**kw) -> dict:
    """Attribute dict with every value quoted for the grammar. Values are stored
    already-quoted because Assertion.render() emits them verbatim."""
    return {k: _qattr(v) for k, v in kw.items() if v not in (None, "")}


# ── shared primitives ────────────────────────────────────────────────────────
# The lock, the atomic write and the attribute quoting are not board-specific:
# they are the answer to "how do two harnesses write one BMD file without losing
# a write". `liljack_tasks` reuses them by these names rather than shipping a
# second dialect of the same three decisions — a second copy is how the era
# helpers in the trainer ended up disagreeing three ways.
lock = _lock
write_atomic = _write_atomic
qattr = _qattr
qattr_all = _attrs
check_arg = _check_arg


def _prose(items: list, notes: list, negs: list) -> str:
    """The human-readable body — DERIVED, never authored. Regenerating it on
    every write is what makes it impossible for the prose and the claims to
    disagree; a hand-written summary is a second source of truth."""
    lines = ["What each agent is working on. This section is GENERATED from the",
             "claims in the footer on every update — edit the claims (or call",
             "liljack_board.upsert / mark), never this text.", ""]
    for ag in AGENTS:
        mine = [i for i in items if i["agent"] == ag]
        if not mine:
            lines.append(f"{ag}: nothing on the board.")
            continue
        mine.sort(key=lambda i: _STATE_ORDER.get(i["state"], 9))
        lines.append(f"{ag}:")
        for i in mine:
            bit = f"  {_MARK.get(i['state'], '·')} {i['task']} — {i['state']}"
            if i["progress"]:
                bit += f" {i['progress']}"
            if i["note"]:
                bit += f" ({i['note']})"
            lines.append(bit)
    if notes:
        lines.append("")
        lines.append("board notes:")
        for n in notes:
            bit = f"  {n['topic']}: {n['value']}"
            if n["reason"]:
                bit += f" — {n['reason']}"
            lines.append(bit)
    if negs:
        lines.append("")
        lines.append("explicitly NOT the case (the engine has no negation-as-failure,")
        lines.append("so an absence is written down rather than left to be inferred):")
        for n in negs:
            bit = "  not " + "/".join(n["args"])
            if n["reason"]:
                bit += f" — {n['reason']}"
            lines.append(bit)
    return "\n".join(lines)


def _render(items: list, notes: list, negs: list, header: dict) -> str:
    B = bmd()
    doc = B.BmdDoc()
    doc.header = dict(header)
    doc.header.update({"title": "lilJack agent attention board",
                       "kind": "agent-board", "domain": "liljack",
                       "version": "1", "updated": now()})
    doc.pasems = [B.Pasem(name="agent-board", text=_prose(items, notes, negs))]
    out = []
    for i in items:
        out.append(B.fact(PRED_TASK, i["agent"], i["task"],
                          **_attrs(state=i["state"], progress=i["progress"],
                                   note=i["note"], updated=i["updated"])))
    for n in notes:
        out.append(B.fact(PRED_NOTE, n["topic"], n["value"],
                          **_attrs(reason=n["reason"], updated=n["updated"])))
    for n in negs:
        out.append(B.Assertion("neg", n["pred"], list(n["args"]),
                               _attrs(reason=n["reason"], updated=n["updated"])))
    doc.assertions = out
    return doc.render()


def _validate(agent: str, task: str, state=None) -> tuple:
    agent = (agent or "").strip().lower()
    if agent not in AGENTS:
        raise ValueError(f"agent must be one of {AGENTS}")
    t = slug(task or "")
    if not task or t == "unnamed":
        raise ValueError("task is required")
    if state is not None:
        state = str(state).strip().lower()
        if state not in STATES:
            raise ValueError(f"state must be one of {STATES}")
    return agent, t, state


# ── writes ───────────────────────────────────────────────────────────────────

def upsert(agent: str, task: str, state=None, progress=None, note=None,
           path=None) -> dict:
    """Add or update ONE item, in place. Fields left as None keep their value —
    marking a task done must not silently erase its progress or its note."""
    agent, task, state = _validate(agent, task, state)
    p = Path(path) if path else default_path()
    with _lock(p):
        board = read(p)
        items, notes, negs = board["items"], board["notes"], board["negs"]
        row = next((i for i in items if i["agent"] == agent and i["task"] == task), None)
        if row is None:
            row = {"agent": agent, "task": task, "state": state or "queued",
                   "progress": "", "note": "", "updated": ""}
            items.append(row)
        elif state:
            row["state"] = state
        if progress is not None:
            row["progress"] = str(progress).strip()[:MAX_PROGRESS]
        if note is not None:
            row["note"] = str(note).strip()[:MAX_NOTE]
        row["updated"] = now()
        # A positive claim retracts its own explicit negation, or the board
        # would assert both at once.
        negs = [n for n in negs
                if not (n["pred"] == PRED_TASK and n["args"][:2] == [agent, task])]
        _write_atomic(p, _render(items, notes, negs, board["header"]))
        return dict(row)


def mark(agent: str, task: str, state: str, path=None) -> dict:
    """The 'easily markable' call: done | blocked | active | queued."""
    if not state:
        raise ValueError(f"state is required (one of {STATES})")
    return upsert(agent, task, state=state, path=path)


def set_note(topic: str, value: str, reason=None, path=None) -> dict:
    """A board-level fact that belongs to no single agent (a held queue, a run
    everyone is waiting on)."""
    t = slug(topic or "")
    if not topic or t == "unnamed":
        raise ValueError("topic is required")
    if not value:
        raise ValueError("value is required")
    value, dropped = _sanitize_value(value)
    if not value:
        raise ValueError("value is required")
    overflow = ""
    if len(value) > MAX_VALUE:
        overflow = value[MAX_VALUE:]
        value = value[:MAX_VALUE - 1] + "…"
    p = Path(path) if path else default_path()
    with _lock(p):
        board = read(p)
        row = next((n for n in board["notes"] if n["topic"] == t), None)
        if row is None:
            row = {"topic": t, "value": "", "reason": "", "updated": ""}
            board["notes"].append(row)
        row["value"] = value
        parts = [str(reason).strip()] if reason is not None and str(reason).strip() else []
        if dropped:
            parts.append("value sanitized (bare BMD arg): " + dropped)
        if overflow:
            parts.append("value truncated past " + str(MAX_VALUE) + ": " + overflow)
        if reason is not None or parts:
            row["reason"] = "; ".join(parts)[:MAX_NOTE]
        row["updated"] = now()
        _write_atomic(p, _render(board["items"], board["notes"], board["negs"],
                                 board["header"]))
        return dict(row)


def negate(agent: str, task: str, reason: str = "", path=None) -> dict:
    """State that an agent is NOT on a task, explicitly.

    ⚠ This is not bookkeeping. The toolbox_root engine has no negation-as-failure, so
    an item merely missing from the board can never be read as "nobody is doing
    this" — if that absence is load-bearing (a task moved to the other harness),
    it has to be a claim.
    """
    agent, task, _ = _validate(agent, task)
    p = Path(path) if path else default_path()
    with _lock(p):
        board = read(p)
        items = [i for i in board["items"]
                 if not (i["agent"] == agent and i["task"] == task)]
        negs = [n for n in board["negs"]
                if not (n["pred"] == PRED_TASK and n["args"][:2] == [agent, task])]
        row = {"pred": PRED_TASK, "args": [agent, task],
               "reason": str(reason).strip()[:MAX_NOTE], "updated": now()}
        negs.append(row)
        _write_atomic(p, _render(items, board["notes"], negs, board["header"]))
        return dict(row)


# ── the standing-context block ───────────────────────────────────────────────

def digest(agent=None, path=None, budget: int = DIGEST_BUDGET) -> str:
    """The block that gets injected: the caller's own items first, then one line
    per other agent, then board notes. Bounded — it rides in every session.

    Returns "" when there is no board, so a missing file costs nothing and the
    hook never has to know whether one exists.
    """
    try:
        board = read(path)
    except Exception:
        return ""
    if not board["exists"] or not (board["items"] or board["notes"] or board["negs"]):
        return ""
    me = (agent or "").strip().lower()
    order = ([me] if me in AGENTS else []) + [a for a in AGENTS if a != me]

    def line(ag):
        mine = sorted([i for i in board["items"] if i["agent"] == ag],
                      key=lambda i: _STATE_ORDER.get(i["state"], 9))
        label = f"you ({ag})" if ag == me else ag
        if not mine:
            return f"{label}: —", []
        bits = []
        for i in mine:
            b = f"{_MARK.get(i['state'], '·')} {i['task']}"
            if i["progress"]:
                b += f" {i['progress']}"
            bits.append((i["state"], b))
        return f"{label}: ", bits

    head = f"[lilJack board] updated {board['updated'] or '?'} · " \
           "shared attention board — board_update to change it"
    body = []
    for ag in order:
        label, bits = line(ag)
        body.append((label, bits))
    notes = " · ".join(f"{n['topic']}={n['value']}" for n in board["notes"])
    negs = " · ".join("not " + "/".join(n["args"]) +
                      (f" ({n['reason']})" if n["reason"] else "")
                      for n in board["negs"])

    def assemble(drop_done: bool, drop_negs: bool) -> str:
        out = [head]
        for label, bits in body:
            if not bits:
                out.append(label)
                continue
            keep = [b for s, b in bits if not (drop_done and s == "done")]
            out.append(label + (" · ".join(keep) if keep else "—"))
        if notes:
            out.append("notes: " + notes)
        if negs and not drop_negs:
            out.append(negs)
        return "\n".join(out)

    # Shed the least informative content first — finished work, then the
    # explicit negations — before resorting to a hard cut.
    for dd, dn in ((False, False), (True, False), (True, True)):
        text = assemble(dd, dn)
        if len(text) <= budget:
            return text
    return text[:budget - 1].rstrip() + "…"


# ── CLI ──────────────────────────────────────────────────────────────────────

def main():
    import argparse
    import json
    ap = argparse.ArgumentParser(description="lilJack shared attention board (BMD)")
    ap.add_argument("--path", default="")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show")
    d = sub.add_parser("digest"); d.add_argument("--agent", default="")
    u = sub.add_parser("set"); u.add_argument("agent"); u.add_argument("task")
    u.add_argument("--state", default=None); u.add_argument("--progress", default=None)
    u.add_argument("--note", default=None)
    m = sub.add_parser("mark"); m.add_argument("agent"); m.add_argument("task")
    m.add_argument("state", choices=STATES)
    n = sub.add_parser("note"); n.add_argument("topic"); n.add_argument("value")
    n.add_argument("--reason", default=None)
    g = sub.add_parser("not"); g.add_argument("agent"); g.add_argument("task")
    g.add_argument("--reason", default="")
    a = ap.parse_args()
    p = a.path or None
    if a.cmd == "show":
        print(json.dumps(read(p), indent=2, ensure_ascii=False))
    elif a.cmd == "digest":
        print(digest(a.agent or None, p))
    elif a.cmd == "set":
        print(json.dumps(upsert(a.agent, a.task, a.state, a.progress, a.note, p)))
    elif a.cmd == "mark":
        print(json.dumps(mark(a.agent, a.task, a.state, p)))
    elif a.cmd == "note":
        print(json.dumps(set_note(a.topic, a.value, a.reason, p)))
    elif a.cmd == "not":
        print(json.dumps(negate(a.agent, a.task, a.reason, p)))


if __name__ == "__main__":
    main()
