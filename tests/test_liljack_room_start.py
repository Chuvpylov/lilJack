#!/usr/bin/env python3
"""room --start MOVE-IN: an existing session joins a new room with its role,
the seeded todos land on the moved sessions, and the ACTIONABLE view is never
empty while the room works.

the operator, 2026-09-12: creating a room should be a handoff — TODO carry-over, folder,
role assignment. The old start_room only SPAWNED a fresh session per agent, so a
room created for the live team left every session and every task behind and the
ACTIONABLE tab stayed empty all night.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_workspace import Workspace, session_key, project_key   # noqa: E402
from liljack_app.room_start import start_room, room_context, normalize, intro_text  # noqa: E402
import liljack_tasks as T                                            # noqa: E402


class FakeTmux:
    """Just enough of the tmux adapter for start_room: sessions/create/preflight."""
    socket = 'fixture'

    def __init__(self, live):
        self._live = {r['id']: dict(r) for r in live}
        self.created = []

    def sessions(self, project):
        return [dict(r) for r in self._live.values()]

    def preflight(self, agent, folder, backend='tmux'):
        return True

    def create(self, agent, project, root, cwd=None, intro=None, name=None, backend='tmux'):
        native = f'{self.socket}/{name}' if backend == 'tmux' else name
        sid = session_key(backend, native)
        self.created.append(sid)
        self._live[sid] = {'id': sid, 'agent': agent, 'harness': backend, 'state': 'running',
                           'tmux_name': name, 'pane': '%1', 'cwd': cwd,
                           'project': project_key(project)}
        return sid


class RoomStartMoveInTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.task_path = self.root / 'agent_tasks.bmd'
        # Four live sessions: claude lead, codex worker, deepseek reviewer, claude reviewer.
        self.live = []
        for native, agent in (('socket/lead', 'claude'), ('socket/worker', 'codex'),
                              ('socket/rev', 'deepseek'), ('socket/rev2', 'claude')):
            sid = self.w.observe({'harness': 'tmux', 'session': native, 'agent': agent,
                                  'cwd': str(self.project), 'state': 'running'})['id']
            self.live.append({'id': sid, 'agent': agent, 'harness': 'tmux', 'state': 'running',
                              'tmux_name': native, 'pane': '%1', 'cwd': str(self.project),
                              'project': str(self.project)})
        self.lead, self.worker, self.reviewer, self.reviewer2 = [r['id'] for r in self.live]
        self.tmux = FakeTmux(self.live)

    def payload(self, **over):
        base = {'request_id': 'move-in-1', 'name': 'Handoff room', 'folder': str(self.project),
                'purpose': 'the one-shot handoff', 'backend': 'tmux',
                'agents': [{'agent': 'claude', 'role': 'lead', 'session': self.lead},
                           {'agent': 'codex', 'role': 'worker', 'session': self.worker},
                           {'agent': 'deepseek', 'role': 'reviewer', 'session': self.reviewer}],
                'todo': [{'title': 'carry the work over', 'assignee': 1},
                         {'title': 'review the landing', 'assignee': 2}]}
        base.update(over)
        return base

    def start(self, **over):
        return start_room(self.w, self.tmux, self.project, self.payload(**over),
                          actor='operator', task_path=self.task_path)

    # ── normalize accepts an optional session ────────────────────────────────
    def test_normalize_accepts_optional_session_and_rejects_junk(self):
        norm = normalize(self.project, self.payload())
        self.assertEqual(norm['agents'][0]['session'], self.lead)
        with self.assertRaises(ValueError):
            normalize(self.project, self.payload(agents=[{'agent': 'claude', 'role': 'lead', 'session': ''},
                                                          {'agent': 'codex', 'role': 'worker'}]))
        with self.assertRaises(ValueError):
            normalize(self.project, self.payload(agents=[{'agent': 'claude', 'role': 'lead', 'bogus': 'x'},
                                                          {'agent': 'codex', 'role': 'worker'}]))

    # ── (1) move-in: no spawn, role applied, membership moved ────────────────
    def test_move_in_moves_sessions_with_roles_and_never_spawns(self):
        result = self.start()
        self.assertEqual(result['start_status'], 'complete')
        self.assertEqual(self.tmux.created, [], 'no session is spawned when every agent is a move-in')
        roles = {s['session_id']: s['role'] for s in result['started_sessions']}
        self.assertEqual(roles[self.lead], 'lead')
        self.assertEqual(roles[self.worker], 'worker')
        self.assertEqual(roles[self.reviewer], 'reviewer')
        self.assertTrue(all(s['moved'] for s in result['started_sessions']))
        members = {r[0] for r in self.w.db.execute('SELECT session_id FROM room_members WHERE room_id=?',
                                                   (result['created_room'],))}
        self.assertEqual(members, {self.lead, self.worker, self.reviewer})
        # the sessions table is untouched (moved, not re-created)
        self.assertEqual(self.w.db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0], 4)

    # ── a not-live session is an ERROR, never a spawn ────────────────────────
    def test_not_live_session_is_an_error_not_a_spawn(self):
        # observed in the store, but absent from tmux: "not live", not "unknown"
        ghost = self.w.observe({'harness': 'tmux', 'session': 'socket/ghost', 'agent': 'codex',
                                'cwd': str(self.project), 'state': 'running'})['id']
        with self.assertRaisesRegex(ValueError, 'not live'):
            self.start(agents=[{'agent': 'claude', 'role': 'lead', 'session': ghost},
                               {'agent': 'codex', 'role': 'worker', 'session': self.worker}],
                       todo=[])
        self.assertEqual(self.tmux.created, [], 'the refused start spawns nothing')

    # ── (2) seeded todos land on the moved sessions ──────────────────────────
    def test_seeded_todos_resolve_assignee_to_the_moved_session(self):
        result = self.start()
        seeded = T.tasks(room=result['created_room'], open_only=True, path=self.task_path)
        self.assertEqual(len(seeded), 2)
        by_title = {t['title']: t for t in seeded}
        self.assertEqual(by_title['carry the work over']['owner_session'], self.worker)
        self.assertEqual(by_title['review the landing']['owner_session'], self.reviewer)
        self.assertEqual(set(result['task_ids']), {t['id'] for t in seeded})

    # ── (3) room_context says MOVED and carries the intro ────────────────────
    def test_room_context_marks_a_moved_session_and_intro_says_moved(self):
        result = self.start()
        ctx = room_context(self.w, self.project, result['created_room'], self.worker,
                           task_path=self.task_path)
        self.assertTrue(ctx['moved'], 'a moved session is reported as moved')
        intro = intro_text(self.w, self.project, result['created_room'], self.worker,
                           task_path=self.task_path, moved=True)
        self.assertIn('MOVED', intro)
        self.assertIn('Handoff room', intro)

    def test_new_room_intro_carries_the_safe_post_line(self):
        # 2026-09-12: the transport fix must be stated in every new room intro.
        result = self.start()
        intro = intro_text(self.w, self.project, result['created_room'], self.worker,
                           task_path=self.task_path)
        self.assertIn('--text-file', intro)
        self.assertIn('stdin', intro)
        self.assertIn('send_message', intro)

    # ── (4) ACTIONABLE view merges member registry tasks ─────────────────────
    def test_empty_room_todo_falls_back_to_member_registry_tasks(self):
        # the empty-tab case: no room-scoped todo, but the room's members hold work
        outside = T.propose('carry-over on the queue', 'operator', owner='codex',
                            owner_session=self.worker, path=self.task_path)
        result = self.start(todo=[])            # a room with NO seeded todos
        ctx = room_context(self.w, self.project, result['created_room'], 'operator',
                           task_path=self.task_path)
        ids = {t['id'] for t in ctx['todo']}
        self.assertIn(outside['id'], ids, 'a member-owned registry task fills the empty room tab')
        self.assertTrue(all(t.get('source') == 'registry' for t in ctx['todo']),
                        'every todo entry is marked source:"registry"')

    def test_fallback_skips_a_task_filed_to_another_room(self):
        # the operator live 01:10: an ARCHIVED room's tasks leaked into whatever room the
        # same agents later sat in. A task filed to another room is not this room's.
        leaked = T.propose('archived room work', 'operator', owner='codex',
                           owner_session=self.worker, room='r-archived', path=self.task_path)
        result = self.start(todo=[])
        ctx = room_context(self.w, self.project, result['created_room'], 'operator',
                           task_path=self.task_path)
        self.assertNotIn(leaked['id'], {t['id'] for t in ctx['todo']},
                         "another room's task does not leak into this room's fallback")

    def test_fallback_keeps_unfiled_member_task_and_drops_filed_other_room(self):
        unfiled = T.propose('unfiled carry-over', 'operator', owner='codex',
                            owner_session=self.worker, path=self.task_path)
        filed = T.propose('filed elsewhere', 'operator', owner='codex',
                          owner_session=self.worker, room='r-other', path=self.task_path)
        result = self.start(todo=[])
        ctx = room_context(self.w, self.project, result['created_room'], 'operator',
                           task_path=self.task_path)
        ids = {t['id'] for t in ctx['todo']}
        self.assertIn(unfiled['id'], ids, 'an unfiled member task is the handoff case — eligible')
        self.assertNotIn(filed['id'], ids, 'a task filed to another room is skipped')

    def test_non_empty_room_todo_keeps_viewer_isolation(self):
        # the scoped view wins: a worker must not see another member's task
        T.propose('worker own', 'operator', owner='codex', owner_session=self.worker,
                  room='r-other', path=self.task_path)
        other = T.propose('reviewer private', 'operator', owner='deepseek',
                          owner_session=self.reviewer, room='r-other', path=self.task_path)
        result = self.start(todo=[{'title': 'my room todo', 'assignee': 1}])
        ctx = room_context(self.w, self.project, result['created_room'], self.worker,
                           task_path=self.task_path)
        ids = {t['id'] for t in ctx['todo']}
        self.assertNotIn(other['id'], ids, 'a worker does not read a reviewer\'s task')

    # ── the round-start JSON: four live sessions moved, todos seeded ─────────
    @unittest.skipUnless((Path(__file__).resolve().parents[1] / 'docs/reports/2026-09-12-room-start-next-round.json').exists(),
                         'fixture docs/reports/2026-09-12-room-start-next-round.json not shipped')
    def test_next_round_json_moves_four_live_sessions_and_seeds_todos(self):
        payload = json.loads((ROOT / 'docs/reports/2026-09-12-room-start-next-round.json').read_text())
        payload['request_id'] = 'next-round-test'
        payload['folder'] = str(self.project)
        # the file now fits the caps (the lead shortened it) and already carries
        # session entries; the test swaps in its own fixture sessions.
        self.assertLessEqual(len(payload['name']), 80)
        self.assertTrue(all(len(t['title']) <= T.MAX_TITLE for t in payload['todo']))
        for agent, sid in zip(payload['agents'], [self.lead, self.worker, self.reviewer, self.reviewer2]):
            agent['session'] = sid
        result = start_room(self.w, self.tmux, self.project, payload, actor='operator',
                            task_path=self.task_path)
        self.assertEqual(result['start_status'], 'complete')
        self.assertEqual(self.tmux.created, [], 'all four are moved in, none spawned')
        self.assertEqual(len(result['started_sessions']), 4)
        self.assertEqual(len(result['task_ids']), len(payload['todo']))
        ctx = room_context(self.w, self.project, result['created_room'], 'operator',
                           task_path=self.task_path)
        self.assertEqual(len(ctx['todo']), len(payload['todo']),
                         'every seeded todo shows in room_context')
        self.assertEqual(sum(s['role'] == 'lead' for s in result['started_sessions']), 1)
        self.assertEqual(len([s for s in result['started_sessions'] if s['agent'] == 'claude']), 2)
        self.assertTrue(all(s['moved'] for s in result['started_sessions']))

    # ── a spawn and a move in the same call ──────────────────────────────────
    def test_mixed_spawn_and_move(self):
        result = self.start(agents=[{'agent': 'claude', 'role': 'lead', 'session': self.lead},
                                    {'agent': 'codex', 'role': 'worker'}],
                            todo=[{'title': 'spawned work', 'assignee': 1}])
        self.assertEqual(len(self.tmux.created), 1, 'the one non-session agent is spawned')
        roles = {s['agent']: s['role'] for s in result['started_sessions']}
        moved = [s for s in result['started_sessions'] if s['moved']]
        self.assertEqual([s['session_id'] for s in moved], [self.lead])


if __name__ == '__main__':
    unittest.main()
