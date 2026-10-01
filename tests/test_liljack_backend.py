"""Native UI adapter contract: isolated files, mocked terminal/lifecycle sources."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import Future
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_app.backend import Backend, gather_project_review
from liljack_workspace import Workspace, session_key
import liljack_mail


class BackendTests(unittest.TestCase):
    def test_session_unavailable_uses_heartbeat_state_by_exact_session(self):
        from liljack_app import worker_wake
        with patch.object(worker_wake, 'STATE', self.root/'wake.json'):
            started = self.app.request(self.start_request(todo=[]))
            sid = started['started_sessions'][0]['session_id']
            worker_wake.mark_unavailable(sid, 'No live terminal', now=1234567)
            rows = self.app.snapshot(passive=True)['sessions']
            for row in rows:
                self.assertEqual(row['unavailable'],
                    {'reason': 'No live terminal', 'at': 1234567} if row['id'] == sid else None)
            worker_wake.clear_unavailable(sid)
            self.assertTrue(all(row['unavailable'] is None
                                for row in self.app.snapshot(passive=True)['sessions']))

    def test_open_task_fallback_includes_unscoped_work_and_reports_errors(self):
        import liljack_tasks as tasks
        standalone = tasks.propose('Unscoped work', by='operator', owner='codex')
        blocked = tasks.propose('Waiting work', by='operator', owner='codex')
        tasks.block(blocked['id'], 'codex', 'Waiting for review')
        done = tasks.propose('Finished work', by='operator', owner='codex')
        tasks.complete(done['id'], 'codex')
        result = self.app.snapshot(passive=True)
        self.assertEqual(result['tasks_error'], '')
        rows = {t['id']: t for t in result['tasks']}
        self.assertEqual(set(rows), {standalone['id'], blocked['id']})
        self.assertEqual(rows[standalone['id']]['room'], '')
        self.assertEqual(rows[blocked['id']]['blocked_reason'], 'Waiting for review')
        with patch.object(tasks, 'read', side_effect=OSError('registry unreadable')):
            result = self.app.snapshot(passive=True)
        self.assertEqual(result['tasks'], [])
        self.assertIn('registry unreadable', result['tasks_error'])
        with patch.object(tasks, 'read', return_value={'errors': 1, 'tasks': []}):
            result = self.app.snapshot(passive=True)
        self.assertIn('parse errors', result['tasks_error'])

    def test_protocol_operator_actions_do_not_pump_terminal_input(self):
        from liljack_app import room_protocol as protocol
        import liljack_tasks as tasks
        started = self.app.request(self.start_request(todo=[]))
        room = started['created_room']
        roster = self.app.store.room_roster(self.project, room)
        worker = next(s['id'] for s in roster if s['role'] == 'worker')
        for member in roster:
            protocol.align(self.app.store, self.project, room, member['id'], 'Understand the assigned room task.')
        with patch('liljack_app.room_delivery.pump', side_effect=AssertionError('protocol pumped terminal')), \
             patch('liljack_room_bridge.sync_workspace', side_effect=AssertionError('protocol bridged mail')):
            self.app.request({'action': 'room_phase', 'room': room, 'phase': 'PLAN'})
            response = self.app.request({'action': 'room_plan', 'room': room, 'request_id': 'plan',
                                        'todo': [{'title': 'Verify operator path', 'owner_session': worker}]})
            task = response['room_protocol_result']['todo'][0]
            self.app.request({'action': 'room_phase', 'room': room, 'phase': 'WORK'})
            question = protocol.ask(self.app.store, self.project, room, task['id'], worker,
                                    'Which test?', 'question')
            response = self.app.request({'action': 'room_answer', 'question': question['id'],
                                        'answer': 'The operator integration test.'})
        self.assertEqual(response['room_protocol_result']['answered_by'], 'operator')
        self.assertEqual(tasks.get(task['id'])['state'], 'assigned')
        self.assertEqual(response['room_questions'], [])
        self.tmux.call.assert_not_called()

    def test_room_actions_keep_attachment_metadata_without_delivery(self):
        started = self.app.request(self.start_request(todo=[]))
        room = started['created_room']
        with patch('liljack_app.room_delivery.pump', side_effect=AssertionError('room action pumped input')), \
             patch('liljack_room_bridge.sync_workspace', side_effect=AssertionError('room action bridged mail')):
            result = self.app.request({'action': 'room_state', 'room': room, 'state': 'backlog'})
            self.assertEqual(result['changed_room']['state'], 'backlog')
            self.assertEqual(result['root'], str(self.app.store.root))
            self.assertTrue(all(s.get('pane') and s.get('tmux_name') for s in result['sessions']))
            self.assertTrue(result['bridge']['skipped'])
            result = self.app.request({'action': 'room_view', 'room': room})
            self.assertEqual(result['room_action'], {'room': room, 'view': 'view'})
        self.tmux.call.assert_not_called()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / 'demo'
        self.project.mkdir()
        env = patch.dict(os.environ, {'LILJACK_TASKS': str(self.root / 'tasks.bmd')})
        env.start()
        self.addCleanup(env.stop)
        self.rows = []
        self.tmux = MagicMock(socket='test-socket', binary='/test/tmux')
        self.ribbon = MagicMock(age_s=float('inf'), error=None)
        self.ribbon.fields.return_value = [{'text': 'status: starting…', 'kind': 'other', 'stale': True}]
        self.tmux.sessions.side_effect = lambda _: list(self.rows)
        self.tmux.create.side_effect = self.create
        for target, kwargs in [
            ('liljack_app.backend.Tmux', {'return_value': self.tmux}),
            ('liljack_app.backend.Ribbon', {'return_value': self.ribbon}),
            ('liljack_app.backend.gather_review', {'return_value': {'summary': {'done': 1, 'total': 2}, 'todos': [], 'git': {'rows': []}, 'fit': {'scores': {}}}}),
            ('liljack_workspace.Workspace.refresh', {'return_value': {}}),
            ('liljack_mail.CACHE', {'new': self.root / 'mail'})]:
            patcher = patch(target, **kwargs)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.app = Backend(self.root / 'workspace', self.project)
        self.addCleanup(self.app.close)

    def test_ribbon_snapshot_is_cached_and_json_safe(self):
        value = self.app.snapshot()['status_ribbon']
        self.assertIsNone(value['age_s'])
        self.assertTrue(value['fields'][0]['stale'])
        json.dumps(value, allow_nan=False)
        self.ribbon.refresh.assert_not_called()
        self.ribbon.age_s = 2.5
        self.ribbon.fields.return_value = [{'text': '☑277/438', 'kind': 'todo', 'stale': False}]
        value = self.app.snapshot()['status_ribbon']
        self.assertEqual(value['age_s'], 2.5)
        self.assertEqual(value['fields'][0]['kind'], 'todo')

    def start_request(self, **overrides):
        return {'action': 'room_start', 'request_id': 'start-1', 'name': 'Team',
                'purpose': 'Review the room workflow', 'folder': str(self.project),
                'agents': [{'agent': 'codex', 'role': 'worker'},
                           {'agent': 'claude', 'role': 'lead'},
                           {'agent': 'codex', 'role': 'reviewer'}],
                'todo': [{'title': 'Worker private task', 'assignee': 0},
                         {'title': 'Lead task'}, {'title': 'Reviewer private task', 'assignee': 2}], **overrides}

    def test_start_reserves_team_before_launch_and_is_durable_idempotent(self):
        from liljack_app.room_start import room_context
        import liljack_tasks as tasks
        folder = self.root / 'different working folder'
        folder.mkdir()
        req = self.start_request(folder=str(folder))
        def launch(*args, **kwargs):
            snapshot = self.app.store.snapshot(self.project)
            self.assertEqual(len(snapshot['sessions']), 3)
            members = snapshot['sessions']
            lead = next(s['id'] for s in members if s['role'] == 'lead')
            self.assertEqual(len({s['room_id'] for s in members}), 1)
            self.assertTrue(all(s['lead_id'] == lead for s in members if s['role'] != 'lead'))
            self.assertEqual(len(tasks.tasks()), 3)
            self.assertEqual(kwargs['cwd'], str(folder))
            return self.create(*args, **kwargs)
        self.tmux.create.side_effect = launch
        first = self.app.request(req)
        self.assertEqual(first['start_status'], 'complete')
        self.assertEqual(first['start_errors'], first['errors'])
        self.assertEqual([c.args[0] for c in self.tmux.create.call_args_list], ['claude', 'codex', 'codex'])
        self.assertEqual(len(first['task_ids']), 3)
        ids = [s['session_id'] for s in first['started_sessions']]
        self.assertEqual(len(set(ids)), 3)
        views = [room_context(self.app.store, self.project, first['created_room'], sid) for sid in ids]
        self.assertEqual([len(v['todo']) for v in views], [1, 3, 1])
        self.assertEqual(views[0]['todo'][0]['title'], 'Worker private task')
        room_context(self.app.store, self.project, first['created_room'], ids[0], acknowledge=True)
        before = self.tmux.create.call_count
        retry = self.app.request(req)
        self.assertEqual(retry['task_ids'], first['task_ids'])
        self.assertEqual(retry['created_room'], first['created_room'])
        self.assertEqual(retry['started_sessions'][0]['intro_state'], 'acknowledged')
        self.assertEqual(self.tmux.create.call_count, before)
        reopened = Workspace(self.app.store.root)
        try:
            room = reopened.snapshot(self.project)['rooms'][0]
            self.assertEqual(room['start_status'], 'complete')
            self.assertEqual(room['folder'], str(folder))
            self.assertEqual(room['task_ids'], first['task_ids'])
        finally:
            reopened.close()
        with self.assertRaisesRegex(ValueError, 'different room start'):
            self.app.request({**req, 'name': 'Changed'})

    def test_start_preflight_rejects_entire_request_without_side_effects(self):
        from liljack_app.runtime import Tmux
        # Use real validation for the directory while mocking executable lookup.
        self.tmux.preflight.side_effect = lambda agent, cwd, backend='tmux': Tmux.preflight(self.tmux, agent, cwd, backend=backend)
        variants = [dict(agents=[]), dict(agents=[{'agent':'codex','role':'worker'}]),
                    dict(agents=[{'agent':'codex','role':'lead'}]*2),
                    dict(agents=[{'agent':'unknown','role':'lead'}]),
                    dict(folder=str(self.root / 'missing')),
                    dict(todo=[{'title':'bad', 'assignee': True}]),
                    dict(todo=[{'title':'bad', 'assignee': 9}]), dict(purpose='two\nlines')]
        for change in variants:
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.app.request(self.start_request(**change))
            self.assertEqual(self.app.store.snapshot(self.project)['rooms'], [])
        self.tmux.create.assert_not_called()
        self.assertFalse((self.root / 'tasks.bmd').exists())

    def test_cli_start_and_context_use_project_identity_and_acknowledge(self):
        import contextlib
        import io
        from liljack_app import cli
        spec = self.root / 'room.json'
        spec.write_text(json.dumps(self.start_request()))
        env = {'LILJACK_AGENT':'operator', 'LILJACK_WORKSPACE_PROJECT':str(self.project),
               'LILJACK_WORKSPACE_ROOT':str(self.app.store.root),
               'LILJACK_TASKS':str(self.root / 'tasks.bmd')}
        with patch.dict(os.environ, env, clear=True), patch.object(cli,'Tmux',return_value=self.tmux):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(cli.room_cli(['--start',str(spec)]), 0)
            result = json.loads(output.getvalue())
        self.assertEqual(result['start_status'], 'complete')
        worker = result['started_sessions'][0]['session_id']
        env.update(LILJACK_AGENT='codex', LILJACK_WORKSPACE_SESSION=worker)
        with patch.dict(os.environ, env, clear=True):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(cli.room_cli(['--context']), 0)
            context = json.loads(output.getvalue())
        self.assertEqual(context['role'], 'worker')
        self.assertEqual(len(context['todo']), 1)
        self.assertEqual(context['started_sessions'][0]['intro_state'], 'acknowledged')
        with patch.dict(os.environ, env, clear=True), self.assertRaises(PermissionError):
            cli.room_cli(['--start',str(spec)])


    def test_start_partial_failure_retains_success_and_does_not_relaunch(self):
        def launch(*args, **kwargs):
            if self.rows:
                raise RuntimeError('injected launch uncertainty')
            return self.create(*args, **kwargs)
        self.tmux.create.side_effect = launch
        req = self.start_request()
        result = self.app.request(req)
        self.assertEqual(result['start_status'], 'partial')
        self.assertEqual(result['start_errors'], result['errors'])
        self.assertIn('injected launch uncertainty', result['start_errors'][0]['message'])
        self.assertEqual([s['state'] for s in result['started_sessions']], ['uncertain','running','not_started'])
        self.assertEqual(len(result['task_ids']), 3)
        self.assertEqual(result['errors'][0]['stage'], 'launch')
        room = self.app.request({'action':'refresh'})['rooms'][0]
        self.assertEqual(room['start_status'], 'partial')
        self.assertEqual(room['start_errors'], result['errors'])
        self.app.request(req)
        self.assertEqual(self.tmux.create.call_count, 2)
        self.tmux.call.assert_not_called()

    def test_start_interrupted_coordinator_is_not_replayed(self):
        def crash(*args, **kwargs):
            self.create(*args, **kwargs)
            raise KeyboardInterrupt()
        self.tmux.create.side_effect = crash
        req = self.start_request()
        with self.assertRaises(KeyboardInterrupt):
            self.app.request(req)
        result = self.app.request(req)
        self.assertEqual(result['start_status'], 'partial')
        self.assertEqual(result['errors'][0]['stage'], 'recovery')
        self.assertEqual(result['started_sessions'][1]['state'], 'uncertain')
        self.assertEqual(self.tmux.create.call_count, 1)

    def test_start_intros_isolate_other_rooms_and_duplicate_provider_tasks(self):
        import liljack_tasks as tasks
        other = self.app.store.room_create(self.project, 'Other room', purpose='Other purpose', actor='operator')['id']
        self.app.store.post(self.project, 'operator', 'NEVER_COPY_MESSAGE', 'other-message', destination=other)
        tasks.seed_room_tasks(str(self.project), other, [{'title':'NEVER_COPY_TODO', 'owner':'codex'}], 'operator', 'other')
        result = self.app.request(self.start_request())
        prompts = {c.kwargs['name']:c.kwargs['intro'] for c in self.tmux.create.call_args_list}
        for prompt in prompts.values():
            self.assertIn('Other room: Other purpose | open TODOs: 1', prompt)
            self.assertNotIn('NEVER_COPY', prompt)
            self.assertIn('--context', prompt)
        for call in self.tmux.create.call_args_list:
            if call.args[0] == 'codex' and 'role: worker.' in call.kwargs['intro']:
                self.assertIn('Worker private task', call.kwargs['intro'])
                self.assertNotIn('Reviewer private task', call.kwargs['intro'])
        self.assertEqual(result['start_status'], 'complete')

    def test_intro_roster_counts_closed_sessions_instead_of_listing_them(self):
        """⚠ A CLOSED SESSION IS NOT A TEAMMATE YOU CAN TALK TO.

        The roster listed EVERY member the room had ever had, one 40-character
        session id per line. Measured live 2026-09-10: 16 lines of which 10 were
        exited or stopped — noise charged to the OPENING TURN of every agent
        spawned into the room, and four consecutive codex sessions each spent
        that turn on onboarding without reaching the work.

        Live members stay named in full, because you address them by id. Closed
        ones are counted, because the only fact that outlives them is that they
        existed.
        """
        from liljack_app.room_start import intro_text
        store = self.app.store
        room = store.room_create(self.project, 'Roster', actor='operator')['id']
        # one live teammate, three closed ones across two agents
        live = store.observe({'harness': 'test', 'session': 'live-1', 'agent': 'claude',
                              'cwd': str(self.project), 'state': 'running'})['id']
        store.room_move(self.project, live, room, actor='operator')
        closed = []
        for n, agent in (('dead-1', 'codex'), ('dead-2', 'codex'), ('dead-3', 'deepseek')):
            sid = store.observe({'harness': 'test', 'session': n, 'agent': agent,
                                 'cwd': str(self.project), 'state': 'exited'})['id']
            store.room_move(self.project, sid, room, actor='operator')
            closed.append(sid)

        text = intro_text(store, str(self.project), room, live)
        for sid in closed:
            self.assertNotIn(sid, text, 'a closed session must not take a roster line')
        self.assertIn(live, text, 'a live teammate is still named in full')
        self.assertIn('closed session(s) not listed', text,
                      'the closed ones are COUNTED, not silently dropped')
        # ⚠ counted per agent, so "who has been through here" survives
        self.assertIn('codex x2', text)
        self.assertIn('deepseek', text)

    def test_task_seed_failure_is_visible_before_any_launch(self):
        import liljack_tasks as tasks
        seed = tasks.seed_room_tasks
        calls = []
        def broken(*args, **kwargs):
            calls.append(args[4])
            if len(calls) == 2:
                raise OSError('registry unavailable')
            return seed(*args, **kwargs)
        with patch.object(tasks, 'seed_room_tasks', side_effect=broken):
            result = self.app.request(self.start_request())
        self.assertEqual(result['start_status'], 'partial')
        self.assertEqual(result['errors'][0]['stage'], 'tasks')
        self.assertEqual(len(result['task_ids']), 1)
        self.assertEqual(len(set(calls)), 2)
        self.tmux.create.assert_not_called()

    def test_create_requires_room_lead_and_rejects_second_lead_before_spawn(self):
        room = self.app.store.room_create(self.project, 'Roles', actor='operator')['id']
        with self.assertRaisesRegex(ValueError, 'exactly one lead'):
            self.app.request({'action':'create', 'agent':'codex', 'room':room})
        self.tmux.create.assert_not_called()
        lead = self.app.request({'action':'create','agent':'claude','room':room,'role':'lead'})['created']
        with self.assertRaisesRegex(ValueError, 'already has a lead'):
            self.app.request({'action':'create','agent':'codex','room':room,'role':'lead'})
        worker = self.app.request({'action':'create','agent':'codex','room':room})['created']
        team = self.app.store.db.execute('SELECT role,parent_id FROM team WHERE session_id=?',(worker,)).fetchone()
        self.assertEqual(tuple(team), ('worker',lead))
        self.assertEqual(self.tmux.create.call_count, 2)

    def test_spawn_backend_flag_threads_to_preflight_and_create(self):
        from liljack_app.room_start import normalize
        self.assertEqual(normalize(self.project, self.start_request())['backend'], 'tmux')
        self.assertEqual(normalize(self.project, self.start_request(backend='native'))['backend'], 'native')
        with self.assertRaises(ValueError):
            normalize(self.project, self.start_request(backend='bogus'))
        sid = self.app.request({'action': 'create', 'agent': 'shell', 'backend': 'native'})['created']
        (call,) = self.tmux.create.call_args_list
        self.assertEqual(call.kwargs.get('backend'), 'native')
        self.assertEqual(self.tmux.preflight.call_args.kwargs.get('backend'), 'native')

    def test_review_worker_preserves_assigned_dag_lanes(self):
        graph={'rows':[{'id':'merge','lane':1,'open':[0,1],'merge':True}],
               'lanes':2,'truncated':False,'error':None}
        with patch('liljack_app.backend.gather_review',return_value={'git_dag':graph}) as gather:
            value=gather_project_review(self.project)
        gather.assert_called_once_with(repo=self.project)
        self.assertEqual(value['git_dag'],graph)

    def test_review_snapshot_never_waits_and_preserves_last_good_on_failure(self):
        executor = MagicMock()
        future = Future()
        executor.submit.return_value = future
        with patch('liljack_app.backend.ThreadPoolExecutor', return_value=executor):
            first = self.app.review_snapshot()
        self.assertTrue(first['loading'])
        self.assertIsNone(first['age_s'])
        for _ in range(100):
            self.assertTrue(self.app.review_snapshot()['loading'])
        executor.submit.assert_called_once()
        future.set_result({'summary': {'done': 3, 'total': 4}, 'todos': [],
                           'git': {'rows': []}, 'fit': {'score': float('nan')}})
        value = self.app.review_snapshot()
        self.assertEqual(value['summary']['done'], 3)
        self.assertFalse(value['loading'])
        self.assertIsNone(value['fit']['score'])
        json.dumps(value, allow_nan=False)
        failed = Future()
        failed.set_exception(RuntimeError('collector unavailable'))
        self.app.review_future = failed
        value = self.app.review_snapshot()
        self.assertEqual(value['summary']['done'], 3)
        self.assertEqual(value['error'], 'collector unavailable')
        executor.submit.assert_called_once()

    def create(self, agent, project, root, *, cwd=None, intro=None, name=None, backend='tmux'):
        number = len(self.rows)
        name = name or f'lj-{number}'
        native = f'test-socket/{name}' if backend == 'tmux' else name
        sid = session_key(backend, native)
        self.rows.append({'id': sid, 'harness': backend, 'backend': backend, 'session': native,
                          'agent': agent, 'cwd': str(cwd or self.project),
                          'project': str(project), 'state': 'running',
                          'tmux_name': name, 'pane': f'%{number}' if backend == 'tmux' else ''})
        return sid

    def test_room_log_pages_full_text_without_dms_or_receipts(self):
        room=self.app.store.room_create(self.project,'Log',actor='operator')['id']
        other=self.app.store.room_create(self.project,'Other',actor='operator')['id']
        first=self.app.store.post(self.project,'operator','日本語\n'+'x'*5000,'log1',destination=room)
        second=self.app.store.post(self.project,'operator','second','log2',destination=room)
        self.app.store.post(self.project,'operator','not this room','log3',destination=other)
        with patch.object(self.app,'snapshot',return_value={}):
            value=self.app.request({'action':'room_log','room':room,'limit':1})['room_log']
            self.assertEqual([m['id'] for m in value['messages']],[second['id']])
            self.assertTrue(value['has_more'])
            older=self.app.request({'action':'room_log','room':room,'before_seq':value['next_before'],'limit':1})['room_log']
        self.assertEqual(older['messages'][0]['text'],first['text'])
        self.assertFalse(older['has_more'])
        self.assertEqual(self.app.store.db.execute('SELECT state FROM room_delivery WHERE message_id=?',(first['id'],)).fetchone()[0],'pending')
        with self.assertRaises(ValueError):
            self.app.request({'action':'room_log','room':'s-private'})

    def test_room_lead_handoff_is_dynamic_and_atomic(self):
        room = self.app.request({'action': 'room_create', 'name': 'Review'})['created_room']
        ids = [self.app.request({'action': 'create', 'agent': agent, 'room': room,
                                'role': 'lead' if i == 0 else 'worker'})['created']
               for i, agent in enumerate(('codex', 'claude', 'deepseek'))]
        for chosen in (ids[0], ids[1], ids[2], ids[0]):
            result = self.app.request({'action': 'room_lead', 'room': room, 'session': chosen})
            rows = {r['id']: r for r in result['sessions']}
            self.assertEqual([r['id'] for r in rows.values() if r['role'] == 'lead'], [chosen])
            for sid in ids:
                self.assertEqual(rows[sid]['lead_id'], None if sid == chosen else chosen)
        before = list(self.app.store.db.execute('SELECT * FROM team'))
        with self.assertRaises(PermissionError):
            self.app.store.room_lead(self.project, room, ids[1], actor=ids[0])
        outsider = self.app.request({'action': 'create', 'agent': 'claude'})['created']
        self.app.store.assign(outsider, 'worker', ids[0], actor='operator')
        with self.assertRaises(ValueError):
            self.app.request({'action': 'room_lead', 'room': room, 'session': ids[1]})
        self.assertEqual(list(self.app.store.db.execute('SELECT * FROM team WHERE session_id != ?', (outsider,))), before)
        with self.assertRaises(ValueError):
            self.app.request({'action': 'room_lead', 'room': room, 'session': outsider})

    def test_operator_controls_exact_live_terminal(self):
        ids = [self.app.request({'action': 'create', 'agent': 'claude'})['created'] for _ in range(2)]
        for operation, key in [('submit', 'Enter'), ('interrupt', 'C-c')]:
            self.app.request({'action': 'control', 'session': ids[1], 'operation': operation})
            self.tmux.call.assert_called_with('send-keys', '-t', '%1', key)
        self.tmux.call.reset_mock()
        with self.assertRaises(ValueError):
            self.app.request({'action': 'control', 'session': ids[1], 'operation': 'stop'})
        self.tmux.call.assert_not_called()
        self.app.request({'action': 'control', 'session': ids[1], 'operation': 'stop', 'confirmed': True})
        self.tmux.call.assert_called_with('kill-session', '-t', self.rows[1]['tmux_name'])
        self.tmux.call.reset_mock()
        self.rows[1]['state'] = 'exited'
        with self.assertRaises(ValueError):
            self.app.request({'action': 'control', 'session': ids[1], 'operation': 'submit'})
        self.tmux.call.assert_not_called()

    def test_refresh_retains_identity_roles_and_binding(self):
        sid = self.create('claude', self.project, self.app.store.root)
        observed = self.app.store.observe(self.rows[0])
        self.app.store.assign(sid, 'lead', actor='operator')
        snapshot = self.app.request({'action': 'refresh'})
        self.assertEqual(len(snapshot['sessions']), 1)
        row = snapshot['sessions'][0]
        self.assertEqual(row['id'], observed['id'])
        self.assertEqual(row['role'], 'lead')
        self.assertEqual(row['terminal']['pane'], '%0')
        self.assertEqual(row['pane'], '%0')
        self.assertIn('terminal', row['capabilities'])
        self.assertEqual(snapshot['root'], str(self.app.store.root))
        self.assertEqual(self.app.request({'action': 'refresh'})['sessions'][0]['id'], sid)
        self.tmux.call.assert_not_called()

    def test_create_into_selected_named_room(self):
        room = self.app.request({'action': 'room_create', 'name': 'Development'})['created_room']
        result = self.app.request({'action': 'create', 'agent': 'claude', 'room': room, 'role': 'lead'})
        row = next(s for s in result['sessions'] if s['id'] == result['created'])
        self.assertEqual(row['room_id'], room)
        self.assertIn('terminal', row['capabilities'])

    def test_repeated_providers_create_distinct_terminals(self):
        ids = []
        for provider in ['claude', 'claude', 'deepseek', 'deepseek', 'deepseek', 'deepseek']:
            result = self.app.request({'action': 'create', 'agent': provider})
            ids.append(result['created'])
        self.assertEqual(len(set(ids)), 6)
        self.assertEqual(len(result['sessions']), 6)
        self.assertEqual(len({r['terminal']['pane'] for r in result['sessions']}), 6)
        self.assertEqual(sum(r['agent'] == 'claude' for r in result['sessions']), 2)
        self.assertEqual(sum(r['agent'] == 'deepseek' for r in result['sessions']), 4)
        self.tmux.call.assert_not_called()

    def test_posts_keep_unicode_ids_privacy_and_mail_delivery(self):
        recipient = self.create('claude', self.project, self.app.store.root)
        other = self.create('codex', self.project, self.app.store.root)
        self.app.request({'action': 'refresh'})
        req = {'action': 'post', 'text': '  Привіт 日本語 🌱  ', 'language': 'uk',
               'destination': recipient, 'request_id': 'private-one'}
        first = self.app.request(req)
        second = self.app.request(req)
        self.assertEqual(first['posted'], second['posted'])
        row = next(m for m in second['messages'] if m['id'] == first['posted'])
        self.assertEqual(row['destination'], recipient)
        self.assertEqual(row['text'], req['text'])
        self.assertEqual(row['language'], 'uk')
        self.assertEqual(self.app.store.snapshot(self.project, viewer=other)['messages'], [])
        mailbox = liljack_mail._read(self.root / 'mail' / 'mailbox.jsonl')
        self.assertEqual(mailbox, [])
        self.assertEqual(first['bridge']['exported'], 0)
        liljack_mail._append(self.root / 'mail' / 'mailbox.jsonl', {
            'id': 'fixture-reply', 'project': 'demo', 'frm': 'deepseek',
            'to': 'all', 'text': '確認しました — перевірено', 'workspace_language': 'ja'})
        refreshed = self.app.request({'action': 'refresh'})
        imported = next(m for m in refreshed['messages'] if m['text'].startswith('確認'))
        self.assertEqual(imported['destination'], 'room')
        self.assertEqual(imported['language'], 'ja')
        self.assertIn(first['posted'], {m['id'] for m in refreshed['messages']})
        self.tmux.call.assert_not_called()

    def test_stage_cleans_instruction_without_terminal_side_effects(self):
        agent = self.create('claude', self.project, self.app.store.root)
        shell = self.create('shell', self.project, self.app.store.root)
        exited = self.create('deepseek', self.project, self.app.store.root)
        self.rows[-1]['state'] = 'exited'
        self.app.request({'action': 'refresh'})
        for bad_target in [shell, exited, 'missing']:
            with self.assertRaisesRegex(ValueError, 'live agent terminal'):
                self.app.request({'action': 'stage', 'destination': bad_target, 'text': 'not posted'})
        self.assertEqual(self.app.store.snapshot(self.project)['messages'], [])
        result = self.app.request({'action': 'stage', 'destination': agent,
                                   'text': 'Привіт\x1b[201~\x03\u202e日本語', 'request_id': 'stage-one'})
        self.assertEqual(result['target'], agent)
        for forbidden in ['\x1b', '\x03', '\u202e']:
            self.assertNotIn(forbidden, result['stage'])
        self.assertIn(result['posted'], result['stage'])
        self.assertIn('YOUR_REPLY', result['stage'])
        self.assertIn('日本語', result['stage'])
        self.tmux.call.assert_not_called()
        self.tmux.create.assert_not_called()

    def test_disappeared_terminal_revoked_immediately_and_role_survives_reconnect(self):
        sid = self.create('claude', self.project, self.app.store.root)
        self.app.request({'action': 'refresh'})
        self.app.store.assign(sid, 'lead', actor='operator')
        saved = self.rows.pop()
        vanished = self.app.request({'action': 'refresh'})['sessions'][0]
        self.assertNotIn('terminal', vanished['capabilities'])
        self.assertEqual(vanished['state'], 'stopped')
        self.assertEqual(vanished['role'], 'lead')
        self.rows.append(saved)
        fresh = self.app.request({'action': 'refresh'})['sessions'][0]
        self.assertEqual(fresh['id'], sid)
        self.assertEqual(fresh['role'], 'lead')
        self.assertIn('terminal', fresh['capabilities'])
        reopened = Workspace(self.app.store.root)
        try:
            persisted = reopened.snapshot(self.project)['sessions'][0]
            self.assertEqual(persisted['id'], sid)
            self.assertEqual(persisted['role'], 'lead')
        finally:
            reopened.close()

    def test_mailbox_failure_does_not_hide_local_post_or_terminals(self):
        sid = self.create('claude', self.project, self.app.store.root)
        with patch('liljack_room_bridge.sync_workspace', side_effect=OSError('fixture mailbox unavailable')):
            result = self.app.request({'action': 'post', 'text': 'kept locally', 'request_id': 'offline'})
        self.assertEqual(result['sessions'][0]['id'], sid)
        self.assertEqual(result['messages'][0]['id'], result['posted'])
        self.assertTrue(result['bridge']['incomplete'])
        self.assertTrue(result['bridge']['issues'])

    def test_files_list_action_returns_packet_with_room_root_marked(self):
        folder = self.root / 'browse-folder'
        folder.mkdir()
        (folder / 'a.txt').write_text('a')
        (folder / 'sub').mkdir()
        room = self.app.store.room_create(self.project, 'Browse', folder=str(folder),
                                          actor='operator')['id']
        with patch.object(self.app, 'snapshot', return_value={}):
            ok = self.app.request({'action': 'files_list', 'room': room, 'path': ''})
        self.assertEqual(ok['files']['error'], '')
        self.assertEqual(ok['files']['root'], os.path.realpath(str(folder)))
        self.assertTrue(ok['files']['is_room_root'])
        self.assertEqual(ok['files']['room_root'], os.path.realpath(str(folder)))
        self.assertEqual([e['name'] for e in ok['files']['entries']], ['sub', 'a.txt'])
        # unconfined: browsing up past the room root works, and is marked as such
        with patch.object(self.app, 'snapshot', return_value={}):
            up = self.app.request({'action': 'files_list', 'room': room, 'path': '..'})
        self.assertEqual(up['files']['error'], '')
        self.assertEqual(up['files']['root'], os.path.realpath(str(self.root)))
        self.assertFalse(up['files']['is_room_root'])

    def test_team_assignment_cannot_cross_backend_project(self):
        foreign = self.app.store.observe({'harness': 'tmux', 'session': 'foreign/session',
                                           'agent': 'claude', 'cwd': str(self.root / 'Other')})['id']
        with self.assertRaisesRegex(ValueError, 'does not belong'):
            self.app.request({'action': 'assign', 'session': foreign, 'role': 'lead'})
        row = self.app.store.snapshot(self.root / 'Other')['sessions'][0]
        self.assertEqual(row['role'], 'worker')

    def test_private_stdio_protocol_recovers_after_bad_requests(self):
        # Actual main() and stdin/stdout pipes; discovery/lifecycle adapters are
        # replaced inside the child so no live source is even read.
        script = '''from unittest.mock import patch
from types import SimpleNamespace
from liljack_app.backend import main
fake = SimpleNamespace(binary='', socket='fixture', sessions=lambda project: [])
with patch('liljack_app.backend.Tmux', return_value=fake), patch('liljack_workspace.Workspace.refresh', return_value={}):
    main()
'''
        env = {**os.environ, 'LILJACK_CACHE': str(self.root / 'stdio-mail'),
               'LILJACK_WORKSPACE_ROOT': str(self.root / 'stdio-workspace'),
               'PYTHONPATH': str(Path(__file__).resolve().parents[1] / 'toolbox')}
        payload = '\n'.join(['not json', '[]', json.dumps({'action': 'unknown'}),
                              'x' * 65537, json.dumps({'action': 'refresh'}),
                              json.dumps({'action': 'post', 'text': 'Привіт 日本語',
                                          'request_id': 'stdio-one'})]) + '\n'
        run = subprocess.run([sys.executable, '-c', script, '--stdio',
                              str(self.root / 'stdio-workspace'), str(self.project)],
                             input=payload, text=True, capture_output=True, env=env, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        output = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertEqual(len(output), 6)
        self.assertEqual([r['ok'] for r in output], [False, False, False, False, True, True])
        self.assertIn('protocol limit', output[3]['error'])
        self.assertEqual(output[4]['data']['sessions'], [])
        self.assertEqual(output[5]['data']['messages'][0]['text'], 'Привіт 日本語')
        self.assertEqual(output[5]['data']['bridge']['exported'], 1)
        self.assertEqual(run.stderr, '')
        mail = liljack_mail._read(self.root / 'stdio-mail' / 'mailbox.jsonl')
        self.assertEqual(len(mail), 1)
        self.assertEqual(mail[0]['text'], 'Привіт 日本語')


if __name__ == '__main__':
    unittest.main()
