#!/usr/bin/env python3
"""Multi-room leak: an UNFILED registry row (room="") owned by an AGENT that sits
in more than one active room used to fill EVERY one of those rooms' todo views.

Measured 2026-09-20 in r-bcbc0454 (the operator: "debug multi room liljack issues"):
the claude worker's context listed thui-combination-lab, hui-image-scratch-lifetime
and live-overlays-invisible-wezterm — all room="" — none filed to that room, and
worker_wake.room_allows() never nudges a room-less row to a session in a room, so
they were shown everywhere and driven nowhere. Rule now: a room-less row is this
room's work only when BOUND to a member session, or by agent name when that agent
has no session in any OTHER active room (ambiguous -> shown nowhere).
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_workspace import Workspace, session_key, project_key   # noqa: E402
from liljack_app.room_start import start_room, room_context           # noqa: E402
import liljack_tasks as T                                            # noqa: E402
from test_liljack_room_start import FakeTmux                          # noqa: E402


class TwoRoomLeak(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.project = root / 'demo'
        self.w = Workspace(root / 'workspace')
        self.addCleanup(self.w.close)
        self.task_path = root / 'agent_tasks.bmd'
        self.live = []
        for native, agent in (('socket/a-lead', 'claude'), ('socket/a-codex', 'codex'),
                              ('socket/b-lead', 'claude'), ('socket/b-deepseek', 'deepseek')):
            sid = self.w.observe({'harness': 'tmux', 'session': native, 'agent': agent,
                                  'cwd': str(self.project), 'state': 'running'})['id']
            self.live.append({'id': sid, 'agent': agent, 'harness': 'tmux', 'state': 'running',
                              'tmux_name': native, 'pane': '%1', 'cwd': str(self.project),
                              'project': str(self.project)})
        self.a_lead, self.a_codex, self.b_lead, self.b_deepseek = [r['id'] for r in self.live]
        tmux = FakeTmux(self.live)
        def start(rid, name, agents):
            payload = {'request_id': rid, 'name': name, 'folder': str(self.project),
                       'purpose': 'p', 'backend': 'tmux', 'agents': agents, 'todo': []}
            return start_room(self.w, tmux, self.project, payload, actor='operator',
                              task_path=self.task_path)['created_room']
        self.room_a = start('a', 'Room A', [{'agent': 'claude', 'role': 'lead', 'session': self.a_lead},
                                            {'agent': 'codex', 'role': 'worker', 'session': self.a_codex}])
        self.room_b = start('b', 'Room B', [{'agent': 'claude', 'role': 'lead', 'session': self.b_lead},
                                            {'agent': 'deepseek', 'role': 'worker', 'session': self.b_deepseek}])

    def ids(self, room, viewer='operator'):
        return {t['id'] for t in room_context(self.w, self.project, room, viewer,
                                              task_path=self.task_path)['todo']}

    def test_unbound_row_of_an_agent_in_two_rooms_is_shown_nowhere(self):
        loose = T.propose('claude unfiled unbound', 'operator', owner='claude', path=self.task_path)
        self.assertNotIn(loose['id'], self.ids(self.room_a), 'ambiguous row must not fill room A')
        self.assertNotIn(loose['id'], self.ids(self.room_b), 'ambiguous row must not fill room B')

    def test_unbound_row_of_an_agent_in_one_room_fills_that_room_only(self):
        loose = T.propose('codex unfiled unbound', 'operator', owner='codex', path=self.task_path)
        self.assertIn(loose['id'], self.ids(self.room_a), 'codex sits only in room A: handoff case')
        self.assertNotIn(loose['id'], self.ids(self.room_b))

    def test_bound_row_follows_its_session(self):
        bound = T.propose('claude unfiled bound to B', 'operator', owner='claude',
                          owner_session=self.b_lead, path=self.task_path)
        self.assertNotIn(bound['id'], self.ids(self.room_a))
        self.assertIn(bound['id'], self.ids(self.room_b))
        self.assertIn(bound['id'], self.ids(self.room_b, viewer=self.b_lead), 'the bound session sees it')


if __name__ == '__main__':
    unittest.main()
