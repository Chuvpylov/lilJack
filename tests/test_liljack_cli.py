import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace, session_key
from liljack_app import cli
import liljack_mail


class CliTests(unittest.TestCase):
    def test_alignment_and_phase_cli_use_session_authority(self):
        self.project.mkdir()
        room = self.store.room_create(self.project, 'Alignment', actor='operator')['id']
        self.store.room_move(self.project, self.lead, room, actor='operator')
        self.store.room_move(self.project, self.worker, room, actor='operator')
        for agent, sid in [('claude', self.lead), ('deepseek', self.worker)]:
            with self.env(agent, sid), patch.object(cli, '_sync_room', side_effect=AssertionError('mail bridge')):
                code, result = self.invoke(['room', *self.args, '--to', room, '--align', 'Understand and wait.'])
            self.assertEqual(code, 0, result)
            self.assertEqual(result['phase'], 'ALIGN')
        with self.env('deepseek', self.worker):
            code, result = self.invoke(['room', *self.args, '--to', room, '--phase', 'PLAN'])
            self.assertEqual(code, 2)
        with self.env('claude', self.lead), patch.dict(os.environ, {'LILJACK_TASKS': str(self.root/'tasks.bmd')}):
            code, result = self.invoke(['room', *self.args, '--to', room, '--phase', 'PLAN'])
            self.assertEqual(code, 0, result)
            self.assertEqual(result['phase'], 'PLAN')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.project = self.root / 'demo'
        self.store = Workspace(self.root / 'workspace')
        self.lead = self.observe('lead', 'claude')
        self.worker = self.observe('worker', 'deepseek')
        self.store.assign(self.lead, 'lead', actor='operator')
        self.store.assign(self.worker, 'worker', self.lead, actor='operator')
        self.args = ['--root', str(self.store.root), '--project', str(self.project)]

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def observe(self, name, agent):
        return self.store.observe({'harness': 'test', 'session': name, 'agent': agent,
                                   'cwd': str(self.project), 'state': 'running'})['id']

    def env(self, agent='claude', sid=None):
        result = {'LILJACK_AGENT': agent}
        if sid is not None:
            result['LILJACK_WORKSPACE_SESSION'] = sid
        return patch.dict(os.environ, result, clear=True)

    def invoke(self, args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = cli.main(args)
        return code, json.loads(stdout.getvalue() or stderr.getvalue())

    def test_identity_checks_and_shell_mapping(self):
        with self.env(sid=self.lead):
            self.assertEqual(cli.caller(self.store, self.project), self.lead)
        for agent, sid in [('codex', self.lead), ('claude', 'missing'), ('unknown', None)]:
            with self.env(agent, sid), self.assertRaises(PermissionError):
                cli.caller(self.store, self.project)
        with self.env('operator'):
            self.assertEqual(cli.caller(self.store, self.project), 'operator')
        with patch.dict(os.environ, {}, clear=True), self.assertRaises(PermissionError):
            cli.caller(self.store, self.project)
        shell = self.observe('shell', 'shell')
        with self.env('operator', shell):
            self.assertEqual(cli.caller(self.store, self.project), shell)
        with self.env(sid=self.lead), self.assertRaises(PermissionError):
            cli.caller(self.store, self.root / 'Other')

    def test_denied_spawn_never_constructs_tmux(self):
        with self.env('deepseek', self.worker), patch.object(cli, 'Tmux') as tmux:
            code, result = self.invoke(['session', 'spawn', *self.args])
            self.assertEqual(code, 2)
            tmux.assert_not_called()
        with self.env(sid=self.lead), patch.object(cli, 'Tmux') as tmux:
            code, result = self.invoke(['session', 'spawn', *self.args, '--role', 'lead'])
            self.assertEqual(code, 2)
            tmux.assert_not_called()

    def test_lead_spawn_reviewer_assigned_before_binding(self):
        native = 'socket/lj-child'
        sid = session_key('tmux', native)
        row = {'id': sid, 'harness': 'tmux', 'session': native, 'agent': 'deepseek',
               'cwd': str(self.project), 'state': 'running', 'tmux_name': 'lj-child', 'pane': '%4'}
        tmux = MagicMock(socket='socket')
        tmux.create.return_value = sid
        tmux.sessions.return_value = [row]
        with self.env(sid=self.lead), patch.object(cli, 'Tmux', return_value=tmux):
            code, result = self.invoke(['session', 'spawn', *self.args, '--role', 'reviewer', '--agent', 'deepseek'])
        self.assertEqual(code, 0, result)
        self.assertEqual(result['role'], 'reviewer')
        self.assertEqual(result['parent'], self.lead)
        child = next(s for s in self.store.snapshot(self.project)['sessions'] if s['id'] == sid)
        self.assertEqual(child['role'], 'reviewer')
        self.assertIn('terminal', child['capabilities'])
        self.assertTrue(self.store.may_control(self.lead, sid))

    def test_send_has_no_implicit_submit_and_rejects_newlines(self):
        row = {'id': self.worker, 'state': 'running', 'pane': '%4', 'tmux_name': 'lj-child'}
        tmux = MagicMock()
        tmux.sessions.return_value = [row]
        with self.env(sid=self.lead), patch.object(cli, 'Tmux', return_value=tmux):
            code, result = self.invoke(['session', 'send', *self.args, '--session', self.worker, '--text', 'Привіт 日本語'])
        self.assertEqual(code, 0, result)
        tmux.call.assert_called_once_with('send-keys', '-t', '%4', '-l', '--', 'Привіт 日本語')
        with self.env(sid=self.lead), patch.object(cli, 'Tmux') as factory:
            code, result = self.invoke(['session', 'send', *self.args, '--session', self.worker, '--text', 'bad\ncommand'])
            self.assertEqual(code, 2)
            factory.assert_not_called()

    def test_room_uses_isolated_mailbox_and_private_visibility(self):
        with self.env(sid=self.lead), patch.object(liljack_mail, 'CACHE', self.root / 'mail'):
            code, result = self.invoke(['room', *self.args, '--text', 'Привіт 日本語', '--request-id', 'test'])
        self.assertEqual(code, 0, result)
        self.assertEqual(result['bridge']['exported'], 1)
        self.store.post(self.project, 'operator', 'private', 'private', destination=self.worker)
        with self.env(sid=self.lead), patch.object(liljack_mail, 'CACHE', self.root / 'mail'):
            code, result = self.invoke(['room', *self.args])
        self.assertEqual(code, 0, result)
        self.assertEqual([m['text'] for m in result['messages']], ['Привіт 日本語'])
        with self.env('unknown'), patch.object(cli, 'sync_workspace') as sync:
            code, result = self.invoke(['room', *self.args])
            self.assertEqual(code, 2)
            sync.assert_not_called()

    def test_room_stays_usable_when_mailbox_is_unavailable(self):
        with self.env(sid=self.lead), patch.object(cli, 'sync_workspace', side_effect=OSError('unavailable')):
            code, result = self.invoke(['room', *self.args, '--text', 'local message', '--request-id', 'offline'])
        self.assertEqual(code, 0, result)
        self.assertEqual(result['text'], 'local message')
        self.assertTrue(result['bridge']['incomplete'])
        self.assertTrue(result['bridge']['issues'])

    def test_room_text_file_and_stdin_deliver_backticks_verbatim(self):
        # 2026-09-12: backticks / $( ) inside a double-quoted --text were substituted
        # by the shell (a `git stash` ran in the demo tree). The body must arrive
        # byte-identical through the file and stdin transports.
        body = 'run `git stash` and echo $(pwd) — verbatim\nsecond line'
        src = self.root / 'body.txt'
        src.write_text(body, encoding='utf-8')
        with self.env(sid=self.lead), patch.object(liljack_mail, 'CACHE', self.root / 'mail'):
            code, result = self.invoke(['room', *self.args, '--text-file', str(src), '--request-id', 'tf'])
        self.assertEqual(code, 0, result)
        self.assertEqual(result['text'], body)
        with self.env(sid=self.lead), patch('sys.stdin', io.StringIO(body)), \
                patch.object(liljack_mail, 'CACHE', self.root / 'mail'):
            code, result = self.invoke(['room', *self.args, '--text', '-', '--request-id', 'si'])
        self.assertEqual(code, 0, result)
        self.assertEqual(result['text'], body)
        # both bodies round-trip out of the store unchanged
        with self.env(sid=self.lead), patch.object(liljack_mail, 'CACHE', self.root / 'mail'):
            code, result = self.invoke(['room', *self.args])
        self.assertEqual(code, 0, result)
        self.assertEqual([m['text'] for m in result['messages']].count(body), 2)

    def test_import_has_no_renderer_dependencies(self):
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[1] / 'toolbox'))
        result = subprocess.run([sys.executable, '-c',
            'import sys; import liljack_app.cli; assert not any(n in sys.modules for n in ("PIL", "liljack_app.desktop", "liljack_app.surface"))'],
            capture_output=True, text=True, env=env, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
