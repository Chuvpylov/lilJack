"""Room delivery uses exact lifecycle mappings and durable acknowledgement."""
import json
import fcntl
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace
from liljack_app.room_delivery import pump, read_message, native_state, supersede_stale
import liljack_codex


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = str(self.root / 'project')
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.lead = self.w.observe({'harness':'tmux','session':'socket/lead','agent':'codex','cwd':self.project})['id']
        self.peer = self.w.observe({'harness':'tmux','session':'socket/peer','agent':'claude','cwd':self.project})['id']
        self.room = self.w.room_create(self.project, 'Work', actor='operator')['id']
        for sid in (self.lead, self.peer):
            self.w.room_move(self.project, sid, self.room, actor='operator')
        self.w.assign(self.lead, 'lead', actor='operator')
        self.live = [{'id':self.lead,'agent':'codex','state':'running'}]
        self.lookup = Mock(return_value={'native':'exact-thread','event':'Stop','age':3,'at':'9999-01-01T00:00:00+00:00'})
        self.send = Mock(return_value=subprocess.CompletedProcess([],0))

    def post(self, key='one', sender='operator', destination=None):
        return self.w.post(self.project, sender, 'Please continue', key, destination=destination or self.room)

    def tick(self):
        pump(self.w,self.project,self.live,lookup=self.lookup,send=self.send)

    def delivery(self, message):
        return dict(self.w.db.execute('SELECT * FROM room_delivery WHERE message_id=?',(message['id'],)).fetchone())

    def test_user_and_teammate_queue_but_lead_and_dm_do_not_wake(self):
        m=self.post();self.post(sender=self.peer);self.post(sender=self.lead);self.post('dm',destination=self.peer)
        self.assertEqual(self.w.db.execute('SELECT COUNT(*) FROM room_delivery').fetchone()[0],2)
        self.tick();self.tick();self.post()
        self.send.assert_called_once()
        self.assertEqual(self.send.call_args.args[0],'exact-thread')
        self.assertIn(m['id'],self.send.call_args.args[1])
        self.assertEqual(self.delivery(m)['state'],'submitted')

    def test_busy_blocked_ended_missing_hook_and_missing_terminal_wait(self):
        m=self.post()
        for event in ('PreToolUse','PermissionRequest','SessionEnd','SubagentStop','Interrupt'):
            self.lookup.return_value={'native':'exact','event':event,'age':10}
            self.tick();self.assertEqual(self.delivery(m)['state'],'pending')
        self.lookup.return_value=None;self.tick()
        self.assertIn('exact-session',self.delivery(m)['reason'])
        self.live=[];self.tick();self.assertIn('disconnected',self.delivery(m)['reason'])
        self.send.assert_not_called()

    def test_missing_or_multiple_leads_wait_and_claude_lead_uses_own_harness(self):
        m=self.post();self.w.assign(self.lead,'worker',actor='operator');self.tick()
        self.assertIn('exactly one',self.delivery(m)['reason'])
        self.w.assign(self.lead,'lead',actor='operator');self.w.assign(self.peer,'lead',actor='operator');self.tick()
        self.send.assert_not_called()
        self.w.assign(self.lead,'worker',actor='operator');self.live=[{'id':self.peer,'agent':'claude','state':'running'}];self.tick()
        self.lookup.assert_called_with(self.peer,self.project,'claude')
        self.assertEqual(self.delivery(m)['state'],'submitted')

    def test_teammate_notice_delivers_and_new_lead_does_not_wake_itself(self):
        m=self.post(sender=self.peer);self.tick()
        self.assertEqual(self.delivery(m)['state'],'submitted')
        self.send.assert_called_once()
        read_message(self.w,self.project,self.lead,m['id'])
        own=self.post('later',sender=self.peer)
        self.w.assign(self.lead,'worker',actor='operator')
        self.w.assign(self.peer,'lead',actor='operator')
        self.live=[{'id':self.peer,'agent':'claude','state':'running'}]
        self.tick()
        self.assertEqual(self.delivery(own)['state'],'skipped')
        self.send.assert_called_once()

    def test_frozen_audience_prevents_new_member_receiving_old_request(self):
        self.w.room_move(self.project,self.lead,'',actor='operator')
        m=self.post();self.w.room_move(self.project,self.lead,self.room,actor='operator');self.tick()
        self.send.assert_not_called();self.assertEqual(self.delivery(m)['state'],'pending')

    def test_read_is_authorized_and_next_message_waits_for_ack_and_new_stop(self):
        first=self.post();second=self.post('two');self.tick();self.tick()
        self.send.assert_called_once()
        read_message(self.w,self.project,self.peer,first['id'])
        self.assertEqual(self.delivery(first)['state'],'submitted')
        read_message(self.w,self.project,self.lead,first['id'])
        self.assertEqual(self.delivery(first)['state'],'read')
        self.lookup.return_value['at']='2000-01-01T00:00:00+00:00'
        self.tick();self.send.assert_called_once()
        self.lookup.return_value['at']='9999-01-01T00:00:00+00:00';self.tick()
        self.assertEqual(self.send.call_count,2)
        self.assertIn(second['id'],self.send.call_args.args[1])
        other=self.w.observe({'harness':'tmux','session':'other','agent':'codex','cwd':self.project})['id']
        with self.assertRaises(PermissionError):read_message(self.w,self.project,other,first['id'])

    def test_timeout_is_uncertain_and_not_retried_after_reopen(self):
        m=self.post();self.send.side_effect=subprocess.TimeoutExpired('codex',10)
        self.tick();self.assertEqual(self.delivery(m)['state'],'uncertain')
        reopened=Workspace(self.w.root)
        try:pump(reopened,self.project,self.live,lookup=self.lookup,send=self.send)
        finally:reopened.close()
        self.send.assert_called_once()

    def test_pending_read_before_dispatch_clears_only_explicit_message(self):
        first=self.post();second=self.post('two')
        read_message(self.w,self.project,self.peer,first['id'])
        self.assertEqual(self.delivery(first)['state'],'pending')
        result=read_message(self.w,self.project,self.lead,first['id'])
        self.assertEqual(result['delivery']['state'],'read')
        self.assertEqual(self.delivery(first)['target'],self.lead)
        self.assertEqual(self.delivery(second)['state'],'pending')
        self.tick()
        self.send.assert_called_once()
        self.assertIn(second['id'],self.send.call_args.args[1])

    def test_pending_pinned_read_works_while_paused(self):
        m=self.post()
        self.lookup.return_value=None;self.tick()
        self.assertEqual(self.delivery(m)['target'],self.lead)
        (self.w.root/'room-delivery.stop').touch()
        read_message(self.w,self.project,self.lead,m['id'])
        self.assertEqual(self.delivery(m)['state'],'read')
        self.tick();self.send.assert_not_called()

    def test_expiry_uses_message_age_preserves_history_and_inflight(self):
        old=self.post();fresh=self.post('fresh');inflight=self.post('inflight')
        for m in (old,inflight):
            self.w.db.execute("UPDATE messages SET created_at='2026-01-01T00:00:00+00:00' WHERE id=?",(m['id'],))
        self.w.db.execute("UPDATE messages SET created_at='2026-01-01T00:15:00+00:00' WHERE id=?",(fresh['id'],))
        self.w.db.execute("UPDATE room_delivery SET state='uncertain' WHERE message_id=?",(inflight['id'],))
        self.assertEqual(supersede_stale(self.w,self.project,at='2026-01-01T00:20:00+00:00'),1)
        self.assertEqual(self.delivery(old)['state'],'superseded')
        self.assertEqual(self.delivery(fresh)['state'],'pending')
        self.assertEqual(self.delivery(inflight)['state'],'uncertain')
        self.assertEqual(read_message(self.w,self.project,self.lead,old['id'])['text'],'Please continue')
        self.assertEqual(supersede_stale(self.w,self.project,at='2026-01-01T00:20:00+00:00'),0)

    def test_expiry_runs_while_paused_without_waking(self):
        old=self.post()
        self.w.db.execute("UPDATE messages SET created_at='2000-01-01T00:00:00+00:00' WHERE id=?",(old['id'],))
        (self.w.root/'room-delivery.stop').touch()
        self.tick()
        self.assertEqual(self.delivery(old)['state'],'superseded')
        self.send.assert_not_called()

    def test_read_by_sole_lead_acknowledges_but_never_transfers(self):
        # Decision 2026-09-13 (lead, per the operator "finish with your decisions"): READ COUNTS.
        # A read by the current, sole lead acknowledges an unclaimed pending notice, so the
        # heartbeat stops re-paging a lead for what it already read. A notice pinned to
        # somebody else is never transferred by a read, and a second lead cannot claim it.
        m=self.post()
        self.lookup.return_value=None;self.tick()
        self.w.assign(self.lead,'worker',actor='operator')
        self.w.assign(self.peer,'lead',actor='operator')
        read_message(self.w,self.project,self.peer,m['id'])
        d=self.delivery(m);self.assertEqual(d['state'],'read');self.assertEqual(d['target'],self.peer)
        self.w.assign(self.lead,'lead',actor='operator')          # takes over: the workspace keeps exactly one lead
        read_message(self.w,self.project,self.lead,m['id'])
        d=self.delivery(m);self.assertEqual(d['state'],'read');self.assertEqual(d['target'],self.peer)   # pinned: not transferred
        n=self.post('two')
        self.lookup.return_value=None;self.tick()
        read_message(self.w,self.project,self.peer,n['id'])     # a former lead's read claims nothing
        self.assertEqual(self.delivery(n)['state'],'pending')
        read_message(self.w,self.project,self.lead,n['id'])     # the sole lead's read acknowledges
        d=self.delivery(n);self.assertEqual(d['state'],'read');self.assertEqual(d['target'],self.lead)

    def test_concurrent_window_skips_dispatch_and_crash_is_reported(self):
        m=self.post()
        with (self.w.root/'room-delivery.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.tick();self.send.assert_not_called()
        self.w.db.execute("UPDATE room_delivery SET state='sending',updated_at='2000-01-01T00:00:00+00:00' WHERE message_id=?",(m['id'],))
        self.tick();self.assertEqual(self.delivery(m)['state'],'uncertain');self.send.assert_not_called()

    def test_lifecycle_changes_before_send_and_workspace_stop_file_prevent_wake(self):
        m=self.post()
        idle=dict(self.lookup.return_value)
        self.lookup.side_effect=[idle,{**idle,'event':'UserPromptSubmit'}]
        self.tick();self.send.assert_not_called()
        self.assertEqual(self.delivery(m)['state'],'pending')
        self.assertIn('changed',self.delivery(m)['reason'])
        self.lookup.side_effect=None
        (self.w.root/'room-delivery.stop').touch()
        self.tick();self.send.assert_not_called()

    def test_message_only_cursor_skips_heartbeats_without_losing_messages(self):
        first=self.post()
        before=self.w.events(self.project,viewer=self.lead,messages_only=True)
        self.assertEqual([e['data']['id'] for e in before['events']],[first['id']])
        old=self.w.db.execute("SELECT COUNT(*) FROM events WHERE kind='session'").fetchone()[0]
        for stamp in ('2026-01-01','2026-01-02'):
            self.w.observe({'harness':'tmux','session':'socket/lead','agent':'codex','cwd':self.project,'at':stamp})
        self.assertEqual(self.w.db.execute("SELECT COUNT(*) FROM events WHERE kind='session'").fetchone()[0],old)
        second=self.post('two')
        after=self.w.events(self.project,before['cursor'],viewer=self.lead,messages_only=True)
        self.assertEqual([e['data']['id'] for e in after['events']],[second['id']])

    def test_native_lookup_uses_exact_hook_mapping_and_latest_event(self):
        state=self.root/'codex'
        with patch.dict(os.environ,{'LILJACK_WORKSPACE_SESSION':self.lead}):
            liljack_codex.record_event({'hook_event_name':'Stop','session_id':'native-A','cwd':self.project},root=state)
        mapped=native_state(self.lead,self.project,state/'lifecycle.sqlite3')
        self.assertEqual(mapped['native'],'native-A')
        self.assertIsNone(native_state(self.peer,self.project,state/'lifecycle.sqlite3'))
        with patch.dict(os.environ,{'LILJACK_WORKSPACE_SESSION':self.peer}):
            liljack_codex.record_event({'hook_event_name':'Stop','session_id':'native-B','cwd':self.project},root=state)
        with patch.dict(os.environ,{'LILJACK_WORKSPACE_SESSION':self.lead}):
            liljack_codex.record_event({'hook_event_name':'SubagentStop','session_id':'native-A','agent_id':'child','cwd':self.project},root=state)
        self.assertEqual(native_state(self.lead,self.project,state/'lifecycle.sqlite3')['event'],'SubagentStop')


if __name__=='__main__':unittest.main()
