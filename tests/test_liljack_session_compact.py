#!/usr/bin/env python3
"""Handoff digest: compact a session's own record, then move it in.

the operator, 2026-09-12: structure room migration as a process — compact before the
move, leave the garbage (the replayed notice backlog) behind, the full session
always reachable through Apollo. Compaction is DERIVED: the archive and index
are byte-identical before and after (K3).
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_workspace import Workspace, session_key, project_key   # noqa: E402
import liljack_tasks as T                                            # noqa: E402
import liljack_handoff as H                                          # noqa: E402
from liljack_app.room_start import start_room, room_context          # noqa: E402


class FakeTmux:
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
                           'tmux_name': name, 'pane': '%1', 'cwd': cwd, 'project': project_key(project)}
        return sid


class CompactHandoffTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.task_path = self.root / 'agent_tasks.bmd'
        self.archive = self.root / 'archive'
        self.native = 'claude-uuid-1'
        self.sid = self.w.observe({'harness': 'claude', 'session': self.native, 'agent': 'claude',
                                   'cwd': str(self.project), 'state': 'running'})['id']
        self.old_room = self.w.room_create(self.project, 'Old room',
                                           purpose='spec docs/superpowers/specs/x.md', actor='operator')['id']
        self.w.room_move(self.project, self.sid, self.old_room, actor='operator')
        self.w.assign(self.sid, 'lead', actor='operator')
        T.propose('carry me over', 'operator', owner='claude', owner_session=self.sid, path=self.task_path)
        self.w.post(self.project, self.sid,
                    'I own toolbox/liljack_handoff.py and tests/test_liljack_session_compact.py',
                    'p1', destination=self.old_room)
        self.w.post(self.project, self.sid, 'Verdict: the landing is ACCEPTED', 'p2',
                    destination=self.old_room)
        # a fake archive: index entry + a transcript blob
        (self.archive / '2026' / '09' / 'demo').mkdir(parents=True)
        blob = self.archive / '2026' / '09' / 'demo' / f'{self.native}.jsonl.gz'
        blob.write_bytes(b'fake-transcript-bytes')
        (self.archive / 'index.jsonl').write_text(json.dumps(
            {'session_id': self.native, 'archive_path': '2026/09/demo/claude-uuid-1.jsonl.gz',
             'project_slug': 'demo'}) + '\n', encoding='utf-8')
        self.blob = blob
        self.tmux = FakeTmux([{'id': self.sid, 'agent': 'claude', 'harness': 'claude', 'state': 'running',
                               'tmux_name': 'claude-uuid-1', 'pane': '%1', 'cwd': str(self.project),
                               'project': str(self.project)}])

    def digest(self):
        return H.compact(self.w, self.project, self.sid, task_path=self.task_path,
                         archive_root=self.archive)

    # ── K1: the digest lists its tasks, verdicts, files, spec, apollo link ───
    def test_digest_carries_the_sessions_own_record(self):
        d = self.digest()
        self.assertEqual([t['id'] for t in d['tasks']], ['carry-me-over'])
        self.assertIn('toolbox/liljack_handoff.py', d['files'])
        self.assertEqual(d['spec'], 'docs/superpowers/specs/x.md')
        self.assertTrue(any('ACCEPTED' in v['text'] for v in d['verdicts']))
        self.assertEqual(d['apollo'], 'apollo://liljack-sessions/2026/09/demo/claude-uuid-1.jsonl.gz')
        text = H.render_digest(d)
        self.assertIn('carry-me-over', text)
        self.assertIn('apollo://liljack-sessions', text)
        # written beside the index, not into it
        self.assertTrue(Path(d['written']['json']).exists())
        self.assertTrue(Path(d['written']['text']).exists())

    def test_digest_is_size_capped(self):
        for i in range(40):
            self.w.post(self.project, self.sid, f'verdict number {i} ' + 'x' * 200,
                        f'v{i}', destination=self.old_room)
        d = self.digest()
        self.assertLessEqual(len(H.render_digest(d)), H.DIGEST_MAX_BYTES)
        # the cap is a hard, honest truncation (never a silent cut)
        capped = H.render_digest(d, max_bytes=500)
        self.assertLessEqual(len(capped), 500 + 120)
        self.assertIn('digest truncated', capped)

    def test_handoff_render_excludes_tasks_filed_to_another_room(self):
        # the K4 post / intro must never name another room's task (the operator 01:10).
        filed = T.propose('filed to another room', 'operator', owner='claude', owner_session=self.sid,
                          room='r-other', path=self.task_path)
        d = self.digest()
        self.assertIn(filed['id'], [t['id'] for t in d['tasks']])
        # the on-disk digest (room_id=None) is the session's own record — it keeps it
        self.assertIn(filed['id'], H.render_digest(d))
        # the room-scoped render (the intro / K4 post) drops it and says so
        scoped = H.render_digest(d, room_id=self.old_room)
        self.assertNotIn(filed['id'], scoped)
        self.assertIn('carry-me-over', scoped)
        self.assertIn('another room', scoped)

    # ── K3: nothing pruned — archive + index byte-identical ──────────────────
    def test_compact_and_move_leave_the_archive_and_index_byte_identical(self):
        before_index = (self.archive / 'index.jsonl').read_bytes()
        before_blob = self.blob.read_bytes()
        d = self.digest()
        payload = {'request_id': 'move-1', 'name': 'New room', 'folder': str(self.project),
                   'purpose': 'the handoff', 'backend': 'tmux',
                   'agents': [{'agent': 'claude', 'role': 'lead', 'session': self.sid}],
                   'todo': []}
        result = start_room(self.w, self.tmux, self.project, payload, actor='operator',
                            task_path=self.task_path, handoff_root=self.archive)
        self.assertEqual(result['start_status'], 'complete')
        self.assertEqual((self.archive / 'index.jsonl').read_bytes(), before_index,
                         'the index is byte-identical after a compact+move')
        self.assertEqual(self.blob.read_bytes(), before_blob,
                         'the archived transcript is byte-identical after a compact+move')

    # ── K2: the digest is the intro; old-room notices are skipped, not deleted ─
    def test_move_in_resumes_on_the_digest_and_skips_old_notices(self):
        self.digest()
        notice = self.w.post(self.project, 'operator', 'a pending notice', 'n1',
                             destination=self.old_room)
        self.w.db.execute("UPDATE room_delivery SET target=?, state='pending', "
                          "reason='Waiting for room lead' WHERE message_id=?", (self.sid, notice['id']))
        payload = {'request_id': 'move-2', 'name': 'New room', 'folder': str(self.project),
                   'purpose': 'the handoff', 'backend': 'tmux',
                   'agents': [{'agent': 'claude', 'role': 'lead', 'session': self.sid}],
                   'todo': []}
        result = start_room(self.w, self.tmux, self.project, payload, actor='operator',
                            task_path=self.task_path, handoff_root=self.archive)
        new_room = result['created_room']
        intro = self.w.db.execute('SELECT text FROM session_intros WHERE session_id=?',
                                  (self.sid,)).fetchone()[0]
        self.assertIn('HANDOFF DIGEST', intro, 'the intro carries the digest')
        self.assertIn('carry-me-over', intro)
        # K4: the moved session posted its digest in the new room
        posts = self.w.db.execute("SELECT m.text FROM messages m JOIN message_recipients r "
                                  "ON r.message_id=m.id WHERE m.sender=? AND m.destination=?",
                                  (self.sid, new_room)).fetchall()
        self.assertTrue(any('HANDOFF' in p[0] and 'carry-me-over' in p[0] for p in posts),
                        'the moved session\'s first post is generated from the digest')
        # the old-room notice is skipped, never deleted
        row = self.w.db.execute('SELECT state,reason FROM room_delivery WHERE message_id=?',
                                (notice['id'],)).fetchone()
        self.assertEqual(row['state'], 'skipped')
        self.assertIn('moved', row['reason'].lower())
        self.assertIsNotNone(self.w.db.execute('SELECT 1 FROM messages WHERE id=?',
                                               (notice['id'],)).fetchone())

    def test_move_in_without_a_digest_still_works(self):
        payload = {'request_id': 'move-3', 'name': 'New room', 'folder': str(self.project),
                   'purpose': 'no digest', 'backend': 'tmux',
                   'agents': [{'agent': 'claude', 'role': 'lead', 'session': self.sid}],
                   'todo': []}
        result = start_room(self.w, self.tmux, self.project, payload, actor='operator',
                            task_path=self.task_path, handoff_root=self.archive)
        self.assertEqual(result['start_status'], 'complete')
        ctx = room_context(self.w, self.project, result['created_room'], self.sid,
                           task_path=self.task_path)
        self.assertTrue(ctx['moved'])


if __name__ == '__main__':
    unittest.main()
