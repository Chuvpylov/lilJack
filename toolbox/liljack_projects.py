#!/usr/bin/env python3
"""
liljack_projects.py — per-project context folders on Apollo (repo: mcp-projects).

Mirror (local, always written first): ~/.claude/session_archive/mcp-projects/
  <slug>/context.md                     generated digest (no LLM)
  <slug>/TODO.md                        mirror of live TODO.md
  <slug>/chains/YYYY-MM.chains.jsonl.gz monthly chain rollup
  <slug>/sessions.jsonl                 session index (pointers into liljack-sessions)

Push: Apollo REST via session_archive._apollo_push_file; .sync.json records
sha1 per pushed path ONLY on success → offline runs retry on next sync.

Usage:
  python3 toolbox/liljack_projects.py [--no-push] [cwd]
"""
import gzip
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from session_archive import (ARCHIVE_ROOT, _apollo_url, _apollo_online,
                             _apollo_push_file, _load_index, _project_slug)
from liljack_todo import find_todo, parse_todo, status_line
import liljack_chains

MIRROR = ARCHIVE_ROOT / "mcp-projects"
REPO   = "mcp-projects"


def _first_paragraph(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").split("\n\n")[0].strip()
    except Exception:
        return ""


def _recent_chain_facts(slug: str, limit: int = 20):
    recs = []
    cdir = liljack_chains.CHAINS_DIR / slug
    if cdir.is_dir():
        for f in sorted(cdir.glob("*.chains.jsonl"),
                        key=lambda p: p.stat().st_mtime, reverse=True)[:limit]:
            for line in f.read_text(errors="replace").splitlines():
                try:
                    recs.append(json.loads(line))
                except Exception:
                    pass
    return recs


def build_context_md(slug: str, project_dir=None) -> str:
    parts = [f"# {slug} — context", ""]
    if project_dir:
        pd = Path(project_dir)
        intro = _first_paragraph(pd / "CLAUDE.md") or _first_paragraph(pd / "README.md")
        if intro:
            parts += [intro, ""]
        tp = find_todo(pd)
        if tp:
            parsed = parse_todo(tp)
            parts += ["## TODO", f"{status_line(parsed)}",
                      f"scope: {parsed['scope'][:300]}", ""]
    recs = _recent_chain_facts(slug)
    if recs:
        parts.append("## Recent work")
        for r in recs[:5]:
            parts.append(f"- {r.get('ts','')[:10]} {r.get('human','')[:100]}")
        files = {}
        for r in recs:
            for f in r.get("outcome", {}).get("files_edited", []):
                files[f] = files.get(f, 0) + 1
        if files:
            parts.append("")
            parts.append("## Hot files")
            for f, n in sorted(files.items(), key=lambda kv: -kv[1])[:8]:
                parts.append(f"- {f} ({n})")
    return "\n".join(parts) + "\n"


def _month_rollups(slug: str) -> dict:
    """{'YYYY-MM': jsonl-bytes} concatenated from per-session chain files."""
    months: dict = {}
    cdir = liljack_chains.CHAINS_DIR / slug
    if not cdir.is_dir():
        return {}
    for f in sorted(cdir.glob("*.chains.jsonl")):
        for line in f.read_text(errors="replace").splitlines():
            try:
                month = (json.loads(line).get("ts", "") or "0000-00")[:7]
            except Exception:
                continue
            months.setdefault(month if len(month) == 7 else "undated", []).append(line)
    return {m: ("\n".join(lines) + "\n").encode() for m, lines in months.items()}


def sync_project(slug: str, project_dir=None, push: bool = True) -> dict:
    root = MIRROR / slug
    root.mkdir(parents=True, exist_ok=True)
    written = []

    def _write(rel: str, data: bytes):
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if not p.exists() or p.read_bytes() != data:
            p.write_bytes(data)
        written.append(rel)

    _write("context.md", build_context_md(slug, project_dir).encode())
    tp = find_todo(project_dir) if project_dir else None
    if tp:
        _write("TODO.md", tp.read_bytes())
    from liljack_codex_archive import load_index as load_codex_index
    session_index = list(_load_index().values()) + load_codex_index() + load_codex_index(harness="opencode")
    sessions = [json.dumps({"session_id": m["session_id"],
                            "harness": m.get("harness", "claude-code"),
                            "archived_at": m.get("archived_at", ""),
                            "messages": m.get("message_count", 0),
                            "liljack_repo_path": m.get("repo_path", "")})
                for m in session_index if m.get("project_slug") == slug]
    if sessions:
        _write("sessions.jsonl", ("\n".join(sessions) + "\n").encode())
    for month, blob in _month_rollups(slug).items():
        _write(f"chains/{month}.chains.jsonl.gz", gzip.compress(blob))

    pushed = []
    if push:
        url = _apollo_url()
        if url and _apollo_online(url):
            state_f = MIRROR / ".sync.json"
            try:
                state = json.loads(state_f.read_text())
            except Exception:
                state = {}
            for rel in written:
                data = (root / rel).read_bytes()
                sha = hashlib.sha1(data).hexdigest()
                rp = f"{slug}/{rel}"
                if state.get(rp) == sha:
                    continue
                if _apollo_push_file(url, REPO, rp, data):
                    state[rp] = sha
                    pushed.append(rel)
            state_f.write_text(json.dumps(state, indent=1))
    return {"written": written, "pushed": pushed}


def sync_current(cwd=None, push: bool = True) -> dict:
    cwd = cwd or os.getcwd()
    slug = _project_slug(str(cwd).replace("/", "-"))
    return sync_project(slug, project_dir=cwd, push=push)


if __name__ == "__main__":
    argv = sys.argv[1:]
    push = "--no-push" not in argv
    argv = [a for a in argv if a != "--no-push"]
    r = sync_current(argv[0] if argv else None, push=push)
    print(f"[liljack projects] wrote {len(r['written'])} · pushed {len(r['pushed'])} → {MIRROR}")
