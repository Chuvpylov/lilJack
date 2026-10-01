#!/usr/bin/env python3
"""The notice relay must not wake the lead about a notice it already read.

The lead reads the room in bulk (--since / --messages-only), which never wrote
the delivery table, so a batch of already-seen notices stayed `pending` and the
pump woke the lead once per message — ~20 turns on 2026-09-12. A read notice in
the same room with a seq >= this one proves the recipient read past it; the
relay skips the redundant wake (state `skipped`), it never deletes history.

Pins heartbeat-route-by-session sub-TODO (d).
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_workspace import Workspace                       # noqa: E402
from liljack_app.room_delivery import pump                    # noqa: E402


class RelayReadSkipTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = str(self.root / 'project')
        self.w = Workspace(self.root / 'workspace')
        self.addCleanup(self.w.close)
        self.lead = self.w.observe({'harness': 'tmux', 'session': 'socket/lead',
                                    'agent': 'codex', 'cwd': self.project})['id']
        self.peer = self.w.observe({'harness': 'tmux', 'session': 'socket/peer',
                                    'agent': 'claude', 'cwd': self.project})['id']
        self.room = self.w.room_create(self.project, 'Work', actor='operator')['id']
        for sid in (self.lead, self.peer):
            self.w.room_move(self.project, sid, self.room, actor='operator')
        self.w.assign(self.lead, 'lead', actor='operator')
        self.live = [{'id': self.lead, 'agent': 'codex', 'state': 'running'}]
        self.lookup = Mock(return_value={'native': 'exact', 'event': 'Stop', 'age': 3,
                                         'at': '9999-01-01T00:00:00+00:00'})
        self.send = Mock(return_value=subprocess.CompletedProcess([], 0))

    def post(self, key):
        return self.w.post(self.project, 'operator', 'Please continue', key,
                           destination=self.room)

    def tick(self):
        pump(self.w, self.project, self.live, lookup=self.lookup, send=self.send)

    def delivery(self, message):
        return dict(self.w.db.execute(
            'SELECT * FROM room_delivery WHERE message_id=?', (message['id'],)).fetchone())

    def mark_read(self, message):
        self.w.db.execute("UPDATE room_delivery SET state='read',target=?,"
                          "reason='Read by assigned lead',updated_at='2026-01-01T00:00:00Z' "
                          "WHERE message_id=?", (self.lead, message['id']))

    def test_relay_skips_a_notice_the_recipient_already_read_past(self):
        old = self.post('old')
        later = self.post('later')
        self.mark_read(later)            # the lead read PAST the older notice
        self.tick()
        self.assertEqual(self.delivery(old)['state'], 'skipped',
                         "an already-read notice is skipped, not woken")
        self.assertIn('already read', self.delivery(old)['reason'])
        self.send.assert_not_called()

    def test_relay_still_delivers_a_genuinely_new_notice(self):
        later = self.post('later')
        self.mark_read(later)
        new = self.post('new')           # seq > the read watermark
        self.tick()
        self.send.assert_called_once()
        self.assertEqual(self.delivery(new)['state'], 'submitted',
                         "a notice after the read watermark is delivered")
        self.assertIn(new['id'], self.send.call_args.args[1])

    def test_no_read_watermark_means_normal_delivery(self):
        first = self.post('first')
        self.tick()
        self.send.assert_called_once()
        self.assertEqual(self.delivery(first)['state'], 'submitted',
                         "with nothing read, the first notice is delivered as before")


if __name__ == '__main__':
    unittest.main()
