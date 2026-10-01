#!/usr/bin/env python3
"""room --context must hand each agent the two cursors the standing set demands:
the message sequence (messages.seq, chronological) and the event cursor
(events.seq, snapshot). item 8: every agent opened with a paragraph about which
cursor it did NOT get, because room_context returned neither.

fail-before: room_context had no `last_seq` and no `cursor` key, so intro_text
could not name them.
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
from liljack_workspace import Workspace  # noqa: E402
from liljack_app.room_start import room_context, intro_text  # noqa: E402


class RoomContextCursorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.w = Workspace(str(Path(self.tmp.name) / "workspace"))
        self.addCleanup(self.w.close)
        self.project = str(Path(self.tmp.name) / "demo")
        lead = self.w.observe({"harness": "tmux", "session": "socket/lead",
                               "agent": "claude", "cwd": self.project,
                               "state": "running"})["id"]
        self.w.assign(lead, "lead", actor="operator")
        self.room = self.w.room_create(self.project, "T", actor="operator")["id"]
        self.w.room_move(self.project, lead, self.room, actor="operator")
        self.lead = lead

    def test_room_context_reports_message_sequence_and_event_cursor(self):
        self.w.post(self.project, "operator", "hello room", "req-1", destination=self.room)
        ctx = room_context(self.w, self.project, self.room, self.lead)
        self.assertIn("cursor", ctx, "--context must carry the event cursor")
        self.assertIn("last_seq", ctx, "--context must carry the message sequence")
        # messages.seq and events.seq are monotonic; both are >= 1 after a post.
        self.assertGreaterEqual(ctx["last_seq"], 1)
        self.assertGreaterEqual(ctx["cursor"], 1)
        # the two are distinct tables with distinct sequences — the whole point.
        self.assertIsInstance(ctx["last_seq"], int)
        self.assertIsInstance(ctx["cursor"], int)

    def test_intro_names_both_cursors(self):
        intro = intro_text(self.w, self.project, self.room, self.lead)
        self.assertIn("message sequence", intro)
        self.assertIn("event cursor", intro)
        self.assertIn("--since", intro)


if __name__ == "__main__":
    unittest.main()
