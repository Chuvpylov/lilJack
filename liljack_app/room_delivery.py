"""Durable human/teammate room notifications to the exact idle lead.

Driven by the room heartbeat or UI refresh under one dispatch lock. A transport
timeout is ambiguous, so it is reported without automatic replay.
"""
import os
import subprocess
import fcntl

from liljack_workspace import project_key, utcnow
from .lead_transport import harness_for, wake

STALE_AFTER_S = 15 * 60


def supersede_stale(store, project, *, at=None):
    """Expire automatic wake eligibility, never message history or read state."""
    stamp = at or utcnow()
    return store.db.execute(
        "UPDATE room_delivery SET state='superseded',reason='Wake expired after 15 minutes; message remains unread in room history',updated_at=? "
        "WHERE state='pending' AND message_id IN "
        "(SELECT id FROM messages WHERE project=? AND julianday(?)-julianday(created_at)>?/86400.0)",
        (stamp, project_key(project), stamp, STALE_AFTER_S)).rowcount


def skip_pending_for(store, session_id, room_id="", *, reason=None):
    """Mark a moved session's pending notices SKIPPED — never deleted.

    ⚠ THE GARBAGE A MOVE MUST NOT CARRY. When a session moves rooms, notices
    still queued for it in the OLD room are the backlog that must not be
    replayed into its new room (the 02:22-03:15 replay the lead received). The
    delivery row is labelled `skipped` with the reason; the message itself stays
    in room history and unread. Scoped to the old room when given.
    """
    reason = reason or "Session moved rooms; pending notice not replayed"
    if room_id:
        return store.db.execute(
            "UPDATE room_delivery SET state='skipped',reason=?,updated_at=? "
            "WHERE target=? AND state='pending' AND message_id IN "
            "(SELECT id FROM messages WHERE destination=?)",
            (reason, utcnow(), session_id, room_id)).rowcount
    return store.db.execute(
        "UPDATE room_delivery SET state='skipped',reason=?,updated_at=? "
        "WHERE target=? AND state='pending'",
        (reason, utcnow(), session_id)).rowcount


def native_state(target, project, path=None):
    """Compatibility entry point; the pump uses harness-aware lookup directly."""
    from .lead_lifecycle import lookup
    result = lookup(target, project, 'codex', path=path or os.environ.get('LILJACK_CODEX_DB'))
    return None if result['state'] == 'unknown' else result


def pump(store, project, live, *, lookup=None, send=None):
    supersede_stale(store, project)
    if (store.root / 'room-delivery.stop').exists():
        return
    if lookup is None:
        from functools import partial
        from .lead_lifecycle import lookup as lifecycle_lookup
        lookup = partial(lifecycle_lookup, workspace_root=store.root)
    # Two UI windows can refresh the same workspace at once.
    with (store.root / 'room-delivery.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        return _pump(store, project, live, lookup=lookup, send=send)


def _pump(store, project, live, *, lookup, send):
    project = project_key(project)
    store.db.execute("UPDATE room_delivery SET state='uncertain',reason='Submission interrupted; review before retrying' "
                     "WHERE state='sending' AND julianday('now')-julianday(updated_at)>30.0/86400 "
                     "AND message_id IN (SELECT id FROM messages WHERE project=?)", (project,))
    live = {r['id']: r for r in live if r.get('state') != 'exited'}
    # At most one submit per refresh. Never bypass a prior outstanding message
    # to the same lead, even if its Stop event has not changed yet.
    rows = store.db.execute("SELECT d.*,m.destination,m.seq,m.sender FROM room_delivery d JOIN messages m ON m.id=d.message_id "
                            "WHERE m.project=? AND d.state='pending' ORDER BY m.seq LIMIT 100", (project,)).fetchall()
    for item in rows:
        mid = item['message_id']
        def status(reason, target=None):
            store.db.execute("UPDATE room_delivery SET reason=?,target=COALESCE(?,target),updated_at=? WHERE message_id=? AND state='pending'",
                             (reason, target, utcnow(), mid))
        targets = [r[0] for r in store.db.execute(
            "SELECT t.session_id FROM team t JOIN room_members rm ON rm.session_id=t.session_id "
            "JOIN message_recipients mr ON mr.recipient=t.session_id WHERE t.role='lead' "
            "AND rm.room_id=? AND mr.message_id=?", (item['destination'], mid))]
        if len(targets) != 1:
            status('Assign exactly one lead in this room');continue
        target = targets[0]
        if item['sender'] == target:
            store.db.execute("UPDATE room_delivery SET state='skipped',reason='Lead does not wake itself',updated_at=? WHERE message_id=?",
                             (utcnow(), mid))
            continue
        # ⚠ DO NOT WAKE ABOUT A NOTICE THE RECIPIENT HAS ALREADY READ. The lead
        # reads the room in bulk (--since / --messages-only), which never wrote
        # this table, so a batch of already-seen notices stayed 'pending' and the
        # pump woke the lead once per message — ~20 turns on 2026-09-12, every
        # one already seen. A READ notice in the SAME room with a seq >= this one
        # proves the recipient has read past it, so skip the redundant wake.
        # Nothing is deleted: the message stays unread in room history; only the
        # wake is dropped, and the reason says why.
        watermark = store.db.execute(
            "SELECT MAX(m.seq) FROM room_delivery d JOIN messages m ON m.id=d.message_id "
            "WHERE d.target=? AND d.state='read' AND m.project=? AND m.destination=?",
            (target, project, item['destination'])).fetchone()[0]
        if watermark is not None and item['seq'] <= watermark:
            store.db.execute("UPDATE room_delivery SET state='skipped',reason='Recipient already read a later notice in this room',updated_at=? "
                             "WHERE message_id=? AND state='pending'", (utcnow(), mid))
            continue
        if item['target'] and item['target'] != target:
            status('Original lead changed; delivery needs review');continue
        if target not in live:
            status('Lead terminal is disconnected', target);continue
        harness = harness_for(live[target].get('agent'))
        if not harness:
            status('No automatic wake transport for this harness', target);continue
        if store.db.execute("SELECT 1 FROM room_delivery WHERE target=? AND state IN ('sending','submitted','uncertain') LIMIT 1", (target,)).fetchone():
            status('Waiting for the lead to read its previous notification', target);continue
        state = lookup(target, project, harness)
        if not state:
            status('Waiting for exact-session lifecycle hook', target);continue
        if state['event'] != 'Stop' or state.get('state', 'idle') != 'idle' or not 2 <= state['age'] <= 86400:
            status('Waiting for idle lead (latest event: ' + state['event'] + ')', target);continue
        previous = store.db.execute("SELECT MAX(updated_at) FROM room_delivery WHERE target=? AND state='read'", (target,)).fetchone()[0]
        if previous and state.get('at', '') <= previous:
            status('Waiting for the lead to finish its previous turn', target);continue
        fresh = lookup(target, project, harness)
        if (not fresh or any(fresh.get(k) != state.get(k) for k in ('native', 'event', 'at', 'state'))
                or not 2 <= fresh['age'] <= 86400):
            status('Lead lifecycle changed before delivery', target);continue
        claimed = store.db.execute("UPDATE room_delivery SET state='sending',target=?,native_session=?,reason='Submitting wake notice',updated_at=? "
                                   "WHERE message_id=? AND state='pending'", (target, state['native'], utcnow(), mid)).rowcount
        if not claimed:
            continue
        notice = f"Room notice: read ./liljack room --message {mid} and coordinate in that room."
        try:
            result = (send(state['native'], notice, project) if send else
                      wake(state['native'], notice, project, harness=harness, target=live[target]))
            outcome = 'submitted' if result.returncode == 0 else 'uncertain'
            reason = 'Wake transport accepted; awaiting agent read' if result.returncode == 0 else 'Wake transport failed; review before retrying'
        except (OSError, subprocess.SubprocessError, RuntimeError, ValueError):
            outcome, reason = 'uncertain', 'Wake transport outcome unknown; review before retrying'
        store.db.execute("UPDATE room_delivery SET state=?,reason=?,updated_at=? WHERE message_id=? AND state='sending'",
                         (outcome, reason, utcnow(), mid))
        return


def read_message(store, project, viewer, message_id):
    row = store.db.execute('SELECT * FROM messages WHERE project=? AND id=?', (project_key(project), message_id)).fetchone()
    if not row:
        raise ValueError('Message not found')
    message = store._message(row)
    if not store._visible(message, viewer):
        raise PermissionError('Message is not addressed to this session')
    # An explicit read must not require a wake first. Resolve an unclaimed
    # notice using the same room-lead/frozen-audience rule as dispatch; never
    # transfer a notice already pinned to somebody else merely on a read.
    targets = [r[0] for r in store.db.execute(
        "SELECT t.session_id FROM team t JOIN room_members rm ON rm.session_id=t.session_id "
        "JOIN message_recipients mr ON mr.recipient=t.session_id WHERE t.role='lead' "
        "AND rm.room_id=? AND mr.message_id=?", (row['destination'], message_id))]
    if targets == [viewer]:
        store.db.execute("UPDATE room_delivery SET state='read',target=?,reason='Read by assigned lead before wake',updated_at=? "
                         "WHERE message_id=? AND state='pending' AND target IN ('',?)",
                         (viewer, utcnow(), message_id, viewer))
    store.db.execute("UPDATE room_delivery SET state='read',reason='Read by assigned lead',updated_at=? "
                     "WHERE message_id=? AND target=? AND state IN ('submitted','sending','uncertain')", (utcnow(), message_id, viewer))
    return store._message(row)
