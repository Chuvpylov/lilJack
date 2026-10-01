#!/usr/bin/env python3
"""tests/test_liljack_todo.py — TODO.md parser/check/note/enforcement."""
import sys, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent / "toolbox"))

SAMPLE = """# Demo — Scope
Build the demo end to end.

## M1
- [ ] write the parser
- [x] set up repo
  > lj: 2026-07-26 bootstrap
## M2
- [ ] ship it!
"""

def _tmp_todo():
    d = pathlib.Path(tempfile.mkdtemp())
    (d / ".git").mkdir()
    (d / "TODO.md").write_text(SAMPLE)
    return d

def test_ids_stable():
    from liljack_todo import item_id, normalize
    assert normalize("  Ship IT!  ") == "ship it"
    assert item_id("ship it!") == item_id("Ship   It")
    assert len(item_id("x")) == 7

def test_parse():
    from liljack_todo import parse_todo
    d = _tmp_todo()
    p = parse_todo(d / "TODO.md")
    assert p["scope"] == "Build the demo end to end."
    assert p["counts"] == {"done": 1, "total": 3}
    assert p["milestones"][0]["title"] == "M1"
    assert p["milestones"][0]["items"][1]["notes"] == ["2026-07-26 bootstrap"]

def test_find_todo_walks_up():
    from liljack_todo import find_todo
    d = _tmp_todo()
    sub = d / "a" / "b"; sub.mkdir(parents=True)
    assert find_todo(sub) == d / "TODO.md"
    assert find_todo(tempfile.mkdtemp()) is None

def test_check_and_note():
    from liljack_todo import parse_todo, check_item, append_note, item_id
    d = _tmp_todo()
    hit = check_item(d / "TODO.md", item_id("write the parser"), note="done by run lj-x")
    assert hit and hit["text"] == "write the parser"
    p = parse_todo(d / "TODO.md")
    assert p["counts"]["done"] == 2
    assert "done by run lj-x" in p["milestones"][0]["items"][0]["notes"][0]
    assert append_note(d / "TODO.md", "ship", "attempted lj-y, not confirmed")
    p = parse_todo(d / "TODO.md")
    assert "attempted lj-y" in p["milestones"][1]["items"][0]["notes"][0]

def test_inject_line_and_segment():
    from liljack_todo import inject_line, segment
    d = _tmp_todo()
    line = inject_line(str(d), known_project=True)
    assert line.startswith("[lilJack TODO] ☑1/3")
    assert "write the parser" in line
    assert segment(str(d)) == "☑1/3"
    empty = pathlib.Path(tempfile.mkdtemp()); (empty / ".git").mkdir()
    assert "no TODO.md" in inject_line(str(empty), known_project=True)
    assert inject_line(str(empty), known_project=False) == ""
    assert segment(str(empty)) == ""

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print(f"  ✓ {name}")
    print("OK test_liljack_todo")
