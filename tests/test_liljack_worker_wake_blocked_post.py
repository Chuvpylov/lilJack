"""Lead posts reach exact blocked owners via the existing worker heartbeat."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'toolbox'), str(ROOT / 'liljack_app')]
from liljack_workspace import Workspace
import worker_wake as W


class BlockedPostTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.w = Workspace(self.root / 'workspace'); self.addCleanup(self.w.close)
        self.project = str(self.root / 'project')
        self.room = self.w.room_create(self.project, 'A', actor='operator')['id']
        self.other = self.w.room_create(self.project, 'B', actor='operator')['id']
        self.people = {}
        for name, room, role in [('lead', self.room, 'lead'), ('blocked', self.room, 'worker'),
                                 ('active', self.room, 'worker'), ('foreign', self.other, 'worker'),
                                 ('second', self.room, 'worker')]:
            sid = self.w.observe({'harness':'tmux', 'session':'sock/'+name,
                                  'agent':'claude' if name=='lead' else 'codex', 'cwd':self.project})['id']
            self.w.room_move(self.project, sid, room, actor='operator')
            self.w.assign(sid, role, actor='operator'); self.people[name] = sid
        self.tasks = [self.task('blocked'), self.task('active', state='active'), self.task('foreign')]
        self.live = [{'id':s, 'state':'running'} for s in self.people.values()]
        self.lookup = Mock(return_value={'state':'idle','event':'Stop','age':10,
                                        'native':'thread','at':'2026-09-17T00:00:00Z'})
        rp = types.ModuleType('review_panel'); rp.room_scope=lambda: ''; rp.todos=lambda **kw: []
        ll = types.ModuleType('lead_lifecycle'); ll.lookup=self.lookup
        lt = types.ModuleType('liljack_tasks'); lt.PENDING={'assigned','active','proposed'}
        lt.tasks=lambda **kw: self.tasks
        self.addCleanup(patch.stopall)
        patch.dict(sys.modules, {'review_panel':rp,'lead_lifecycle':ll,'liljack_tasks':lt}).start()
        patch.object(W,'STATE',self.root/'wake.json').start()
        patch.object(W,'pick',return_value=None).start()
        self.send=patch.object(W,'nudge',return_value={'submitted':True,'sent':True,'reason':''}).start()
        self.clock=patch.object(W.time,'time',return_value=2_000_000_000).start()
        self.tmux=Mock(); self.tmux.sessions.side_effect=lambda p:self.live

    def task(self, name, state='blocked'):
        return {'id':'task-'+name, 'owner':'codex', 'owner_session':self.people[name],
                'room':self.other if name=='foreign' else self.room, 'state':state,
                'updated':'2026-01-01T00:00:00Z', 'note':'waiting for lead'}

    def post(self, key='post', sender='lead', destination=None):
        return self.w.post(self.project,self.people[sender],'Ready for review',key,
                           destination=destination or self.room)

    def tick(self, dry=False):
        return W.tick(self.project,root=self.w.root,dry_run=dry,tmux=self.tmux)

    def test_lead_post_wakes_only_exact_blocked_owner_once(self):
        m=self.post(); self.tick(); self.tick()
        self.send.assert_called_once()
        choice=self.send.call_args.args[1]
        self.assertEqual(choice['session'],self.people['blocked'])
        self.assertIn(m['id'],self.send.call_args.args[2])
        self.assertEqual(self.tasks[0]['state'],'blocked')

    def test_each_blocked_member_once_not_each_task(self):
        self.tasks += [self.task('second'),dict(self.task('blocked'),id='another')]
        self.post(); self.tick(); self.tick(); self.tick()
        self.assertEqual({c.args[1]['session'] for c in self.send.call_args_list},
                         {self.people['blocked'],self.people['second']})
        self.assertEqual(self.send.call_count,2)

    def test_cooldown_preserves_next_post(self):
        self.post();self.tick();self.post('two');self.tick()
        self.assertEqual(self.send.call_count,1)
        self.clock.return_value += W.COOLDOWN_S+1
        self.tick();self.tick();self.assertEqual(self.send.call_count,2)

    def test_busy_then_idle(self):
        self.post();self.lookup.return_value['state']='busy';self.tick()
        self.send.assert_not_called()
        self.lookup.return_value['state']='idle';self.tick();self.send.assert_called_once()

    def test_dry_run_does_not_consume_post(self):
        self.post();self.tick(True);self.send.assert_not_called()
        self.assertFalse(W.STATE.exists())
        self.tick();self.send.assert_called_once()

    def test_move_to_other_room_does_not_follow(self):
        self.post();self.w.room_move(self.project,self.people['blocked'],self.other,actor='operator')
        self.tick();self.send.assert_not_called()

    def test_peer_post_does_not_wake(self):
        self.post(sender='active');self.tick();self.send.assert_not_called()

    def test_private_lead_post_does_not_wake(self):
        self.post(destination=self.people['blocked']);self.tick();self.send.assert_not_called()

    def test_missing_exact_owner_does_not_fallback_to_agent(self):
        self.tasks[0]['owner_session']='';self.post();self.tick();self.send.assert_not_called()

    def test_missing_terminal_waits(self):
        self.live=[r for r in self.live if r['id']!=self.people['blocked']]
        self.post();self.tick();self.send.assert_not_called()

    def test_old_post_before_block_is_not_replayed(self):
        self.post();self.tasks[0]['updated']='9999-01-01T00:00:00Z'
        self.tick();self.send.assert_not_called()

    def test_unknown_lifecycle_waits(self):
        self.post();self.lookup.return_value=None;self.tick();self.send.assert_not_called()

    def test_uncertain_send_is_not_replayed(self):
        self.send.side_effect=RuntimeError('unknown submission outcome')
        self.post();self.tick();self.clock.return_value+=W.COOLDOWN_S+1;self.tick()
        self.send.assert_called_once()

    def test_newer_post_after_uncertain_send_waits_cooldown_then_notifies(self):
        first=self.post()
        self.send.side_effect=[RuntimeError('unknown submission outcome'),
                               {'submitted':True,'sent':True,'reason':''}]
        self.tick()
        second=self.post('later')
        self.tick();self.send.assert_called_once()
        self.clock.return_value+=W.COOLDOWN_S+1
        self.tick();self.tick()
        self.assertEqual(self.send.call_count,2)
        self.assertIn(first['id'],self.send.call_args_list[0].args[2])
        self.assertIn(second['id'],self.send.call_args_list[1].args[2])

    def test_restart_deduplicates(self):
        self.post();self.tick()
        saved=json.loads(W.STATE.read_text());W.reset_room_cache()
        self.clock.return_value+=W.COOLDOWN_S+1;self.tick()
        self.send.assert_called_once();self.assertEqual(json.loads(W.STATE.read_text()),saved)

    def test_multiple_leads_wait(self):
        self.post(); self.w.assign(self.people['active'],'lead',actor='operator')
        self.tick(); self.send.assert_not_called()

    def test_frozen_audience_required(self):
        m=self.post()
        self.w.db.execute('DELETE FROM message_recipients WHERE message_id=? AND recipient=?',
                          (m['id'],self.people['blocked']))
        self.tick();self.send.assert_not_called()

    def test_lifecycle_changes_before_send(self):
        self.post();self.lookup.side_effect=[self.lookup.return_value,{'state':'busy'}]
        self.tick();self.send.assert_not_called()

    def test_persistence_failure_prevents_send(self):
        self.post()
        with patch.object(Path,'replace',side_effect=OSError('write unavailable')):
            self.tick()
        self.send.assert_not_called()

    def test_dispatch_lock_prevents_concurrent_send(self):
        import fcntl
        self.post()
        with (self.w.root/'room-delivery.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.tick();self.send.assert_not_called()
        self.tick();self.send.assert_called_once()

    def test_no_post_no_wake(self):
        self.tick();self.send.assert_not_called()

    def test_transport_refusal_does_not_replay(self):
        self.send.return_value={'submitted':False,'sent':False}
        self.post();self.tick();self.clock.return_value+=W.COOLDOWN_S+1;self.tick()
        self.send.assert_called_once()

    def test_notification_stop_file_respected(self):
        self.post();(self.w.root/'room-delivery.stop').touch()
        self.tick();self.send.assert_not_called()

    def test_corrupt_state_does_not_replay(self):
        self.post();W.STATE.write_text('{broken')
        self.tick();self.send.assert_not_called()


if __name__=='__main__': unittest.main()
