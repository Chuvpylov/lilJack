"""Frozen named-room audiences and strict local-only private delivery."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace
from liljack_room_bridge import sync_workspace
from liljack_app import cli
from liljack_app.backend import Backend
from liljack_app.runtime import room_instruction
import liljack_mail as mail


class RoomTests(unittest.TestCase):
    def test_shared_room_folder_resolver_is_scoped_and_checks_existence(self):
        self.project.mkdir()
        folder = self.root / 'Selected folder'
        folder.mkdir()
        room = self.w.room_create(self.project,'Folder',folder=str(folder),actor='operator')['id']
        self.assertEqual(self.w.room_folder(self.project,room),str(folder))
        self.assertEqual(self.w.room_folder(self.project),str(self.project))
        with self.assertRaisesRegex(ValueError,'does not belong'):
            self.w.room_folder(self.root/'Another project',room)
        self.w.db.execute('UPDATE rooms SET folder=? WHERE id=?',('',room))
        self.assertEqual(self.w.room_folder(self.project,room),str(self.project))
        self.w.db.execute('UPDATE rooms SET folder=? WHERE id=?',(str(folder),room))
        folder.rmdir()
        with self.assertRaisesRegex(ValueError,'existing directory'):
            self.w.room_folder(self.project,room)
        self.assertEqual(self.w.room_folder(self.project,room,require_exists=False),str(folder))

    def test_old_schema_migration_preserves_room_and_is_idempotent(self):
        import sqlite3
        root = self.root / 'old-workspace'
        root.mkdir()
        con = sqlite3.connect(root / 'workspace.sqlite3')
        con.execute('CREATE TABLE rooms (id TEXT PRIMARY KEY, project TEXT NOT NULL, name TEXT NOT NULL, collapsed INTEGER NOT NULL DEFAULT 0, resolved_at TEXT)')
        con.execute('INSERT INTO rooms VALUES(?,?,?,?,?)', ('r-old', str(self.project), 'Old room', 1, '2026-09-08'))
        con.commit()
        con.close()
        for _ in range(2):
            migrated = Workspace(root)
            try:
                room = migrated.snapshot(self.project)['rooms'][0]
                self.assertEqual(room['id'], 'r-old')
                self.assertEqual(room['folder'], str(self.project))
                self.assertEqual(room['purpose'], '')
                self.assertTrue(room['collapsed'])
                self.assertIsNone(room['resolved_at'])
                self.assertEqual(room['state'], 'active')
                migration = migrated.db.execute("SELECT data FROM events WHERE kind='room_state_migrated'").fetchall()
                self.assertEqual(len(migration), 1)
                self.assertEqual(json.loads(migration[0][0])['previous_resolved_at'], '2026-09-08')
                self.assertEqual(migrated.db.execute('SELECT count(*) FROM rooms').fetchone()[0], 1)
            finally:
                migrated.close()

    def test_spawn_membership_and_role_rollback_together(self):
        self.w.room_lead(self.project, self.room, self.a, actor='operator')
        sid = self.w.observe({'harness':'test', 'session':'fresh', 'agent':'codex', 'cwd':str(self.project)})['id']
        original = self.w._emit
        def fail(project, kind, data):
            if kind == 'team':
                raise RuntimeError('injected event failure')
            return original(project, kind, data)
        with patch.object(self.w, '_emit', side_effect=fail), self.assertRaises(RuntimeError):
            self.w.attach_spawned(self.project, sid, self.room, 'worker', actor='operator')
        self.assertIsNone(self.w.db.execute('SELECT * FROM room_members WHERE session_id=?', (sid,)).fetchone())
        self.assertIsNone(self.w.db.execute('SELECT * FROM team WHERE session_id=?', (sid,)).fetchone())

    def test_staged_reply_preserves_room_or_private_conversation(self):
        import shlex
        for destination, expected in ((self.room, self.room), ('room', 'room'), (self.b, self.a)):
            message = {'id': 'm-test', 'project': str(self.project), 'sender': self.a,
                       'destination': destination, 'text': 'Please review'}
            instruction = room_instruction(message, self.root)
            command = shlex.split(instruction.split('replacing YOUR_REPLY: ', 1)[1])
            self.assertEqual(command[command.index('--to') + 1], expected)
            self.assertEqual(command[command.index('--reply') + 1], 'm-test')

    def test_cli_post_defaults_to_current_membership_and_allows_explicit_lobby(self):
        args = ['--root', str(self.w.root), '--project', str(self.project), '--text', 'Team update']
        with patch.dict(os.environ, {'LILJACK_AGENT': 'claude', 'LILJACK_WORKSPACE_SESSION': self.a}, clear=True), patch.object(mail, 'CACHE', self.root / 'mail'):
            for extra, destination in (([], self.room), (['--to', 'room'], 'room')):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    cli.room_cli(args + extra)
                self.assertEqual(json.loads(out.getvalue())['destination'], destination)
            self.move(self.a, '')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                cli.room_cli(args)
            self.assertEqual(json.loads(out.getvalue())['destination'], 'room')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.a, self.b, self.c = [self.w.observe({'harness': 'test', 'session': provider,
            'agent': provider, 'cwd': str(self.project), 'state': 'running'})['id']
            for provider in ('claude', 'codex', 'deepseek')]
        self.room = self.w.room_create(self.project, '🌻 Україна 日本語', actor='operator')['id']
        self.move(self.a, self.room)
        self.move(self.b, self.room)

    def move(self, sid, room):
        return self.w.room_move(self.project, sid, room, actor='operator')

    def ids(self, viewer):
        return {m['id'] for m in self.w.snapshot(self.project, viewer=viewer)['messages']}

    def event_ids(self, viewer):
        return {e['data']['id'] for e in self.w.events(self.project, viewer=viewer)['events'] if e['kind'] == 'message'}

    def test_membership_collapse_and_refresh_persist(self):
        self.w.room_update(self.project, self.room, True, actor='operator')
        second = self.w.room_create(self.project, 'Review', actor='operator')['id']
        self.move(self.a, second)
        self.move(self.b, '')
        self.w.observe({'harness': 'test', 'session': 'claude', 'agent': 'claude', 'cwd': str(self.project)})
        reopened = Workspace(self.w.root)
        try:
            snapshot = reopened.snapshot(self.project)
            by_id = {s['id']: s for s in snapshot['sessions']}
            self.assertEqual(by_id[self.a]['room_id'], second)
            self.assertEqual(by_id[self.b]['room_id'], '')
            self.assertEqual(by_id[self.c]['room_id'], '')
            self.assertEqual(snapshot['rooms'][0], {'id': self.room, 'name': '🌻 Україна 日本語', 'collapsed': True,
                'resolved': False, 'resolved_at': None, 'folder': str(self.project), 'purpose': '',
                'state': 'active', 'state_at': self.w._room(str(self.project), self.room)['state_at'],
                'phase': 'ALIGN', 'phase_at': self.w._room(str(self.project), self.room)['phase_at'],
                'start_status': None, 'start_errors': [], 'started_sessions': [], 'task_ids': []})
            reopened.room_update(self.project, self.room, False, actor='operator')
            self.assertFalse(reopened.snapshot(self.project)['rooms'][0]['collapsed'])
        finally:
            reopened.close()

    def test_audience_frozen_on_join_leave_and_retry(self):
        old = self.w.post(self.project, self.a, 'earlier', 'old', destination=self.room)
        self.assertEqual(set(old['audience']), {'operator', self.a, self.b})
        self.move(self.b, '')
        self.move(self.c, self.room)
        fresh = self.w.post(self.project, self.a, 'later', 'new', destination=self.room)
        self.assertIn(old['id'], self.ids(self.b))
        self.assertNotIn(fresh['id'], self.ids(self.b))
        self.assertNotIn(old['id'], self.ids(self.c))
        self.assertIn(fresh['id'], self.ids(self.c))
        self.assertEqual(self.event_ids(self.b), self.ids(self.b))
        self.assertEqual(self.event_ids(self.c), self.ids(self.c))
        self.move(self.a, '')
        retry = self.w.post(self.project, self.a, 'earlier', 'old', destination=self.room)
        self.assertEqual(retry, old)
        with self.assertRaises(PermissionError):
            self.w.post(self.project, self.a, 'not a member now', 'denied', destination=self.room)

    def test_restarted_session_of_the_same_agent_sees_room_history(self):
        """⚠ A RESTARTED AGENT IS NOT A NEW PARTICIPANT.

        Room visibility rested entirely on the audience captured AT POST TIME,
        so a session that did not exist when a message was sent could never see
        it. Every restarted agent woke into a room with no history: measured
        2026-09-10, a fresh codex session reported "earlier named-room messages
        are not exposed to this new session by the recipient-filtered feed; I
        cannot claim a room-history last-read sequence" — and it was right.

        ⚠ THE FROZEN AUDIENCE STAYS FROZEN. The first cut of this fix widened to
        "any current member" and broke
        test_audience_frozen_on_join_leave_and_retry, which
        test_room_replies_cannot_widen_audience depends on. The discriminator is
        whether this AGENT already had access, not whether it is a member now.
        """
        old = self.w.post(self.project, self.a, 'before the restart', 'r1',
                          destination=self.room)
        self.assertIn(old['id'], self.ids(self.b))          # codex saw it live

        # codex's terminal restarts: same agent, brand-new session id, joins the room.
        b2 = self.w.observe({'harness': 'test', 'session': 'codex-2', 'agent': 'codex',
                             'cwd': str(self.project), 'state': 'running'})['id']
        self.assertNotEqual(b2, self.b)
        self.move(b2, self.room)
        self.assertIn(old['id'], self.ids(b2),
                      'a restarted session of the same agent must see its own history')

        # ⚠ AND A GENUINELY NEW AGENT STILL SEES NOTHING. deepseek was never a
        # recipient under any session, so joining late reveals no history.
        self.move(self.c, self.room)
        self.assertNotIn(old['id'], self.ids(self.c),
                         'joining late must not reveal earlier discussion')

    def test_lead_handover_demotes_the_previous_lead(self):
        """⚠ EXACTLY ONE LEAD. Promoting a session to lead only ever wrote the
        NEW row, so the old lead kept the role and the room had two. the operator
        surrendered lead to deepseek and claude stayed lead — and demoting
        claude by hand was then refused by the children rule, so the UI had no
        way out at all.

        The rule already existed one layer up (main.c's new-room dialog: "Exactly
        one lead: promoting one demotes the previous") and the ROLE menu already
        promised "takes over from <agent>". This is the store keeping it.
        """
        self.w.assign(self.a, 'lead', actor='operator')
        self.w.assign(self.b, 'worker', self.a, actor='operator')
        roles = lambda: {r[0]: r[1] for r in self.w.db.execute(
            "SELECT session_id, role FROM team")}
        self.assertEqual(roles()[self.a], 'lead')

        # hand over to b
        self.w.assign(self.b, 'lead', actor='operator')
        after = roles()
        self.assertEqual(after[self.b], 'lead')
        self.assertEqual(after[self.a], 'reviewer',
                         'the previous lead is demoted to reviewer, matching room_lead')
        leads = [s for s, r in after.items() if r == 'lead']
        self.assertEqual(leads, [self.b], 'exactly one lead')

        # ⚠ AND THE HANDOVER MUST NOT LEAVE THE ROOM UNABLE TO CHANGE ROLES.
        # The demoted lead's children now answer to the new lead, so demoting it
        # is legal; before, "reassign this lead's children" made it permanent.
        parent = self.w.db.execute("SELECT parent_id FROM team WHERE session_id=?",
                                   (self.a,)).fetchone()[0]
        self.assertEqual(parent, self.b, "the old lead now reports to the new one")
        self.w.assign(self.a, 'reviewer', actor='operator')   # must not raise
        self.assertEqual(roles()[self.a], 'reviewer')

        # the new lead answers to nobody
        self.assertIsNone(self.w.db.execute(
            "SELECT parent_id FROM team WHERE session_id=?", (self.b,)).fetchone()[0])

    def test_lead_handover_emits_event_per_reparented_child(self):
        """⚠ ONE TEAM EVENT PER RE-PARENTED CHILD. assign(lead) re-parented the
        old lead's children and demoted the old lead, but only emitted events for
        the old and the new lead — a five-member handover updated the table for
        all five yet emitted two events. Consumers that rebuild parent_id from
        events then saw a hierarchy the table no longer had. room_lead emits one
        event per member; assign must do the same.
        """
        self.w.assign(self.a, 'lead', actor='operator')
        d = self.w.observe({'harness': 'test', 'session': 'gpt', 'agent': 'gpt',
                            'cwd': str(self.project), 'state': 'running'})['id']
        e = self.w.observe({'harness': 'test', 'session': 'llama', 'agent': 'llama',
                            'cwd': str(self.project), 'state': 'running'})['id']
        for sid in (self.b, self.c, d, e):
            self.move(sid, self.room)
            self.w.assign(sid, 'worker', self.a, actor='operator')
        team_count = lambda: self.w.db.execute(
            "SELECT count(*) FROM events WHERE kind='team'").fetchone()[0]
        before = team_count()
        self.w.assign(self.b, 'lead', actor='operator')
        self.assertEqual(team_count() - before, 5,
                         'a five-member handover emits 5 team events: old lead, '
                         'new lead, and three re-parented children')
        roles = {r[0]: r[1] for r in self.w.db.execute(
            "SELECT session_id, role FROM team")}
        self.assertEqual(roles[self.a], 'reviewer')
        for child in (self.c, d, e):
            self.assertEqual(roles[child], 'worker')
            self.assertEqual(self.w.db.execute(
                "SELECT parent_id FROM team WHERE session_id=?", (child,)).fetchone()[0],
                self.b, 'each re-parented child now reports to the new lead')

    def test_lead_handover_retargets_pending_room_delivery(self):
        """⚠ PENDING NOTICES FOLLOW THE NEW LEAD. room_lead re-targets unsent
        room delivery; assign(lead) did not, so a notice already routed to the
        outgoing lead stayed stranded on a demoted session. Both paths must
        re-target identically.
        """
        self.w.assign(self.a, 'lead', actor='operator')
        self.w.assign(self.b, 'worker', self.a, actor='operator')
        msg = self.w.post(self.project, self.b, 'needs the lead', 'n1', destination=self.room)
        # the pump has already routed this unsent notice to the outgoing lead
        self.w.db.execute("UPDATE room_delivery SET target=?, state='pending', "
                          "reason='Waiting for room lead' WHERE message_id=?", (self.a, msg['id']))
        self.w.assign(self.b, 'lead', actor='operator')
        row = self.w.db.execute("SELECT target, state, reason FROM room_delivery "
                                "WHERE message_id=?", (msg['id'],)).fetchone()
        self.assertEqual(row['state'], 'pending')
        self.assertEqual(row['target'], '',
                         'the pending notice must be re-targeted, not left on the old lead')
        self.assertIn('leadership changed', row['reason'].lower())

    def test_room_replies_cannot_widen_audience(self):
        old = self.w.post(self.project, self.a, 'original', 'old', destination=self.room)
        self.move(self.c, self.room)
        with self.assertRaisesRegex(PermissionError, 'not addressed'):
            self.w.post(self.project, self.c, 'guessing reply ID', 'c', destination=self.room, reply_to=old['id'])
        with self.assertRaisesRegex(ValueError, 'widen'):
            self.w.post(self.project, self.a, 'new member would see parent', 'a', destination=self.room, reply_to=old['id'])
        with self.assertRaisesRegex(ValueError, 'widen'):
            self.w.post(self.project, self.a, 'public reply', 'public', reply_to=old['id'])
        narrowed = self.w.post(self.project, self.a, 'private followup', 'dm', destination=self.b, reply_to=old['id'])
        self.assertNotIn(narrowed['id'], self.ids(self.c))
        self.move(self.c, '')
        safe = self.w.post(self.project, self.b, 'same audience', 'safe', destination=self.room, reply_to=old['id'])
        self.assertEqual(set(safe['audience']), set(old['audience']))

    def test_dm_never_becomes_room_visible(self):
        self.move(self.c, self.room)
        dm = self.w.post(self.project, self.a, 'only B', 'dm', destination=self.b)
        self.assertNotIn(dm['id'], self.ids(self.c))
        self.assertNotIn(dm['id'], self.event_ids(self.c))
        with self.assertRaisesRegex(ValueError, 'keep its participants'):
            self.w.post(self.project, self.b, 'room reply', 'reply', destination=self.room, reply_to=dm['id'])
        valid = self.w.post(self.project, self.b, 'back to A', 'valid', destination=self.a, reply_to=dm['id'])
        self.assertIn(valid['id'], self.ids(self.a))
        self.assertNotIn(valid['id'], self.ids(self.c))

    def test_membership_is_operator_only_and_project_scoped(self):
        with self.assertRaises(PermissionError):
            self.w.room_create(self.project, 'unauthorized', actor=self.a)
        with self.assertRaises(PermissionError):
            self.w.room_update(self.project, self.room, True, actor=self.a)
        with self.assertRaises(PermissionError):
            self.w.room_move(self.project, self.c, self.room, actor=self.a)
        foreign = self.w.room_create(self.root / 'Other', 'Other', actor='operator')['id']
        with self.assertRaises(ValueError): self.move(self.c, foreign)
        with self.assertRaises(ValueError): self.w.room_update(self.project, foreign, True, actor='operator')
        with self.assertRaises(ValueError): self.w.room_update(self.project, self.room, 'false', actor='operator')
        with self.assertRaises(ValueError): self.w.post(self.project, self.a, 'invalid destination', 'bad', destination=[])
        with self.assertRaises(ValueError): self.move(self.a, [])

    def test_bridge_exports_only_lobby_not_private_or_named_room(self):
        self.w.post(self.project, self.a, 'named secret', 'named', destination=self.room)
        self.w.post(self.project, self.a, 'DM secret', 'dm', destination=self.b)
        public = self.w.post(self.project, self.a, 'lobby public', 'public')
        result = sync_workspace(self.w, self.project, mail_root=self.root / 'mail')
        self.assertEqual(result['exported'], 1)
        self.assertEqual(result['issues'], [])
        rows = mail._read(self.root / 'mail' / 'mailbox.jsonl')
        self.assertEqual([r['text'] for r in rows], ['lobby public'])
        self.assertEqual(rows[0]['workspace_message'], public['id'])
        self.assertEqual(rows[0]['to'], 'all')

    def test_cli_room_controls_and_frozen_visibility_of_delivery_issues(self):
        args = ['--root', str(self.w.root), '--project', str(self.project)]
        named = self.w.post(self.project, self.a, 'private room', 'named', destination=self.room)
        def invoke(more):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                result = cli.room_cli(args + more)
            self.assertEqual(result, 0)
            return json.loads(out.getvalue())
        with patch.dict(os.environ, {'LILJACK_AGENT': 'operator'}, clear=True), patch.object(mail, 'CACHE', self.root / 'mail'):
            created = invoke(['--create', 'CLI Room'])['created_room']
            moved = invoke(['--move', self.c, '--into', created])
            self.assertEqual(next(s for s in moved['sessions'] if s['id'] == self.c)['room_id'], created)
            collapsed = invoke(['--collapse', created])
            self.assertTrue(next(r for r in collapsed['rooms'] if r['id'] == created)['collapsed'])
            invoke(['--expand', created])
        with patch.dict(os.environ, {'LILJACK_AGENT': 'deepseek', 'LILJACK_WORKSPACE_SESSION': self.c}, clear=True), patch.object(mail, 'CACHE', self.root / 'mail'):
            hidden = invoke([])
            self.assertNotIn(named['id'], {m['id'] for m in hidden['messages']})
            self.assertNotIn(named['id'], {i.get('message_id') for i in hidden['bridge']['issues']})

    def test_backend_room_actions_and_create_validation(self):
        tmux = MagicMock(socket='fixture', binary='')
        tmux.sessions.return_value = []
        with patch('liljack_app.backend.Tmux', return_value=tmux), patch.object(Workspace, 'refresh', return_value={}), patch.object(mail, 'CACHE', self.root / 'mail'):
            backend = Backend(self.w.root, self.project)
            try:
                room = backend.request({'action': 'room_create', 'name': 'Backend'})['created_room']
                moved = backend.request({'action': 'room_move', 'session': self.a, 'room': room})
                self.assertEqual(next(s for s in moved['sessions'] if s['id'] == self.a)['room_id'], room)
                result = backend.request({'action': 'room_update', 'room': room, 'collapsed': True})
                self.assertTrue(next(r for r in result['rooms'] if r['id'] == room)['collapsed'])
                with self.assertRaises(ValueError):
                    backend.request({'action': 'create', 'agent': 'claude', 'room': 'r-missing'})
                tmux.create.assert_not_called()
            finally:
                backend.store.close()


if __name__ == '__main__':
    unittest.main()
