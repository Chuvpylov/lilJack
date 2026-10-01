#!/usr/bin/env python3
"""--since / events() must return ROOM message events to a restarted session of
an agent that was already in the room's audience, exactly as snapshot()/--context
does. This is the "item 14 remainder": --context showed the room message but
--since returned only session events, so a lead that read the room in bulk
(--since) never saw the operator's room post.

fail-before: events() used plain _visible (viewer in frozen audience), so a NEW
session id of the same agent saw nothing. snapshot() has the same-agent fallback
(json_extract(data,'$.agent')), so the two disagreed.
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
from liljack_workspace import Workspace  # noqa: E402


class SinceRoomMessagesTests(unittest.TestCase):
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
        # the operator posts a room message while the ORIGINAL lead session is a member.
        self.w.post(self.project, "operator", "hello room", "req-1", destination=self.room)

    def _restart_lead(self):
        """Same agent (claude), NEW session id — a restarted lead."""
        lead2 = self.w.observe({"harness": "tmux", "session": "socket/lead2",
                                "agent": "claude", "cwd": self.project,
                                "state": "running"})["id"]
        self.w.room_move(self.project, lead2, self.room, actor="operator")
        return lead2

    def test_since_returns_room_message_to_restarted_same_agent(self):
        lead2 = self._restart_lead()
        # snapshot()/--context already shows it (the same-agent fallback).
        snap = self.w.snapshot(self.project, viewer=lead2)
        self.assertEqual([m["text"] for m in snap["messages"]], ["hello room"],
                         "snapshot shows the room message to the restarted lead")
        # --since must agree.
        ev = self.w.events(self.project, 0, viewer=lead2)
        texts = [e["data"].get("text") for e in ev["events"] if e["kind"] == "message"]
        self.assertEqual(texts, ["hello room"],
                         "--since returns the room message event to the restarted lead")

    def test_since_keeps_frozen_audience_for_a_different_agent(self):
        # A genuinely NEW agent (deepseek) that never joined the room still sees
        # nothing — the audience stays frozen; joining late reveals no history.
        other = self.w.observe({"harness": "tmux", "session": "socket/deep",
                                "agent": "deepseek", "cwd": self.project,
                                "state": "running"})["id"]
        self.w.room_move(self.project, other, self.room, actor="operator")
        ev = self.w.events(self.project, 0, viewer=other)
        texts = [e["data"].get("text") for e in ev["events"] if e["kind"] == "message"]
        self.assertEqual(texts, [], "a different, late-joining agent does not read room history")


if __name__ == "__main__":
    unittest.main()
