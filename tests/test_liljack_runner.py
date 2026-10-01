#!/usr/bin/env python3
"""tests/test_liljack_runner.py — prompt/ledger/result-parse; tmux never invoked."""
import json, sys, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent / "toolbox"))

def _proj(tmp):
    p = tmp / "demo"; p.mkdir(); (p / ".git").mkdir()
    (p / "TODO.md").write_text("# Demo — Scope\nShip it.\n\n## M1\n- [ ] build the widget\n- [ ] test the widget\n")
    return p

def test_prompt_has_sentinel_and_scope():
    from liljack_runner import build_prompt
    p = build_prompt("Ship it.", "build the widget")
    assert "Ship it." in p and "build the widget" in p
    assert "ITEM COMPLETE" in p and "ITEM INCOMPLETE" in p

def test_start_dry_run_and_ledger():
    import liljack_runner as lr
    tmp = pathlib.Path(tempfile.mkdtemp())
    lr.RUNS = tmp / "runs"; lr.LEDGER = lr.RUNS / "ledger.jsonl"
    proj = _proj(tmp)
    ev = lr.start_run(proj, dry_run=True)
    assert ev["status"] == "dry-run" and ev["item_text"] == "build the widget"
    assert ev["run_id"].startswith("lj-demo-")
    ev2 = lr.start_run(proj, ref="test the", dry_run=True)
    assert ev2["item_text"] == "test the widget"
    runs = lr.list_runs()
    assert {r["run_id"] for r in runs} >= {ev["run_id"], ev2["run_id"]}

def test_parse_result_and_finish():
    import liljack_runner as lr
    from liljack_todo import parse_todo
    tmp = pathlib.Path(tempfile.mkdtemp())
    lr.RUNS = tmp / "runs"; lr.LEDGER = lr.RUNS / "ledger.jsonl"
    proj = _proj(tmp)
    ev = lr.start_run(proj, dry_run=True)
    stream = pathlib.Path(ev["stream"]); stream.parent.mkdir(parents=True, exist_ok=True)
    stream.write_text(
        json.dumps({"type": "system", "subtype": "init"}) + "\n" +
        json.dumps({"type": "result", "is_error": False,
                    "result": "Widget built and verified.\nITEM COMPLETE"}) + "\n")
    r = lr.parse_result(stream)
    assert r["complete"] and not r["is_error"]
    fin = lr.finish_run(ev["run_id"])
    assert fin["status"] == "done"
    parsed = parse_todo(proj / "TODO.md")
    assert parsed["counts"]["done"] == 1
    assert "lj-demo-" in parsed["milestones"][0]["items"][0]["notes"][0]
    # incomplete path
    ev2 = lr.start_run(proj, ref="test the", dry_run=True)
    s2 = pathlib.Path(ev2["stream"]); s2.write_text(json.dumps(
        {"type": "result", "is_error": False, "result": "Blocked.\nITEM INCOMPLETE"}) + "\n")
    fin2 = lr.finish_run(ev2["run_id"])
    assert fin2["status"] == "failed"
    parsed = parse_todo(proj / "TODO.md")
    assert parsed["counts"]["done"] == 1          # still unchecked
    assert any("not confirmed" in n for n in parsed["milestones"][0]["items"][1]["notes"])
    # timeout path: empty stream → status timeout
    ev3 = lr.start_run(proj, ref="test the", dry_run=True)
    pathlib.Path(ev3["stream"]).write_text("")
    fin3 = lr.finish_run(ev3["run_id"])
    assert fin3["status"] == "timeout"

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print(f"  ✓ {name}")
    print("OK test_liljack_runner")
