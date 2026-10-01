#!/usr/bin/env python3
"""Peer-bound identity regression; private socket/state, no agents launched."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLBOX = Path(__file__).resolve().parents[1] / 'toolbox'
sys.path.insert(0, str(TOOLBOX))
import liljack_agentd as D


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ljid-')
        self.d = D.Daemon(root=self.tmp.name, board=False, sessions=False)

    def tearDown(self):
        self.d.shutdown()
        self.tmp.cleanup()

    def test_forged_and_missing_identity(self):
        for identity in ('deepseek', 'codex', 'claude', ''):
            for op in ('take_control', 'shutdown', 'spawn', 'stop', 'kill'):
                for claims in ({}, {'from': 'operator'}, {'who': 'operator'}):
                    with self.subTest(identity=identity, op=op, claims=claims):
                        r = self.d.handle({'op': op, 'agent': 'codex', **claims}, identity=identity)
                        self.assertTrue(r.get('refused'), r)
        self.assertEqual(self.d.holder(), 'operator')
        self.assertFalse(self.d._stop.is_set())
        self.assertEqual(self.d.agents, {})
        self.assertTrue(self.d.handle({'op': 'list'})['ok'])

    def test_holder_and_operator(self):
        self.assertTrue(self.d.handle({'op': 'give_control', 'to': 'claude'}, identity='operator')['ok'])
        self.assertTrue(self.d.handle({'op': 'give_control', 'to': 'codex'}, identity='claude')['ok'])
        r = self.d.handle({'op': 'give_control', 'to': 'claude', 'from': 'codex'}, identity='deepseek')
        self.assertTrue(r.get('refused'), r)
        self.assertEqual(self.d.holder(), 'codex')
        self.assertTrue(self.d.handle({'op': 'take_control'}, identity='operator')['ok'])
        self.assertEqual(self.d.holder(), 'operator')
        with patch.object(self.d, 'op_send', return_value={}) as send:
            self.assertTrue(self.d.handle({'op': 'send', 'from': 'operator'}, identity='deepseek')['refused'])
            send.assert_not_called()

    def test_peer_environment_fail_closed(self):
        peer, other = socket.socketpair()
        with peer, other:
            for raw, expected in ((b'LILJACK_AGENT=deepseek\0', 'deepseek'),
                                  (b'LILJACK_AGENT=invalid\0CODEX_HOME=x\0', ''),
                                  (b'CODEX_HOME=x\0', 'codex'),
                                  (b'CLAUDECODE=1\0', 'claude'), (b'', '')):
                with patch.object(D.Path, 'read_bytes', return_value=raw):
                    self.assertEqual(D.peer_identity(peer), expected)
            with patch.object(D.Path, 'read_bytes', side_effect=PermissionError):
                self.assertEqual(D.peer_identity(peer), '')

    def test_socket_binds_launch_identity(self):
        self.d.serve(block=False)
        code = """
import json, socket, sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.connect(sys.argv[1])
f=s.makefile('rwb',buffering=0)
for req in json.loads(sys.argv[2]):
 f.write((json.dumps(req)+'\\n').encode())
 print(f.readline().decode().strip())
s.close()
"""
        # Foreground client only. The server binds SO_PEERCRED to this child's
        # initial environment; later requests cannot change that binding.
        env = {'PATH': os.defpath, 'CUDA_VISIBLE_DEVICES': '', 'LILJACK_AGENT': 'deepseek'}
        requests = [dict(op='take_control', who='operator'), dict(op='shutdown', **{'from': 'operator'}),
                    dict(op='take_control'), dict(op='control')]
        r = subprocess.run([sys.executable, '-c', code, str(self.d.sock_path), json.dumps(requests)],
                           env=env, capture_output=True, text=True, timeout=10, check=True)
        rows = [json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual(len(rows), 4)
        for row in rows[:3]:
            self.assertTrue(row.get('refused'), row)
        self.assertEqual(rows[3]['holder'], 'operator')
        self.assertFalse(self.d._stop.is_set())
        for named, allowed in (('operator', True), ('', False)):
            env['LILJACK_AGENT'] = named
            r = subprocess.run([sys.executable, '-c', code, str(self.d.sock_path),
                                json.dumps([{'op': 'take_control'}])],
                               env=env, capture_output=True, text=True, timeout=10, check=True)
            self.assertEqual(json.loads(r.stdout)['ok'], allowed)

    def test_tmux_preserves_raw_history_without_replay(self):
        transcript = D.A.Transcript(Path(self.tmp.name) / 'history.jsonl')
        agent = D.A.TmuxAgent('codex', transcript)
        history = b'previous session\n'
        agent._raw_log.write_bytes(history)
        with patch.object(agent, 'alive', return_value=False), \
             patch.object(agent, 'ensure_session'), \
             patch.object(agent, '_tmux', return_value='123'), \
             patch.object(D.A.threading, 'Thread') as thread:
            agent.spawn(['fixture-only'])
            self.assertEqual(agent._raw_log.read_bytes(), history)
            self.assertEqual(thread.call_args.kwargs['args'], (len(history),))
        with agent._raw_log.open('ab') as fh:
            fh.write(b'new session\n')
        with patch.object(agent, 'alive', return_value=False), \
             patch.object(agent, '_tmux', return_value='0'), \
             patch.object(transcript, 'write') as write:
            agent._pump_loop(len(history))
            write.assert_called_once_with('out', 'new session')
        self.assertEqual(agent._raw_log.read_bytes(), history + b'new session\n')


if __name__ == '__main__':
    unittest.main()
