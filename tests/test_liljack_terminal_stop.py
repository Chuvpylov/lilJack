"""Stopped tmux sessions must not retain running state after losing a binding."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace
from liljack_app import cli


class TerminalStopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name) / 'demo'
        self.w = Workspace(Path(self.tmp.name) / 'workspace')
        self.addCleanup(self.w.close)

    def observe(self, native, harness='tmux', state='running', project=None):
        return self.w.observe({'harness': harness, 'session': native, 'agent': 'codex',
                               'cwd': str(project or self.project), 'state': state})['id']

    def row(self, sid):
        return json.loads(self.w.db.execute('SELECT data FROM sessions WHERE id=?', (sid,)).fetchone()[0])

    def test_unbind_stops_atomically_and_preserves_history(self):
        sid = self.observe('socket/child')
        self.w.assign(sid, 'lead', actor='operator')
        room = self.w.room_create(self.project, 'Review', actor='operator')['id']
        self.w.room_move(self.project, sid, room, actor='operator')
        self.w.post(self.project, sid, 'keep this', 'message', destination=room)
        self.w.bind_terminal(sid, 'socket', 'child', '%1', actor='operator')
        self.w.unbind_terminal(sid, actor='operator')
        snapshot = self.w.snapshot(self.project)
        row = snapshot['sessions'][0]
        self.assertEqual((row['state'], row['event']), ('stopped', 'Detached'))
        self.assertEqual((row['role'], row['room_id']), ('lead', room))
        self.assertNotIn('terminal', row['capabilities'])
        self.assertEqual(snapshot['messages'][0]['text'], 'keep this')
        cursor = snapshot['cursor']
        self.w.unbind_terminal(sid, actor='operator')
        self.assertEqual(self.w.snapshot(self.project)['cursor'], cursor)

    def test_reconcile_recovers_legacy_rows_only_in_scanned_socket_and_project(self):
        dead = self.observe('socket/dead')  # Historical stop already deleted binding.
        live = self.observe('socket/live')
        other = self.observe('socket-other/live')
        foreign_binding = self.observe('socket/rebound')
        self.w.bind_terminal(foreign_binding, 'other', 'rebound', '%2', actor='operator')
        other_project = self.observe('socket/elsewhere', project=Path(self.tmp.name) / 'Other')
        provider = self.observe('socket/provider', harness='codex')
        exited = self.observe('socket/exited', state='exited')
        self.w.reconcile_terminals(self.project, 'socket', [live], actor='operator')
        self.assertEqual(self.row(dead)['state'], 'stopped')
        for sid in (live, other, foreign_binding, other_project, provider):
            self.assertEqual(self.row(sid)['state'], 'running')
        self.assertEqual(self.row(exited)['state'], 'exited')
        cursor = self.w.snapshot(self.project)['cursor']
        self.w.reconcile_terminals(self.project, 'socket', [live], actor='operator')
        self.assertEqual(self.w.snapshot(self.project)['cursor'], cursor)

    def test_unbind_rolls_back_binding_and_state_if_event_write_fails(self):
        sid = self.observe('socket/child')
        self.w.bind_terminal(sid, 'socket', 'child', '%1', actor='operator')
        emit = self.w._emit
        def fail_session(project, kind, data):
            if kind == 'session':
                raise RuntimeError('injected event failure')
            return emit(project, kind, data)
        cursor = self.w.snapshot(self.project)['cursor']
        with patch.object(self.w, '_emit', side_effect=fail_session), self.assertRaises(RuntimeError):
            self.w.unbind_terminal(sid, actor='operator')
        snapshot = self.w.snapshot(self.project)
        self.assertEqual(snapshot['cursor'], cursor)
        self.assertEqual(snapshot['sessions'][0]['state'], 'running')
        self.assertIn('terminal', snapshot['sessions'][0]['capabilities'])

    def test_cli_stop_updates_state_only_after_successful_kill(self):
        sid = self.observe('socket/child')
        self.w.bind_terminal(sid, 'socket', 'child', '%1', actor='operator')
        tmux = MagicMock()
        tmux.sessions.return_value = [{'id': sid, 'state': 'running', 'tmux_name': 'child'}]
        args = ['session', 'stop', '--root', str(self.w.root), '--project', str(self.project), '--session', sid]
        with patch.dict(os.environ, {'LILJACK_AGENT': 'operator'}, clear=True), patch.object(cli, 'Tmux', return_value=tmux):
            tmux.call.side_effect = RuntimeError('kill failed')
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(cli.main(args), 2)
            self.assertEqual(self.row(sid)['state'], 'running')
            tmux.call.side_effect = None
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cli.main(args), 0)
        tmux.call.assert_called_with('kill-session', '-t', 'child')
        self.assertEqual(self.row(sid)['state'], 'stopped')


if __name__ == '__main__':
    unittest.main()
