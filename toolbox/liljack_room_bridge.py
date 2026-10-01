"""Explicit workspace ↔ legacy mailbox adapter, without PTY input side effects.

Provider mail is represented by provider mailbox identities, never silently
assigned to one particular terminal. A file lock serializes bridge writers;
deterministic IDs recover a crash between mailbox append and SQLite ledger.
The existing append-only mailbox and its acknowledgements remain unchanged.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
from pathlib import Path

import liljack_mail as mail
from liljack_workspace import project_key


def _provider(store, project, identity):
    if identity == 'operator':
        return 'operator'
    row = store.db.execute('SELECT data FROM sessions WHERE id=? AND project=?', (identity, project)).fetchone()
    if not row:
        raise ValueError('unknown workspace participant')
    provider = json.loads(row[0]).get('agent')
    if provider not in mail.AGENTS:
        raise ValueError('participant has no mailbox provider')
    return provider


def _identity(store, project, provider):
    if provider == 'operator':
        return provider
    return store.observe({'harness': 'mailbox', 'session': project + ':' + provider,
                          'agent': provider, 'cwd': project, 'state': 'observed',
                          'task': 'Shared provider mailbox (not an attached terminal)'})['id']


def _destination(store, project, identity):
    if identity != 'room':
        raise ValueError('message remains workspace-only: private and named-room messages are never provider-exported')
    return 'all'


def sync_workspace(store, project, *, mail_root=None, mailbox_project=None):
    """Import/export explicit messages; return imported/exported counts and issues.

    Tests MUST pass mail_root. Legacy basename-only project labels are associated
    only with the supplied canonical cwd; callers with colliding project names
    must use distinct mailbox_project labels. Exact-cwd metadata is also checked.
    Provider DMs are private provider identities, not guessed terminal identities.
    """
    project = project_key(project)
    label = mailbox_project or Path(project).name
    if not label or len(label) > 80:
        raise ValueError('mailbox project must be 1..80 characters')
    root = Path(mail_root) if mail_root is not None else mail.CACHE
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    result = {'imported': 0, 'exported': 0, 'issues': []}
    store.db.execute("""CREATE TABLE IF NOT EXISTS mailbox_bridge (
        mailbox_id TEXT PRIMARY KEY, message_id TEXT UNIQUE NOT NULL,
        direction TEXT NOT NULL CHECK(direction IN ('in','out'))
    )""")
    with (root / 'workspace-bridge.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # The legacy mailbox has basename labels. Claim a label under this
        # same lock so a second cwd cannot silently ingest the first project's
        # private mail. Corrupt ownership metadata fails closed.
        ownership_path = root / 'workspace-projects.json'
        ownership = json.loads(ownership_path.read_text()) if ownership_path.exists() else {}
        if not isinstance(ownership, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                                  for k, v in ownership.items()):
            raise ValueError('invalid mailbox project ownership metadata')
        if label in ownership and ownership[label] != project:
            raise ValueError('mailbox project label is already assigned to another cwd; choose mailbox_project')
        if label not in ownership:
            ownership[label] = project
            temporary = root / 'workspace-projects.json.tmp'
            temporary.write_text(json.dumps(ownership, ensure_ascii=False))
            temporary.chmod(0o600)
            temporary.replace(ownership_path)
        mailbox_path, _ = mail._paths(root)
        rows = mail._read(mailbox_path)
        by_id = {r.get('id'): r for r in rows if isinstance(r.get('id'), str)}
        for row in rows:
            mid = row.get('id')
            if row.get('project') != label or not isinstance(mid, str) or len(mid) > 200:
                continue
            if row.get('workspace_project') and row['workspace_project'] != project:
                continue
            if store.db.execute('SELECT 1 FROM mailbox_bridge WHERE mailbox_id=?', (mid,)).fetchone():
                continue
            origin = row.get('workspace_message')
            expected_id = 'workspace-' + hashlib.sha256(origin.encode('utf-8', errors='replace')).hexdigest()[:32] if isinstance(origin, str) else None
            if origin and mid == expected_id and store.db.execute(
                    'SELECT 1 FROM messages WHERE id=? AND project=?', (origin, project)).fetchone():
                store.db.execute("INSERT OR IGNORE INTO mailbox_bridge VALUES(?,?,'out')", (mid, origin))
                continue
            if row.get('frm') not in mail.AGENTS or row.get('to') not in mail.AGENTS + ('all',):
                result['issues'].append({'mailbox_id': mid, 'reason': 'unknown mailbox participant'})
                continue
            try:
                sender = _identity(store, project, row['frm'])
                destination = 'room' if row['to'] == 'all' else _identity(store, project, row['to'])
                reply = None
                if isinstance(row.get('workspace_reply'), str):
                    parent = store.db.execute('SELECT * FROM messages WHERE id=? AND project=?',
                                              (row['workspace_reply'], project)).fetchone()
                    if parent and (parent['destination'] == 'room' or
                                   {parent['sender'], parent['destination']} == {sender, destination}):
                        reply = parent['id']
                message = store.post(project, sender, row.get('text'),
                                     'mail:' + hashlib.sha256(mid.encode()).hexdigest(),
                                     destination=destination, language=row.get('workspace_language') or 'und', reply_to=reply)
                store.db.execute("INSERT OR IGNORE INTO mailbox_bridge VALUES(?,?,'in')", (mid, message['id']))
                result['imported'] += 1
            except (ValueError, TypeError, UnicodeError) as exc:
                result['issues'].append({'mailbox_id': mid, 'reason': 'invalid Unicode payload' if isinstance(exc, UnicodeError) else str(exc)})
        pending = store.db.execute('SELECT m.* FROM messages m LEFT JOIN mailbox_bridge b ON '
                                   "b.message_id=m.id WHERE m.project=? AND m.destination='room' AND b.message_id IS NULL ORDER BY m.seq",
                                   (project,)).fetchall()
        for message in pending:
            mid = 'workspace-' + hashlib.sha256(message['id'].encode()).hexdigest()[:32]
            try:
                if len(message['text']) > mail.MAX_TEXT:
                    raise ValueError('message retained locally: exceeds mailbox 4000-character cap')
                sender = _provider(store, project, message['sender'])
                destination = _destination(store, project, message['destination'])
                if sender == destination:
                    raise ValueError('private message retained locally: provider mailbox cannot route to itself')
                row = {'id': mid, 'ts': message['created_at'], 'frm': sender, 'to': destination,
                       'subject': 'lilJack workspace', 'text': mail._scrub(message['text']),
                       'project': label, 'thread': 'liljack-workspace', 'schema_version': 1,
                       'workspace_project': project, 'workspace_message': message['id'],
                       'workspace_sender': message['sender'], 'workspace_destination': message['destination'],
                       'workspace_language': message['language'], 'workspace_reply': message['reply_to']}
                if mid not in by_id:
                    mail._append(mailbox_path, row)
                    by_id[mid] = row
                store.db.execute("INSERT OR IGNORE INTO mailbox_bridge VALUES(?,?,'out')", (mid, message['id']))
                result['exported'] += 1
            except (ValueError, TypeError, UnicodeError) as exc:
                result['issues'].append({'message_id': message['id'], 'reason': 'invalid Unicode payload' if isinstance(exc, UnicodeError) else str(exc)})
    return result
