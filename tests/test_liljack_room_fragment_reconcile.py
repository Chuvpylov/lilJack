#!/usr/bin/env python3
"""coord-e — a room folder's CLAUDE.md/AGENTS.md fragment names the room that is LIVE there.

2026-09-17: "ring dashboard rewiew" was started twice in brain/orc. The twin that
was later REMOVED (r-3ac16ef7) wrote its fragment last, and removing it never
rewrote the folder, so agents launched in orc aligned into a dead room. the operator:
"fix the coordination bugs". After any lifecycle change, reconcile():
  R1 removing the twin rewrites the fragment for the other active room in the folder;
  R2 resolving the last active room removes the fragment;
  R3 a change in another folder leaves this folder's fragment alone;
  R4 reopening writes it back.
"""
import sys, tempfile, unittest
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(REPO / "toolbox"))


class Reconcile(unittest.TestCase):
    def test_fragment_follows_live_room(self):
        from liljack_workspace import Workspace
        from liljack_app import room_instructions as ri
        with tempfile.TemporaryDirectory() as tmp:
            project = Path(tmp) / "project"; project.mkdir()
            orc = Path(tmp) / "orc"; orc.mkdir(); other = Path(tmp) / "other"; other.mkdir()
            w = Workspace(Path(tmp) / "ws")
            try:
                live = w.room_create(str(project), "ring dashboard", actor="operator", folder=str(orc), purpose="p")["id"]
                twin = w.room_create(str(project), "ring dashboard", actor="operator", folder=str(orc), purpose="p")["id"]
                elsewhere = w.room_create(str(project), "else", actor="operator", folder=str(other), purpose="q")["id"]
                ri.ensure_fragment(orc, twin, "ring dashboard", "p", str(project), ["STANDING"])   # the twin wrote last
                w.room_state(str(project), twin, "removed", actor="operator", force=True)
                ri.reconcile(w, str(project), twin)
                text = (orc / "CLAUDE.md").read_text()
                self.assertIn(f"({live})", text, "R1 fragment still names the removed twin")
                self.assertNotIn(twin, text)
                self.assertIn(f"({live})", (orc / "AGENTS.md").read_text())
                before = (orc / "CLAUDE.md").read_text()
                w.room_state(str(project), elsewhere, "resolved", actor="operator")
                ri.reconcile(w, str(project), elsewhere)
                self.assertEqual((orc / "CLAUDE.md").read_text(), before, "R3 another folder's change touched orc")
                w.room_state(str(project), live, "resolved", actor="operator")
                ri.reconcile(w, str(project), live)
                self.assertFalse((orc / "CLAUDE.md").exists(), "R2 fragment left behind with no live room")
                w.room_state(str(project), live, "active", actor="operator")
                ri.reconcile(w, str(project), live)
                self.assertIn(f"({live})", (orc / "CLAUDE.md").read_text(), "R4 reopen did not write the fragment back")
            finally:
                w.close()


if __name__ == "__main__":
    r = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Reconcile))
    print("OK test_liljack_room_fragment_reconcile" if r.wasSuccessful() else "FAIL test_liljack_room_fragment_reconcile")
    sys.exit(0 if r.wasSuccessful() else 1)
