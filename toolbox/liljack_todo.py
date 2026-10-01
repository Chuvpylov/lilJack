#!/usr/bin/env python3
"""
liljack_todo.py — per-project TODO.md rule.

TODO.md format (single source of truth, no sidecar):
  # <Project> — Scope
  <prose: full scope of work>
  ## <Milestone>
  - [ ] item          ← id = MD5(normalized text)[:7]
    > lj: <note>      ← result notes appended by lilJack

Usage:
  python3 toolbox/liljack_todo.py status [path]
  python3 toolbox/liljack_todo.py list [path]
  python3 toolbox/liljack_todo.py check <id|text-match> [--note "..."]
  python3 toolbox/liljack_todo.py init [path]      # headless claude drafts TODO.md.draft
  python3 toolbox/liljack_todo.py --segment <cwd>  # ribbon segment (cached)
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ITEM_RE      = re.compile(r'^(\s*)- \[( |x|X)\] (.+?)\s*$')
MILESTONE_RE = re.compile(r'^##\s+(.+?)\s*$')
NOTE_RE      = re.compile(r'^\s*>\s*lj:\s*(.*)$')
SEG_CACHE    = Path.home() / ".cache" / "liljack" / "todo_segment.json"


def normalize(text: str) -> str:
    return re.sub(r'\s+', ' ', text.strip().lower()).rstrip('.!?,;:')


def item_id(text: str) -> str:
    return hashlib.md5(normalize(text).encode()).hexdigest()[:7]


def find_todo(cwd):
    p = Path(cwd).resolve()
    for d in [p, *p.parents]:
        if (d / "TODO.md").is_file():
            return d / "TODO.md"
        if (d / ".git").exists():
            return None          # repo root reached, no TODO.md
    return None


def parse_todo(path) -> dict:
    scope, milestones, cur, seen_h1 = [], [], None, False
    in_item = False   # last consumed line belonged to an item (wrapped prose follows it)
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        m = MILESTONE_RE.match(line)
        if m:
            cur = {"title": m.group(1), "items": []}
            milestones.append(cur)
            in_item = False
            continue
        im = ITEM_RE.match(line)
        if im:
            if cur is None:
                cur = {"title": "", "items": []}
                milestones.append(cur)
            # `cont` = the item's own wrapped lines. Kept OUT of `text`, whose
            # hash is the item id (`liljack todo check <id>`) and must not move.
            cur["items"].append({"id": item_id(im.group(3)), "text": im.group(3),
                                 "done": im.group(2) in "xX", "notes": [], "cont": []})
            in_item = True
            continue
        nm = NOTE_RE.match(line)
        if nm and cur and cur["items"]:
            cur["items"][-1]["notes"].append(nm.group(1))
            continue
        if in_item and line[:1].isspace() and line.strip() and cur and cur["items"]:
            cur["items"][-1]["cont"].append(line.strip())
            continue
        in_item = False
        if line.startswith("# "):
            seen_h1 = True
            continue
        if cur is None and seen_h1 and line.strip():
            scope.append(line.strip())
    items = [i for ms in milestones for i in ms["items"]]
    return {"scope": " ".join(scope), "milestones": milestones,
            "counts": {"done": sum(1 for i in items if i["done"]), "total": len(items)}}


def next_pending(parsed, n: int = 3) -> list:
    return [i for ms in parsed["milestones"] for i in ms["items"] if not i["done"]][:n]


def status_line(parsed) -> str:
    c, nxt = parsed["counts"], next_pending(parsed, 1)
    s = f"☑{c['done']}/{c['total']}"
    return s + (f" · next: {nxt[0]['text'][:60]}" if nxt else "")


def _match_line(line, ref, want_pending):
    im = ITEM_RE.match(line)
    if not im or (want_pending and im.group(2) in "xX"):
        return None
    if ref == item_id(im.group(3)) or ref.lower() in im.group(3).lower():
        return im
    return None


def check_item(path, ref: str, note: str = ""):
    path = Path(path)
    out, hit = [], None
    for line in path.read_text(encoding="utf-8").splitlines():
        im = _match_line(line, ref, want_pending=True) if hit is None else None
        if im:
            hit = {"id": item_id(im.group(3)), "text": im.group(3)}
            out.append(f"{im.group(1)}- [x] {im.group(3)}")
            if note:
                out.append(f"{im.group(1)}  > lj: {note}")
        else:
            out.append(line)
    if hit:
        path.write_text("\n".join(out) + "\n", encoding="utf-8")
    return hit


def append_note(path, ref: str, note: str) -> bool:
    path = Path(path)
    out, hit = [], False
    for line in path.read_text(encoding="utf-8").splitlines():
        out.append(line)
        if not hit:
            im = _match_line(line, ref, want_pending=False)
            if im:
                out.append(f"{im.group(1)}  > lj: {note}")
                hit = True
    if hit:
        path.write_text("\n".join(out) + "\n", encoding="utf-8")
    return hit


def _clip(text: str, n: int) -> str:
    """Markdown emphasis stripped, cut at a word boundary with an ellipsis."""
    import re as _re
    t = _re.sub(r"[*`_]+", "", text or "").strip()
    if len(t) <= n:
        return t
    cut = t[:n].rsplit(" ", 1)[0]
    return (cut if len(cut) >= n // 2 else t[:n]) + "…"


def inject_line(cwd: str, known_project: bool) -> str:
    """Turn-1 enforcement text for liljack_inject. Empty string = nothing to say."""
    try:
        if not known_project:
            return ""
        tp = find_todo(cwd)
        if not tp:
            return "[lilJack TODO rule] no TODO.md — run: liljack todo init"
        parsed = parse_todo(tp)
        c   = parsed["counts"]
        nxt = next_pending(parsed, 3)
        txt = f"[lilJack TODO] ☑{c['done']}/{c['total']}"
        if nxt:
            # milestone + the item's first line, cut at a word — not the first
            # 60 chars of raw markdown ("**⏸ `ab.py cifar10-cutmix-40k` PAUSED 2026-08-25 ~23:25 (Ant")
            title = next((ms["title"] for ms in parsed["milestones"] if nxt[0] in ms["items"]), "")
            where = f" ({_clip(title, 45)})" if title else ""
            body = " ".join([nxt[0]["text"]] + nxt[0].get("cont", [])[:3])
            txt += f" · next{where}: {_clip(body, 140)}"
        if nxt[1:]:
            txt += "\n  pending: " + " · ".join(_clip(i["text"], 50) for i in nxt[1:])
        return txt
    except Exception:
        return ""


def segment(cwd: str) -> str:
    """Ribbon segment '☑3/12' or ''. mtime-cached, never raises."""
    try:
        tp = find_todo(cwd)
        if not tp:
            return ""
        mtime = os.path.getmtime(tp)
        try:
            c = json.loads(SEG_CACHE.read_text())
            if c.get("path") == str(tp) and c.get("mtime") == mtime:
                return c.get("seg", "")
        except Exception:
            pass
        cnt = parse_todo(tp)["counts"]
        seg = f"☑{cnt['done']}/{cnt['total']}"
        try:
            SEG_CACHE.parent.mkdir(parents=True, exist_ok=True)
            SEG_CACHE.write_text(json.dumps({"path": str(tp), "mtime": mtime, "seg": seg}))
        except Exception:
            pass
        return seg
    except Exception:
        return ""


def build_init_prompt() -> str:
    return (
        "Read this repository (README, CLAUDE.md, docs/, recent git log) and draft a TODO.md "
        "describing the FULL remaining scope of work on this project.\n"
        "Output format — exactly this markdown shape, nothing else:\n"
        "# <Project> — Scope\n"
        "<one paragraph: the full scope of work>\n\n"
        "## <Milestone name>\n"
        "- [ ] <concrete item>\n"
        "- [ ] <concrete item>\n\n"
        "Rules: 2-6 milestones, 3-10 items each, items concrete enough to hand to an engineer. "
        "Print ONLY the markdown."
    )


def cmd_init(cwd: str) -> int:
    root = Path(cwd).resolve()
    if (root / "TODO.md").exists():
        print("[liljack todo] TODO.md already exists — refusing to overwrite")
        return 1
    print("[liljack todo] drafting via headless claude (this takes a minute)…")
    r = subprocess.run(["claude", "-p", build_init_prompt(), "--max-turns", "12"],
                       capture_output=True, text=True, cwd=str(root), timeout=900)
    draft = r.stdout.strip()
    if not draft.startswith("#"):
        print(f"[liljack todo] draft failed: {(r.stderr or draft)[:200]}")
        return 1
    (root / "TODO.md.draft").write_text(draft + "\n", encoding="utf-8")
    print(f"[liljack todo] wrote {root/'TODO.md.draft'} — review, then: mv TODO.md.draft TODO.md")
    return 0


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--segment":
        print(segment(argv[1] if len(argv) > 1 else os.getcwd()))
        return 0
    cmd = argv[0] if argv else "status"
    if cmd == "init":
        return cmd_init(argv[1] if len(argv) > 1 else os.getcwd())
    tp = find_todo(argv[1] if len(argv) > 1 and cmd != "check" else os.getcwd())
    if not tp:
        print("[liljack todo] no TODO.md found (rule: every project carries one) — run: liljack todo init")
        return 1
    parsed = parse_todo(tp)
    if cmd == "status":
        print(f"[liljack todo] {tp}")
        if parsed["scope"]:
            print(f"  scope: {parsed['scope'][:120]}")
        print(f"  {status_line(parsed)}")
        for it in next_pending(parsed, 3):
            print(f"    ○ {it['id']} {it['text']}")
    elif cmd == "list":
        for ms in parsed["milestones"]:
            if ms["title"]:
                print(f"## {ms['title']}")
            for it in ms["items"]:
                print(f"  {'☑' if it['done'] else '○'} {it['id']} {it['text']}")
    elif cmd == "check":
        if len(argv) < 2:
            print("usage: liljack todo check <id|text-match> [--note ...]")
            return 1
        note = ""
        if "--note" in argv:
            note = " ".join(argv[argv.index("--note") + 1:])
        ts = time.strftime("%Y-%m-%d")
        hit = check_item(tp, argv[1], note=f"{ts} {note}".strip())
        print(f"[liljack todo] ☑ {hit['id']} {hit['text']}" if hit
              else f"[liljack todo] no pending item matches {argv[1]!r}")
        return 0 if hit else 1
    else:
        print(f"[liljack todo] unknown subcommand: {cmd}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
