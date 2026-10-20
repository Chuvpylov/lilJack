#!/usr/bin/env python3
"""Opening a room session must not re-parse the task registry once per room.

2026-10-19 report: "claude and any other worker in the room opens with a huge
delay". Measured on the live 2.6 MB registry: `room --context` took 7.7 s of
pure CPU, 8.7 s of it in 29 full `bmd.parse` calls — one per other room for the
"open TODOs" digest. start_room builds that same context for EVERY agent before
launching any, and every agent's first action runs `--context` again.

The contract: within one process an unchanged registry is parsed once; a write
is seen by the next read; a caller mutating what read() returned cannot poison
the next reader.
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_workspace import Workspace                               # noqa: E402
from liljack_app.room_start import start_room, room_context, intro_text  # noqa: E402
import liljack_board as board                                         # noqa: E402
import liljack_tasks as T                                             # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_liljack_room_start import FakeTmux                          # noqa: E402


class ParseCounter:
    def __init__(self):
        self.B = board.bmd()
        self.real = self.B.parse
        self.calls = 0

    def __enter__(self):
        def counted(*a, **k):
            self.calls += 1
            return self.real(*a, **k)
        self.B.parse = counted
        return self

    def __exit__(self, *exc):
        self.B.parse = self.real


class RegistryParseOnceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.project.mkdir()
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.task_path = self.root / 'agent_tasks.bmd'
        T.propose('seed task', by='operator', owner='claude', path=self.task_path)

    def test_room_context_parses_registry_once_across_many_rooms(self):
        live = []
        for native, agent in (('socket/lead', 'claude'), ('socket/worker', 'codex')):
            sid = self.w.observe({'harness': 'tmux', 'session': native, 'agent': agent,
                                  'cwd': str(self.project), 'state': 'running'})['id']
            live.append({'id': sid, 'agent': agent, 'harness': 'tmux', 'state': 'running',
                         'tmux_name': native, 'pane': '%1', 'cwd': str(self.project),
                         'project': str(self.project)})
        for i in range(12):
            self.w.room_create(self.project, f'other {i}', actor='operator', folder=str(self.project))
        result = start_room(self.w, FakeTmux(live), self.project,
                            {'request_id': 'parse-once', 'name': 'Main', 'folder': str(self.project),
                             'purpose': 'p', 'backend': 'tmux',
                             'agents': [{'agent': 'claude', 'role': 'lead', 'session': live[0]['id']},
                                        {'agent': 'codex', 'role': 'worker', 'session': live[1]['id']}],
                             'todo': []},
                            actor='operator', task_path=self.task_path)
        room = result['created_room']
        with ParseCounter() as c:
            ctx = room_context(self.w, self.project, room, live[1]['id'], task_path=self.task_path)
            intro_text(self.w, self.project, room, live[0]['id'], task_path=self.task_path)
        self.assertGreaterEqual(len(ctx['other_rooms']), 12)
        self.assertLessEqual(c.calls, 1, f'registry parsed {c.calls} times for one context + one intro')

    def test_write_is_seen_and_caller_mutation_does_not_leak(self):
        first = T.read(self.task_path)
        first['tasks'][0]['title'] = 'POISONED'
        first['tasks'].append({'id': 'ghost'})
        second = T.read(self.task_path)
        self.assertEqual([t['title'] for t in second['tasks']], ['seed task'])
        T.propose('second task', by='operator', owner='codex', path=self.task_path)
        third = T.read(self.task_path)
        self.assertEqual(sorted(t['title'] for t in third['tasks']), ['second task', 'seed task'])


if __name__ == '__main__':
    unittest.main(verbosity=1)
