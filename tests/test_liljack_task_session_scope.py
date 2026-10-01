#!/usr/bin/env python3
"""coord-c — task views and moves are scoped to the caller's exact session.

Room r-bcbc0454 (2026-09-17): a deepseek worker read three ring-room tasks, bound
to another deepseek session in another room, as "My assigned work", because
task_list(owner=deepseek) and task_update authorise by agent name only. the operator:
"fix the coordination bugs". A launched session (LILJACK_WORKSPACE_SESSION) may
move only rows whose owner_session is its own or blank; its own agent's rows
bound to other sessions are hidden from its lists (with a count). The operator
CLI (no session) and the operator keep full reach.
"""
import os, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox")); sys.path.insert(0, str(ROOT))


class SessionScope(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.env = patch.dict(os.environ, {"LILJACK_TASKS": str(base/"tasks.bmd"), "LILJACK_BOARD": str(base/"board.bmd")})
        self.env.start()
        os.environ.pop("LILJACK_WORKSPACE_SESSION", None)
        import liljack_tasks as T
        self.T = T
        self.ring = T.propose("analyse ring dashboard", by="operator", owner="deepseek", room="r-ring", owner_session="s-ring")
        self.mine = T.propose("capture tabs", by="operator", owner="deepseek", room="r-liljack", owner_session="s-liljack")
        self.loose = T.propose("old unbound row", by="operator", owner="deepseek")

    def tearDown(self):
        self.env.stop(); self.tmp.cleanup()

    def as_session(self, sid):
        return patch.dict(os.environ, {"LILJACK_WORKSPACE_SESSION": sid})

    def test_other_session_row_cannot_be_moved(self):
        T = self.T
        with self.as_session("s-liljack"):
            with self.assertRaises(T.Denied) as cm:
                T.set_state(self.ring["id"], "active", "deepseek")
            self.assertIn("s-ring", str(cm.exception))
            with self.assertRaises(T.Denied):
                T.set_progress(self.ring["id"], "1/2", "deepseek")
            with self.assertRaises(T.Denied):
                T.unassign(self.ring["id"], "deepseek", "not mine")
            T.set_state(self.mine["id"], "active", "deepseek")        # own session: fine
            T.set_state(self.loose["id"], "active", "deepseek")       # unbound row: fine (and now bound)
        self.assertEqual(T.get(self.loose["id"])["owner_session"], "s-liljack")
        with self.as_session("s-ring"):
            T.set_state(self.ring["id"], "active", "deepseek")
        T.set_state(self.ring["id"], "blocked", "deepseek", note="operator CLI, no session")
        T.set_state(self.ring["id"], "active", "operator")

    def test_list_hides_own_agent_rows_of_other_sessions(self):
        import liljack_mcp as m
        with self.as_session("s-liljack"), patch.dict(os.environ, {"LILJACK_AGENT": "deepseek"}), patch.object(m, "AGENT", "deepseek"), patch.object(m, "_mirror", lambda: None):
            out = m.t_task_list(owner="deepseek", open_only=True)
        self.assertIn(self.mine["id"], out)
        self.assertIn(self.loose["id"], out)
        self.assertNotIn(self.ring["id"], out)
        self.assertIn("1 deepseek task bound to other sessions hidden", out)
        with patch.object(m, "_mirror", lambda: None):
            self.assertIn(self.ring["id"], m.t_task_list(owner="deepseek", open_only=True))   # no session: full view

    def test_long_id_duplicate_is_addressable(self):
        """Found fixing coord-d: _find normalised '<60-char id>-2' back to the
        original id, so the duplicate row a second room start creates could not be
        addressed — abandoning it hit the ORIGINAL (done) row instead."""
        T = self.T
        title = "improve file browser popup window with proper folder browser controlls and icons"
        a = T.propose(title, by="operator", owner="claude", room="r-live", owner_session="s-live")
        b = T.propose(title, by="operator", owner="claude", room="r-dead", owner_session="s-dead")
        self.assertNotEqual(a["id"], b["id"])
        self.assertEqual(T.get(b["id"])["room"], "r-dead")
        T.abandon(b["id"], "operator", "duplicate room start")
        self.assertEqual(T.get(a["id"])["state"], "assigned")
        self.assertEqual(T.get(b["id"])["state"], "abandoned")



class RoomStampOnMove(unittest.TestCase):
    """Multi-room (the operator 2026-09-20): a launched session that claims or starts an
    UNFILED row files it to the session's own room, so the wake loop can drive it
    (room_allows never nudges a room-less row into a room) and no other room's
    context shows it."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.env = patch.dict(os.environ, {"LILJACK_TASKS": str(base/"tasks.bmd"), "LILJACK_BOARD": str(base/"board.bmd"),
                                           "LILJACK_WORKSPACE_SESSION": "s-liljack"})
        self.env.start()
        import liljack_tasks as T
        self.T = T
        self.room = patch.object(T.mail, "session_room", lambda sid: "r-liljack" if sid == "s-liljack" else "")
        self.room.start()

    def tearDown(self):
        self.room.stop(); self.env.stop(); self.tmp.cleanup()

    def test_start_stamps_room_and_session(self):
        T = self.T
        row = T.propose("unfiled", by="operator", owner="claude")
        with patch.object(T.mail, "detect_agent", lambda: "claude", create=True):
            out = T.start(row["id"], by="claude")
        self.assertEqual(out["owner_session"], "s-liljack")
        self.assertEqual(out["room"], "r-liljack", "starting from a room files the row there")

    def test_claim_stamps_room_and_session(self):
        T = self.T
        row = T.propose("unowned", by="operator")
        out = T.claim(row["id"], by="claude")
        self.assertEqual(out["owner_session"], "s-liljack")
        self.assertEqual(out["room"], "r-liljack")

    def test_filed_row_keeps_its_room(self):
        T = self.T
        row = T.propose("filed", by="operator", owner="claude", room="r-other")
        out = T.start(row["id"], by="claude")
        self.assertEqual(out["room"], "r-other", "an explicit room is never overwritten")


if __name__ == "__main__":
    r = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    print("OK test_liljack_task_session_scope" if r.wasSuccessful() else "FAIL test_liljack_task_session_scope")
    sys.exit(0 if r.wasSuccessful() else 1)
