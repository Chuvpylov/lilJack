#!/usr/bin/env python3
"""
liljack_chains.py — full reasoning-chain extraction for agent training.

One record per human turn: human msg → [thinking/say → tool → result]* → final.
Thinking is stored VERBATIM when present — but note: Claude Code writes
thinking blocks to local transcripts as empty text + signature (encrypted),
so on this machine `thinking` is usually "". The `say` field carries Claude's
VISIBLE narration between tool calls (the reason→action→check voice) and is
the realistic training signal. One narration covers every tool call issued
before the next result comes back (a batch of parallel calls shares it).
Tool inputs are stored in full (`input`, strings capped at INPUT_MAX) and
results as head+tail (`result`, `result_len`, `is_error`) beside the old
200/300-char digests; `uuid` points at the verbatim assistant record in the
liljack archive (local + Apollo liljack-sessions), so nothing is lost by the cap.

Output: ~/.claude/session_archive/chains/<project>/<session_id>.chains.jsonl

Usage:
  python3 toolbox/liljack_chains.py --current      # newest session (Stop hook)
  python3 toolbox/liljack_chains.py --backfill     # sweep entire archive
"""
import gzip
import json
import sys
from pathlib import Path

ARCHIVE_ROOT    = Path.home() / ".claude" / "session_archive"
CHAINS_DIR      = ARCHIVE_ROOT / "chains"
CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"

EDIT_TOOLS = {"Edit", "Write", "NotebookEdit"}

# Synthetic user records that are not real human turns (slash commands,
# hook caveats, task notifications) — never start a chain from these.
NOISE_PREFIXES = ("<local-command", "<command-name", "<command-message",
                  "<task-notification", "<system-reminder", "Caveat:")


# Full tool I/O, added 2026-09-02. The 200/300-char digests stay for every
# consumer that already reads them; these fields are what training gets.
# Transcripts on Apollo are verbatim, so the chain keeps enough to LEARN from
# and a `uuid` pointer for exact retrieval — not a second copy of everything.
INPUT_MAX   = 8_000    # per string value of a tool input (commands, edit strings)
RESULT_HEAD = 6_000    # head of a tool result …
RESULT_TAIL = 2_000    # … and its tail, with the elided middle counted


def _full_input(inp) -> dict:
    """The tool input as-is, each string value capped at INPUT_MAX."""
    if not isinstance(inp, dict):
        return {"value": _digest(inp, INPUT_MAX)}
    out = {}
    for k, v in inp.items():
        if isinstance(v, str):
            out[k] = v[:INPUT_MAX]
        else:
            try:
                out[k] = json.loads(json.dumps(v)[:INPUT_MAX]) if len(json.dumps(v)) <= INPUT_MAX else json.dumps(v)[:INPUT_MAX]
            except Exception:
                out[k] = str(v)[:INPUT_MAX]
    return out


def _result_text(content) -> str:
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(b.get("text", "") for b in content
                         if isinstance(b, dict) and b.get("type") == "text")
    try:
        return json.dumps(content, ensure_ascii=False)
    except Exception:
        return str(content)


def _head_tail(text: str, head: int = RESULT_HEAD, tail: int = RESULT_TAIL) -> str:
    if len(text) <= head + tail:
        return text
    cut = len(text) - head - tail
    return text[:head] + f"\n…[{cut} chars elided]…\n" + text[-tail:]


def _digest(obj, limit: int) -> str:
    if obj is None:
        return ""
    if isinstance(obj, str):
        return obj[:limit]
    if isinstance(obj, dict) and "file_path" in obj:
        rest = {k: v for k, v in obj.items() if k != "file_path"}
        return (obj["file_path"] + " " + json.dumps(rest, ensure_ascii=False))[:limit]
    if isinstance(obj, list):          # tool_result content blocks
        txt = " ".join(b.get("text", "") for b in obj
                       if isinstance(b, dict) and b.get("type") == "text")
        return txt[:limit]
    try:
        return json.dumps(obj, ensure_ascii=False)[:limit]
    except Exception:
        return str(obj)[:limit]


def _human_text(content):
    """Return message text if this user record is a real human turn, else None."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        texts = [b.get("text", "") for b in content
                 if isinstance(b, dict) and b.get("type") == "text"]
        if texts and not any(isinstance(b, dict) and b.get("type") == "tool_result"
                             for b in content):
            return " ".join(texts)
    return None


def _finish(cur) -> dict:
    cur.pop("_think", None)
    cur.pop("_say", None)
    files = sorted(cur.pop("_files"))
    tests = "none"
    for s in cur["steps"]:
        if s["tool"] == "Bash" and "test" in s["input_digest"].lower():
            body = (s.get("result") or s["result_digest"]).lower()
            tests = "fail" if "fail" in body else "pass"
    cur["outcome"] = {"files_edited": files, "tests": tests,
                      "turn_count": cur.pop("_turns")}
    return cur


def extract_chains(records, project: str = "", session_id: str = "",
                   source: str = "human") -> list:
    chains, cur, pending = [], None, {}
    for rec in records:
        if not isinstance(rec, dict):
            continue
        msg = rec.get("message") or {}
        if not isinstance(msg, dict):
            continue
        rtype = rec.get("type")
        if rtype == "user":
            human = _human_text(msg.get("content"))
            if human is not None:
                if cur:
                    chains.append(_finish(cur))
                if human.lstrip().startswith(NOISE_PREFIXES):
                    cur = None          # synthetic turn — don't chain what follows
                    continue
                cur = {"project": project, "session_id": session_id,
                       "ts": rec.get("timestamp", ""), "source": source,
                       "human": human, "steps": [], "final": "",
                       "_files": set(), "_turns": 0, "_think": "", "_say": ""}
            elif isinstance(msg.get("content"), list):
                got_result = False
                for b in msg["content"]:
                    if isinstance(b, dict) and b.get("type") == "tool_result":
                        got_result = True
                        step = pending.pop(b.get("tool_use_id"), None)
                        if step is not None:
                            txt = _result_text(b.get("content"))
                            step["result_digest"] = _digest(b.get("content"), 300)
                            step["result"] = _head_tail(txt)
                            step["result_len"] = len(txt)
                            step["is_error"] = bool(b.get("is_error"))
                # Narration is cleared when a RESULT comes back, not when a
                # tool is called: one "I'll check X and Y" precedes a batch of
                # parallel calls, and every call in that batch is what it
                # explains. Clearing on tool_use gave the say to the first
                # call only — 22% measured coverage for a session that
                # narrated before nearly every batch (2026-09-02).
                if got_result and cur is not None:
                    cur["_think"] = cur["_say"] = ""
        elif rtype == "assistant" and cur is not None:
            cur["_turns"] += 1
            # NB: Claude Code streams one content block per assistant record,
            # so thinking/narration accumulate on the CHAIN until a tool_use
            # consumes them — never reset per record.
            for b in msg.get("content") or []:
                if not isinstance(b, dict):
                    continue
                bt = b.get("type")
                if bt == "thinking":
                    cur["_think"] += b.get("thinking", "")
                elif bt == "text":
                    cur["_say"] = b.get("text", "")
                    cur["final"] = cur["_say"]
                elif bt == "tool_use":
                    inp = b.get("input") or {}
                    step = {"i": len(cur["steps"]) + 1, "thinking": cur["_think"],
                            "say": cur["_say"], "tool": b.get("name", ""),
                            "input_digest": _digest(inp, 200), "result_digest": "",
                            "input": _full_input(inp), "result": "", "result_len": 0,
                            "is_error": False, "uuid": rec.get("uuid", "")}
                    cur["steps"].append(step)
                    pending[b.get("id")] = step
                    if b.get("name") in EDIT_TOOLS and inp.get("file_path"):
                        cur["_files"].add(inp["file_path"])
    if cur:
        chains.append(_finish(cur))
    return chains


def _read_jsonl(path: Path) -> list:
    opener = gzip.open if path.suffix == ".gz" else open
    out = []
    with opener(path, "rt", errors="replace") as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except Exception:
                pass
    return out


def _redact_live_secrets(text: str) -> tuple:
    """Replace known live credential literals. Returns (clean, n).

    Scrub at CAPTURE, not only at corpus-build time. A measurement on
    2026-07-29 found 239 live-secret occurrences sitting in 42 archived chain
    files at rest — the corpus gate was filtering them out at the very end of
    the pipe while the archive kept accumulating more.

    Known literals only, and the mesh topology is deliberately kept: a chain
    about debugging a node stays useful, it just stops carrying the key.
    Failing to import the scrubber must not silently disable this, so the
    caller reports what happened.
    """
    # The literal list comes from session_archive (LILJACK_SECRETS_SCRIPTS);
    # "empty" means no source is configured and nothing is known to redact.
    try:
        from session_archive import _live_secrets
        vals, status = _live_secrets()
    except Exception:
        return text, -1                      # -1 = scrubber unavailable
    if status.startswith("unavailable"):
        return text, -1
    n = 0
    for s in vals:
        if s and s in text:
            n += text.count(s)
            text = text.replace(s, "[REDACTED]")
    # A password on a command line is not in any credential FILE, so the exact-
    # literal pass above never catches sshpass -p or --password (short values).
    # exactly how a short ssh password accumulated across sessions. Strip the
    # value here at capture, keeping the flag so the chain stays legible.
    import re as _re
    _val = r"""('[^']*'|"[^"]*"|\S+)"""
    for _pat in (_re.compile(r"(?i)(sshpass\s+-p\s*)" + _val),
                 _re.compile(r"(?i)(--password[=\s]+)" + _val)):
        text, k = _pat.subn(r"\1[REDACTED]", text)
        n += k
    return text, n


def write_chains(chains, project: str):
    if not chains:
        return None
    out_dir = CHAINS_DIR / (project or "unknown")
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{chains[0]['session_id']}.chains.jsonl"
    blob = "\n".join(json.dumps(c, ensure_ascii=False) for c in chains) + "\n"
    blob, n = _redact_live_secrets(blob)
    if n > 0:
        print(f"[chains] redacted {n} live-secret occurrence(s) before archiving",
              file=sys.stderr)
    elif n < 0:
        print("[chains] ! scrubber unavailable — archiving UNSCRUBBED",
              file=sys.stderr)
    out.write_text(blob, encoding="utf-8")
    return out


def extract_current() -> int:
    """Stop-hook entry: extract chains from the newest live session."""
    sys.path.insert(0, str(Path(__file__).parent))
    from session_archive import _project_slug
    sessions = sorted(CLAUDE_PROJECTS.glob("*/*.jsonl"),
                      key=lambda p: p.stat().st_mtime, reverse=True)
    if not sessions:
        return 0
    sess = sessions[0]
    project = _project_slug(sess.parent.name)
    chains = extract_chains(_read_jsonl(sess), project=project, session_id=sess.stem)
    write_chains(chains, project)
    return len(chains)


def backfill(verbose: bool = False, force: bool = False) -> int:
    """Re-extract chains for archived sessions. `force` rewrites existing
    chain files too — needed whenever the record schema grows (2026-09-02:
    full tool I/O + narration attribution), or old chains stay thin forever."""
    total = 0
    for gz in ARCHIVE_ROOT.glob("[0-9]*/[0-9]*/*/*.jsonl.gz"):
        if gz.name.endswith(".subagents.tar.gz"):
            continue
        project = gz.parent.name
        sid = gz.name.replace(".jsonl.gz", "")
        out = CHAINS_DIR / project / f"{sid}.chains.jsonl"
        if out.exists() and not force:
            continue
        try:
            chains = extract_chains(_read_jsonl(gz), project=project, session_id=sid)
            write_chains(chains, project)
            total += len(chains)
            if verbose:
                print(f"  {project}/{sid[:8]}: {len(chains)} chain(s)")
        except Exception as e:
            if verbose:
                print(f"  skip {gz.name}: {e}")
    return total


if __name__ == "__main__":
    if "--backfill" in sys.argv:
        n = backfill(verbose=True, force="--force" in sys.argv)
        print(f"[liljack chains] backfilled {n} chain(s) → {CHAINS_DIR}")
    else:
        print(f"[liljack chains] {extract_current()} chain(s) from current session")
