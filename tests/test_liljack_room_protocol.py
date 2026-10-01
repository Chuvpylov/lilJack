"""Protocol invariants and repair of interrupted cross-store operations."""
import json
from contextlib import closing
import os
# A launched session (LILJACK_WORKSPACE_SESSION) is refused moves on rows bound to
# other sessions; these fixtures act as many sessions, so run them unbound.
os.environ.pop("LILJACK_WORKSPACE_SESSION", None)
from pathlib import Path
import sqlite3
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
import liljack_tasks as tasks
from liljack_workspace import Workspace
from liljack_app import room_protocol as p, room_registry as registry


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name)
        self.task_path = self.project / 'tasks.bmd'
        self.w = Workspace(self.project / 'workspace')
        self.addCleanup(self.w.close)
        rows = [dict(harness='test', session=name, cwd=str(self.project), agent=agent, state='running')
                for name, agent in [('lead','codex'),('worker','deepseek')]]
        payload = dict(name='Protocol', folder=str(self.project), purpose='Review', agents=[dict(agent='codex',role='lead'), dict(agent='deepseek',role='worker')])
        result, _ = self.w.reserve_room_start(self.project,'protocol-start',payload,rows,actor='operator')
        self.room = result['created_room']
        self.lead, self.worker = [r['session_id'] for r in result['started_sessions']]

    def phase(self, phase, actor=None):
        return p.transition(self.w,self.project,self.room,phase,actor or self.lead,task_path=self.task_path)

    def aligned(self):
        for who in (self.lead,self.worker):
            p.align(self.w,self.project,self.room,who,'I understand the assigned review scope.')
        self.phase('PLAN')

    def plan(self, title='Review source', actor=None, request_id='plan1'):
        return p.plan(self.w,self.project,self.room,actor or self.lead,
                      [{'title':title,'owner_session':self.worker}],request_id,task_path=self.task_path)['todo'][0]

    def working(self):
        self.aligned();task=self.plan();self.phase('WORK');return task

    def ask(self, task=None, request_id='ask1'):
        task = task or self.working()
        return p.ask(self.w,self.project,self.room,task['id'],self.worker,'Which acceptance threshold?',request_id,task_path=self.task_path)

    def answer(self, question, actor=None):
        return p.answer(self.w,self.project,question['id'],actor or self.lead,'Use the agreed test threshold.',task_path=self.task_path)

    def test_alignment_gate_and_authority(self):
        self.assertEqual(self.w._room(str(self.project),self.room)['phase'],'ALIGN')
        with self.assertRaisesRegex(ValueError,'waiting for alignment'):self.phase('PLAN')
        with self.assertRaises(PermissionError):self.phase('PLAN',self.worker)
        with self.assertRaises(ValueError):p.align(self.w,self.project,self.room,self.worker,'two\nlines')
        p.align(self.w,self.project,self.room,self.worker,'Understand and wait.')
        p.align(self.w,self.project,self.room,self.worker,'Understand and wait.')
        self.assertEqual(self.w.db.execute('select count(*) from room_alignment').fetchone()[0],1)
        self.assertFalse(p.task_gate(self.project,{'id':'t','room':self.room,'state':'assigned'},self.w.root)['allowed'])
        with self.assertRaisesRegex(ValueError,'waiting for alignment'):self.phase('PLAN')

    def test_all_edges_and_review_return_requires_new_todo(self):
        task=self.working()
        self.assertTrue(p.task_gate(self.project,task,self.w.root)['allowed'])
        with self.assertRaisesRegex(ValueError,'finish all open'):self.phase('REVIEW')
        tasks.complete(task['id'],'deepseek',path=self.task_path)
        self.phase('REVIEW')
        with self.assertRaises(PermissionError):self.phase('RESOLVED')
        with self.assertRaisesRegex(ValueError,'new actionable'):self.phase('WORK','operator')
        task2=self.plan('New review followup','operator','plan2')
        self.phase('WORK','operator')
        tasks.complete(task2['id'],'deepseek',path=self.task_path)
        self.phase('REVIEW');self.phase('RESOLVED','operator')
        self.assertEqual(self.w._room(str(self.project),self.room)['state'],'resolved')
        events=[json.loads(r[0])['phase'] for r in self.w.db.execute("select data from events where kind='room_phase'")]
        self.assertEqual(events,['PLAN','WORK','REVIEW','WORK','REVIEW','RESOLVED'])

    def test_illegal_edges_and_empty_plan(self):
        for phase in ('WORK','REVIEW','RESOLVED'):
            with self.assertRaisesRegex(ValueError,'illegal'):self.phase(phase)
        self.aligned()
        with self.assertRaisesRegex(ValueError,'actionable'):self.phase('WORK')

    def test_phase_event_failure_rolls_back(self):
        self.aligned();self.plan()
        with patch.object(self.w,'_emit',side_effect=RuntimeError('event failure')):
            with self.assertRaises(RuntimeError):self.phase('WORK')
        self.assertEqual(self.w._room(str(self.project),self.room)['phase'],'PLAN')

    def test_plan_scoped_lead_batch_idempotence(self):
        self.aligned()
        with self.assertRaises(PermissionError):self.plan(actor=self.worker)
        first=self.plan();again=self.plan()
        self.assertEqual(first['id'],again['id'])
        self.assertEqual(first['by'],self.lead)
        with self.assertRaisesRegex(ValueError,'different plan'):self.plan('Changed payload')
        with self.assertRaises(PermissionError):
            p.plan(self.w,self.project,self.room,self.lead,[{'title':'bad','owner_session':'foreign'}],'bad',task_path=self.task_path)
        self.assertEqual(len(tasks.tasks(path=self.task_path)),1)

    def test_worker_tick_holds_alignment_until_work(self):
        from liljack_app import worker_wake
        task = tasks.propose('Seeded work waits for alignment', by='operator', owner='deepseek',
                             room=self.room, owner_session=self.worker, path=self.task_path)
        review = types.SimpleNamespace(room_scope=lambda: '', todos=lambda **kw: [
            {'agent': 'deepseek', 'task': task['id'], 'state': 'active',
             'updated': '2026-01-01T00:00:00Z'}])
        lifecycle = types.SimpleNamespace(lookup=lambda *a, **kw: {'state': 'idle'})
        tmux = types.SimpleNamespace(sessions=lambda project: [])
        queues = []
        def capture(project, lookup, open_items, **kwargs):
            queues.append({owner: list(ids) for owner, ids in open_items.items()})
        with patch.dict(os.environ, {'LILJACK_TASKS': str(self.task_path)}), \
             patch.dict(sys.modules, {'review_panel': review, 'lead_lifecycle': lifecycle}), \
             patch.object(worker_wake, 'candidates', return_value=[]), \
             patch.object(worker_wake, 'pick', side_effect=capture):
            worker_wake.tick(self.project, root=self.w.root, dry_run=True, tmux=tmux)
            self.assertNotIn(task['id'], queues[-1].get('deepseek', []))
            self.aligned()
            worker_wake.tick(self.project, root=self.w.root, dry_run=True, tmux=tmux)
            self.assertNotIn(task['id'], queues[-1].get('deepseek', []))
            self.phase('WORK')
            worker_wake.tick(self.project, root=self.w.root, dry_run=True, tmux=tmux)
            self.assertIn(task['id'], queues[-1]['deepseek'])

    def test_question_blocks_and_answer_routes_and_unblocks(self):
        task=self.working();q=self.ask(task)
        self.assertEqual(tasks.get(task['id'],self.task_path)['state'],'blocked')
        self.assertEqual(tasks.pending_tasks(path=self.task_path),[])
        self.assertEqual(p.question_digest(self.w,self.project)[0]['lead'],self.lead)
        self.assertFalse(p.task_gate(self.project,task,self.w.root)['allowed'])
        with self.assertRaises(PermissionError):self.answer(q,self.worker)
        answer=self.answer(q)
        self.assertEqual(answer['status'],'answered')
        self.assertEqual(tasks.get(task['id'],self.task_path)['state'],'assigned')
        self.assertEqual(p.question_digest(self.w,self.project),[])
        self.assertTrue(p.task_gate(self.project,tasks.get(task['id'],self.task_path),self.w.root)['allowed'])
        rows=self.w.db.execute('select destination,text from messages').fetchall()
        self.assertTrue(any(r[0]==self.room and q['id'] in r[1] for r in rows))
        self.assertTrue(any(r[0]==self.worker and 'Use the agreed' in r[1] for r in rows))
        count=len(rows);self.answer(q)
        self.assertEqual(self.w.db.execute('select count(*) from messages').fetchone()[0],count)
        log=tasks.history(task['id'],path=self.task_path)
        self.assertEqual(log[-1]['by'],self.lead)

    def test_duplicate_ask_and_cross_session_refused(self):
        task=self.working();q=self.ask(task)
        self.assertEqual(self.ask(task)['id'],q['id'])
        with self.assertRaises(PermissionError):
            p.ask(self.w,self.project,self.room,task['id'],self.lead,'Not mine','other',task_path=self.task_path)
        with self.assertRaises(ValueError):
            p.ask(self.w,self.project,self.room,task['id'],self.worker,'Changed','ask1',task_path=self.task_path)

    def test_interrupted_block_and_answer_repair(self):
        task=self.working()
        with patch.object(registry,'question_transition',side_effect=OSError('registry unavailable')):
            with self.assertRaises(OSError):self.ask(task)
        q=p.question_digest(self.w,self.project)[0]
        self.assertEqual(q['status'],'opening');self.assertIn('unavailable',q['reason'])
        self.assertFalse(p.task_gate(self.project,task,self.w.root)['allowed'])
        self.assertEqual(p.reconcile(self.w,self.project,task_path=self.task_path)['errors'],[])
        with patch.object(registry,'question_transition',side_effect=OSError('registry unavailable')):
            with self.assertRaises(OSError):self.answer(q)
        self.assertEqual(p.question_digest(self.w,self.project)[0]['status'],'answering')
        self.assertEqual(p.reconcile(self.w,self.project,task_path=self.task_path)['errors'],[])
        self.assertEqual(p.question_digest(self.w,self.project),[])

    def test_unrelated_block_is_not_cleared(self):
        task=self.working();q=self.ask(task)
        tasks.block(task['id'],'deepseek','Waiting for GPU authorization',path=self.task_path)
        with self.assertRaisesRegex(ValueError,'block changed'):self.answer(q)
        self.assertEqual(tasks.get(task['id'],self.task_path)['state'],'blocked')
        self.assertTrue(p.question_digest(self.w,self.project)[0]['reason'])

    def test_strikes_are_deduped_and_escalate_once(self):
        q=self.ask()
        for attempt in ('one','one','two'):
            p.question_strike(self.w,self.project,q['id'],attempt,'Lead did not answer')
        self.assertIsNone(p.question_digest(self.w,self.project)[0]['escalated_at'])
        p.question_strike(self.w,self.project,q['id'],'three','Lead did not answer')
        p.question_strike(self.w,self.project,q['id'],'four','Lead did not answer')
        self.assertEqual(self.w.db.execute("select count(*) from events where kind='room_question_escalated'").fetchone()[0],1)
        self.assertEqual(self.w.db.execute("select count(*) from messages where destination='operator'").fetchone()[0],1)
        self.assertEqual(p.question_digest(self.w,self.project)[0]['strikes'],4)

    def test_question_heartbeat_arm_retire_and_escalation_integration(self):
        import liljack_heartbeat as heartbeat
        from liljack_app import worker_wake
        from liljack_app.room_delivery import read_message
        task = self.working(); question = self.ask(task)
        # A pending room notice is its own delivery work. Read those notices
        # through the real lead acknowledgement path before testing task arming.
        for row in self.w.db.execute('SELECT message_id FROM room_delivery').fetchall():
            read_message(self.w, self.project, self.lead, row[0])
        review = types.SimpleNamespace(open_work=lambda: {'open': 0})
        with patch.dict(os.environ, {'LILJACK_TASKS': str(self.task_path)}), \
             patch.dict(sys.modules, {'review_panel': review}), \
             patch.object(worker_wake, 'STATE', self.project/'wake.json'):
            arm = heartbeat.arm_decision(tasks_file=self.task_path, workspace_root=self.w.root)
            self.assertFalse(arm['arm'], arm)
            self.assertEqual(arm['blocked_tasks'], 1)
            reason = heartbeat._no_work_left({'delivery': {'pending': 0}})
            self.assertIn(task['id'], reason)
            self.assertIsNone(heartbeat._no_work_left({}, qrep={'open_questions': 1}))
            for n in range(3):
                result = worker_wake.question_tick(self.project, self.w.root, dry_run=False,
                    now=1000000+n*(worker_wake.COOLDOWN_S+1))
                self.assertEqual(result['open_questions'], 1)
            stored = p.question_digest(self.w, self.project)[0]
            self.assertEqual(stored['strikes'], 3)
            self.assertTrue(stored['escalated_at'])
            self.assertEqual(self.w.db.execute("SELECT COUNT(*) FROM messages WHERE destination='operator'").fetchone()[0], 1)
            self.answer(question)
            self.assertEqual(p.question_digest(self.w, self.project), [])
            self.assertTrue(heartbeat.arm_decision(tasks_file=self.task_path, workspace_root=self.w.root)['arm'])

    def test_immediate_escalation_and_operator_answer_without_lead(self):
        q=self.ask()
        self.w.assign(self.worker,'worker',actor='operator')
        self.w.assign(self.lead,'worker',actor='operator')
        self.assertIsNone(p.question_digest(self.w,self.project)[0]['lead'])
        p.question_strike(self.w,self.project,q['id'],'dead','Lead unreachable',immediate=True)
        self.assertIsNotNone(p.question_digest(self.w,self.project)[0]['escalated_at'])
        self.assertEqual(self.answer(q,'operator')['status'],'answered')

    def test_notice_failure_retained_and_replayed(self):
        task=self.working()
        with patch.object(self.w,'post',side_effect=OSError('post unavailable')):
            q=self.ask(task)
        self.assertTrue(p.status(self.w,self.project,self.room)['notice_errors'])
        self.assertEqual(p.reconcile(self.w,self.project,task_path=self.task_path)['errors'],[])
        self.assertEqual(p.status(self.w,self.project,self.room)['notice_errors'],[])

    def test_unknown_gate_waits(self):
        result=p.task_gate(self.project,{'id':'task','room':'unknown','state':'assigned'},self.project/'missing')
        self.assertFalse(result['allowed']);self.assertTrue(result['unknown'])

    def test_registry_parse_error_cannot_claim_review_ready(self):
        self.working()
        with patch.object(tasks, 'read', return_value={'errors': 1, 'tasks': []}):
            with self.assertRaisesRegex(ValueError, 'parse errors'):self.phase('REVIEW')
        self.assertEqual(self.w._room(str(self.project), self.room)['phase'], 'WORK')

    def test_todo_adapter_failure_is_visible(self):
        with patch.object(tasks, 'room_tasks', side_effect=OSError('workspace unreadable')):
            result=p.todo_snapshot(self.w,self.project,self.room,task_path=self.task_path)
        self.assertEqual(result['todos'],[])
        self.assertIn('workspace unreadable',result['todos_error'])

    def test_legacy_phase_migration_is_recorded_once(self):
        root=self.project/'legacy';root.mkdir()
        db=sqlite3.connect(root/'workspace.sqlite3')
        db.execute('CREATE TABLE rooms(id TEXT PRIMARY KEY,project TEXT,name TEXT,collapsed INTEGER DEFAULT 0)')
        db.execute('INSERT INTO rooms VALUES(?,?,?,0)',('legacy',str(self.project),'Existing work'))
        db.commit();db.close()
        for _ in range(2):
            with closing(Workspace(root)) as store:
                room=store._room(str(self.project),'legacy')
                self.assertEqual(room['phase'],'WORK')
                self.assertTrue(room['phase_at'])
                self.assertEqual(store.db.execute("SELECT COUNT(*) FROM events WHERE kind='room_phase_migrated'").fetchone()[0],1)

    def test_answer_recovery_does_not_reset_later_completion(self):
        task=self.working();q=self.ask(task)
        original=registry.question_transition
        def interrupted(*args,**kwargs):
            result=original(*args,**kwargs)
            if args[3]=='answer':
                raise OSError('crashed after registry commit')
            return result
        with patch.object(registry,'question_transition',side_effect=interrupted):
            with self.assertRaises(OSError):self.answer(q)
        tasks.complete(task['id'],'deepseek',note='Verified after receiving answer',path=self.task_path)
        self.assertEqual(p.reconcile(self.w,self.project,task_path=self.task_path)['errors'],[])
        self.assertEqual(tasks.get(task['id'],self.task_path)['state'],'done')
        self.assertEqual(p.question_digest(self.w,self.project),[])


if __name__ == '__main__':unittest.main()
