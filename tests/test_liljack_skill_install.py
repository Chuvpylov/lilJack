"""Exercise the real installer in an isolated home; never modify personal skills."""
import os
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
NAMES = ('room-protocol', 'report-format', 'registry-moves', 'evidence-rules')

class SkillInstallTests(unittest.TestCase):
    def test_install_idempotent_and_preserves_personal_files(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            config = home / '.config/opencode/opencode.jsonc'
            config.parent.mkdir(parents=True)
            original = '// personal settings\n{"skills": {"paths": ["/personal",],}, "theme": "mine",}\n'
            config.write_text(original)
            personal = home / '.codex/skills/personal/SKILL.md'
            personal.parent.mkdir(parents=True)
            personal.write_text('untouched')
            env = dict(os.environ, HOME=temp, CODEX_HOME=str(home / '.codex'),
                       XDG_CONFIG_HOME=str(home / '.config'), LILJACK_BIN_DIR=str(home / 'bin'))
            for _ in range(2):
                result = subprocess.run(['bash', str(REPO / 'liljack_app/install.sh')], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                for name in NAMES:
                    source = REPO / 'plugins/liljack/skills' / name
                    for base in (home / '.codex/skills', home / '.claude/skills', home / '.claude/plugins/liljack-room-skills/skills'):
                        self.assertEqual((base / name).resolve(), source.resolve())
                        self.assertTrue((base / name / 'SKILL.md').is_file())
            text = config.read_text()
            self.assertIn('// personal settings', text)
            self.assertIn('"/personal"', text)
            self.assertIn('"theme": "mine"', text)
            self.assertEqual(text.count(str(REPO / 'plugins/liljack/skills')), 1)
            self.assertEqual(personal.read_text(), 'untouched')

    def test_collision_refused_without_overwriting(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            personal = home / '.codex/skills/room-protocol/SKILL.md'
            personal.parent.mkdir(parents=True)
            personal.write_text('my personal skill')
            env = dict(os.environ, HOME=temp, CODEX_HOME=str(home / '.codex'),
                       XDG_CONFIG_HOME=str(home / '.config'), LILJACK_BIN_DIR=str(home / 'bin'))
            result = subprocess.run(['bash', str(REPO / 'liljack_app/install.sh')], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(personal.read_text(), 'my personal skill')
            self.assertFalse((home / '.claude/skills/room-protocol').exists())

    def test_jsonc_insertions_and_rejection(self):
        spec = importlib.util.spec_from_file_location('install_skills', REPO / 'liljack_app/install_skills.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for original in ('{}', '{"theme":"mine",}', '{"skills":{}}',
                         '{"skills":{"paths":[]}}',
                         '// url comment\n{"url":"https://example.test", "skills":{"paths":["old",],},}'):
            updated = module.config_with_path(original, '/repo/skills')
            self.assertEqual(module.config_with_path(updated, '/repo/skills'), updated)
            self.assertEqual(updated.count('/repo/skills'), 1)
        for bad in ('{', '{"skills":[]}', '{"skills":{"paths":false}}',
                    '{"skills":{},"skills":{}}', '{"skills":{"paths":[5]}}'):
            with self.assertRaises(ValueError):
                module.config_with_path(bad, '/repo/skills')

if __name__ == '__main__':
    unittest.main()
