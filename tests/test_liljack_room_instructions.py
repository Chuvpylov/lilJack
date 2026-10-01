"""T5 room-folder-instructions: marked fragment written when absent, refreshed
inside markers, foreign files untouched, removed on resolve (file deleted only
when nothing else remains)."""
import sys, tempfile
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(REPO / "toolbox"))
from liljack_app import room_instructions as ri

def main():
    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp); lines = ["STANDING SET", "- one registry"]
        touched = ri.ensure_fragment(f, "r-1", "Room", "purpose", "/p", lines, "- ball — build")
        assert sorted(Path(t).name for t in touched) == ["AGENTS.md", "CLAUDE.md"]
        text = (f / "CLAUDE.md").read_text()
        assert ri.BEGIN in text and ri.END in text and "--context --to r-1" in text and "one registry" in text and "ball — build" in text
        # idempotent
        assert ri.ensure_fragment(f, "r-1", "Room", "purpose", "/p", lines, "- ball — build") == []
        # refresh inside markers keeps foreign text around it
        (f / "CLAUDE.md").write_text("# mine\n" + text + "\ntail\n")
        ri.ensure_fragment(f, "r-1", "Room", "purpose 2", "/p", lines)
        t2 = (f / "CLAUDE.md").read_text(); assert t2.startswith("# mine\n") and t2.rstrip().endswith("tail") and "purpose 2" in t2 and t2.count(ri.BEGIN) == 1
        # foreign file without markers is never touched
        (f / "AGENTS.md").write_text("theirs\n"); ri.ensure_fragment(f, "r-1", "Room", "p", "/p", lines)
        assert (f / "AGENTS.md").read_text() == "theirs\n"
        # removal: CLAUDE.md keeps the foreign parts, AGENTS.md untouched, a pure-fragment file is deleted
        (f / "X.md").write_text("")
        removed = ri.remove_fragment(f)
        t3 = (f / "CLAUDE.md").read_text(); assert ri.BEGIN not in t3 and "# mine" in t3 and "tail" in t3
        assert (f / "AGENTS.md").read_text() == "theirs\n"
        (f / "CLAUDE.md").unlink(); ri.ensure_fragment(f, "r-1", "Room", "p", "/p", lines); ri.remove_fragment(f)
        assert not (f / "CLAUDE.md").exists()
        print("room instructions: write/refresh/foreign/remove PASS")

if __name__ == "__main__": main()
