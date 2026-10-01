#!/usr/bin/env python3
"""Durable session/room model shared by lilJack's HUI app and agent clients.

No process launch, terminal input, provider calls or service control here.
The event cursor and snapshot are transactionally consistent. A room post is
deliberate communication, never an instruction automatically injected into PTYs.
All stores can be isolated with --root (tests must use a temporary root).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import math
import os
from pathlib import Path
import sqlite3
import sys
import time
import uuid
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

PROTOCOL = 1
ROOM_STATES = ('active', 'resolved', 'backlog', 'abandoned', 'archived', 'removed')
TRASH_SECONDS = 30 * 86400


class RoomLifecycleError(ValueError):
    def __init__(self, message, **details):
        super().__init__(message)
        self.details = details


def utcnow():
    return datetime.now(timezone.utc).isoformat()


def session_key(harness, native_id):
    """Provider/native IDs cannot collide or introduce path separators."""
    if not harness or not native_id:
        raise ValueError("harness and native session ID are required")
    value = json.dumps([str(harness), str(native_id)], ensure_ascii=False)
    return "s-" + hashlib.sha256(value.encode()).hexdigest()[:32]


def project_key(path):
    return str(Path(path).expanduser().resolve())


def clean(text, limit=8000):
    if not isinstance(text, str) or not text.strip():
        raise ValueError("non-empty text is required")
    if len(text) > limit:
        raise ValueError(f"text exceeds {limit} characters")
    from liljack_mail import _scrub
    return _scrub(text)


def clean_truncate(text, limit, marker="…"):
    """clean()'s truncate-with-marker sibling, for LABELS (a plan title, a mail
    subject): an over-long label is cut with an honest marker instead of refusing
    the whole call (item 4: six subjects and one plan were rejected and resent,
    and the work was lost). clean() stays fail-clear for payloads whose silent
    cut would be data loss. Returns the scrubbed text, always <= limit."""
    if not isinstance(text, str) or not text.strip():
        raise ValueError("non-empty text is required")
    if len(text) > limit:
        text = text[:limit - 1] + marker
    from liljack_mail import _scrub
    return _scrub(text)


class Workspace:
    def __init__(self, root=None):
        self.root = Path(root or os.environ.get(
            "LILJACK_WORKSPACE_ROOT", str(Path.home() / ".cache/liljack/workspace"))).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        path = self.root / "workspace.sqlite3"
        fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
        os.close(fd)
        self.db = sqlite3.connect(path, timeout=5, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS sessions (
                id TEXT PRIMARY KEY, project TEXT NOT NULL, data TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS messages (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
                project TEXT NOT NULL, sender TEXT NOT NULL, destination TEXT NOT NULL,
                text TEXT NOT NULL, language TEXT NOT NULL, reply_to TEXT,
                created_at TEXT NOT NULL, request_id TEXT NOT NULL,
                UNIQUE(project, sender, request_id),
                FOREIGN KEY(reply_to) REFERENCES messages(id)
            );
            CREATE TABLE IF NOT EXISTS events (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL,
                kind TEXT NOT NULL, data TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS events_project ON events(project, seq);
            CREATE INDEX IF NOT EXISTS messages_project ON messages(project, seq);
            CREATE TABLE IF NOT EXISTS team (
                session_id TEXT PRIMARY KEY REFERENCES sessions(id),
                role TEXT NOT NULL DEFAULT 'worker',
                parent_id TEXT REFERENCES sessions(id)
            );
            CREATE TABLE IF NOT EXISTS rooms (
                id TEXT PRIMARY KEY, project TEXT NOT NULL,
                name TEXT NOT NULL, collapsed INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS room_members (
                session_id TEXT PRIMARY KEY REFERENCES sessions(id),
                room_id TEXT NOT NULL REFERENCES rooms(id)
            );
            CREATE TABLE IF NOT EXISTS message_recipients (
                message_id TEXT NOT NULL REFERENCES messages(id),
                recipient TEXT NOT NULL,
                PRIMARY KEY(message_id, recipient)
            );
            CREATE TABLE IF NOT EXISTS bindings (
                session_id TEXT PRIMARY KEY REFERENCES sessions(id),
                socket_name TEXT NOT NULL, tmux_name TEXT NOT NULL,
                pane TEXT NOT NULL, verified_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS room_delivery (
                message_id TEXT PRIMARY KEY REFERENCES messages(id),
                target TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT 'pending',
                reason TEXT NOT NULL DEFAULT 'Waiting for room lead',
                native_session TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL
            );
        """)
        self._ensure_room_columns()
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS room_starts (
                project TEXT NOT NULL, request_id TEXT NOT NULL,
                payload TEXT NOT NULL, room_id TEXT NOT NULL REFERENCES rooms(id),
                result TEXT NOT NULL, PRIMARY KEY(project, request_id)
            );
            CREATE TABLE IF NOT EXISTS session_intros (
                session_id TEXT PRIMARY KEY REFERENCES sessions(id),
                text TEXT NOT NULL, state TEXT NOT NULL, updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS room_start_receipts (
                project TEXT NOT NULL, request_id TEXT NOT NULL,
                payload TEXT NOT NULL, room_id TEXT NOT NULL, result TEXT NOT NULL,
                PRIMARY KEY(project, request_id)
            );
        """)
        from liljack_app.room_protocol import ensure_schema
        ensure_schema(self)

    def close(self):
        self.db.close()

    def _emit(self, project, kind, data):
        self.db.execute("INSERT INTO events(project,kind,data,created_at) VALUES(?,?,?,?)",
                        (project, kind, json.dumps(data, ensure_ascii=False), utcnow()))

    @staticmethod
    def _observation(row):
        harness, native = row.get("harness"), row.get("session")
        sid = session_key(harness, native)
        project = project_key(row.get("project") or row["cwd"])
        parent = row.get("parent_session")
        data = {"id": sid, "native_id": native, "harness": harness,
                "agent": row.get("agent") or harness, "project": project,
                "cwd": project_key(row["cwd"]),
                "parent_id": session_key(harness, parent) if parent else None,
                "state": row.get("state") or "unknown", "observed_at": row.get("at"),
                "task": row.get("task") or "", "event": row.get("event") or "",
                "capabilities": ["room"], "terminal": None,
                # ⚠ WHICH MACHINE THIS SESSION RUNS ON. The dashboard answers
                # "who is working, on what, on which host" (the operator 2026-09-12,
                # the dashboard plan). An observation can only speak for the
                # machine that made it, so `host` is this node's name; a row
                # imported from elsewhere carries its own `host` unchanged.
                "host": row.get("host") or platform.node() or ""}
        # Arbitrary imported prose must not bypass the shared text scrubber.
        if data["task"]:
            data["task"] = clean(data["task"], 1000)
        return data

    def observe(self, row):
        """Import an observation; project identity is independent of room cwd."""
        data = self._observation(row)
        sid, project = data['id'], data['project']
        self.db.execute("BEGIN IMMEDIATE")
        try:
            old = self.db.execute("SELECT data FROM sessions WHERE id=?", (sid,)).fetchone()
            if old and json.loads(old[0]) == data:
                self.db.execute("COMMIT")
                return data
            self.db.execute("INSERT INTO sessions VALUES(?,?,?) ON CONFLICT(id) DO UPDATE "
                            "SET project=excluded.project,data=excluded.data",
                            (sid, project, json.dumps(data, ensure_ascii=False)))
            if not old or {k:v for k,v in json.loads(old[0]).items() if k != 'observed_at'} != {k:v for k,v in data.items() if k != 'observed_at'}:
                self._emit(project, "session", data)
            self.db.execute("COMMIT")
        except BaseException:
            self.db.execute("ROLLBACK")
            raise
        return data

    def _participant(self, project, identity):
        if identity == "operator":
            return
        row = self.db.execute("SELECT project FROM sessions WHERE id=?", (identity,)).fetchone()
        if not row or row[0] != project:
            raise ValueError("session does not belong to this project")

    def _ensure_room_columns(self):
        """Serialize additive migration before any snapshot read transaction."""
        self.db.execute('BEGIN IMMEDIATE')
        try:
            cols = {r[1] for r in self.db.execute('PRAGMA table_info(rooms)')}
            for name, declaration in (('resolved_at', 'TEXT'),
                                      ('folder', "TEXT NOT NULL DEFAULT ''"),
                                      ('purpose', "TEXT NOT NULL DEFAULT ''"),
                                      ('state', "TEXT NOT NULL DEFAULT 'active'"),
                                      ('state_at', 'TEXT'),
                                      ('phase', "TEXT NOT NULL DEFAULT 'WORK'"),
                                      ('phase_at', 'TEXT'),
                                      ('review_tasks', "TEXT NOT NULL DEFAULT '[]'")):
                if name not in cols:
                    self.db.execute(f'ALTER TABLE rooms ADD COLUMN {name} {declaration}')
            if 'state' not in cols:
                when = utcnow()
                for row in self.db.execute('SELECT * FROM rooms').fetchall():
                    self._emit(row['project'], 'room_state_migrated', {
                        'room_id': row['id'], 'state': 'active', 'state_at': when,
                        'previous_resolved_at': row['resolved_at']})
                self.db.execute("UPDATE rooms SET state='active',state_at=?,resolved_at=NULL", (when,))
            if 'phase' not in cols:
                when = utcnow()
                for row in self.db.execute('SELECT id,project FROM rooms').fetchall():
                    self._emit(row['project'], 'room_phase_migrated', {'room': row['id'], 'phase': 'WORK', 'phase_at': when})
                self.db.execute('UPDATE rooms SET phase_at=?', (when,))
            for row in self.db.execute("SELECT id,project FROM rooms WHERE folder='' OR folder IS NULL").fetchall():
                self.db.execute('UPDATE rooms SET folder=? WHERE id=?',
                                (project_key(row['project']), row['id']))
            self.db.execute('COMMIT')
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def room_resolve(self, project, room_id, resolved=True, *, actor):
        """Compatibility entry point; state is the sole lifecycle authority."""
        return self.room_state(project, room_id, 'resolved' if resolved else 'active', actor=actor)

    def room_roster(self, project, room_id):
        project = project_key(project)
        self._room(project, room_id)
        return [self._decorate(json.loads(r[0])) for r in self.db.execute(
            'SELECT s.data FROM sessions s JOIN room_members m ON m.session_id=s.id '
            'WHERE s.project=? AND m.room_id=? ORDER BY s.id', (project, room_id))]

    def room_live_sessions(self, project, room_id):
        # Unknown/uncertain and planned launches are conservative blockers too.
        return [s for s in self.room_roster(project, room_id)
                if s.get('terminal') or s.get('state') not in ('stopped', 'exited', 'not_started')]

    @staticmethod
    def days_until_purge(room):
        if room['state'] != 'removed':
            return None
        try:
            when = datetime.fromisoformat(room['state_at'])
            if when.tzinfo is None:
                return 30
            return max(0, math.ceil((TRASH_SECONDS - (datetime.now(timezone.utc) - when).total_seconds()) / 86400))
        except (TypeError, ValueError):
            return 30  # Missing/invalid timestamps never authorize destruction.

    def room_state(self, project, room_id, state, *, actor, force=False):
        if actor != 'operator':
            raise PermissionError('only the operator can change room state')
        if state not in ROOM_STATES:
            raise ValueError('state must be one of ' + ', '.join(ROOM_STATES))
        if not isinstance(force, bool):
            raise ValueError('force must be a boolean')
        project = project_key(project)
        self.db.execute("BEGIN IMMEDIATE")
        try:
            room = self._room(project, room_id)
            if state in ('removed', 'archived') and not force:
                live = self.room_live_sessions(project, room_id)
                if live:
                    raise RoomLifecycleError('Live or pending sessions block ' + state + ': ' +
                                     ', '.join(f"{s.get('agent', 'session')} {s['id']}" for s in live) +
                                     '; explicit force required; no sessions stopped',
                                     code='live_sessions', live_sessions=[s['id'] for s in live])
            previous = room['state']
            if previous != state:
                when = utcnow()
                self.db.execute('UPDATE rooms SET state=?,state_at=?,resolved_at=? WHERE id=?',
                                (state, when, when if state == 'resolved' else None, room_id))
                room = self._room(project, room_id)
                self._emit(project, 'room_state', {**room, 'previous_state': previous, 'force': force})
            self.db.execute("COMMIT")
            return {**room, 'resolved': state == 'resolved'}
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def room_purge(self, project, room_id, *, actor):
        """Explicit catalog purge; messages, sessions and event archive survive."""
        if actor != 'operator':
            raise PermissionError('only the operator can purge rooms')
        project = project_key(project)
        self.db.execute('BEGIN IMMEDIATE')
        try:
            room = self._room(project, room_id)
            if room['state'] != 'removed':
                raise ValueError('only removed rooms can be purged')
            days = self.days_until_purge(room)
            if days:
                raise RoomLifecycleError(f'Room remains in trash: {days} days until purge',
                                         code='trash_retention', days_until_purge=days)
            live = self.room_live_sessions(project, room_id)
            if live:
                raise RoomLifecycleError('Live or pending sessions block purge: ' + ', '.join(s['id'] for s in live),
                                         code='live_sessions', live_sessions=[s['id'] for s in live])
            members = [s['id'] for s in self.room_roster(project, room_id)]
            self._emit(project, 'room_purged', {**room, 'members': members, 'history_retained': True})
            self.db.execute('INSERT INTO room_start_receipts SELECT * FROM room_starts WHERE room_id=?', (room_id,))
            self.db.execute('DELETE FROM room_starts WHERE room_id=?', (room_id,))
            self.db.execute('DELETE FROM room_members WHERE room_id=?', (room_id,))
            self.db.execute('DELETE FROM rooms WHERE id=?', (room_id,))
            self.db.execute('COMMIT')
            return {'purged_room': room_id, 'history_retained': True}
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def room_move_all(self, project, room_id, target_room, *, actor):
        if actor != 'operator':
            raise PermissionError('only the operator can move room teams')
        project = project_key(project)
        self.db.execute('BEGIN IMMEDIATE')
        try:
            source = self._room(project, room_id)
            target = self._room(project, target_room)
            if source['state'] in ('removed', 'archived') or target['state'] != 'active':
                raise ValueError('restore the source and activate the target before moving a team')
            if room_id == target_room:
                raise ValueError('source and target rooms must differ')
            moving = self.room_roster(project, room_id)
            if not moving:
                raise ValueError('source room has no team')
            roster = moving + self.room_roster(project, target_room)
            leads = [s for s in roster if s['role'] == 'lead']
            if len(leads) != 1:
                raise ValueError('combined room must have exactly one lead; no sessions moved')
            ids = {s['id'] for s in roster}
            for s in roster:
                if s.get('lead_id') and s['lead_id'] not in ids:
                    raise ValueError('team has a parent outside the combined room; no sessions moved')
            for r in self.db.execute('SELECT session_id,parent_id FROM team WHERE parent_id IS NOT NULL'):
                if r['parent_id'] in ids and r['session_id'] not in ids:
                    raise ValueError('team has a child outside the combined room; no sessions moved')
            self.db.execute('UPDATE room_members SET room_id=? WHERE room_id=?', (target_room, room_id))
            # Unparented destination workers join the sole lead, as in room_start.
            for s in roster:
                if s['role'] != 'lead' and not s.get('lead_id'):
                    self.db.execute('INSERT INTO team VALUES(?,?,?) ON CONFLICT(session_id) '
                                    'DO UPDATE SET parent_id=excluded.parent_id', (s['id'], s['role'], leads[0]['id']))
                    self._emit(project, 'team', {'session_id': s['id'], 'role': s['role'], 'parent_id': leads[0]['id']})
            result = {'room': room_id, 'target_room': target_room, 'moved_sessions': [s['id'] for s in moving]}
            self._emit(project, 'room_team_moved', result)
            self.db.execute('COMMIT')
            return result
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def _room(self, project, room_id):
        row = self.db.execute("SELECT * FROM rooms WHERE id=? AND project=?", (room_id, project)).fetchone()
        if not row:
            raise ValueError("room does not belong to this project")
        return {**dict(row), "collapsed": bool(row["collapsed"])}

    @staticmethod
    def room_fields(project, name, folder=None, purpose='', *, validate_folder=True):
        name = clean(name, 80).strip()
        if not isinstance(purpose, str):
            raise ValueError('purpose must be one line of text')
        purpose = clean(purpose, 500).strip() if purpose.strip() else ''
        if any(ord(ch) < 32 or 127 <= ord(ch) < 160 for ch in name + purpose):
            raise ValueError('room name and purpose must be single lines without controls')
        if folder is not None and (not isinstance(folder, str) or not folder.strip()
                                   or any(ord(ch) < 32 for ch in folder)):
            raise ValueError('folder must be a directory path')
        folder_path = project_key(project if folder is None else folder)
        if validate_folder and folder is not None and not Path(folder_path).is_dir():
            raise ValueError('room folder must be an existing directory')
        return {'name': name, 'folder': folder_path, 'purpose': purpose}

    def room_folder(self, project, room_id='', *, require_exists=True):
        """Shared root for room launches and file browsing; no filesystem writes.

        Empty room means the project. Legacy empty folder means the project;
        an unknown/foreign room never silently falls back to another scope.
        """
        project = project_key(project)
        room = self._room(project, room_id) if room_id else None
        folder = project_key((room.get('folder') if room else None) or project)
        if require_exists and not Path(folder).is_dir():
            raise ValueError('room folder must be an existing directory')
        return folder

    def room_create(self, project, name, *, actor, folder=None, purpose=''):
        if actor != "operator":
            raise PermissionError("only the operator can create rooms")
        project = project_key(project)
        fields = self.room_fields(project, name, folder, purpose)
        room = {"id": "r-" + uuid.uuid4().hex, **fields, "collapsed": False,
                'state': 'active', 'state_at': utcnow()}
        room.update(phase='ALIGN', phase_at=room['state_at'])
        self.db.execute("BEGIN IMMEDIATE")
        try:
            # ⚠ Column-explicit on purpose: the positional form here broke the
            # moment resolved_at was added ("table rooms has 5 columns but 4
            # values were supplied"). A positional INSERT is a promise that the
            # schema will never grow, and this one already has.
            self.db.execute("INSERT INTO rooms (id, project, name, collapsed, folder, purpose,state_at,phase,phase_at) VALUES (?,?,?,0,?,?,?,?,?)",
                            (room["id"], project, fields['name'], fields['folder'], fields['purpose'], room['state_at'], 'ALIGN', room['phase_at']))
            self._emit(project, "room", room)
            self.db.execute("COMMIT")
            return room
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def start_result(self, project, request_id, payload=None):
        row = self.db.execute('SELECT payload,result FROM room_starts WHERE project=? AND request_id=?',
                              (project_key(project), request_id)).fetchone()
        if not row:
            row = self.db.execute('SELECT payload,result FROM room_start_receipts WHERE project=? AND request_id=?',
                                  (project_key(project), request_id)).fetchone()
        if not row:
            return None
        if payload is not None and json.loads(row['payload']) != payload:
            raise ValueError('request_id already used for a different room start')
        result = json.loads(row['result'])
        # C consumes start_errors on the action response as well as each room.
        # Keep errors for the original API contract and CLI callers.
        result['start_errors'] = result['errors']
        for member in result['started_sessions']:
            intro = self.db.execute('SELECT state FROM session_intros WHERE session_id=?',
                                    (member['session_id'],)).fetchone()
            if intro:
                member['intro_state'] = intro[0]
        return result

    def room_start_status(self, room_id):
        row = self.db.execute('SELECT project,request_id FROM room_starts WHERE room_id=?', (room_id,)).fetchone()
        result = self.start_result(*row) if row else None
        return {'start_status': result['start_status'] if result else None,
                'start_errors': result['errors'] if result else [],
                'started_sessions': result['started_sessions'] if result else [],
                'task_ids': result['task_ids'] if result else []}

    def reserve_room_start(self, project, request_id, payload, rows, *, actor):
        """Room, all planned members, team and retry record become visible together."""
        if actor != 'operator':
            raise PermissionError('only the operator can start rooms')
        project = project_key(project)
        if not isinstance(request_id, str) or not 1 <= len(request_id) <= 128:
            raise ValueError('request_id must be 1..128 characters')
        self.db.execute('BEGIN IMMEDIATE')
        try:
            existing = self.start_result(project, request_id, payload)
            if existing:
                self.db.execute('COMMIT')
                return existing, False
            if len(rows) != len(payload['agents']) or sum(a['role'] == 'lead' for a in payload['agents']) != 1:
                raise ValueError('room start needs matching sessions and exactly one lead')
            room = {'id': 'r-' + uuid.uuid4().hex, 'collapsed': False, 'state': 'active', 'state_at': utcnow(),
                    **{k: payload[k] for k in ('name', 'folder', 'purpose')}}
            room.update(phase='ALIGN', phase_at=room['state_at'])
            self.db.execute('INSERT INTO rooms(id,project,name,folder,purpose,state_at,phase,phase_at) VALUES(?,?,?,?,?,?,?,?)',
                            (room['id'], project, room['name'], room['folder'], room['purpose'], room['state_at'], 'ALIGN', room['phase_at']))
            self._emit(project, 'room', room)
            # ⚠ A MOVE ROW IS NOT A SPAWN. `row['move']` means the session
            # already exists and must NOT be re-inserted — only its room
            # membership and team role change. A spawn row is inserted as before.
            sessions = []
            for row in rows:
                if row.get('move'):
                    existing = self.db.execute('SELECT data FROM sessions WHERE id=? AND project=?',
                                               (row['id'], project)).fetchone()
                    if not existing:
                        raise ValueError(f"session {row['id']} is not a session of this project")
                    sessions.append(json.loads(existing['data']))
                else:
                    sessions.append(self._observation(row))
            lead = next(s['id'] for s, a in zip(sessions, payload['agents']) if a['role'] == 'lead')
            for session, row in zip(sessions, rows):
                if session['project'] != project:
                    raise ValueError('planned session belongs to another project')
                if row.get('move'):
                    continue
                self.db.execute('INSERT INTO sessions VALUES(?,?,?)',
                                (session['id'], project, json.dumps(session, ensure_ascii=False)))
                self._emit(project, 'session', session)
            result = {'created_room': room['id'], 'started_sessions': [], 'task_ids': [],
                      'start_status': 'starting', 'errors': []}
            for index, (session, agent, row) in enumerate(zip(sessions, payload['agents'], rows)):
                sid, role = session['id'], agent['role']
                parent = None if role == 'lead' else lead
                if row.get('move'):
                    self.db.execute('INSERT INTO team VALUES(?,?,?) ON CONFLICT(session_id) '
                                    'DO UPDATE SET role=excluded.role,parent_id=excluded.parent_id',
                                    (sid, role, parent))
                    self.db.execute('INSERT INTO room_members VALUES(?,?) ON CONFLICT(session_id) '
                                    'DO UPDATE SET room_id=excluded.room_id', (sid, room['id']))
                else:
                    self.db.execute('INSERT INTO team VALUES(?,?,?)', (sid, role, parent))
                    self.db.execute('INSERT INTO room_members VALUES(?,?)', (sid, room['id']))
                self._emit(project, 'team', {'session_id': sid, 'role': role, 'parent_id': parent})
                self._emit(project, 'room_member', {'session_id': sid, 'room_id': room['id']})
                result['started_sessions'].append({'index': index, 'session_id': sid,
                    'agent': agent['agent'], 'role': role,
                    'state': 'running' if row.get('move') else 'planned',
                    'moved': bool(row.get('move')), 'intro_state': 'pending'})
            self.db.execute('INSERT INTO room_starts VALUES(?,?,?,?,?)',
                            (project, request_id, json.dumps(payload, ensure_ascii=False), room['id'],
                             json.dumps(result, ensure_ascii=False)))
            self._emit(project, 'room_start', result)
            self.db.execute('COMMIT')
            return result, True
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def save_start(self, project, request_id, result):
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('UPDATE room_starts SET result=? WHERE project=? AND request_id=?',
                            (json.dumps(result, ensure_ascii=False), project_key(project), request_id))
            self._emit(project_key(project), 'room_start', result)
            self.db.execute('COMMIT')
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def spawn_parent(self, project, room_id, role, *, actor):
        parent = self.authorize_spawn(project, actor=actor, role=role)
        if not room_id:
            return parent
        room = self._room(project_key(project), room_id)
        if room['state'] != 'active':
            raise ValueError('reopen the room before adding agents')
        leads = [r[0] for r in self.db.execute('SELECT t.session_id FROM team t JOIN room_members m '
                 'ON m.session_id=t.session_id WHERE m.room_id=? AND t.role=?', (room_id, 'lead'))]
        if role == 'lead':
            if leads:
                raise ValueError('room already has a lead')
            return None
        if len(leads) != 1:
            raise ValueError('room needs exactly one lead before adding workers or reviewers')
        if actor != 'operator' and actor != leads[0]:
            raise PermissionError('only this room lead may spawn its members')
        return leads[0]

    def attach_spawned(self, project, session_id, room_id, role, *, actor):
        """Assign an unassigned planned session and its room in one transaction."""
        project = project_key(project)
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self._participant(project, session_id)
            parent = self.spawn_parent(project, room_id, role, actor=actor)
            if self.db.execute('SELECT 1 FROM team WHERE session_id=?', (session_id,)).fetchone():
                raise PermissionError('spawn assignment requires a new unassigned session')
            self.db.execute('INSERT INTO team VALUES(?,?,?)', (session_id, role, parent))
            if room_id:
                self.db.execute('INSERT INTO room_members VALUES(?,?)', (session_id, room_id))
                self._emit(project, 'room_member', {'session_id': session_id, 'room_id': room_id})
            self._emit(project, 'team', {'session_id': session_id, 'role': role, 'parent_id': parent})
            self.db.execute('COMMIT')
            return parent
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def save_intro(self, session_id, text):
        # ⚠ A MOVED SESSION MAY ALREADY HAVE AN INTRO. The plain INSERT hit the
        # session_id primary key, so a move-in could never receive its new room's
        # intro. Upsert resets the state to 'pending' so the moved agent is asked
        # to acknowledge the NEW room, not the one it left.
        self.db.execute('INSERT INTO session_intros VALUES(?,?,?,?) '
                        'ON CONFLICT(session_id) DO UPDATE SET text=excluded.text,'
                        'state=excluded.state,updated_at=excluded.updated_at',
                        (session_id, clean(text, 48000), 'pending', utcnow()))

    def intro_state(self, session_id, state):
        if state not in ('submitted', 'available', 'acknowledged', 'uncertain'):
            raise ValueError('invalid intro state')
        self.db.execute("UPDATE session_intros SET state=?,updated_at=? WHERE session_id=? AND state!='acknowledged'",
                        (state, utcnow(), session_id))

    def room_update(self, project, room_id, collapsed, *, actor):
        if actor != "operator":
            raise PermissionError("only the operator can change rooms")
        if not isinstance(collapsed, bool):
            raise ValueError("collapsed must be a boolean")
        project = project_key(project)
        self.db.execute("BEGIN IMMEDIATE")
        try:
            room = self._room(project, room_id)
            self.db.execute("UPDATE rooms SET collapsed=? WHERE id=?", (int(collapsed), room_id))
            room["collapsed"] = collapsed
            self._emit(project, "room", room)
            self.db.execute("COMMIT")
            return room
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def room_move(self, project, session_id, room_id="", *, actor):
        if actor != "operator":
            raise PermissionError("only the operator can move room members")
        if not isinstance(room_id, str):
            raise ValueError("room must be an ID or empty string")
        project = project_key(project)
        self.db.execute("BEGIN IMMEDIATE")
        try:
            self._participant(project, session_id)
            if session_id == "operator":
                raise ValueError("move an individual session")
            if room_id:
                if self._room(project, room_id)['state'] != 'active':
                    raise ValueError('activate the room before adding members')
                self.db.execute("INSERT INTO room_members VALUES(?,?) ON CONFLICT(session_id) "
                                "DO UPDATE SET room_id=excluded.room_id", (session_id, room_id))
            else:
                self.db.execute("DELETE FROM room_members WHERE session_id=?", (session_id,))
            result = {"session_id": session_id, "room_id": room_id or ""}
            self._emit(project, "room_member", result)
            self.db.execute("COMMIT")
            return result
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def _message(self, row):
        message = dict(row)
        message["audience"] = [r[0] for r in self.db.execute(
            "SELECT recipient FROM message_recipients WHERE message_id=? ORDER BY recipient", (message["id"],))]
        delivery = self.db.execute("SELECT target,state,reason FROM room_delivery WHERE message_id=?", (message["id"],)).fetchone()
        if delivery:
            message["delivery"] = dict(delivery)
        return message

    def assign(self, session_id, role, parent_id=None, *, actor):
        """Only the operator assigns authority; leads cannot promote themselves."""
        if actor != "operator":
            raise PermissionError("only the operator can assign team roles")
        if role not in ("lead", "worker", "reviewer"):
            raise ValueError("role must be lead, worker or reviewer")
        self.db.execute("BEGIN IMMEDIATE")
        try:
            row = self.db.execute("SELECT project FROM sessions WHERE id=?", (session_id,)).fetchone()
            if not row:
                raise ValueError("unknown session")
            project = row[0]
            if parent_id:
                self._participant(project, parent_id)
                parent = self.db.execute("SELECT role FROM team WHERE session_id=?", (parent_id,)).fetchone()
                if not parent or parent[0] != "lead":
                    raise ValueError("parent session must be a lead")
                seen, cursor = {session_id}, parent_id
                while cursor:
                    if cursor in seen:
                        raise ValueError("team hierarchy cannot contain a cycle")
                    seen.add(cursor)
                    parent = self.db.execute("SELECT parent_id FROM team WHERE session_id=?", (cursor,)).fetchone()
                    cursor = parent[0] if parent else None
            # ⚠ A HANDOVER MUST DEMOTE THE PREVIOUS LEAD, OR THE ROOM HAS TWO.
            # Promoting a session to lead only ever wrote the NEW row, so the
            # old lead kept the role and the room ended up with two of them.
            # the operator surrendered lead to deepseek and claude stayed lead; then
            # demoting claude by hand was refused by the children rule below,
            # so there was no way out from the UI at all.
            #
            # The rule already existed one layer up — main.c's new-room dialog
            # says "Exactly one lead: promoting one demotes the previous" — and
            # the ROLE menu already promises "takes over from <agent>". This is
            # the store finally keeping that promise.
            if role == "lead":
                here = self.db.execute(
                    "SELECT room_id FROM room_members WHERE session_id=?", (session_id,)).fetchone()
                room = here[0] if here else None
                if room:
                    former = [r[0] for r in self.db.execute(
                        "SELECT t.session_id FROM team t JOIN room_members m ON m.session_id=t.session_id "
                        "WHERE t.role='lead' AND m.room_id=? AND t.session_id<>?", (room, session_id))]
                else:
                    former = [r[0] for r in self.db.execute(
                        "SELECT t.session_id FROM team t JOIN sessions s ON s.id=t.session_id "
                        "WHERE t.role='lead' AND s.project=? AND t.session_id<>?", (project, session_id))]
                for old_lead in former:
                    # Children follow the role, not the person who used to hold it.
                    # Every re-parented child gets its OWN team event: room_lead
                    # emits one event per member, and consumers that rebuild
                    # parent_id from events must not be shown a stale hierarchy.
                    children = list(self.db.execute(
                        "SELECT session_id, role FROM team WHERE parent_id=? AND session_id<>?",
                        (old_lead, session_id)))
                    self.db.execute("UPDATE team SET parent_id=? WHERE parent_id=? AND session_id<>?",
                                    (session_id, old_lead, session_id))
                    for child, child_role in children:
                        self._emit(project, "team",
                                   {"session_id": child, "role": child_role, "parent_id": session_id})
                    # ONE demotion policy for both handoff paths: the outgoing
                    # lead becomes a REVIEWER, matching room_lead's demotion.
                    self.db.execute("UPDATE team SET role='reviewer', parent_id=? WHERE session_id=?",
                                    (session_id, old_lead))
                    self._emit(project, "team",
                               {"session_id": old_lead, "role": "reviewer", "parent_id": session_id})
                # the new lead answers to nobody, even if it was a child a moment ago
                parent_id = None
                # Pending room delivery follows the new lead, exactly as room_lead
                # re-targets it; only unsent notices are moved.
                if room:
                    self.db.execute(
                        "UPDATE room_delivery SET target='',reason='Room leadership changed; waiting for delivery',updated_at=? "
                        "WHERE state='pending' AND message_id IN (SELECT id FROM messages WHERE project=? AND destination=?)",
                        (utcnow(), project, room))
            if role != "lead" and self.db.execute("SELECT 1 FROM team WHERE parent_id=?", (session_id,)).fetchone():
                raise ValueError("reassign this lead's children before changing its role")
            self.db.execute("INSERT INTO team VALUES(?,?,?) ON CONFLICT(session_id) DO UPDATE "
                            "SET role=excluded.role,parent_id=excluded.parent_id", (session_id, role, parent_id))
            data = {"session_id": session_id, "role": role, "parent_id": parent_id}
            self._emit(project, "team", data)
            self.db.execute("COMMIT")
            return data
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def room_lead(self, project, room_id, session_id, *, actor):
        """Operator-selected, atomic handoff; provider identity never determines role."""
        if actor != 'operator':
            raise PermissionError('only the operator can assign room leadership')
        project = project_key(project)
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self._room(project, room_id)
            members = [r[0] for r in self.db.execute('SELECT session_id FROM room_members WHERE room_id=?', (room_id,))]
            if session_id not in members:
                raise ValueError('Choose a member of this room')
            for sid in members:
                for child in self.db.execute('SELECT session_id FROM team WHERE parent_id=?', (sid,)):
                    if child[0] not in members and sid != session_id:
                        raise ValueError('Reassign children outside this room before handing off leadership')
            for sid in members:
                old = self.db.execute('SELECT role FROM team WHERE session_id=?', (sid,)).fetchone()
                role = 'lead' if sid == session_id else ('reviewer' if old and old[0] == 'lead' else old[0] if old else 'worker')
                parent = None if sid == session_id else session_id
                self.db.execute('INSERT INTO team VALUES(?,?,?) ON CONFLICT(session_id) DO UPDATE SET role=excluded.role,parent_id=excluded.parent_id', (sid, role, parent))
                self._emit(project, 'team', {'session_id': sid, 'role': role, 'parent_id': parent})
            # Only unsent notices can safely follow a new lead. Submitted notices
            # retain their original recipient and acknowledgement semantics.
            self.db.execute("UPDATE room_delivery SET target='',reason='Room leadership changed; waiting for delivery',updated_at=? WHERE state='pending' AND message_id IN (SELECT id FROM messages WHERE project=? AND destination=?)", (utcnow(), project, room_id))
            self.db.execute('COMMIT')
            return {'room': room_id, 'lead': session_id}
        except BaseException:
            self.db.execute('ROLLBACK')
            raise

    def authorize_spawn(self, project, *, actor, parent_id=None, role="worker"):
        """Check authority before creating any process; return the effective parent.

        A lead may create workers/reviewers directly under itself. Only the
        operator may create another lead or choose an unrelated parent.
        """
        project = project_key(project)
        self._participant(project, actor)
        if role not in ("lead", "worker", "reviewer"):
            raise ValueError("invalid team role")
        if actor != "operator":
            row = self.db.execute("SELECT role FROM team WHERE session_id=?", (actor,)).fetchone()
            if not row or row[0] != "lead" or role == "lead" or parent_id not in (None, actor):
                raise PermissionError("leads may spawn only workers/reviewers under themselves")
            parent_id = actor
        if parent_id:
            self._participant(project, parent_id)
            row = self.db.execute("SELECT role FROM team WHERE session_id=?", (parent_id,)).fetchone()
            if not row or row[0] != "lead":
                raise ValueError("parent session must be a lead")
        return parent_id

    def assign_spawned(self, session_id, role="worker", parent_id=None, *, actor):
        """Assign a freshly observed child without impersonating the operator.

        This trusted runtime operation must only follow successful process
        creation. Existing team membership is immutable through this path; ordinary
        reassignment remains operator-only. Discovery may already have bound
        this new terminal in another UI process before assignment completes.
        """
        self.db.execute("BEGIN IMMEDIATE")
        try:
            row = self.db.execute("SELECT project FROM sessions WHERE id=?", (session_id,)).fetchone()
            if not row:
                raise ValueError("unknown session")
            parent_id = self.authorize_spawn(row[0], actor=actor, parent_id=parent_id, role=role)
            if session_id == parent_id or self.db.execute(
                    "SELECT 1 FROM team WHERE session_id=?", (session_id,)).fetchone():
                raise PermissionError("spawn assignment requires a new unassigned session")
            self.db.execute("INSERT INTO team VALUES(?,?,?)", (session_id, role, parent_id))
            data = {"session_id": session_id, "role": role, "parent_id": parent_id}
            self._emit(row[0], "team", data)
            self.db.execute("COMMIT")
            return data
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def may_control(self, actor, target):
        target_row = self.db.execute("SELECT project FROM sessions WHERE id=?", (target,)).fetchone()
        if not target_row:
            return False
        if actor == "operator":
            return True
        actor_row = self.db.execute("SELECT s.project,t.role FROM sessions s LEFT JOIN team t "
                                    "ON t.session_id=s.id WHERE s.id=?", (actor,)).fetchone()
        if not actor_row or actor_row[0] != target_row[0] or actor_row[1] != "lead" or actor == target:
            return False
        cursor, seen = target, set()
        while cursor and cursor not in seen:
            seen.add(cursor)
            row = self.db.execute("SELECT parent_id FROM team WHERE session_id=?", (cursor,)).fetchone()
            cursor = row[0] if row else None
            if cursor == actor:
                return True
        return False

    def bind_terminal(self, session_id, socket_name, tmux_name, pane, *, actor):
        """Trusted adapter calls only after successful tmux discovery; 10s lease."""
        if not self.may_control(actor, session_id):
            raise PermissionError("session is outside your control scope")
        if not all(isinstance(v, str) and v for v in (socket_name, tmux_name, pane)):
            raise ValueError("complete terminal identity is required")
        row = self.db.execute("SELECT project FROM sessions WHERE id=?", (session_id,)).fetchone()
        self.db.execute("BEGIN IMMEDIATE")
        try:
            old = self.db.execute("SELECT * FROM bindings WHERE session_id=?", (session_id,)).fetchone()
            self.db.execute("INSERT INTO bindings VALUES(?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET "
                            "socket_name=excluded.socket_name,tmux_name=excluded.tmux_name,"
                            "pane=excluded.pane,verified_at=excluded.verified_at",
                            (session_id, socket_name, tmux_name, pane, time.time()))
            if not old or tuple(old)[1:4] != (socket_name, tmux_name, pane):
                self._emit(row[0], "terminal_bound", {"session_id": session_id, "backend": "tmux"})
            self.db.execute("COMMIT")
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def _mark_terminal_stopped(self, project, data):
        """Update tmux state within the caller's binding transaction."""
        if data['harness'] == 'tmux' and data['state'] not in ('stopped', 'exited', 'planned', 'not_started', 'uncertain'):
            data.update(state='stopped', event='Detached')
            self.db.execute('UPDATE sessions SET data=? WHERE id=?',
                            (json.dumps(data, ensure_ascii=False), data['id']))
            self._emit(project, 'session', data)

    def unbind_terminal(self, session_id, *, actor):
        if not self.may_control(actor, session_id):
            raise PermissionError("session is outside your control scope")
        self.db.execute("BEGIN IMMEDIATE")
        try:
            row = self.db.execute("SELECT project,data FROM sessions WHERE id=?", (session_id,)).fetchone()
            deleted = self.db.execute("DELETE FROM bindings WHERE session_id=?", (session_id,))
            if deleted.rowcount:
                self._emit(row[0], "terminal_unbound", {"session_id": session_id})
            self._mark_terminal_stopped(row['project'], json.loads(row['data']))
            self.db.execute("COMMIT")
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def reconcile_terminals(self, project, socket_name, live_ids, *, actor):
        """Revoke vanished bindings after SUCCESSFUL complete socket discovery.

        Discovery errors must not call this method with an invented empty list.
        Roles and history persist, so reconnecting the same ID keeps its team.
        """
        if actor != "operator":
            raise PermissionError("only the operator adapter reconciles terminal discovery")
        project = project_key(project)
        live_ids = set(live_ids)
        self.db.execute("BEGIN IMMEDIATE")
        try:
            rows = self.db.execute("SELECT s.id,s.data,b.socket_name FROM sessions s LEFT JOIN bindings b "
                                   "ON b.session_id=s.id WHERE s.project=? "
                                   "AND (b.socket_name=? OR b.session_id IS NULL)",
                                   (project, socket_name)).fetchall()
            for row in rows:
                if row["id"] in live_ids:
                    continue
                data = json.loads(row["data"])
                # Older stop calls removed the binding without updating state.
                # Only recover unbound rows whose native identity names this socket.
                if row['socket_name'] is None and not (
                        data['harness'] == 'tmux' and
                        data['native_id'].startswith(socket_name + '/')):
                    continue
                deleted = self.db.execute("DELETE FROM bindings WHERE session_id=?", (row["id"],))
                if deleted.rowcount:
                    self._emit(project, "terminal_unbound", {"session_id": row["id"]})
                self._mark_terminal_stopped(project, data)
            self.db.execute("COMMIT")
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def _decorate(self, session):
        room = self.db.execute("SELECT r.id FROM room_members m JOIN rooms r ON r.id=m.room_id "
                               "WHERE m.session_id=? AND r.project=?", (session["id"], session["project"])).fetchone()
        session["room_id"] = room[0] if room else ""
        team = self.db.execute("SELECT role,parent_id FROM team WHERE session_id=?", (session["id"],)).fetchone()
        session["role"] = team[0] if team else "worker"
        session["lead_id"] = team[1] if team else None
        intro = self.db.execute('SELECT state FROM session_intros WHERE session_id=?', (session['id'],)).fetchone()
        session['intro_state'] = intro[0] if intro else None
        binding = self.db.execute("SELECT * FROM bindings WHERE session_id=?", (session["id"],)).fetchone()
        if binding and 0 <= time.time() - binding["verified_at"] <= 10:
            session["capabilities"] = ["room", "terminal"]
            session["terminal"] = dict(binding)
        return session

    def post(self, project, sender, text, request_id, destination="room", language="und", reply_to=None):
        """Retry-safe post. `sender` is supplied by the trusted client adapter.

        This library is not an authentication boundary. An IPC transport must
        bind sender to its peer; it must not pass arbitrary JSON sender claims.
        Session DMs are visible only to sender/recipient (and operator).
        """
        project = project_key(project)
        text = clean(text)
        if not isinstance(destination, str) or not destination:
            raise ValueError("destination must be a non-empty string")
        if reply_to is not None and not isinstance(reply_to, str):
            raise ValueError("reply_to must be a message ID")
        if not isinstance(request_id, str) or not 1 <= len(request_id) <= 128:
            raise ValueError("request_id must be 1..128 characters")
        if not isinstance(language, str) or not 1 <= len(language) <= 40:
            raise ValueError("language must be a short language tag")
        self.db.execute("BEGIN IMMEDIATE")
        try:
            self._participant(project, sender)
            existing = self.db.execute("SELECT * FROM messages WHERE project=? AND sender=? "
                                       "AND request_id=?", (project, sender, request_id)).fetchone()
            if existing:
                value = self._message(existing)
                if any(value[k] != v for k, v in {
                    "text": text, "destination": destination, "language": language,
                    "reply_to": reply_to}.items()):
                    raise ValueError("request_id already used for a different message")
                self.db.execute("COMMIT")
                return value
            audience = []
            if isinstance(destination, str) and destination.startswith("r-"):
                self._room(project, destination)
                audience = sorted({"operator", *(r[0] for r in self.db.execute(
                    "SELECT m.session_id FROM room_members m JOIN sessions s ON s.id=m.session_id "
                    "WHERE m.room_id=? AND s.project=?", (destination, project)))})
                if sender not in audience:
                    raise PermissionError("join this room before posting")
            elif destination != "room":
                self._participant(project, destination)
            if reply_to:
                parent = self.db.execute("SELECT * FROM messages WHERE id=?", (reply_to,)).fetchone()
                if not parent or parent["project"] != project:
                    raise ValueError("reply target is not in this project")
                parent = self._message(parent)
                if not self._visible(parent, sender):
                    raise PermissionError("reply target was not addressed to this session")
                if parent["destination"].startswith("r-"):
                    recipients = set(audience) if destination.startswith("r-") else {"operator", sender, destination}
                    if destination == "room" or not recipients.issubset(parent["audience"]):
                        raise ValueError("room reply cannot widen its original audience")
                elif parent["destination"] != "room":
                    participants = {parent["sender"], parent["destination"]}
                    if destination == "room" or destination.startswith("r-") or {sender, destination} != participants:
                        raise ValueError("a direct-message reply must keep its participants")
            identity = "m-" + hashlib.sha256(json.dumps(
                [project, sender, request_id], ensure_ascii=False).encode()).hexdigest()[:32]
            self.db.execute("INSERT INTO messages(id,project,sender,destination,text,language,"
                            "reply_to,created_at,request_id) VALUES(?,?,?,?,?,?,?,?,?)",
                            (identity, project, sender, destination, text, language,
                             reply_to, utcnow(), request_id))
            self.db.executemany("INSERT INTO message_recipients VALUES(?,?)",
                                ((identity, recipient) for recipient in audience))
            sender_role = self.db.execute("SELECT role FROM team WHERE session_id=?", (sender,)).fetchone()
            if destination.startswith("r-") and (sender == "operator" or not sender_role or sender_role[0] != 'lead'):
                self.db.execute("INSERT INTO room_delivery(message_id,updated_at) VALUES(?,?)", (identity, utcnow()))
            value = self._message(self.db.execute("SELECT * FROM messages WHERE id=?", (identity,)).fetchone())
            self._emit(project, "message", value)
            self.db.execute("COMMIT")
            return value
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    @staticmethod
    def _visible(message, viewer):
        if viewer == "operator" or message["destination"] == "room":
            return True
        if message["destination"].startswith("r-"):
            return viewer in message.get("audience", [])
        return viewer in (message["sender"], message["destination"])

    def _viewer_agent(self, viewer):
        """The provider/agent label of a session, or None. Mirrors snapshot()'s
        same-agent rule so events() and snapshot() agree about a restarted
        session's visibility into room history."""
        row = self.db.execute("SELECT data FROM sessions WHERE id=?", (viewer,)).fetchone()
        if not row or not row[0]:
            return None
        try:
            return (json.loads(row[0]) or {}).get("agent") or None
        except (ValueError, TypeError):
            return None

    def _agent_read(self, message, viewer_agent):
        """snapshot()'s same-agent fallback, for the events feed: a restarted
        session of an agent that was already in a room message's frozen audience
        has read it, so the event is visible to it too. A genuinely NEW agent
        joining late stays blind — the audience is not widened."""
        if not viewer_agent or not str(message.get("destination") or "").startswith("r-"):
            return False
        audience = message.get("audience") or []
        if not audience:
            return False
        placeholders = ",".join("?" for _ in audience)
        row = self.db.execute(
            f"SELECT 1 FROM sessions WHERE id IN ({placeholders}) "
            "AND json_extract(data,'$.agent')=? LIMIT 1",
            (*audience, viewer_agent)).fetchone()
        return row is not None

    def snapshot(self, project, viewer="operator", limit=200):
        project = project_key(project)
        limit = max(1, min(int(limit), 1000))
        self.db.execute("BEGIN")
        try:
            self._participant(project, viewer)
            cursor = self.db.execute("SELECT COALESCE(MAX(seq),0) FROM events").fetchone()[0]
            sessions = [self._decorate(json.loads(r[0])) for r in self.db.execute(
                "SELECT data FROM sessions WHERE project=? ORDER BY id", (project,))]
            # ⚠ A RESTARTED AGENT IS NOT A NEW PARTICIPANT. Visibility rested
            # entirely on message_recipients — the audience captured AT POST
            # TIME — so a session that did not exist when a message was sent
            # could never see it. Every restarted agent woke into a room with no
            # history: measured 2026-09-10, a fresh codex session reported
            # "earlier named-room messages are not exposed to this new session
            # by the recipient-filtered feed; I cannot claim a room-history
            # last-read sequence", and it was right.
            #
            # Third instance today of one durable fact keyed on an ephemeral
            # session id (the others: the board/registry mirror, and the worker
            # todo view).
            #
            # ⚠ THE AUDIENCE STAYS FROZEN, AND THAT IS DELIBERATE. A genuinely
            # NEW member must not read what was said before it joined —
            # test_audience_frozen_on_join_leave_and_retry, and
            # test_room_replies_cannot_widen_audience depends on it. My first
            # cut widened to "any current member" and broke exactly that.
            #
            # The discriminator is whether this AGENT already had access: if an
            # earlier session of the same agent was in the audience, the agent
            # has read it, and a new session of that agent seeing it exposes
            # nothing new. An agent that was never a recipient still sees
            # nothing, so joining a room late reveals no history.
            viewer_agent = None
            row = self.db.execute("SELECT data FROM sessions WHERE id=?", (viewer,)).fetchone()
            if row and row[0]:
                try:
                    viewer_agent = (json.loads(row[0]) or {}).get("agent") or None
                except (ValueError, TypeError):
                    viewer_agent = None
            sql = ("SELECT * FROM messages WHERE project=? AND "
                   "(?='operator' OR destination='room' OR sender=? OR destination=? OR EXISTS "
                   "(SELECT 1 FROM message_recipients mr WHERE mr.message_id=messages.id "
                   "AND mr.recipient=?)")
            args = [project, viewer, viewer, viewer, viewer]
            if viewer_agent:
                sql += (" OR EXISTS (SELECT 1 FROM message_recipients mr2 "
                        "JOIN sessions s2 ON s2.id=mr2.recipient "
                        "WHERE mr2.message_id=messages.id "
                        "AND json_extract(s2.data,'$.agent')=?)")
                args.append(viewer_agent)
            sql += ") ORDER BY seq DESC LIMIT ?"
            args.append(limit)
            messages = self.db.execute(sql, args).fetchall()
            messages = [self._message(r) for r in reversed(messages)]
            rooms = [{"id": r["id"], "name": r["name"], "collapsed": bool(r["collapsed"]),
                      "resolved": r['state'] == 'resolved', "resolved_at": r["resolved_at"],
                      'state': r['state'], 'state_at': r['state_at'],
                      'phase': r['phase'], 'phase_at': r['phase_at'],
                      **({'days_until_purge': self.days_until_purge(r)} if r['state'] == 'removed' else {}),
                      "folder": r['folder'], "purpose": r['purpose'],
                      **self.room_start_status(r['id'])}
                     for r in self.db.execute("SELECT * FROM rooms WHERE project=? ORDER BY rowid", (project,))]
            self.db.execute("COMMIT")
        except BaseException:
            self.db.execute("ROLLBACK")
            raise
        return {"protocol": PROTOCOL, "cursor": cursor, "project": project,
                "sessions": sessions, "rooms": rooms, "messages": messages,
                'room_states': {state: sum(r['state'] == state for r in rooms) for state in ROOM_STATES}}

    def events(self, project, after=0, viewer="operator", limit=200, messages_only=False):
        project = project_key(project)
        after = int(after)
        if after < 0:
            raise ValueError("cursor cannot be negative")
        self._participant(project, viewer)
        viewer_agent = self._viewer_agent(viewer)
        rows = self.db.execute("SELECT * FROM events WHERE project=? AND seq>? " + ("AND kind='message' " if messages_only else "") + "ORDER BY seq LIMIT ?",
                               (project, after, max(1, min(int(limit), 1000)))).fetchall()
        events = []
        for r in rows:
            data = json.loads(r["data"])
            if r["kind"] != "message" or self._visible(data, viewer) or self._agent_read(data, viewer_agent):
                events.append({"seq": r["seq"], "kind": r["kind"], "data": data,
                               "created_at": r["created_at"]})
        return {"protocol": PROTOCOL, "cursor": rows[-1]["seq"] if rows else after,
                "events": events}

    def refresh(self, project):
        import liljack_sessions
        observed = liljack_sessions.snapshot()
        project = project_key(project)
        for row in observed["sessions"]:
            if row.get("cwd") and project_key(row["cwd"]) == project:
                self.observe(row)
        return observed["errors"]


class Selection:
    """UI focus cannot retarget in-progress typing when sessions reorder."""
    def __init__(self):
        self.session_id = None
        self.focus = "sessions"

    def select(self, session_id, sessions):
        if session_id not in {s["id"] for s in sessions}:
            raise ValueError("unknown session")
        self.session_id, self.focus = session_id, "sessions"

    def focus_terminal(self, sessions):
        selected = next((s for s in sessions if s["id"] == self.session_id), None)
        if not selected or "terminal" not in selected.get("capabilities", []):
            raise ValueError("selected session has no attached terminal")
        self.focus = "terminal"

    def reconcile(self, sessions):
        if self.session_id not in {s["id"] for s in sessions}:
            self.session_id, self.focus = None, "sessions"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root")
    parser.add_argument("--project", default=os.getcwd())
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args(argv)
    store = Workspace(args.root)
    try:
        errors = store.refresh(args.project) if args.refresh else {}
        print(json.dumps({**store.snapshot(args.project), "source_errors": errors}, ensure_ascii=False))
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
