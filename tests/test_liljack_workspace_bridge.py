import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace
from liljack_room_bridge import sync_workspace
import liljack_mail as mail


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.w = Workspace(self.root / 'workspace')
        self.mail = self.root / 'mail'

    def tearDown(self):
        self.w.close()
        self.tmp.cleanup()

    def observe(self, name, provider='claude'):
        return self.w.observe({'harness': 'test', 'session': name, 'agent': provider,
                               'cwd': str(self.project), 'state': 'running'})['id']

    def sync(self):
        return sync_workspace(self.w, self.project, mail_root=self.mail)

    def incoming(self, mid, to='all', frm='claude', project='demo', **extra):
        mail._append(self.mail / 'mailbox.jsonl', {'id': mid, 'to': to, 'frm': frm,
            'project': project, 'text': '  Привіт 日本語 🌱\noriginal text  ', **extra})

    def test_multilingual_roundtrip_and_retry(self):
        text = '  Привіт 日本語 🌱\noriginal text  '
        original = self.w.post(self.project, 'operator', text, 'one', language='uk')
        self.assertEqual(self.sync()['exported'], 1)
        self.assertEqual(self.sync()['exported'], 0)
        rows = mail._read(self.mail / 'mailbox.jsonl')
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['text'], text)
        self.assertEqual(rows[0]['to'], 'all')
        # Simulate loss of SQLite bookkeeping after the append.
        self.w.db.execute('DELETE FROM mailbox_bridge')
        self.assertEqual(self.sync()['imported'], 0)
        self.assertEqual(len(mail._read(self.mail / 'mailbox.jsonl')), 1)
        self.incoming('remote')
        self.assertEqual(self.sync()['imported'], 1)
        self.assertEqual(self.sync()['imported'], 0)
        snapshot = self.w.snapshot(self.project)
        self.assertEqual(len(snapshot['messages']), 2)
        self.assertEqual(snapshot['messages'][1]['text'], text)
        self.assertEqual(snapshot['messages'][0]['id'], original['id'])

    def test_private_provider_mail_is_not_room_or_arbitrary_terminal(self):
        a, b = self.observe('first'), self.observe('second')
        self.incoming('private', to='codex')
        self.incoming('different-project', project='Other')
        self.assertEqual(self.sync()['imported'], 1)
        message = self.w.snapshot(self.project)['messages'][0]
        self.assertNotEqual(message['destination'], 'room')
        self.assertNotIn(message['sender'], (a, b))
        self.assertEqual(self.w.snapshot(self.project, viewer=a)['messages'], [])
        self.assertEqual(self.w.events(self.project, viewer=b)['events'][-1]['kind'], 'session')
        self.assertEqual(len(mail._read(self.mail / 'mailbox.jsonl')), 2)

    def test_private_outbound_ambiguous_and_oversized_stay_local(self):
        a = self.observe('first')
        self.observe('second')
        self.w.post(self.project, 'operator', 'private', 'private', destination=a)
        self.w.post(self.project, 'operator', 'x' * 4001, 'large')
        result = self.sync()
        self.assertEqual(result['exported'], 0)
        self.assertEqual(len(result['issues']), 1)
        self.assertIn('4000-character cap', result['issues'][0]['reason'])
        self.assertFalse((self.mail / 'mailbox.jsonl').exists())

    def test_even_unique_private_recipient_stays_local(self):
        a = self.observe('first')
        self.w.post(self.project, 'operator', 'private', 'private', destination=a)
        result = self.sync()
        self.assertEqual(result['exported'], 0)
        self.assertEqual(result['issues'], [])
        self.assertFalse((self.mail / 'mailbox.jsonl').exists())

    def test_malformed_optional_metadata_does_not_stop_following_mail(self):
        self.incoming('odd-metadata', workspace_message={'not': 'an ID'},
                      workspace_reply=['not an ID'])
        self.incoming('bad-language', workspace_language=['not a tag'])
        self.incoming('valid-after-malformed')
        result = self.sync()
        self.assertEqual(result['imported'], 2)
        self.assertEqual(len(result['issues']), 1)
        self.assertEqual(len(self.w.snapshot(self.project)['messages']), 2)

    def test_exact_cwd_metadata_does_not_cross_import(self):
        self.incoming('foreign-cwd', workspace_project=str(self.root / 'elsewhere' / 'demo'))
        self.assertEqual(self.sync()['imported'], 0)

    def test_mailbox_label_collision_refuses_second_project(self):
        self.incoming('private', to='codex')
        self.sync()
        other = self.root / 'other' / 'demo'
        with self.assertRaisesRegex(ValueError, 'already assigned'):
            sync_workspace(self.w, other, mail_root=self.mail)
        self.assertEqual(self.w.snapshot(other)['messages'], [])
        result = sync_workspace(self.w, other, mail_root=self.mail, mailbox_project='OtherProject')
        self.assertEqual(result['imported'], 0)

    def test_spawn_assignment_can_follow_concurrent_ui_binding(self):
        lead, child = self.observe('lead'), self.observe('child')
        self.w.assign(lead, 'lead', actor='operator')
        self.w.bind_terminal(child, 'socket', 'new-child', '%8', actor='operator')
        self.w.assign_spawned(child, 'reviewer', actor=lead)
        self.assertTrue(self.w.may_control(lead, child))
        with self.assertRaises(PermissionError):
            self.w.assign_spawned(child, actor=lead)

    def test_root_is_canonical_and_future_binding_not_valid(self):
        import os
        old = os.getcwd()
        try:
            os.chdir(self.root)
            relative = Workspace('relative-workspace')
            self.assertEqual(relative.root, self.root / 'relative-workspace')
            relative.close()
        finally:
            os.chdir(old)
        sid = self.observe('clock')
        self.w.bind_terminal(sid, 'socket', 'name', '%0', actor='operator')
        self.w.db.execute('UPDATE bindings SET verified_at=verified_at+3600')
        self.assertNotIn('terminal', self.w.snapshot(self.project)['sessions'][0]['capabilities'])

    def test_explicit_authority_and_lead_spawn(self):
        lead, worker = self.observe('lead'), self.observe('worker')
        with self.assertRaises(TypeError):
            self.w.assign(lead, 'lead')
        with self.assertRaises(TypeError):
            self.w.bind_terminal(lead, 'sock', 'name', '%0')
        with self.assertRaises(TypeError):
            self.w.unbind_terminal(lead)
        self.w.assign(lead, 'lead', actor='operator')
        with self.assertRaises(PermissionError):
            self.w.authorize_spawn(self.project, actor=worker)
        with self.assertRaises(PermissionError):
            self.w.authorize_spawn(self.project, actor=lead, role='lead')
        self.assertEqual(self.w.authorize_spawn(self.project, actor=lead), lead)
        self.w.assign_spawned(worker, 'reviewer', actor=lead)
        self.assertTrue(self.w.may_control(lead, worker))
        self.w.bind_terminal(worker, 'sock', 'name', '%0', actor=lead)
        self.w.unbind_terminal(worker, actor=lead)
        self.assertEqual(self.w.events(self.project)['events'][-1]['kind'], 'terminal_unbound')
        with self.assertRaises(PermissionError):
            self.w.assign_spawned(worker, actor=lead)
        with self.assertRaises(PermissionError):
            self.w.assign(worker, 'lead', actor=lead)
        with self.assertRaises(ValueError):
            self.w.authorize_spawn(self.root / 'Other', actor=lead)


if __name__ == '__main__':
    unittest.main()
