"""UI-independent delivery and exact-pane wake transports, with no live writes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace, session_key
from liljack_app.lead_transport import wake
from liljack_app.room_delivery import pump
import liljack_heartbeat as heartbeat


class RoomHeartbeatTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = str(self.root/'project')
        self.w = Workspace(self.root/'workspace')
        self.addCleanup(self.w.close)
        self.row = {'harness':'tmux','session':'test/lj-lead','agent':'claude',
                    'cwd':self.project,'state':'running','tmux_name':'lj-lead','pane':'%4',
                    'id':session_key('tmux','test/lj-lead')}
        self.lead = self.w.observe(self.row)['id']
        self.w.assign(self.lead,'lead',actor='operator')
        room = self.w.room_create(self.project,'Review',actor='operator')['id']
        self.w.room_move(self.project,self.lead,room,actor='operator')
        self.message = self.w.post(self.project,'operator','request','one',destination=room)
        self.tmux = Mock(socket='test',binary='/test/tmux')
        self.tmux.sessions.return_value = [self.row]
        self.tmux.call.return_value = subprocess.CompletedProcess([],0)
        self.lookup = Mock(return_value={'native':'claude-thread','event':'Stop','age':4,
                                        'at':'2026-09-09T00:00:00+00:00','state':'idle'})
        self.send = Mock(return_value=subprocess.CompletedProcess([],0))

    def tick(self, **kw):
        return heartbeat.room_tick(self.project,self.w.root,root=self.root,tmux=self.tmux,
                                   lookup=self.lookup,send=self.send,**kw)

    def test_tick_without_ui_delivers_once_and_preserves_ledger(self):
        result=self.tick(dry_run=False)
        self.assertEqual(result['delivery'],{'submitted':1})
        self.tick(dry_run=False)
        self.send.assert_called_once()
        self.lookup.assert_called_with(self.lead,self.project,'claude')
        self.assertIn(self.message['id'],self.send.call_args.args[1])

    def test_dry_run_and_stop_switch_do_not_dispatch(self):
        before=self.w.snapshot(self.project)['cursor']
        self.assertEqual(self.tick()['action'],'room-dry-run')
        self.tmux.sessions.assert_not_called()
        self.assertEqual(before,self.w.snapshot(self.project)['cursor'])
        (self.root/'heartbeat.stop').touch()
        self.assertEqual(self.tick(dry_run=False)['action'],'stopped')
        self.send.assert_not_called()

    def test_failed_discovery_does_not_mark_sessions_stopped(self):
        self.tmux.sessions.side_effect=RuntimeError('discovery failed')
        with self.assertRaises(RuntimeError):self.tick(dry_run=False)
        self.assertEqual(self.w.snapshot(self.project)['sessions'][0]['state'],'running')
        self.send.assert_not_called()

    def test_claude_and_opencode_use_exact_pane_without_codex_queue(self):
        for harness,agent in [('claude','claude'),('opencode','deepseek')]:
            self.row['agent']=agent
            self.tmux.call.reset_mock()
            with patch('liljack_app.lead_transport.subprocess.run') as run:
                wake('thread','Room notice',self.project,harness=harness,target=self.row,tmux=self.tmux)
            run.assert_not_called()
            self.assertEqual(self.tmux.call.call_args_list[0].args,
                             ('send-keys','-t','%4','-l','--','Room notice'))
            self.assertEqual(self.tmux.call.call_args_list[1].args,('send-keys','-t','%4','Enter'))

    def test_replaced_or_dead_pane_never_receives_keys(self):
        for changed in ({'pane':'%9'},{'state':'exited'},{'agent':'shell'}):
            self.tmux.sessions.return_value=[{**self.row,**changed}]
            with self.assertRaises(RuntimeError):
                wake('thread','Room notice',self.project,harness='claude',target=self.row,tmux=self.tmux)
        self.tmux.call.assert_not_called()

    def test_failed_text_write_never_submits(self):
        self.tmux.call.side_effect=subprocess.TimeoutExpired('tmux',5)
        with self.assertRaises(subprocess.TimeoutExpired):
            wake('thread','Room notice',self.project,harness='claude',target=self.row,tmux=self.tmux)
        self.tmux.call.assert_called_once()

    def test_codex_uses_exact_native_queue(self):
        with patch('liljack_app.lead_transport.subprocess.run') as run:
            wake('thread','Room notice',self.project,harness='codex',target=self.row)
        self.assertEqual(run.call_args.args[0],['codex','queue','--thread','thread','--message','Room notice'])

    def test_default_pump_transport_wakes_claude_and_records_acceptance(self):
        with patch('liljack_app.lead_transport.Tmux',return_value=self.tmux):
            pump(self.w,self.project,[self.row],lookup=self.lookup)
        self.assertEqual(self.tmux.call.call_count,2)
        self.assertIn(self.message['id'],self.tmux.call.call_args_list[0].args[-1])
        row=self.w.db.execute('SELECT state,native_session FROM room_delivery').fetchone()
        self.assertEqual(tuple(row),('submitted','claude-thread'))


if __name__=='__main__':unittest.main()
