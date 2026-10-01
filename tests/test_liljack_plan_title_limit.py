#!/usr/bin/env python3
"""room --plan title limit (160) must TRUNCATE with an honest marker, not refuse
the whole call (item 4: six subjects and one plan were rejected and resent).

fail-before: room_registry.plan_tasks ran clean(title, MAX_TITLE), which raises
ValueError on a 161-char title, losing the plan. The fix (folded into the
plan_tasks edit) truncates to head[:MAX_TITLE-1] + "…" like tasks._note.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

# a launched session is refused moves on other sessions; these fixtures act as
# many sessions, so run unbound (same guard as test_liljack_room_protocol.py).
os.environ.pop("LILJACK_WORKSPACE_SESSION", None)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
import liljack_tasks as tasks  # noqa: E402
from liljack_workspace import Workspace  # noqa: E402
from liljack_app import room_protocol as p  # noqa: E402


class PlanTitleLimitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name)
        self.task_path = self.project / "tasks.bmd"
        self.w = Workspace(self.project / "workspace")
        self.addCleanup(self.w.close)
        rows = [dict(harness="test", session=name, cwd=str(self.project), agent=agent, state="running")
                for name, agent in [("lead", "codex"), ("worker", "deepseek")]]
        payload = dict(name="Protocol", folder=str(self.project), purpose="Review",
                       agents=[dict(agent="codex", role="lead"), dict(agent="deepseek", role="worker")])
        result, _ = self.w.reserve_room_start(self.project, "plan-title", payload, rows, actor="operator")
        self.room = result["created_room"]
        self.lead, self.worker = [r["session_id"] for r in result["started_sessions"]]
        for who in (self.lead, self.worker):
            p.align(self.w, self.project, self.room, who, "I understand the scope.")
        p.transition(self.w, self.project, self.room, "PLAN", self.lead, task_path=self.task_path)

    def test_plan_title_over_limit_truncates_with_marker(self):
        title = "R" * (tasks.MAX_TITLE + 1)
        todo = p.plan(self.w, self.project, self.room, self.lead,
                      [{"title": title, "owner_session": self.worker}],
                      "plan-long", task_path=self.task_path)["todo"][0]
        self.assertEqual(len(todo["title"]), tasks.MAX_TITLE,
                         "an over-long title is stored at exactly MAX_TITLE, not refused")
        self.assertEqual(todo["title"], "R" * (tasks.MAX_TITLE - 1) + "…",
                         "the title keeps its head and marks the cut")


if __name__ == "__main__":
    unittest.main()
