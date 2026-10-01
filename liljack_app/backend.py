"""Private, foreground JSON-lines adapter for the C UI's existing lilJack stores.

The C parent owns stdin/stdout pipes. This is not a network service or security
sandbox: local same-UID processes already own these stores. No terminal rendering
or PTY traffic crosses this protocol. Agents use the separately scoped room/session
CLI, never this operator UI adapter.
"""
from __future__ import annotations
import hashlib
import json
import math
import platform
import os
from pathlib import Path
import re
import sys
import uuid
import time
from concurrent.futures import ThreadPoolExecutor

from liljack_workspace import Workspace, project_key, RoomLifecycleError
from .native import PROJECT_ROOT
from .runtime import Tmux, NativeBackend, room_instruction
from .status_ribbon import Ribbon
from .review_panel import snapshot as gather_review


def gather_project_review(project):
    return gather_review(repo=project)


class Backend:
    def __init__(self, root, project):
        self.store = Workspace(root)
        self.project = project_key(project)
        self.tmux = Tmux()
        self.ribbon = Ribbon()
        self.review_executor = None
        self.review_future = None
        self.review_value = None
        self.review_stamp = None
        self.review_next = 0
        self.review_error = None

    def close(self):
        self.ribbon.stop()
        if self.review_executor:
            self.review_executor.shutdown(wait=False, cancel_futures=True)
        self.store.close()

    def review_snapshot(self):
        from liljack_mail import _scrub
        def scrub(value):
            if isinstance(value, str):
                return _scrub(value)
            if isinstance(value, dict):
                return {scrub(k): scrub(v) for k, v in value.items()}
            if isinstance(value, list):
                return [scrub(v) for v in value]
            if isinstance(value, float) and not math.isfinite(value):
                return None
            return value
        now = time.monotonic()
        if self.review_future is not None and self.review_future.done():
            try:
                self.review_value = scrub(self.review_future.result())
                self.review_stamp, self.review_error = now, None
            except Exception as exc:
                self.review_error = _scrub(str(exc)) or type(exc).__name__
            self.review_future = None
            self.review_next = now + 30
        if self.review_future is None and now >= self.review_next:
            if self.review_executor is None:
                self.review_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='app-review')
            self.review_future = self.review_executor.submit(gather_project_review, self.project)
        return {**(self.review_value or {}), 'loading': self.review_future is not None,
                'age_s': None if self.review_stamp is None else now - self.review_stamp,
                'error': self.review_error}

    def ribbon_snapshot(self):
        from liljack_mail import _scrub
        age = self.ribbon.age_s
        return {'fields': [{**field, 'text': _scrub(field['text'])} for field in self.ribbon.fields()],
                'age_s': age if math.isfinite(age) else None,
                'error': _scrub(self.ribbon.error) if self.ribbon.error else None}

    def snapshot(self, *, passive=False):
        issues = self.store.refresh(self.project)
        managed = self.tmux.sessions(self.project)
        from .room_delivery import pump
        if not passive:
            pump(self.store, self.project, managed)
        tmux_rows = [r for r in managed if r.get('backend') != 'native']
        self.store.reconcile_terminals(self.project, self.tmux.socket,
                                       (row['id'] for row in tmux_rows), actor='operator')
        for row in managed:
            self.store.observe(row)
            if row.get('backend') == 'native':
                # Native sessions bind against the sessiond socket, and their
                # pane slot carries the supervisor's daemon id (never empty, so
                # the binding validates like a tmux binding does).
                self.store.bind_terminal(row['id'], NativeBackend().sock, row['tmux_name'],
                                         row.get('daemon_id') or row['tmux_name'], actor='operator')
            else:
                self.store.bind_terminal(row['id'], self.tmux.socket, row['tmux_name'], row['pane'], actor='operator')
        try:
            from liljack_room_bridge import sync_workspace
            bridge = ({'skipped': True, 'reason': 'Room action does not deliver messages'}
                      if passive else sync_workspace(self.store, self.project))
        except (ImportError, OSError, ValueError) as exc:
            from liljack_mail import _scrub
            bridge = {'incomplete': True, 'issues': [{'reason': _scrub(str(exc))[:300]}]}
        result = self.store.snapshot(self.project, viewer='operator')
        from .room_protocol import status as protocol_status, todo_snapshot, open_task_snapshot
        for room in result['rooms']:
            room.update(protocol_status(self.store, self.project, room['id']))
            room.update(todo_snapshot(self.store, self.project, room['id']))
        result['room_questions'] = [q for room in result['rooms'] for q in room['questions']]
        # ⚠ DMs TO THE OPERATOR ARE ACTIONABLE WORK, LIKE QUESTIONS. the operator 2026-09-12
        # 07:01: "add DMs in the Actionable room tile tab so agents can ask me
        # questions". Mail carries a room since mailbox-session-scoping, so each
        # room shows only its own; untagged legacy rows are shown everywhere
        # rather than hidden. `unread_dms` is the tab badge count.
        for room in result['rooms']:
            room['dms'] = self.room_dms(room['id'])
            room['unread_dms'] = sum(1 for d in room['dms'] if not d['read'])
        result['room_dms'] = [d for room in result['rooms'] for d in room['dms']]
        result.update(open_task_snapshot(self.store, self.project))
        bindings = {s['id']: s for s in managed}
        # Bounded visible working set, app-owned sessions first. Durable store keeps all.
        result['sessions'].sort(key=lambda s: (s['id'] in bindings, s.get('observed_at') or ''), reverse=True)
        result['sessions'] = result['sessions'][:64]
        from .worker_wake import unavailable
        from liljack_mail import _scrub
        for row in result['sessions']:
            status = unavailable(row['id'])
            row['unavailable'] = ({'reason': _scrub(status['reason']), 'at': status['at']}
                                  if status else None)
            live = bindings.get(row['id'])
            if live:
                row.update(tmux_name=live['tmux_name'], pane=live['pane'],
                           backend=live.get('backend', 'tmux'),
                           daemon_id=live.get('daemon_id', ''))
        mood = 'ready'
        try:
            match = re.search(r'mood\(liljack,\s*([a-z_]+)\)', (PROJECT_ROOT/'orc_data/liljack.dsl').read_text())
            if match: mood = match.group(1)
        except OSError:
            pass
        result['dashboard'] = self.dashboard(result)
        result.update(tmux_binary=self.tmux.binary or '', socket=self.tmux.socket,
                      sessiond_sock=NativeBackend().sock,
                      root=str(self.store.root), mood=mood, errors=issues, bridge=bridge,
                      status_ribbon=self.ribbon_snapshot(),
                      review_panel=self.review_snapshot(),
                      layout=str(self.store.root/('native-layout-'+hashlib.sha256(self.project.encode()).hexdigest()[:12]+'.txt')))
        return result

    DM_LIMIT = 40

    def dashboard(self, result):
        """The home screen's data, aggregated from what the snapshot already has.

        ⚠ NO NEW POLLING AND NO SECOND SOURCE OF TRUTH. Agents come from the
        session rows, work from the registry rows the snapshot already carries,
        git from the review panel. Anything missing stays None/'' so the C side
        can render an honest dash instead of a zero (checklist G5).
        """
        agents, seen = [], set()
        for row in result.get('sessions', []):
            sid = row.get('id') or ''
            if not sid or sid in seen:
                continue
            seen.add(sid)
            if row.get('state') in ('stopped', 'exited', 'not_started'):
                continue
            agents.append({'session': sid, 'agent': row.get('agent') or '',
                           'role': row.get('role') or 'worker',
                           'state': row.get('state') or 'unknown',
                           'host': row.get('host') or '',
                           'task': row.get('task') or '',
                           'room': row.get('room') or '',
                           'unavailable': (row.get('unavailable') or {}).get('reason')
                                          if row.get('unavailable') else None})
        by_session = {a['session']: a for a in agents}
        work, done, total = [], 0, 0
        for t in result.get('tasks', []) or []:
            state = t.get('state') or ''
            total += 1
            if state == 'done':
                done += 1
            owner_session = t.get('owner_session') or ''
            work.append({'task': t.get('task') or t.get('id') or '',
                         'agent': t.get('agent') or t.get('owner') or '',
                         'session': owner_session,
                         'state': state,
                         'progress': t.get('progress') or '',
                         'room': t.get('room') or '',
                         'known_agent': owner_session in by_session})
        git = (result.get('review_panel') or {}).get('git') or {}
        hosts = sorted({a['host'] for a in agents if a['host']})
        return {'agents': agents, 'work': work,
                'totals': {'agents': len(agents), 'tasks': total, 'done': done,
                           'open': total - done},
                'hosts': hosts,
                'host': platform.node() or '',
                'git': {'branch': git.get('branch') or '',
                        'commits': git.get('commits') if isinstance(git.get('commits'), (int, list)) else None,
                        'rows': len(git.get('rows') or []) if isinstance(git.get('rows'), list) else None},
                'error': (result.get('review_panel') or {}).get('error') or ''}

    def room_dms(self, room, limit=None):
        """Mail addressed to operator that belongs to `room`, oldest first.

        Read state is per agent in the mailbox, so `read` means operator has seen
        it. A row with no room is legacy (pre-scoping) and is shown in every
        room rather than lost."""
        try:
            import liljack_mail as mail
        except ImportError:
            return []
        try:
            # ⚠ session='operator' is the OPERATOR read: the operator is not a room member,
            # so the mailbox must not scope his view to whatever room the calling
            # process happens to sit in. Per-room filtering happens right below.
            rows = mail.inbox('operator', unread_only=False, limit=200, session='operator')
        except (OSError, ValueError):
            return []
        room = (room or '').strip()
        out = []
        for row in rows:
            where = (row.get('room') or '').strip()
            if where and where != room:
                continue
            out.append({'id': row.get('id', ''), 'agent': row.get('frm', ''),
                        'session': row.get('frm_session', ''), 'ts': row.get('ts', ''),
                        'subject': row.get('subject', ''), 'text': row.get('text', ''),
                        'read': bool(row.get('read')), 'room': where})
        return out[-(limit or self.DM_LIMIT):]

    def room_dm_reply(self, room, mail_id, text):
        """Answer one DM as operator, in the asker's own session, and mark it read.

        The reply is session-addressed so it reaches the exact agent session that
        asked, not every session of that provider (mailbox-session-scoping)."""
        import liljack_mail as mail
        text = (text or '').strip()
        if not text:
            raise ValueError('a reply needs text')
        row = next((d for d in self.room_dms(room, limit=200) if d['id'] == mail_id), None)
        if row is None:
            raise ValueError('no such message in this room')
        sent = mail.send(text, row['agent'] or 'all', frm='operator',
                         subject='Re: ' + (row['subject'] or 'your question'),
                         project=self.project, to_session=row['session'], room=room)
        mail.ack([mail_id], 'operator')
        return {'replied_to': mail_id, 'id': sent['id'], 'to': sent['to'],
                'to_session': sent.get('to_session', ''), 'room': sent.get('room', '')}

    def request(self, req):
        action = req.get('action')
        if action == 'room_dm_reply':
            value = self.room_dm_reply(req['room'], req['mail'], req.get('text', ''))
            return {**self.snapshot(passive=True), 'room_dm_result': value}
        if action in ('room_phase', 'room_answer', 'room_plan', 'room_protocol_retry'):
            from . import room_protocol as protocol
            if action == 'room_phase':
                value = protocol.transition(self.store, self.project, req['room'], req['phase'], 'operator')
            elif action == 'room_answer':
                value = protocol.answer(self.store, self.project, req['question'], 'operator', req['answer'])
            elif action == 'room_plan':
                value = protocol.plan(self.store, self.project, req['room'], 'operator', req['todo'], req['request_id'])
            else:
                value = protocol.reconcile(self.store, self.project)
            return {**self.snapshot(passive=True), 'room_protocol_result': value}
        if action in ('room_state', 'room_purge'):
            # Refresh observations without pumping delivery or sending terminal input.
            self.store.refresh(self.project)
            for row in self.tmux.sessions(self.project):
                self.store.observe(row)
            if action == 'room_state':
                result = {'changed_room': self.store.room_state(self.project, req['room'], req['state'],
                          actor='operator', force=req.get('force', False))}
                from .room_instructions import reconcile
                reconcile(self.store, self.project, req['room'])
            else:
                result = self.store.room_purge(self.project, req['room'], actor='operator')
            return {**self.snapshot(passive=True), **result}
        if action == 'room_move_all':
            result = self.store.room_move_all(self.project, req['room'], req['target_room'], actor='operator')
            return {**self.snapshot(passive=True), **result}
        if action in ('room_view', 'room_status', 'room_review', 'room_files', 'room_tools'):
            room = self.store._room(self.project, req['room'])
            view = action.removeprefix('room_')
            packet = {'room_action': {'room': room['id'], 'view': view},
                      'room_roster': self.store.room_roster(self.project, room['id'])}
            if view == 'status':
                packet['status_ribbon'] = self.ribbon_snapshot()
            elif view == 'review':
                packet['review_panel'] = self.review_snapshot()
            elif view == 'files':
                from .file_browser import list_files
                packet['files'] = list_files(self.store, self.project, room=room['id'], path=req.get('path') or '')
            elif view == 'tools':
                packet['room_tools'] = {'room': room['id'], 'folder': room['folder'],
                    'actions': ['room_view', 'room_status', 'room_review', 'room_files', 'room_move_all', 'room_state', 'room_purge']}
            return {**self.snapshot(passive=True), **packet}
        if action == 'room_log':
            room = req['room']
            self.store._room(self.project, room)
            limit = max(1, min(int(req.get('limit', 100)), 100))
            before = int(req.get('before_seq') or 0)
            rows = self.store.db.execute(
                'SELECT * FROM messages WHERE project=? AND destination=? '
                'AND (?=0 OR seq<?) ORDER BY seq DESC LIMIT ?',
                (self.project, room, before, before, limit + 1)).fetchall()
            from liljack_mail import _scrub
            messages = [self.store._message(row) for row in reversed(rows[:limit])]
            for message in messages:
                message['text'] = _scrub(message['text'])
            packet = {'room': room, 'messages': messages, 'has_more': len(rows) > limit,
                      'next_before': messages[0]['seq'] if messages else None,
                      'error': None}
            return {**self.snapshot(), 'room_log': packet}
        if action == 'files_list':
            from .file_browser import list_files
            packet = list_files(self.store, self.project, room=req.get('room') or '',
                                path=req.get('path') or '')
            return {**self.snapshot(), 'files': packet}
        if action == 'refresh':
            return self.snapshot()
        if action == 'room_create':
            room = self.store.room_create(self.project, req['name'], actor='operator',
                                          folder=req.get('folder'), purpose=req.get('purpose', ''))
            return {**self.snapshot(), 'created_room': room['id']}
        if action == 'room_start':
            from .room_start import start_room
            result = start_room(self.store, self.tmux, self.project, req, actor='operator')
            return {**self.snapshot(), **result}
        if action == 'room_update':
            self.store.room_update(self.project, req['room'], req['collapsed'], actor='operator')
            return self.snapshot()
        if action == 'room_move':
            self.store.room_move(self.project, req['session'], req.get('room') or '', actor='operator')
            return self.snapshot()
        if action == 'create':
            from .room_start import create_session
            sid = create_session(self.store, self.tmux, self.project, req['agent'], actor='operator',
                                 room_id=req.get('room') or '', role=req.get('role', 'worker'),
                                 backend=req.get('backend', 'tmux'))
            return {**self.snapshot(), 'created': sid}
        if action == 'room_lead':
            self.store.room_lead(self.project, req['room'], req['session'], actor='operator')
            return self.snapshot()
        if action == 'control':
            operation = req.get('operation')
            if operation not in ('submit', 'interrupt', 'stop'):
                raise ValueError('Unknown terminal control')
            self.store._participant(self.project, req['session'])
            live = next((r for r in self.tmux.sessions(self.project) if r['id'] == req['session'] and r['state'] != 'exited'), None)
            if not live:
                raise ValueError('Target has no live terminal')
            if operation == 'stop':
                if req.get('confirmed') is not True:
                    raise ValueError('Confirm stopping this session')
                if live.get('backend') == 'native':
                    NativeBackend().stop(live.get('daemon_id') or live['tmux_name'])
                else:
                    self.tmux.call('kill-session', '-t', live['tmux_name'])
                self.store.unbind_terminal(req['session'], actor='operator')
            else:
                if live.get('backend') == 'native':
                    NativeBackend().send(live.get('daemon_id') or live['tmux_name'],
                                         '\r' if operation == 'submit' else '\x03')
                else:
                    self.tmux.call('send-keys', '-t', live['pane'], 'Enter' if operation == 'submit' else 'C-c')
            return {**self.snapshot(), 'control_sent': operation, 'target': req['session']}
        if action == 'assign':
            self.store._participant(self.project, req['session'])
            self.store.assign(req['session'], req['role'], req.get('parent') or None, actor='operator')
            return self.snapshot()
        if action in ('post', 'stage'):
            target = req.get('destination') or 'room'
            if action == 'stage':
                live = next((r for r in self.tmux.sessions(self.project) if r['id'] == target), None)
                if not live or live['state'] == 'exited' or live['agent'] == 'shell':
                    raise ValueError('Stage requires a live agent terminal')
            msg = self.store.post(self.project, 'operator', req['text'], req.get('request_id') or uuid.uuid4().hex,
                                  destination=target, language=req.get('language', 'und'), reply_to=req.get('reply_to'))
            result = self.snapshot()
            result['posted'] = msg['id']
            if action == 'stage':
                result.update(stage=room_instruction(msg, self.store.root), target=target)
            return result
        raise ValueError('Unknown UI request')


def main():
    if len(sys.argv) != 4 or sys.argv[1] != '--stdio':
        raise SystemExit('Private adapter requires inherited stdio pipes')
    backend = Backend(sys.argv[2] or None, sys.argv[3])
    try:
        for line in sys.stdin:
            try:
                if len(line) > 65536:
                    raise ValueError('Request exceeds protocol limit')
                req = json.loads(line)
                result = {'ok': True, 'action': req.get('action'), 'data': backend.request(req)}
            except Exception as exc:
                from liljack_mail import _scrub
                result = {'ok': False, 'error': _scrub(str(exc))[:300]}
                if isinstance(exc, RoomLifecycleError):
                    result['error_details'] = exc.details
            print(json.dumps(result, ensure_ascii=False, separators=(',', ':')), flush=True)
    finally:
        backend.close()


if __name__ == '__main__':
    main()
