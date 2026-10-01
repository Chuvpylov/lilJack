"""App launch identity and operator environment regression checks."""
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import time
import uuid
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_app.runtime import Tmux, app_child_env


def sid_pane(tmux, root, sid):
    """The pane id of a freshly created session, by lilJack's own session id."""
    return next(s for s in tmux.sessions(root) if s['id'] == sid)['pane']


class LaunchTests(unittest.TestCase):
    def test_preflight_is_backend_aware(self):
        with tempfile.TemporaryDirectory() as d:
            tmux = Tmux('backend-preflight-test')
            tmux.binary = None
            with patch.object(Tmux, 'command', return_value=['/bin/true']):
                # native never needs the tmux binary
                tmux.preflight('shell', d, backend='native')
                # tmux still demands it
                with self.assertRaises(RuntimeError):
                    tmux.preflight('shell', d, backend='tmux')
            with self.assertRaises(ValueError):
                tmux.preflight('shell', str(Path(d) / 'missing'), backend='native')

    def test_native_intro_argv_and_project_folder_are_separate(self):
        from unittest.mock import MagicMock
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / 'Project'
            folder = Path(directory) / 'Room folder'
            project.mkdir(); folder.mkdir()
            tmux = Tmux('intro-test')
            tmux.binary = '/test/tmux'
            for agent in ('claude', 'codex', 'deepseek', 'shell'):
                with self.subTest(agent=agent), patch('liljack_app.runtime.shutil.which', return_value='/bin/true'), patch.object(tmux, 'call') as call:
                    intro = 'Room purpose and role; literal $(do-not-execute) `literal`'
                    tmux.create(agent, project, directory, cwd=folder, intro=intro)
                    args = call.call_args_list[0].args
                    self.assertEqual(args[args.index('-c')+1], str(folder))
                    self.assertIn('LILJACK_WORKSPACE_PROJECT='+str(project), args)
                    self.assertNotIn('-x', args)
                    self.assertNotIn('-y', args)
                    self.assertFalse(any(c.args[0] == 'resize-window' for c in call.call_args_list))
                    self.assertIn(intro, args)
                    if agent in ('claude','codex'):
                        self.assertEqual(args[args.index(intro)-1], '--')
                    elif agent == 'deepseek':
                        self.assertEqual(args[args.index(intro)-1], '--prompt')
                    self.assertFalse(any(c.args[0] == 'send-keys' for c in call.call_args_list))
            output = 'lj-new\t%1\tcodex\t'+str(folder)+'\t0\t'+str(project)+'\n'
            output += 'lj-old\t%2\tclaude\t'+str(project)+'\t0\t\n'
            output += 'lj-foreign\t%3\tcodex\t'+str(folder)+'\t0\t/another-project\n'
            with patch.object(tmux, 'call', return_value=MagicMock(returncode=0, stdout=output)):
                rows = tmux.sessions(project)
            self.assertEqual([r['tmux_name'] for r in rows], ['lj-new','lj-old'])
            self.assertEqual(rows[0]['cwd'], str(folder))
            self.assertEqual(rows[0]['project'], str(project))

    def test_shell_intro_is_literal_data_not_shell_code(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / 'must-not-exist'
            intro = 'literal $(touch '+str(marker)+') `touch '+str(marker)+'`'
            tmux = Tmux('shell-intro-test')
            tmux.binary = '/test/tmux'
            with patch.dict(os.environ, {'SHELL':'/bin/true'}), patch.object(tmux,'call') as call:
                tmux.create('shell', directory, directory, intro=intro)
            args = call.call_args_list[0].args
            wrapper = args[args.index(sys.executable):]
            result = subprocess.run(wrapper, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout.strip(), intro)
            self.assertFalse(marker.exists())

    def test_ball_liljack_preserves_operator_cuda_environment(self):
        import runpy
        class Launched(Exception):
            pass
        for value in (None, '', '2'):
            with self.subTest(value=value), patch.dict(os.environ, {}, clear=False), patch.object(sys, 'argv', ['ball.py', 'liljack', '--tui']), patch('os.execv', side_effect=Launched) as launch:
                os.environ.pop('CUDA_VISIBLE_DEVICES', None)
                if value is not None:
                    os.environ['CUDA_VISIBLE_DEVICES'] = value
                with self.assertRaises(Launched):
                    runpy.run_path(str(Path(__file__).resolve().parents[1] / 'ball.py'), run_name='__main__')
                self.assertEqual(os.environ.get('CUDA_VISIBLE_DEVICES'), value)
                self.assertEqual('CUDA_VISIBLE_DEVICES' in os.environ, value is not None)
                self.assertEqual(launch.call_args.args[1][1:], ['--tui'])

    def test_no_test_types_into_a_real_shell(self):
        """⚠ A TEST MUST NOT TYPE INTO A SHELL IT HAS NOT ISOLATED FROM $HOME.

        test_real_tmux_session_environment used to send-keys a python one-liner
        into a pane and poll for its output file. The tmux SOCKET was isolated,
        so no live agent was ever touched — but a socket is not a HOME. The pane
        ran a normal bash against the operator's own $HISTFILE, so every run
        appended the probe to ~/.bash_history; the operator found 72 of them on
        2026-09-10 and asked, reasonably, why lilJack was typing CUDA commands
        into his shell.

        The environment of a process is readable from /proc. Ask the kernel, not
        the shell. This guard forbids the shape that caused it: send-keys issued
        against a REAL Tmux object. Asserting on a MOCK (`self.tmux.call.
        assert_called_with('send-keys', ...)`) is a different shape and stays
        legal — that is how the delivery paths are covered without a keystroke.
        """
        offenders = []
        for path in sorted((Path(__file__).resolve().parent).glob('test_*.py')):
            for n, line in enumerate(path.read_text().splitlines(), 1):
                if line.lstrip().startswith('#'):
                    continue
                # ⚠ Built from pieces on purpose: written as a literal, this
                # guard matches its own source and reports itself.
                keys = 'send' + '-keys'
                for q in ("'", '"'):
                    if ('tmux.call(' + q + keys) in line and '.assert_' not in line:
                        offenders.append(f"{path.name}:{n}")
        self.assertEqual(offenders, [],
                         'these tests type into a real shell and will pollute the '
                         "operator's history; read /proc/<pane_pid>/environ instead")

    def test_real_tmux_session_environment(self):
        tmux = Tmux('lj-env-test-' + uuid.uuid4().hex[:10])
        if not tmux.binary:
            self.skipTest('tmux unavailable')
        server = subprocess.Popen([tmux.binary, '-L', tmux.socket, '-f', '/dev/null', '-D'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if tmux.call('show-options', '-g', check=False).returncode == 0:
                    break
                time.sleep(.02)
            self.assertIsNone(server.poll())
            tmux.call('set-environment', '-g', 'CUDA_VISIBLE_DEVICES', 'stale-server-value')
            with tempfile.TemporaryDirectory() as root:
                for value in (None, '', '2'):
                    with self.subTest(value=value), patch.dict(os.environ, {}, clear=False):
                        os.environ.pop('CUDA_VISIBLE_DEVICES', None)
                        if value is not None:
                            os.environ['CUDA_VISIBLE_DEVICES'] = value
                        sid = tmux.create('shell', root, root)
                        # ⚠ NEVER TYPE INTO THE SHELL TO ASK IT A QUESTION.
                        # This used to send-keys `python3 -c "...CUDA_VISIBLE_
                        # DEVICES..." > <tmpdir>/env-<sid>` into the pane and
                        # poll for the file. The tmux SOCKET was isolated, so no
                        # live agent was ever touched — but a socket is not a
                        # HOME: the pane ran a normal bash with the operator's
                        # own $HISTFILE, so every run appended the probe to
                        # ~/.bash_history. the operator found 72 of them in his history
                        # on 2026-09-10 and reasonably asked why lilJack was
                        # typing CUDA commands into his shell.
                        #
                        # The environment of a process is readable directly. Ask
                        # the kernel, not the shell: no keystrokes, no history,
                        # no temp file, no polling, and it asserts the value the
                        # session was actually EXEC'd with rather than whatever
                        # a later interactive shell happens to report.
                        pid = tmux.call('display-message', '-p', '-t', sid_pane(tmux, root, sid),
                                        '#{pane_pid}').stdout.strip()
                        self.assertTrue(pid.isdigit(), 'no pane pid for ' + sid)
                        raw = Path('/proc') / pid / 'environ'
                        env = dict(kv.split('=', 1) for kv in
                                   raw.read_bytes().decode('utf-8', 'replace').split('\0')
                                   if '=' in kv)
                        self.assertEqual(env.get('CUDA_VISIBLE_DEVICES'), value)
                        self.assertEqual('CUDA_VISIBLE_DEVICES' in env, value is not None)

        finally:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()

    def test_operator_cuda_setting_is_preserved(self):
        for value in (None, '', '2'):
            with self.subTest(value=value), patch.dict(os.environ, {}, clear=False):
                os.environ.pop('CUDA_VISIBLE_DEVICES', None)
                if value is not None:
                    os.environ['CUDA_VISIBLE_DEVICES'] = value
                env = app_child_env()
                self.assertEqual(env.get('CUDA_VISIBLE_DEVICES'), value)
                self.assertEqual('CUDA_VISIBLE_DEVICES' in env, value is not None)

    def test_multiple_instances_have_exact_identity_and_harness(self):
        tmux = Tmux('isolated-launch-test')
        ids = []
        with patch('liljack_app.runtime.shutil.which', return_value='/bin/true'), patch.object(tmux, 'call') as call:
            for agent, harness in [('claude', 'claude'), ('claude', 'claude'),
                                   ('deepseek', 'opencode'), ('deepseek', 'opencode'),
                                   ('codex', 'codex'), ('shell', None)]:
                sid = tmux.create(agent, '/tmp', '/tmp/workspace')
                ids.append(sid)
                args = call.call_args_list[-6].args
                self.assertEqual(args[0], 'new-session')
                self.assertIn('LILJACK_WORKSPACE_SESSION=' + sid, args)
                if harness:
                    self.assertIn('LILJACK_HARNESS=' + harness, args)
                else:
                    self.assertIn('LILJACK_HARNESS', args)
                    self.assertEqual(args[args.index('LILJACK_HARNESS') - 1], '-u')
                self.assertIn('LILJACK_AGENT=' + ('operator' if agent == 'shell' else agent), args)
                if agent == 'codex':
                    self.assertIn('--dangerously-bypass-approvals-and-sandbox', args)
                elif agent == 'claude':
                    self.assertIn('--dangerously-skip-permissions', args)
                elif agent == 'deepseek':
                    self.assertIn('OPENCODE_PERMISSION={"*":"allow"}', args)
                else:
                    self.assertFalse(any('bypass' in arg or 'skip-permissions' in arg or 'OPENCODE_PERMISSION=' in arg for arg in args))
        self.assertEqual(len(ids), len(set(ids)))

    def test_new_session_overrides_stale_server_cuda_environment(self):
        tmux = Tmux('isolated-launch-test')
        for value in (None, '', '2'):
            with self.subTest(value=value), patch.dict(os.environ, {}, clear=False), \
                    patch('liljack_app.runtime.shutil.which', return_value='/bin/true'), \
                    patch.object(tmux, 'call') as call:
                os.environ.pop('CUDA_VISIBLE_DEVICES', None)
                if value is not None:
                    os.environ['CUDA_VISIBLE_DEVICES'] = value
                tmux.create('shell', '/tmp', '/tmp/workspace')
                args = call.call_args_list[0].args
                expected = 'CUDA_VISIBLE_DEVICES' + (('=' + value) if value is not None else '')
                if value is None:
                    self.assertEqual(args[args.index('CUDA_VISIBLE_DEVICES') - 1], '-u')
                else:
                    self.assertIn(expected, args)
                    self.assertEqual(args[args.index(expected) - 1], '-e')


if __name__ == '__main__':
    unittest.main(verbosity=2)
