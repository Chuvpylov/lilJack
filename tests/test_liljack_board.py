#!/usr/bin/env python3
"""test_liljack_board.py — the shared attention board (toolbox/liljack_board.py).

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

The format checks are the point of this file. Every one of them corresponds to a
trap this project has already paid for once: a claim written inside a pasem
yielding zero assertions, a `fact` given more than two arguments, an unquoted
separator shredding an attribute list, and an absent claim being read as false.
"""
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
import liljack_board as B          # noqa: E402

PASS = FAIL = 0


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def tmp_board(name="board") -> Path:
    return Path(tempfile.mkdtemp(prefix="ljboard-")) / f"{name}.bmd"


def main():
    bmd = B.bmd()
    # ⚠ Redirect the DEFAULT board before anything writes. A `path=` argument
    # forgotten on a single call in an earlier draft of this file wrote a test
    # fixture straight into the live board — the suite must be incapable of
    # touching it, not merely careful.
    live = B.default_path()
    import os
    os.environ["LILJACK_BOARD"] = str(tmp_board("default-guard"))
    check(B.default_path() != live, "the suite cannot write to the live board")
    p = tmp_board()

    # ── the board starts empty, and an empty board is not an error ───────
    empty = B.read(p)
    check(empty["exists"] is False and empty["items"] == [], "a missing board reads as empty")
    check(B.digest("claude", p) == "", "a missing board injects nothing")

    # ── write, then read back through the trainer's OWN parser ──────────────
    B.upsert("claude", "ja-school-judge", "active", "220/600", "family claude", path=p)
    B.upsert("codex", "h3-ja-fillers", "active", "3 assets", path=p)
    B.upsert("codex", "h1-uk-fillers", "done", "35 assets", path=p)

    raw = p.read_text()
    doc = bmd.parse(raw, strict=True)         # strict: parse-or-reject
    check(len(doc.assertions) == 3, f"every claim parses (got {len(doc.assertions)}/3)")

    # ── the trap: claims belong in the document [[footer]] ───────────────
    check(sum(len(x.assertions) for x in doc.pasems) == 0,
          "no claim is written inside a pasem")
    check(raw.index("[[footer]]") > raw.index("[[/pasem]]"),
          "the footer follows the prose pasem")
    check(all(a.pred == B.PRED_TASK for a in doc.assertions),
          "the document footer holds the board's claims")

    # …and the reason that rule exists: a claim written as PASEM PROSE is text.
    trap = tmp_board("trap")
    trap.parent.mkdir(parents=True, exist_ok=True)
    trap.write_text(
        "[[header]]\ntitle: trap\n[[/header]]\n\n"
        "[[pasem: agent-board]]\n"
        "fact(agent-task, claude, invisible-task | state=active)\n"
        "[[/pasem]]\n")
    td = bmd.parse(trap.read_text(), strict=True)
    check(len(td.all_assertions()) == 0,
          "a claim written as pasem PROSE yields ZERO assertions (the trap)")
    check(B.read(trap)["items"] == [],
          "…and the board reads no items from it, rather than half of one")

    # ── fact arity: at most two args after the predicate ─────────────────
    check(all(len(a.args) == 2 for a in doc.assertions),
          "every fact carries exactly (pred, agent, task) — two args after the predicate")
    try:
        bmd.parse_assertion("fact(agent-task, claude, ja-judge, active, 220/600)")
        check(False, "a fact with three args after the predicate is refused")
    except bmd.BmdError:
        check(True, "a fact with three args after the predicate is refused")
    check(bmd.parse_assertion(
        "fact(agent-task, claude, x | state=active | progress=1/2)").attrs["state"] == "active",
        "the extra data rides in attributes, inside the parens")

    # ── quoting survives the separators the grammar uses ─────────────────
    q = tmp_board("quote")
    nasty = "held: a|b, don't merge (see walk-queue"      # | , ' and an UNBALANCED paren
    B.upsert("claude", "quoting-check", "blocked", "1/2", nasty, path=q)
    back = B.read(q)
    check(len(back["items"]) == 1, "the nasty note did not shred the claim into pieces")
    check(back["items"][0]["note"] == nasty,
          f"a note with | , ' and an unbalanced paren round-trips ({back['items'][0]['note']!r})")
    check(back["items"][0]["state"] == "blocked" and back["items"][0]["progress"] == "1/2",
          "the attributes after the nasty value survive it")
    check(bmd.parse(q.read_text(), strict=True) is not None,
          "and the file still parses strictly")
    # a bare ARGUMENT cannot be quoted (bmd never unquotes args) — so a note
    # value carrying a separator is sanitized to a bare word and the dropped
    # prose is folded into reason automatically (item 7), never a lost update.
    row = B.set_note("walk-queue", "held, mostly", path=q)
    check(row["value"] == "held mostly",
          "a note VALUE carrying a separator is sanitized to a bare word, not mangled")
    back = B.read(q)
    note = next(n for n in back["notes"] if n["topic"] == "walk-queue")
    check(note["value"] == "held mostly", "the sanitized value round-trips")
    check("held, mostly" in note["reason"], "the dropped prose is folded into reason, not lost")
    check(bmd.parse(q.read_text(), strict=True) is not None,
          "and the file still parses strictly")

    # ── upsert updates IN PLACE ──────────────────────────────────────────
    before = len(B.read(p)["items"])
    B.upsert("claude", "ja-school-judge", progress="400/600", path=p)
    after = B.read(p)
    check(len(after["items"]) == before, "upsert of an existing item adds no duplicate")
    row = next(i for i in after["items"] if i["task"] == "ja-school-judge")
    check(row["progress"] == "400/600", "upsert updated the value in place")
    check(row["state"] == "active" and row["note"] == "family claude",
          "fields not passed keep their value")
    check(B.slug("H2 us fillers") == "h2-us-fillers", "task identity is a slug")
    B.upsert("codex", "H1 uk fillers", path=p)
    check(len([i for i in B.read(p)["items"] if i["task"] == "h1-uk-fillers"]) == 1,
          "the same task named differently is still one item")

    # ── mark ─────────────────────────────────────────────────────────────
    B.mark("claude", "ja-school-judge", "done", path=p)
    row = next(i for i in B.read(p)["items"] if i["task"] == "ja-school-judge")
    check(row["state"] == "done", "mark changes the state")
    check(row["progress"] == "400/600", "mark does not erase progress")
    for bad, label in ((lambda: B.mark("hal9000", "x", "done", path=p), "an unknown agent"),
                       (lambda: B.mark("claude", "x", "finished", path=p), "an unknown state"),
                       (lambda: B.upsert("claude", "", path=p), "an empty task")):
        try:
            bad()
            check(False, f"refuses {label}")
        except ValueError:
            check(True, f"refuses {label}")

    # ── the prose is generated, so it cannot disagree with the claims ────
    check("400/600" in p.read_text().split("[[footer]]")[0],
          "the pasem prose is regenerated from the claims")

    # ── an absence is a CLAIM, never an inference ────────────────────────
    b = B.read(p)
    check(not any(i["task"] == "operator-speech-judge" for i in b["items"]),
          "a task nobody has written down is simply absent")
    check(b["negs"] == [],
          "…and absence alone produces NO neg — the board never infers one")
    B.negate("claude", "operator-speech-judge", "handed to codex as H5", path=p)
    b = B.read(p)
    check(len(b["negs"]) == 1 and b["negs"][0]["args"] == ["claude", "operator-speech-judge"],
          "an absence that matters is recorded explicitly with neg()")
    check(b["negs"][0]["reason"] == "handed to codex as H5", "the neg carries its reason")
    check(bmd.parse(p.read_text(), strict=True).assertions[-1].form == "neg",
          "the neg is a real neg() assertion in the footer")
    B.upsert("claude", "operator-speech-judge", "active", path=p)
    b = B.read(p)
    check(b["negs"] == [] and any(i["task"] == "operator-speech-judge" for i in b["items"]),
          "taking the task back retracts the neg — the board never asserts both")

    # ── digest: bounded, caller's own items first ───────────────────────
    d = B.digest("codex", p)
    check(len(d) <= B.DIGEST_BUDGET, f"digest is within budget ({len(d)} ≤ {B.DIGEST_BUDGET})")
    check(d.index("you (codex)") < d.index("claude:"), "the caller's own items come first")
    check(B.digest("claude", p).index("you (claude)") < B.digest("claude", p).index("codex:"),
          "…for the other harness too")
    check("h3-ja-fillers" in d, "the digest names the active task")

    big = tmp_board("big")
    for i in range(40):
        B.upsert("codex", f"task-number-{i}", "done" if i % 2 else "active",
                 f"{i}/40", "a note long enough to matter " * 2, path=big)
    d2 = B.digest("codex", big)
    check(len(d2) <= B.DIGEST_BUDGET, f"a large board still fits the budget ({len(d2)})")
    check("▶ task-number-0" in d2, "an ACTIVE item survives the budget squeeze")
    check(d2.count("✓") < 20, "finished work is what gets dropped first")

    # ── concurrency: two harnesses write the same file ──────────────────
    conc = tmp_board("conc")
    errs, seen_bad = [], []

    def writer(agent, n):
        for i in range(n):
            try:
                B.upsert(agent, f"{agent}-task-{i}", "active", f"{i}/{n}", path=conc)
            except Exception as e:
                errs.append(repr(e))

    def reader(stop):
        while not stop.is_set():
            try:
                if conc.exists():
                    r = B.read(conc)
                    if r["errors"]:
                        seen_bad.append("malformed")
            except Exception as e:
                seen_bad.append(repr(e))

    stop = threading.Event()
    rd = threading.Thread(target=reader, args=(stop,), daemon=True)
    rd.start()
    ts = [threading.Thread(target=writer, args=(a, 25)) for a in ("claude", "codex", "operator")]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    stop.set()
    rd.join(timeout=5)

    final = B.read(conc)
    check(not errs, f"concurrent writers raise nothing ({errs[:2]})")
    check(len(final["items"]) == 75,
          f"no item is lost under concurrency (got {len(final['items'])}/75)")
    check(len({(i['agent'], i['task']) for i in final['items']}) == 75,
          "and none is duplicated")
    check(not seen_bad, f"a concurrent reader never saw a torn board ({seen_bad[:2]})")
    check(bmd.parse(conc.read_text(), strict=True) is not None,
          "the board still parses strictly after 75 interleaved rewrites")
    check(not list(conc.parent.glob("*.tmp")) and not list(conc.parent.glob("*.lock")),
          "no .tmp or .lock file is left behind")

    # a lock held by a dead writer is stolen rather than wedging the board
    lock = Path(str(conc) + ".lock")
    lock.write_text("99999999\n")
    import os as _os
    _os.utime(lock, (0, 0))                      # older than LOCK_STALE
    B.upsert("claude", "after-stale-lock", "active", path=conc)
    check(any(i["task"] == "after-stale-lock" for i in B.read(conc)["items"]),
          "a stale lock is stolen, not waited on forever")

    # ── the live board, if it exists, is well-formed ────────────────────
    if live.exists():
        lb = B.read(live)
        check(lb["errors"] == 0 and lb["stray"] == 0,
              "the live board parses strictly with no stray claims")
        check(all(i["state"] in B.STATES for i in lb["items"]),
              "every live item carries a legal state")
        check(all(i["agent"] in B.AGENTS for i in lb["items"]),
              "every live item belongs to a known agent")
        check(len(B.digest("claude", live)) <= B.DIGEST_BUDGET,
              "the live digest fits the injection budget")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
