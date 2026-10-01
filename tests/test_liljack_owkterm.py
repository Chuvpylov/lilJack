"""Native app launcher and content-cache contracts, isolated from live sessions.

owkTerm sources live in brain/owkTerm; lilJack's build.sh delegates there with LILJACK_DIR set."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'toolbox'))
import ball
from liljack_app import cli


class OwktermTests(unittest.TestCase):
    def test_ball_and_cli_forward_owkterm_arguments(self):
        for launch in (lambda: ball.main(['ball.py', 'owkterm', 'literal value']),
                       lambda: cli.main(['owkterm', 'literal value'])):
            with patch.object(os, 'execv', side_effect=SystemExit) as execute:
                with self.assertRaises(SystemExit):
                    launch()
            execute.assert_called_once_with(str(REPO / 'liljack'),
                                            [str(REPO / 'liljack'), 'owkterm', 'literal value'])

    def test_native_cache_tracks_vt_and_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / 'lilJack' / 'liljack_app'
            app.mkdir(parents=True)
            owk = root / 'owkTerm'
            owk.mkdir()
            shutil.copy(REPO / 'liljack_app/build.sh', app / 'build.sh')
            shutil.copy(REPO.parent / 'owkTerm' / 'build.sh', owk / 'build.sh')
            (owk / 'owkterm_app.c').write_text('int value(void); int main(void) { return value(); }\n')
            (app / 'owkterm_vt.c').write_text('int value(void) { return 0; }\n')
            (app / 'c_render.c').write_text('/* fixture renderer */\n')
            (app / 'owkterm_vt.h').write_text('/* fixture header */\n')
            (app / 'c_render.h').write_text('/* fixture header */\n')
            env = dict(os.environ, LILJACK_BUILD_ROOT=str(root / 'cache'),
                       LILJACK_HUI=str(REPO.parent / 'hui'))
            def build():
                return subprocess.check_output(['bash', str(app / 'build.sh'), 'owkterm'],
                                               env=env, text=True).strip()
            first = build()
            self.assertEqual(first, build())
            (app / 'owkterm_vt.c').write_text('int value(void) { return 9; }\n')
            second = build()
            self.assertNotEqual(first, second)
            self.assertEqual(subprocess.run([second]).returncode, 9)
            (app / 'c_render.c').write_text('/* changed renderer */\n')
            self.assertNotEqual(second, build())

    def test_host_cache_tracks_included_deck_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / 'lilJack' / 'liljack_app'
            app.mkdir(parents=True)
            owk = root / 'owkTerm'
            owk.mkdir()
            deck = root / 'the handheld' / 'app'
            deck.mkdir(parents=True)
            shutil.copy(REPO / 'liljack_app/build.sh', app / 'build.sh')
            shutil.copy(REPO.parent / 'owkTerm' / 'build.sh', owk / 'build.sh')
            (owk / 'owkterm_host.c').write_text('#include "../the handheld/app/owkterm.c"\n')
            source = deck / 'owkterm.c'
            source.write_text('int main(void) { return 0; }\n')
            (root / 'the handheld' / 'hui').symlink_to(REPO.parent / 'the handheld' / 'hui', target_is_directory=True)
            env = dict(os.environ, LILJACK_BUILD_ROOT=str(root / 'cache'),
                       LILJACK_HUI=str(REPO.parent / 'hui'))
            def build():
                return subprocess.check_output(['bash', str(app / 'build.sh'), 'owkterm-host'],
                                               env=env, text=True).strip()
            first = build()
            stamp = Path(first).stat().st_mtime_ns
            self.assertEqual(build(), first)
            self.assertEqual(Path(first).stat().st_mtime_ns, stamp)
            source.write_text('int main(void) { return 7; }\n')
            second = build()
            self.assertNotEqual(first, second)
            self.assertEqual(subprocess.run([second]).returncode, 7)


if __name__ == '__main__':
    unittest.main(verbosity=2)
