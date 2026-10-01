"""liljack room --draw / --draw-file / --canvas-clear append validated strokes;
--draw-image / --attach bring images into the room; --canvas-png lets agents see it."""
import json
import os
os.environ.pop("LILJACK_WORKSPACE_SESSION", None)
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolbox'))
from liljack_app import canvas

ROOM = 'r-' + '0' * 32


def png(path, w, h, rgb):
    """A solid w x h PNG, written with zlib only."""
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return Path(path)


class CanvasModuleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_append_creates_folder_and_normalizes(self):
        r = canvas.append(self.root, ROOM, [{'t': 'line', 'p': [[0, 0], [0.5, 1]], 'c': '#33CCFF'}], 'claude')
        path = Path(r['canvas'])
        self.assertEqual(path, self.root / 'rooms' / ROOM / 'canvas.jsonl')
        row = json.loads(path.read_text().splitlines()[0])
        self.assertEqual((row['t'], row['by'], row['c'], row['w']), ('line', 'claude', '#33ccff', 2))
        self.assertEqual(row['p'], [[0.0, 0.0], [0.5, 1.0]])

    def test_rejects_bad_strokes_before_writing(self):
        for bad in ({'t': 'blob', 'p': [[0, 0]]}, {'t': 'line', 'p': [[2, 0]]}, {'t': 'line', 'p': []},
                    {'t': 'text', 'p': [[0, 0]]}, {'t': 'line', 'p': [[0, 0]], 'c': 'red'}):
            with self.assertRaises(ValueError):
                canvas.append(self.root, ROOM, [bad], 'claude')
        with self.assertRaises(ValueError):
            canvas.append(self.root, ROOM, [{'t': 'clear'}], 'root')
        self.assertFalse((self.root / 'rooms' / ROOM / 'canvas.jsonl').exists())

    def test_read_honours_clear_and_partial_line(self):
        canvas.append(self.root, ROOM, [{'t': 'line', 'p': [[0, 0], [1, 1]]}], 'operator')
        canvas.append(self.root, ROOM, [{'t': 'clear'}, {'t': 'text', 'p': [[0.5, 0.5]], 's': 'hi'}], 'codex')
        with open(canvas.canvas_path(self.root, ROOM), 'a') as fh:
            fh.write('{"t":"line","p":[[0,')
        rows = canvas.read(self.root, ROOM)
        self.assertEqual([(r['t'], r['by']) for r in rows], [('text', 'codex')])


class CliCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'ws'
        # Operator CLI path: LILJACK_AGENT=operator with no bound session (see cli.caller).
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('LILJACK_')}
        self.env.update({'PYTHONPATH': str(ROOT / 'toolbox') + os.pathsep + str(ROOT), 'LILJACK_AGENT': 'operator'})

    def run_room(self, *args):
        return subprocess.run([sys.executable, '-m', 'liljack_app.cli', 'room', '--root', str(self.root),
                               '--project', self.tmp.name, '--to', ROOM, *args],
                              capture_output=True, text=True, env=self.env, cwd=str(ROOT))


class CanvasCliTests(CliCase):
    def test_draw_and_clear_verbs(self):
        r = self.run_room('--draw', json.dumps({'t': 'line', 'p': [[0.1, 0.1], [0.9, 0.9]]}))
        self.assertEqual(r.returncode, 0, r.stderr)
        out = json.loads(r.stdout)
        self.assertEqual(out['appended'], 1)
        strokes = Path(self.tmp.name) / 'strokes.jsonl'
        strokes.write_text('{"t":"text","p":[[0.5,0.5]],"s":"here"}\n{"t":"line","p":[[0,1],[1,0]]}\n')
        r = self.run_room('--draw-file', str(strokes))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout)['appended'], 2)
        r = self.run_room('--canvas-clear')
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = (self.root / 'rooms' / ROOM / 'canvas.jsonl').read_text().splitlines()
        self.assertEqual([(json.loads(l)['t'], json.loads(l)['by']) for l in lines],
                         [('line', 'operator'), ('text', 'operator'), ('line', 'operator'), ('clear', 'operator')])
        self.assertEqual(canvas.read(self.root, ROOM), [])

    def test_bad_draw_exits_nonzero(self):
        r = self.run_room('--draw', '{"t":"line","p":[[5,5]]}')
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.root / 'rooms' / ROOM / 'canvas.jsonl').exists())


class CanvasImageTests(CliCase):
    def test_image_rows_validate(self):
        src = png(Path(self.tmp.name) / 'a.png', 4, 2, (255, 0, 0))
        with self.assertRaises(ValueError):          # outside the room's media folder
            canvas.append(self.root, ROOM, [{'t': 'image', 'f': str(src), 'p': [[0, 0], [1, 1]]}], 'claude')
        with self.assertRaises(ValueError):          # empty rect
            canvas.append_image(self.root, ROOM, src, 'claude', (0.5, 0.5, 0.5, 0.9))
        notimg = Path(self.tmp.name) / 'n.txt'; notimg.write_text('hi')
        with self.assertRaises(ValueError):
            canvas.append_image(self.root, ROOM, notimg, 'claude')
        self.assertFalse((self.root / 'rooms' / ROOM / 'canvas.jsonl').exists())

    def test_draw_image_copies_into_media_and_keeps_aspect(self):
        src = png(Path(self.tmp.name) / 'shot one.png', 4, 2, (255, 0, 0))
        r = self.run_room('--draw-image', str(src))
        self.assertEqual(r.returncode, 0, r.stderr)
        row = json.loads((self.root / 'rooms' / ROOM / 'canvas.jsonl').read_text().splitlines()[-1])
        media = self.root / 'rooms' / ROOM / 'media'
        self.assertEqual((row['t'], row['by']), ('image', 'operator'))
        self.assertEqual(Path(row['f']).parent, media)
        self.assertEqual(Path(row['f']).read_bytes(), src.read_bytes())
        (x0, y0), (x1, y1) = row['p']
        self.assertAlmostEqual(((x1 - x0) * 16) / ((y1 - y0) * 9), 2.0, places=2)   # 4:2 on the 16:9 picture
        self.assertAlmostEqual(x0 + x1, 1.0, places=3)
        r = self.run_room('--draw-image', str(src), '--at', '0,0,0.5,1')
        self.assertEqual(r.returncode, 0, r.stderr)
        row = json.loads((self.root / 'rooms' / ROOM / 'canvas.jsonl').read_text().splitlines()[-1])
        self.assertEqual(row['p'], [[0.0, 0.0], [0.5, 1.0]])

    def test_canvas_png_shows_lines_and_images(self):
        src = png(Path(self.tmp.name) / 'red.png', 8, 8, (255, 0, 0))
        self.assertEqual(self.run_room('--draw-image', str(src), '--at', '0,0,0.5,1').returncode, 0)
        self.assertEqual(self.run_room('--draw', json.dumps({'t': 'line', 'c': '#00ff00', 'w': 30,
                                                             'p': [[0.75, 0], [0.75, 1]]})).returncode, 0)
        out = Path(self.tmp.name) / 'eyes.png'
        r = self.run_room('--canvas-png', str(out))
        self.assertEqual(r.returncode, 0, r.stderr)
        info = json.loads(r.stdout)
        self.assertEqual((info['png'], info['width'], info['height']), (str(out), 1600, 900))
        self.assertEqual(out.read_bytes()[:8], b'\x89PNG\r\n\x1a\n')
        try:
            from PIL import Image
        except ImportError:
            return
        im = Image.open(out).convert('RGB')
        self.assertEqual(im.getpixel((200, 450)), (255, 0, 0))     # the image, left half
        self.assertEqual(im.getpixel((1200, 450)), (0, 255, 0))    # the line, right half

    def test_canvas_png_size_is_bounded(self):
        out = Path(self.tmp.name) / 'x.png'
        for bad in ('100000x100000', '8192x8192', '0x900', '-5x900', '1600', 'axb', '1600x900x2', '15x900'):
            r = self.run_room('--canvas-png', str(out), '--size', bad)
            self.assertNotEqual(r.returncode, 0, bad)
            self.assertIn('--size', r.stderr + r.stdout, bad)
        self.assertFalse(out.exists())
        self.assertEqual(canvas.parse_size('4000x4000'), (4000, 4000))

    def test_attach_copies_and_posts_media_line(self):
        r = subprocess.run([sys.executable, '-m', 'liljack_app.cli', 'room', '--root', str(self.root),
                            '--project', self.tmp.name, '--create', 'pics'],
                           capture_output=True, text=True, env=self.env, cwd=str(ROOT))
        self.assertEqual(r.returncode, 0, r.stderr)
        room = json.loads(r.stdout)['created_room']
        src = png(Path(self.tmp.name) / 'screen.png', 2, 2, (0, 0, 255))
        r = subprocess.run([sys.executable, '-m', 'liljack_app.cli', 'room', '--root', str(self.root),
                            '--project', self.tmp.name, '--to', room, '--attach', str(src)],
                           capture_output=True, text=True, env=self.env, cwd=str(ROOT))
        self.assertEqual(r.returncode, 0, r.stderr)
        copies = list((self.root / 'rooms' / room / 'media').iterdir())
        self.assertEqual(len(copies), 1)
        self.assertEqual(copies[0].read_bytes(), src.read_bytes())
        self.assertIn('media: ' + str(copies[0]), r.stdout)


class CanvasViewTests(CliCase):
    """the operator: 'let the user select rendering and visual representation for each
    model and agent in the room'. One canvas, one view format per member."""

    def draw_sample(self):
        png(Path(self.tmp.name) / 'red.png', 8, 8, (255, 0, 0))
        self.assertEqual(self.run_room('--draw-image', str(Path(self.tmp.name) / 'red.png'), '--at', '0,0,0.5,1').returncode, 0)
        self.assertEqual(self.run_room('--draw', json.dumps({'t': 'line', 'c': '#00ff00', 'w': 30,
                                                             'p': [[0.75, 0], [0.75, 1]]})).returncode, 0)
        self.assertEqual(self.run_room('--draw', json.dumps({'t': 'text', 'p': [[0.6, 0.2]], 's': 'fix <this> & that'})).returncode, 0)

    def test_set_view_and_read_back(self):
        r = self.run_room('--set-view', 'claude=svg+png')
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_room('--set-view', 'codex=jpeg')
        self.assertEqual(r.returncode, 0, r.stderr)
        views = json.loads((self.root / 'rooms' / ROOM / 'views.json').read_text())
        self.assertEqual(views, {'claude': ['svg', 'png'], 'codex': ['jpeg']})
        for bad in ('claude=gif', 'claude=', '=svg', 'nobody'):
            self.assertNotEqual(self.run_room('--set-view', bad).returncode, 0, bad)

    def test_svg_keeps_words_shapes_and_images(self):
        self.draw_sample()
        out = canvas.export(self.root, ROOM, ['svg'], Path(self.tmp.name) / 'v')
        svg = Path(out['svg']).read_text()
        self.assertIn('<svg', svg)
        self.assertIn('fix &lt;this&gt; &amp; that', svg)            # the note is TEXT an agent can read
        self.assertIn('stroke="#00ff00"', svg)
        self.assertIn('<image ', svg)
        import xml.dom.minidom
        xml.dom.minidom.parseString(svg)                               # well-formed

    def test_canvas_view_exports_each_callers_formats(self):
        self.draw_sample()
        self.assertEqual(self.run_room('--set-view', 'operator=jpeg+pdf+svg+png').returncode, 0)
        r = self.run_room('--canvas-view')
        self.assertEqual(r.returncode, 0, r.stderr)
        out = json.loads(r.stdout)
        self.assertEqual(out['who'], 'operator')
        self.assertEqual(sorted(out['files']), ['jpeg', 'pdf', 'png', 'svg'])
        self.assertEqual(Path(out['files']['jpeg']).read_bytes()[:2], b'\xff\xd8')
        self.assertEqual(Path(out['files']['pdf']).read_bytes()[:4], b'%PDF')
        self.assertEqual(Path(out['files']['png']).read_bytes()[:4], b'\x89PNG')
        for f in out['files'].values():
            self.assertEqual(Path(f).parent, self.root / 'rooms' / ROOM / 'views')

    def test_default_view_is_png_and_live_is_not_a_file(self):
        self.draw_sample()
        out = json.loads(self.run_room('--canvas-view').stdout)
        self.assertEqual(list(out['files']), ['png'])
        self.assertEqual(self.run_room('--set-view', 'operator=live').returncode, 0)
        out = json.loads(self.run_room('--canvas-view').stdout)
        self.assertEqual(out['files'], {})
        self.assertIn('live', out['note'])


class CanvasEditTests(CliCase):
    """Agents select, erase, move and undo by stroke id, same ops as lilJack's toolbar."""

    def listing(self):
        r = self.run_room('--canvas-list')
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)['strokes']

    def test_list_erase_move_undo(self):
        self.run_room('--draw', json.dumps({'t': 'line', 'p': [[0.1, 0.5], [0.9, 0.5]]}))
        self.run_room('--draw', json.dumps({'t': 'text', 'p': [[0.5, 0.2]], 's': 'label'}))
        rows = self.listing()
        self.assertEqual([(r['t'], r['by']) for r in rows], [('line', 'operator'), ('text', 'operator')])
        self.assertTrue(all(r['id'] and len(r['bbox']) == 4 for r in rows))
        self.assertEqual(rows[1]['s'], 'label')
        line, note = rows[0]['id'], rows[1]['id']
        self.assertEqual(self.run_room('--canvas-move', line, '--by-delta', '0,0.2').returncode, 0)
        self.assertAlmostEqual(self.listing()[0]['bbox'][1], 0.7, places=3)
        self.assertEqual(self.run_room('--erase', note).returncode, 0)
        self.assertEqual([r['id'] for r in self.listing()], [line])
        self.assertEqual(self.run_room('--undo').returncode, 0)
        self.assertEqual(self.listing(), [])
        self.assertNotEqual(self.run_room('--undo').returncode, 0)        # nothing left of operator's
        self.assertNotEqual(self.run_room('--erase', 'bad id!').returncode, 0)

    def test_legacy_rows_get_line_ids_matching_c(self):
        path = canvas.canvas_path(self.root, ROOM)
        path.write_text('{"t":"line","p":[[0,0],[1,1]],"by":"codex"}\n{"t":"clear","by":"operator"}\n'
                        '{"t":"line","p":[[0,1],[1,0]],"by":"codex"}\n')
        self.assertEqual([r['id'] for r in self.listing()], ['L3'])


class BarvaSceneTests(CliCase):
    """canvas.svg is the Barva-editable scene: one top-level <g> per agent, which
    Barva's importer (hui_vec_import_svg) turns into one layer per agent."""

    def setUp(self):
        super().setUp()
        self.probe = Path(self.tmp.name) / 'barva_probe'
        hui = ROOT.parent / 'hui'
        r = subprocess.run(['gcc', '-std=c11', '-O1', '-w', '-I', str(hui), str(ROOT / 'tests' / 'barva_import_probe.c'),
                            '-lm', '-o', str(self.probe)], capture_output=True, text=True)
        if r.returncode:
            self.skipTest('Barva (hui_vec) probe does not build: ' + r.stderr[-300:])

    def draw_three_agents(self):
        for who, pts in (('operator', [[0.1, 0.1], [0.3, 0.3]]), ('claude', [[0.5, 0.5], [0.6, 0.9]]), ('codex', [[0.7, 0.1], [0.9, 0.2]])):
            canvas.append(self.root, ROOM, [{'t': 'line', 'p': pts, 'w': 8}], who)   # other agents' own writes
        self.run_room('--draw', json.dumps({'t': 'line', 'p': [[0.2, 0.8], [0.4, 0.8]]}))

    def test_scene_is_materialized_with_a_layer_per_agent(self):
        self.draw_three_agents()
        scene = self.root / 'rooms' / ROOM / 'canvas.svg'
        self.assertTrue(scene.exists())                                  # written on every CLI write
        out = subprocess.run([str(self.probe), str(scene)], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        layers = dict((l.split()[1], int(l.split()[2])) for l in out.stdout.splitlines() if l.startswith('layer '))
        self.assertEqual(layers, {'operator': 2, 'claude': 1, 'codex': 1})
        self.assertIn('size 1600 900', out.stdout)

    def test_view_of_one_agent(self):
        self.draw_three_agents()
        out = json.loads(self.run_room('--canvas-view', '--by', 'codex').stdout)
        svg = Path(out['files']['png'].replace('.png', '.svg')) if 'svg' in out['files'] else None
        self.assertEqual(out['only'], 'codex')
        from PIL import Image
        with Image.open(out['files']['png']) as raw:
            im = raw.convert('RGB')
        self.assertEqual(im.getpixel((int(0.8 * 1600), int(0.15 * 900))), (0x7e, 0xe7, 0x87))   # codex's line
        self.assertEqual(im.getpixel((int(0.2 * 1600), int(0.2 * 900))), (0x10, 0x14, 0x18))     # operator's is hidden

    def test_text_views_ascii_unicode_ansi_and_hires(self):
        self.draw_three_agents()
        self.assertEqual(self.run_room('--set-view', 'operator=ascii+unicode+ansi+hires').returncode, 0)
        out = json.loads(self.run_room('--canvas-view').stdout)
        self.assertEqual(sorted(out['files']), ['ansi', 'ascii', 'hires', 'unicode'])
        ascii_art = Path(out['files']['ascii']).read_text()
        rows = ascii_art.rstrip('\n').split('\n')
        self.assertEqual((len(rows), len(rows[0])), (45, 160))
        self.assertTrue(all(ord(ch) < 128 for ch in ascii_art))
        self.assertGreater(sum(ch not in ' \n' for ch in ascii_art), 20)    # the lines are there
        uni = Path(out['files']['unicode']).read_text()
        self.assertTrue(any(0x2801 <= ord(ch) <= 0x28ff for ch in uni))   # braille dots
        self.assertIn('\x1b[38;2;', Path(out['files']['ansi']).read_text())
        from PIL import Image
        with Image.open(out['files']['hires']) as im:
            self.assertEqual(im.size, (3840, 2160))


if __name__ == '__main__':
    unittest.main()
