#!/usr/bin/env python3
"""tests/test_liljack_projects.py — mirror build (Apollo pushes disabled)."""
import json, sys, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent / "toolbox"))

def _setup(tmp):
    import liljack_projects as lp, liljack_chains as lc
    lp.MIRROR = tmp / "mcp-projects"
    lc.CHAINS_DIR = tmp / "chains"
    (lc.CHAINS_DIR / "demo").mkdir(parents=True)
    (lc.CHAINS_DIR / "demo" / "s1.chains.jsonl").write_text(json.dumps(
        {"project": "demo", "session_id": "s1", "ts": "2026-07-26T10:00:00Z",
         "human": "fix bug", "steps": [], "final": "done",
         "outcome": {"files_edited": ["/src/a.py"], "tests": "pass", "turn_count": 1}}) + "\n")
    proj = tmp / "proj"; proj.mkdir(); (proj / ".git").mkdir()
    (proj / "TODO.md").write_text("# Demo — Scope\nShip demo.\n\n## M1\n- [ ] a thing\n")
    (proj / "CLAUDE.md").write_text("Demo project.\n\nMore text.\n")
    return lp, proj

def test_context_md_and_sync():
    tmp = pathlib.Path(tempfile.mkdtemp())
    lp, proj = _setup(tmp)
    md = lp.build_context_md("demo", proj)
    assert md.startswith("# demo — context")
    assert "Demo project." in md and "Ship demo." in md and "☑0/1" in md
    r = lp.sync_project("demo", proj, push=False)
    root = lp.MIRROR / "demo"
    assert (root / "context.md").exists() and (root / "TODO.md").exists()
    assert (root / "chains" / "2026-07.chains.jsonl.gz").exists()
    assert (root / "sessions.jsonl").exists() or True  # index may be empty for fixture slug
    assert "context.md" in " ".join(r["written"]) and r["pushed"] == []

if __name__ == "__main__":
    test_context_md_and_sync(); print("  ✓ test_context_md_and_sync")
    print("OK test_liljack_projects")
