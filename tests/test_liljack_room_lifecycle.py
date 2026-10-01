"""Room transitions, retention and atomic team moves in isolated stores."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from datetime import datetime, timezone, timedelta
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace, ROOM_STATES
from liljack_app.backend import Backend
from liljack_app import cli


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name)
        self.w = Workspace(self.project / 'store')
        self.addCleanup(self.w.close)
        self.room = self.w.room_create(self.project, 'Source', actor='operator')['id']
        self.target = self.w.room_create(self.project, 'Target', actor='operator')['id']

    def member(self, name, role='worker', room=None, state='running', parent=None):
        sid = self.w.observe(dict(harness='test', session=name, agent='codex', cwd=str(self.project), state=state))['id']
        self.w.assign(sid, role, parent, actor='operator')
        self.w.room_move(self.project, sid, room or self.room, actor='operator')
        return sid

    def state(self, value, **kwargs):
        return self.w.room_state(self.project, self.room, value, actor='operator', **kwargs)

    def age(self, days):
        when = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()
        self.w.db.execute('UPDATE rooms SET state_at=? WHERE id=?', (when, self.room))

    def test_transitions_counts_projection_and_events(self):
        for state in ROOM_STATES:
            self.state(state)
            snap = self.w.snapshot(self.project)
            room = next(r for r in snap['rooms'] if r['id'] == self.room)
            self.assertEqual(room['state'], state)
            self.assertTrue(room['state_at'])
            self.assertEqual(room['resolved'], state == 'resolved')
            self.assertEqual(bool(room['resolved_at']), state == 'resolved')
            self.assertEqual(set(snap['room_states']), set(ROOM_STATES))
            self.assertEqual(sum(snap['room_states'].values()), 2)
        events = self.w.db.execute("SELECT data FROM events WHERE kind='room_state'").fetchall()
        self.assertEqual(len(events), 5)
        self.assertEqual(json.loads(events[-1][0])['previous_state'], 'archived')

    def test_live_unknown_pending_and_force_never_remove_session(self):
        for state in ('running', 'unknown', 'uncertain', 'planned'):
            with self.subTest(state=state):
                sid = self.member(state, state=state)
                for target in ('archived', 'removed'):
                    with self.assertRaisesRegex(ValueError, sid):
                        self.state(target)
                self.state('archived', force=True)
                self.assertEqual(next(s for s in self.w.snapshot(self.project)['sessions'] if s['id'] == sid)['state'], state)
                self.state('active')
        with self.assertRaisesRegex(ValueError, 'boolean'):
            self.state('removed', force='true')

    def test_purge_gate_exact_boundary_and_retains_history(self):
        sid = self.member('stopped', role='lead', state='stopped')
        msg = self.w.post(self.project, 'operator', 'Archive evidence', 'proof', destination=self.room)
        self.state('removed')
        with self.assertRaisesRegex(ValueError, '30 days'):
            self.w.room_purge(self.project, self.room, actor='operator')
        self.age(29.999)
        with self.assertRaisesRegex(ValueError, '1 days'):
            self.w.room_purge(self.project, self.room, actor='operator')
        stamp = self.w._room(str(self.project), self.room)['state_at']
        self.state('removed')
        self.assertEqual(stamp, self.w._room(str(self.project), self.room)['state_at'])
        self.age(30)
        self.assertEqual(self.w.days_until_purge(self.w._room(str(self.project), self.room)), 0)
        self.w.room_purge(self.project, self.room, actor='operator')
        self.assertIsNone(self.w.db.execute('SELECT 1 FROM rooms WHERE id=?', (self.room,)).fetchone())
        self.assertIsNotNone(self.w.db.execute('SELECT 1 FROM messages WHERE id=?', (msg['id'],)).fetchone())
        self.assertIsNotNone(self.w.db.execute('SELECT 1 FROM sessions WHERE id=?', (sid,)).fetchone())
        self.assertIsNotNone(self.w.db.execute("SELECT 1 FROM events WHERE kind='room_purged'").fetchone())

    def test_invalid_timestamp_and_live_purge_fail_closed(self):
        self.member('busy')
        self.state('removed', force=True)
        for value in (None, 'invalid', '2020-01-01'):
            self.w.db.execute('UPDATE rooms SET state_at=? WHERE id=?', (value, self.room))
            with self.assertRaisesRegex(ValueError, '30 days'):
                self.w.room_purge(self.project, self.room, actor='operator')
        self.age(31)
        with self.assertRaisesRegex(ValueError, 'Live'):
            self.w.room_purge(self.project, self.room, actor='operator')

    def test_restore_restarts_trash_clock_and_blocks_spawn(self):
        self.state('removed')
        self.age(31)
        with self.assertRaisesRegex(ValueError, 'reopen'):
            self.w.spawn_parent(self.project, self.room, 'lead', actor='operator')
        self.state('active')
        self.state('removed')
        self.assertEqual(self.w.days_until_purge(self.w._room(str(self.project), self.room)), 30)

    def test_operator_and_project_scope(self):
        for operation in (lambda: self.w.room_state(self.project, self.room, 'resolved', actor='codex'),
                          lambda: self.w.room_purge(self.project, self.room, actor='codex'),
                          lambda: self.w.room_move_all(self.project, self.room, self.target, actor='codex')):
            with self.assertRaises(PermissionError): operation()
        with self.assertRaises(ValueError): self.state('deleted')
        with self.assertRaises(ValueError):
            self.w.room_state(self.project / 'foreign', self.room, 'resolved', actor='operator')

    def test_atomic_roster_move_and_frozen_message_audience(self):
        lead = self.member('lead', 'lead')
        worker = self.member('worker', parent=lead)
        outsider = self.member('destination', room=self.target)
        message = self.w.post(self.project, 'operator', 'Before move', 'before', destination=self.room)
        self.w.room_move_all(self.project, self.room, self.target, actor='operator')
        self.assertEqual(self.w.room_roster(self.project, self.room), [])
        roster = self.w.room_roster(self.project, self.target)
        self.assertEqual({s['id'] for s in roster}, {lead, worker, outsider})
        self.assertTrue(all(s['lead_id'] == lead for s in roster if s['role'] != 'lead'))
        after = self.w._message(self.w.db.execute('SELECT * FROM messages WHERE id=?', (message['id'],)).fetchone())
        self.assertEqual(after['audience'], message['audience'])

    def test_conflicting_leads_and_event_failure_roll_back(self):
        # One lead per room (the handoff fix): move the target member in FIRST, then
        # promote it, so the promotion is room-scoped and both rooms keep a lead.
        lead = self.member('lead', 'lead')
        second = self.member('second', 'worker', room=self.target)
        self.w.assign(second, 'lead', actor='operator')
        with self.assertRaisesRegex(ValueError, 'exactly one lead'):
            self.w.room_move_all(self.project, self.room, self.target, actor='operator')
        self.w.assign(second, 'worker', actor='operator')
        before = self.w.snapshot(self.project)
        with patch.object(self.w, '_emit', side_effect=RuntimeError('event unavailable')):
            with self.assertRaises(RuntimeError):
                self.w.room_move_all(self.project, self.room, self.target, actor='operator')
            with self.assertRaises(RuntimeError): self.state('resolved')
        self.assertEqual(self.w.snapshot(self.project), before)
        self.assertEqual(self.w.room_roster(self.project, self.room)[0]['id'], lead)

    def test_external_parent_or_child_refuses_move(self):
        lead = self.member('lead', 'lead')
        other = self.w.room_create(self.project, 'Other', actor='operator')['id']
        child = self.member('external', room=other, parent=lead)
        with self.assertRaisesRegex(ValueError, 'child outside'):
            self.w.room_move_all(self.project, self.room, self.target, actor='operator')
        # One lead per room (the handoff fix): a lead never has a parent, so the
        # "parent outside" guard is exercised by a worker whose lead is elsewhere.
        self.w.assign(child, 'lead', actor='operator')
        self.member('worker2', 'worker', parent=child)
        with self.assertRaisesRegex(ValueError, 'parent outside'):
            self.w.room_move_all(self.project, self.room, self.target, actor='operator')

    def test_purge_retains_start_request_receipt(self):
        payload = {'name': 'Started', 'folder': str(self.project), 'purpose': '', 'agents': [{'agent': 'codex', 'role': 'lead'}]}
        row = dict(harness='test', session='started', agent='codex', cwd=str(self.project), state='stopped')
        result, _ = self.w.reserve_room_start(self.project, 'unique', payload, [row], actor='operator')
        self.room = result['created_room']
        self.state('removed')
        self.age(31)
        self.w.room_purge(self.project, self.room, actor='operator')
        retry, created = self.w.reserve_room_start(self.project, 'unique', payload, [row], actor='operator')
        self.assertFalse(created)
        self.assertEqual(retry['created_room'], self.room)

    def test_backend_routes_are_scoped_without_terminal_input(self):
        app = Backend.__new__(Backend)
        app.store, app.project, app.tmux = self.w, str(self.project), MagicMock()
        app.snapshot = lambda **kwargs: self.w.snapshot(self.project)
        app.tmux.sessions.return_value = []
        app.ribbon_snapshot = MagicMock(return_value={'fields': []})
        app.review_snapshot = MagicMock(return_value={'loading': True})
        for view in ('view', 'status', 'review', 'files', 'tools'):
            result = app.request({'action': 'room_' + view, 'room': self.room})
            self.assertEqual(result['room_action'], {'room': self.room, 'view': view})
        with patch.object(self.w, 'refresh', return_value=[]):
            app.request({'action': 'room_state', 'room': self.room, 'state': 'removed'})
        app.tmux.call.assert_not_called()
        app.tmux.create.assert_not_called()

    def test_cli_lifecycle_does_not_bridge_mail(self):
        with patch.dict(os.environ, {'LILJACK_AGENT': 'operator', 'LILJACK_WORKSPACE_SESSION': ''}), \
             patch.object(cli, 'caller', return_value='operator'), \
             patch.object(Workspace, 'refresh', return_value=[]), patch.object(cli, 'Tmux') as tmux, \
             patch.object(cli, '_sync_room', side_effect=AssertionError('unexpected bridge')):
            tmux.return_value.sessions.return_value = []
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                cli.room_cli(['--root', str(self.w.root), '--project', str(self.project), '--state', 'backlog', '--to', self.room])
            self.assertEqual(json.loads(out.getvalue())['changed_room']['state'], 'backlog')

    def test_removed_and_purged_messages_still_export(self):
        from liljack_room_export import read_room_record
        message = self.w.post(self.project, 'operator', 'Visible archive evidence', 'archive', destination=self.room)
        self.state('removed')
        record = read_room_record(self.w.root, self.project)
        self.assertEqual(next(r for r in record['rooms'] if r['id'] == self.room)['state'], 'removed')
        self.assertIn(message['id'], [m['id'] for m in record['messages']])
        self.age(31)
        self.w.room_purge(self.project, self.room, actor='operator')
        record = read_room_record(self.w.root, self.project)
        self.assertIn(message['id'], [m['id'] for m in record['messages']])
        self.assertNotIn(self.room, [r['id'] for r in record['rooms']])


if __name__ == '__main__':
    unittest.main()
