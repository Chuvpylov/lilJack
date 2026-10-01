#!/usr/bin/env python3
"""
liljack_inject.py — UserPromptSubmit hook for Claude Code

Injects lilJack context before Claude processes each user prompt:
  1. Pending external feedback (liljack signal, external processes, ChatGPT relay)
  2. TODO line (milestone + next item) + recent work from other sessions of
     this project; cheatsheet + memory priors only where no CLAUDE.md exists
     (skipped after turn 1 if unchanged)
  3. Relevant past reasoning chains matching current prompt — what was
     CONCLUDED, not which tools ran (keyword search, this project, ≤180d)
  4. Session health warning (long/stuck sessions only)

Output: JSON with additionalContext → Claude Code appends as system reminder.
No output → no injection (fast path).
"""

import hashlib
import json
import os
import pathlib
import re
import sys
import time

from liljack_common import (
    PROJECT_ROOT, DSL_FILE, CACHE, CONTEXT_FILE, PROJECT_HINTS,
    _load_context, _save_context, _filter_atoms_for_project,
    read_memories, detect_project_context, project_name_of,
)

from liljack_chains import CHAINS_DIR

PAIRS_FILE     = PROJECT_ROOT / "orc_data" / "session_pairs.jsonl"   # mined by the Stop hook; `liljack fetch` + the ▮p: count read it
CHAINS         = CHAINS_DIR                                      # what the inject reads: chains carry OUTCOMES
NOISE_HUMAN    = ("<", "Base directory for this skill", "[Request interrupted")
FEEDBACK_FILE  = CACHE / "pending_feedback.txt"
OVERRIDE_FILE  = CACHE / "task_override.txt"
HEALTH_CACHE   = CACHE / "session_health.json"
# ⚠ liljack_mail and liljack_board keep their OWN roots, so a test that
# redirects lilJack's cache does not redirect theirs — the hooks would read the
# developer's live mailbox and the live board during a test run and the outcome
# would depend on what happened to be unread. These let a test point them
# somewhere safe; None means "the module's own default".
MAIL_ROOT      = None
MAIL_DIGEST_BUDGET = 420   # chars; the mail peek rides in every turn-1 payload
BOARD_PATH     = None

# Words to ignore in keyword matching
STOP_WORDS = {
    # grammatical / structural
    "the","a","an","is","are","was","to","of","in","i","we","let","and","or","it",
    "this","that","with","for","do","can","what","how","why","make","just","now",
    "have","be","at","from","my","me","please","its","use","run","get","set","add",
    # high-frequency generics found across the corpus (top-40 frequency analysis)
    "not","need","help","check","our","looks","too","much","your","hey","want",
    "lets","all","out","also","yes","new","see","put","try","fix","bit","like",
    # corpus boilerplate words appearing in 100+ pairs (auto-generated intent fields)
    "context","session","task","nice","summary","conversation","continued","portion",
    "being","ran","below","covers","earlier","request","previous","primary","intent",
    # ecosystem names too common to signal relevance
    "claude","brain","tool","code","into",
}

FEEDBACK_MAX_BYTES = 2000   # any process may write here; it lands verbatim in context


def read_pending_feedback() -> str:
    """Read and consume pending_feedback.txt, capped."""
    if not FEEDBACK_FILE.exists():
        return ""
    try:
        text = FEEDBACK_FILE.read_text(encoding="utf-8").strip()
        if text:
            FEEDBACK_FILE.write_text("")  # consume
        if len(text) > FEEDBACK_MAX_BYTES:
            text = (text[:FEEDBACK_MAX_BYTES]
                    + f"\n… truncated, {len(text) - FEEDBACK_MAX_BYTES} bytes dropped")
        return text
    except OSError:
        return ""


def _extract_keywords(prompt: str) -> set[str]:
    """Extract non-stop-word keywords from a prompt string."""
    return set(re.findall(r'\b\w{3,}\b', prompt.lower())) - STOP_WORDS


MAX_PAIR_AGE_DAYS = 180   # older work is history, not guidance — stop surfacing it


def find_relevant_pairs(prompt: str, cwd: str = "", limit: int = 3,
                        min_score: int = 3,
                        max_age_days: int = MAX_PAIR_AGE_DAYS) -> list[dict]:
    """Keyword-match prompt against session_pairs.jsonl.

    Confined to the active project and to recent work: a match from another
    project, or from an era whose conventions have since moved on, reads as
    advice about the code in front of you and is not.
    """
    if not PAIRS_FILE.exists() or not prompt:
        return []
    words = _extract_keywords(prompt)
    if len(words) < 2:
        return []
    project = project_name_of(cwd)
    cutoff  = time.time() - max_age_days * 86400
    try:
        matches = []
        for line in PAIRS_FILE.read_text(errors="replace").splitlines():
            try:
                p = json.loads(line)
                if project and p.get("project", "") != project:
                    continue
                if _pair_epoch(p) < cutoff:
                    continue
                intent = p.get("human_intent", "").lower()
                score = sum(1 for w in words if w in intent)
                if score >= min_score:
                    matches.append((score, p))
            except Exception:
                pass
        matches.sort(key=lambda x: x[0], reverse=True)
        return [p for _, p in matches[:limit]]
    except Exception:
        return []


def _pair_epoch(p: dict) -> float:
    """Epoch seconds for a pair's ts; 0.0 (i.e. always stale) if unparseable."""
    ts = p.get("ts", "")
    if not ts:
        return 0.0
    try:
        from datetime import datetime
        return datetime.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S").timestamp()
    except Exception:
        return 0.0


def _has_claude_md(cwd: str) -> bool:
    """True if a CLAUDE.md sits at cwd or any parent up to the repo root.

    Claude Code loads it every session, in far more detail than the one-line
    cheatsheet and the memory-atom labels — injecting those on top is the same
    facts twice at lower fidelity. Projects WITHOUT a CLAUDE.md keep them.
    """
    try:
        p = pathlib.Path(cwd).resolve()
    except Exception:
        return False
    for _ in range(8):
        if (p / "CLAUDE.md").is_file():
            return True
        if (p / ".git").exists() or p.parent == p:
            return False
        p = p.parent
    return False


def _iter_chains(project: str, max_files: int = 60):
    """Chain records for a project (all projects if unknown), newest file first."""
    if not CHAINS.is_dir():
        return
    dirs = [CHAINS / project] if project else [d for d in CHAINS.iterdir() if d.is_dir()]
    files = [f for d in dirs if d.is_dir() for f in d.glob("*.chains.jsonl")]
    files.sort(key=lambda f: f.stat().st_mtime, reverse=True)
    for f in files[:max_files]:
        for line in f.read_text(errors="replace").splitlines():
            try:
                yield json.loads(line)
            except Exception:
                pass


def _one_line(text: str, n: int) -> str:
    """First non-empty line, markdown emphasis stripped, cut at a word boundary."""
    for ln in (text or "").splitlines():
        ln = re.sub(r"[*`#_]+", "", ln).strip()
        if ln:
            break
    else:
        return ""
    if len(ln) <= n:
        return ln
    cut = ln[:n].rsplit(" ", 1)[0]
    return (cut if len(cut) >= n // 2 else ln[:n]) + "…"


_OPENER = re.compile(
    r"^(done|ok(ay)?|recap|summary|status|here'?s"
    r"|(all|everything|both)\b[^.]{0,60}\b(done|built|verified|delivered|landed|set|gathered|wired|working|shipped))\b", re.I)


def _conclusion(final: str, n: int = 120) -> str:
    """The first SUBSTANTIVE line of a chain's final answer.

    Answers open with a flourish — "Done. Both asks delivered:", "All set.
    Status and what happens next:" — which is the least informative line in the
    whole record. Skip openers (short, ending in ':', or starting with one of
    the stock words), table rows and headings; fall back to the first line.
    """
    first = ""
    for ln in (final or "").splitlines():
        raw = ln.strip()
        if not raw or raw.startswith("|") or raw.startswith("```"):
            continue
        ln = re.sub(r"[*`#_]+", "", raw).strip()
        if not ln:
            continue
        first = first or ln
        if ln.endswith(":") or len(ln) < 40 or _OPENER.match(ln):
            continue
        return _one_line(ln, n)
    return _one_line(first, n)


def _is_noise(human: str) -> bool:
    h = (human or "").lstrip()
    return not h or h.startswith(NOISE_HUMAN)


def find_relevant_chains(prompt: str, cwd: str = "", session_id: str = "",
                         limit: int = 3, min_score: int = 3,
                         max_age_days: int = MAX_PAIR_AGE_DAYS) -> list[dict]:
    """Keyword-match the prompt against past reasoning chains.

    Replaces the pairs lookup: a pair says which TOOLS ran, a chain says what
    was CONCLUDED (`final`, tests, files). Same gates — this project only,
    recent only, ≥ min_score keyword hits with at least one in the human
    message so a hit is never manufactured from Claude's own answer text.
    The current session is excluded: its chains are this conversation.
    """
    if not prompt:
        return []
    words = _extract_keywords(prompt)
    if len(words) < 2:
        return []
    project = project_name_of(cwd)
    cutoff  = time.time() - max_age_days * 86400
    hits = []
    for r in _iter_chains(project):
        if session_id and r.get("session_id") == session_id:
            continue
        if _pair_epoch(r) < cutoff:
            continue
        human = r.get("human", "")
        if _is_noise(human):
            continue
        hl, fl = human.lower(), (r.get("final", "") or "").lower()
        in_human = sum(1 for w in words if w in hl)
        score = in_human + sum(1 for w in words if w in fl)
        if in_human >= 1 and score >= min_score:
            hits.append((score, r))
    hits.sort(key=lambda x: (-x[0], x[1].get("ts", "")), reverse=False)
    return [r for _, r in hits[:limit]]


def recent_work(cwd: str, session_id: str = "", n: int = 3) -> list[dict]:
    """The last n human prompts of this project from OTHER sessions —
    the continuity a fresh session lacks ("yesterday you were benchmarking")."""
    project = project_name_of(cwd)
    if not project:
        return []
    out, seen = [], set()
    recs = [r for r in _iter_chains(project, max_files=10)
            if not (session_id and r.get("session_id") == session_id)
            and not _is_noise(r.get("human", ""))]
    recs.sort(key=lambda r: r.get("ts", ""), reverse=True)
    for r in recs:
        key = r.get("human", "")[:60]
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
        if len(out) >= n:
            break
    return out


def has_trait(trait: str) -> bool:
    """Check if liljack.dsl has trait(liljack, <trait>)."""
    if not DSL_FILE.exists():
        return False
    try:
        return bool(re.search(rf'^trait\(liljack,\s*{re.escape(trait)}\)\.',
                              DSL_FILE.read_text(encoding="utf-8"), re.MULTILINE))
    except Exception:
        return False


def build_context(prompt: str, cwd: str = "", session_id: str = "",
                  persist: bool = True) -> dict:
    """Build the lilJack additionalContext for one prompt.

    Returns {"text": str, "parts": [str], "skipped_stable": bool,
             "stable_hash": str}. When persist=False the session context.json
            is read but NOT written — used by the A/B harness so a test run
            never clobbers the live session's dedup state.
    """
    ctx   = _load_context()
    parts = []

    # ── 1. Pending external feedback (always inject, never skipped) ───────────
    feedback = read_pending_feedback()
    if feedback:
        parts.append(f"[lilJack external signal]\n{feedback}")

    # ── 2. Stable content: TODO line + recent work (+ cheatsheet/priors only
    #      where no CLAUDE.md carries them already) ───────────────────────────
    # Build first, then check if we already sent it this session turn.
    project_ctx = detect_project_context(cwd)
    known       = bool(project_ctx)
    has_md      = _has_claude_md(cwd)

    mems_txt = ""
    if not has_md:
        mems     = read_memories()
        mems_rel = _filter_atoms_for_project(mems, cwd) if mems else []
        mems_txt = ("[lilJack memory priors] " + " · ".join(mems_rel)) if mems_rel else ""
    if has_md:
        project_ctx = ""

    try:
        from liljack_todo import inject_line
        todo_txt = inject_line(cwd, known_project=known)
    except Exception:
        todo_txt = ""

    recent_txt = ""
    try:
        recent = recent_work(cwd, session_id)
        if recent:
            recent_txt = "[lilJack recent work] " + " · ".join(
                f"{r.get('ts', '')[:10]} {_one_line(r.get('human', ''), 80)}" for r in recent)
    except Exception:
        recent_txt = ""

    # ── 2b. The shared attention board + mail from the other harness.
    #
    # ⚠ Both are STABLE parts, deduped by stable_hash — deliberately, and this
    # is the whole point of the board (the operator: "not after each message, but a
    # general attention context"). They were briefly appended unconditionally,
    # which re-sent the same block on every turn until the mail was read: the
    # exact cost regression CLAUDE.md records as having gone unnoticed for
    # months when write_context_cache stopped merging. Folded in here, a change
    # to the board or a new message moves the hash and is injected ONCE; an
    # unchanged board and an unchanged inbox cost nothing on turn 2+.
    board_txt = ""
    try:
        import liljack_board
        board_txt = liljack_board.digest("claude", BOARD_PATH)
    except Exception:
        board_txt = ""

    # ⚠ A compact DIGEST, never full bodies (2026-09-07). `unread_block` renders
    # every message in full: with three agents mailing each other it measured
    # 5,575 of a 7,471-byte turn-1 payload — 75%, against a 2 KB budget — and
    # because the inbox changes with every agent message the "stable" hash moved
    # constantly, so the dedup never fired and the block re-sent on every turn.
    # One line per message points at read_messages for the body; inject owns
    # its own budget, the mailbox renderer is not the place to enforce it.
    mail_txt = ""
    try:
        import liljack_mail
        msgs = liljack_mail.inbox(agent="claude", unread_only=True, limit=5, root=MAIL_ROOT)
        if msgs:
            head = f"[lilJack mail] {len(msgs)} unread for claude — read_messages for the text"
            previews = []
            for m in msgs:
                subj = m.get("subject") or _one_line(m.get("text", ""), 70)
                th = f" · {m.get('thread')}" if m.get("thread") else ""
                previews.append(f"  {m.get('ts', '')[:16]} {m.get('frm', '?')}: "
                                f"{_one_line(subj, 80)}{th}")
            # ⚠ The header carries the true unread count, so dropping previews
            # hides nothing — it only shortens the peek. Budgeted like the
            # board's digest rather than capped at a magic count, because
            # subjects grow and a fixed count silently stops fitting.
            while previews and len(head) + 1 + sum(len(l) + 1 for l in previews) > MAIL_DIGEST_BUDGET:
                previews.pop()
            mail_txt = "\n".join([head] + previews)
    except Exception:
        mail_txt = ""

    stable_parts = [p for p in [board_txt, mail_txt, project_ctx, todo_txt,
                                recent_txt, mems_txt] if p]
    stable_blob  = "\n".join(stable_parts)
    stable_hash  = hashlib.md5(stable_blob.encode()).hexdigest()[:8] if stable_blob else ""

    # Skip if: same session_id AND stable content hasn't changed since last turn
    same_session   = bool(session_id and session_id == ctx.get("session_id"))
    stable_matches = stable_hash and stable_hash == ctx.get("stable_hash")
    skip_stable    = same_session and stable_matches

    if not skip_stable:
        parts.extend(stable_parts)

    # ── 3. Relevant past chains (prompt-dependent, never skipped) ────────────
    chains = find_relevant_chains(prompt, cwd, session_id)
    if chains:
        lines = ["[lilJack — similar past work] archive only; verify before relying on it"]
        for r in chains:
            o = r.get("outcome", {}) or {}
            head = f"  [{r.get('ts', '')[:10]}] {_one_line(r.get('human', ''), 70)}"
            tail = _conclusion(r.get("final", ""), 120)
            meta = f"tests={o.get('tests', 'none')}"
            nf = len(o.get("files_edited", []) or [])
            if nf:
                meta += f" · files={nf}"
            lines.append(f"{head} → {tail or '(no conclusion recorded)'} · {meta}")
        parts.append("\n".join(lines))

    # ── 4. Session health warning (long/stuck sessions only) ─────────────────
    if HEALTH_CACHE.exists():
        try:
            health        = json.loads(HEALTH_CACHE.read_text())
            h             = health.get("age_hours", 0)
            n             = health.get("msg_count", 0)
            stuck         = health.get("stuck", False)
            file_age_secs = time.time() - os.path.getmtime(HEALTH_CACHE)
            if stuck and file_age_secs > 90:
                stuck = False
            if h > 2.0 or stuck:
                tag = f"{h:.1f}h · {n} msgs"
                if stuck:
                    parts.append(f"[lilJack session health] ⚠ STUCK detected ({tag}) — try a different approach. run: liljack suggest")
                elif h > 4.0:
                    parts.append(f"[lilJack session health] ⚠ {tag} — context is getting bloated. consider: commit → /clear")
                elif h > 2.0:
                    parts.append(f"[lilJack session health] {tag} — session running long")
        except Exception:
            pass

    # ── Persist context for next turn ─────────────────────────────────────────
    if persist:
        ctx.update({
            "session_id":              session_id,
            "stable_hash":             stable_hash,
            "last_inject_bytes":       sum(len(p) for p in parts),
            "last_inject_skipped_stable": skip_stable,
        })
        _save_context(ctx)

    return {
        "text": "\n\n".join(parts),
        "parts": parts,
        "skipped_stable": skip_stable,
        "stable_hash": stable_hash,
    }


def main():
    raw = sys.stdin.read()
    try:
        data       = json.loads(raw)
        prompt     = data.get("prompt", "")
        cwd        = data.get("cwd", "")
        session_id = data.get("session_id", "")
    except Exception:
        # Bad/empty hook payload → inject nothing (fast path), never guess.
        return

    result = build_context(prompt, cwd, session_id, persist=True)
    if result["text"]:
        print(json.dumps({"additionalContext": result["text"]}))
    # else: exit 0 with no output = no injection (fast path)


if __name__ == "__main__":
    main()
