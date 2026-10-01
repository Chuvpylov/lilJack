#!/usr/bin/env python3
"""T2 standing-set-as-system-prompt: the standing instruction set + room id go
to claude as `--append-system-prompt-file` (prompts/<sid>.md under the
workspace root), so they survive /compact and a restart. codex/opencode keep
their argv (the room-folder AGENTS.md fragment carries the set for them)."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_app import runtime                                  # noqa: E402
from liljack_app.room_start import system_prompt_text, _onboarding_lines, LEAD_RULES  # noqa: E402


class RecordingTmux(runtime.Tmux):
    """Real Tmux.create, but `call` records argv instead of spawning tmux."""
    def __init__(self):
        self.binary = '/bin/true'
        self.socket = 'fixture'
        self.calls = []

    def call(self, *args):
        self.calls.append(list(args))
        return ''

    def new_session_argv(self):
        ns = next(c for c in self.calls if c[0] == 'new-session')
        return ns


class SystemPromptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'ws'
        self.root.mkdir()
        self.project = Path(self.tmp.name) / 'proj'
        self.project.mkdir()
        which = mock.patch.object(runtime.shutil, 'which', lambda x: '/usr/bin/' + x)
        which.start()
        self.addCleanup(which.stop)

    # (a) the text itself
    def test_system_prompt_text_has_standing_set_room_and_context_command(self):
        text = system_prompt_text(self.project, 'r-abc')
        for line in _onboarding_lines():
            self.assertIn(line, text)
        for line in LEAD_RULES:
            self.assertIn(line, text)
        self.assertIn('\nRoom: r-abc\n', text)
        self.assertIn('python3 -m liljack_app.cli room --root $LILJACK_WORKSPACE_ROOT '
                      f'--project {self.project} --context --to r-abc', text)

    def _launch(self, agent, room_id='', intro='hello'):
        tmux = RecordingTmux()
        sid = tmux.create(agent, str(self.project), str(self.root), cwd=str(self.project),
                          intro=intro, name='lj-fixture', room_id=room_id)
        return tmux.new_session_argv(), sid

    # (b) claude gets the flag, the file exists and equals system_prompt_text
    def test_claude_argv_carries_system_prompt_file(self):
        argv, sid = self._launch('claude', room_id='r-abc')
        i = argv.index('--append-system-prompt-file')
        self.assertEqual(argv[i - 1], '--dangerously-skip-permissions')
        path = Path(argv[i + 1])
        self.assertTrue(path.exists())
        self.assertEqual(path, self.root / 'prompts' / f'{sid}.md')
        self.assertEqual(path.read_text(), system_prompt_text(self.project, 'r-abc'))
        self.assertIn('Room: r-abc', path.read_text())
        self.assertEqual(argv[-1], 'hello', 'the one-shot intro is still passed')

    # (c) codex argv untouched
    def test_codex_argv_has_no_flag(self):
        argv, _ = self._launch('codex', room_id='r-abc')
        self.assertNotIn('--append-system-prompt-file', argv)
        self.assertFalse((self.root / 'prompts').exists())

    # (d) no room_id: works, file has no Room line
    def test_claude_without_room_id_has_no_room_line(self):
        argv, sid = self._launch('claude')
        path = Path(argv[argv.index('--append-system-prompt-file') + 1])
        text = path.read_text()
        self.assertNotIn('Room:', text)
        self.assertNotIn('--context --to', text)
        self.assertEqual(text, system_prompt_text(self.project, ''))

    def test_claude_without_intro_has_no_flag(self):
        argv, _ = self._launch('claude', room_id='r-abc', intro=None)
        self.assertNotIn('--append-system-prompt-file', argv)


if __name__ == '__main__':
    unittest.main()
