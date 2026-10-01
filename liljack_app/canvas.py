"""Shared room canvas: one JSON stroke per line in rooms/<room-id>/canvas.jsonl.

Contract: docs/codex/reports/2026-09-21-interactive-room-survey/CANVAS-FORMAT.md.
lilJack (main.c) appends the operator's mouse strokes and polls the file's mtime; agents
append through `liljack room --draw`. The file is append-only so both writers
only ever add bytes; readers tolerate a trailing partial line.
"""
from __future__ import annotations

import json
import os
import re
import secrets
import shutil
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

KINDS = ('line', 'text', 'clear', 'image')
OPS = ('del', 'move', 'style')
_ID = re.compile(r'^[A-Za-z0-9_-]{1,23}$')
AUTHORS = ('operator', 'claude', 'codex', 'deepseek')
_COLOUR = re.compile(r'^#[0-9a-fA-F]{6}$')
MAX_POINTS = 4096
MAX_TEXT = 200


def canvas_path(root, room_id) -> Path:
    """rooms/<room-id>/canvas.jsonl under the workspace root; folder made on demand."""
    room_id = str(room_id or '')
    if not re.match(r'^r-[0-9a-f]{32}$', room_id):
        raise ValueError('canvas needs an exact room id (r-...)')
    folder = Path(root) / 'rooms' / room_id
    folder.mkdir(parents=True, exist_ok=True, mode=0o700)
    return folder / 'canvas.jsonl'


def media_dir(root, room_id) -> Path:
    """rooms/<room-id>/media: every image the canvas or a media: line points at."""
    folder = canvas_path(root, room_id).parent / 'media'
    folder.mkdir(exist_ok=True, mode=0o700)
    return folder


def image_size(path):
    """(width, height) from the file header for png, jpeg, gif, bmp; ValueError otherwise.
    The same formats lilJack's stb_image decodes (tga has no magic; use png)."""
    with open(path, 'rb') as fh:
        head = fh.read(32)
        if head[:8] == b'\x89PNG\r\n\x1a\n' and head[12:16] == b'IHDR':
            return struct.unpack('>II', head[16:24])
        if head[:6] in (b'GIF87a', b'GIF89a'):
            return struct.unpack('<HH', head[6:10])
        if head[:2] == b'BM' and len(head) >= 26:
            w, h = struct.unpack('<ii', head[18:26])
            return w, abs(h)
        if head[:2] == b'\xff\xd8':
            fh.seek(2)
            while True:
                marker = fh.read(2)
                if len(marker) < 2 or marker[0] != 0xff:
                    break
                if marker[1] in (0xd8, 0x01) or 0xd0 <= marker[1] <= 0xd7:
                    continue
                size = fh.read(2)
                if len(size) < 2:
                    break
                length = struct.unpack('>H', size)[0]
                if 0xc0 <= marker[1] <= 0xcf and marker[1] not in (0xc4, 0xc8, 0xcc):
                    h, w = struct.unpack('>xHH', fh.read(5))
                    return w, h
                fh.seek(length - 2, 1)
    raise ValueError('not an image the canvas can show (png, jpg, gif, bmp): %s' % path)


def import_file(root, room_id, src) -> Path:
    """Copy src into the room's media folder as <UTC time>-<name> (unique); a file
    already there is returned as is. Same naming as lj_canvas_import in c_canvas.c."""
    src = Path(src).resolve()
    if not src.is_file():
        raise ValueError('not a file: %s' % src)
    folder = media_dir(root, room_id).resolve()
    if src.parent == folder:
        return src
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    for k in range(1000):
        dest = folder / ('%s-%s' % (stamp, src.name) if not k else '%s-%d-%s' % (stamp, k, src.name))
        try:
            fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            continue
        with os.fdopen(fd, 'wb') as out, open(src, 'rb') as inp:
            shutil.copyfileobj(inp, out)
            out.flush()
            os.fsync(out.fileno())
        return dest
    raise ValueError('no free media name for %s' % src.name)


def default_rect(width, height, aspect=16 / 9):
    """Centred rect keeping the image's aspect on the 16:9 picture, <= 60% of it."""
    a = (width / height) / aspect
    w, h = 0.6, 0.6 / a
    if h > 0.6:
        h, w = 0.6, 0.6 * a
    return (0.5 - w / 2, 0.5 - h / 2, 0.5 + w / 2, 0.5 + h / 2)


def append_image(root, room_id, src, author, rect=None) -> dict:
    """Validate src is an image, copy it into media/, append one image row."""
    width, height = image_size(src)
    x0, y0, x1, y1 = rect if rect is not None else default_rect(width, height)
    stroke = {'t': 'image', 'p': [[x0, y0], [x1, y1]], 'f': '(pending)'}
    validate(stroke, author, media=None)                  # rect checks before any copy
    stroke['f'] = str(import_file(root, room_id, src))
    return append(root, room_id, [stroke], author)


def validate(stroke, author, media='room') -> dict:
    """Return a normalized stroke dict or raise ValueError. `author` overrides `by`."""
    if not isinstance(stroke, dict):
        raise ValueError('stroke must be a JSON object')
    kind = stroke.get('t')
    if kind in OPS:
        return validate_op(stroke, author)
    if kind not in KINDS:
        raise ValueError('t must be one of ' + ', '.join(KINDS + OPS))
    if author not in AUTHORS:
        raise ValueError('by must be one of ' + ', '.join(AUTHORS))
    out = {'t': kind, 'by': author,
           'at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
    if kind == 'clear':
        return out
    out['id'] = secrets.token_hex(6)
    points = stroke.get('p')
    if not isinstance(points, list) or not points or len(points) > MAX_POINTS:
        raise ValueError('p must be a non-empty list of [x, y] pairs (max %d)' % MAX_POINTS)
    norm = []
    for pt in points:
        if not (isinstance(pt, (list, tuple)) and len(pt) == 2):
            raise ValueError('each point is [x, y]')
        x, y = pt
        if not all(isinstance(v, (int, float)) and 0.0 <= v <= 1.0 for v in (x, y)):
            raise ValueError('points are normalized 0..1')
        norm.append([round(float(x), 4), round(float(y), 4)])
    out['p'] = norm
    if kind == 'image':
        if len(norm) != 2 or not (norm[0][0] < norm[1][0] and norm[0][1] < norm[1][1]):
            raise ValueError('image p is [[x0, y0], [x1, y1]] with x0 < x1 and y0 < y1')
        f = stroke.get('f')
        if not isinstance(f, str) or not f:
            raise ValueError('image f is the file path')
        if media is not None:
            path = Path(f)
            if not path.is_absolute() or path.resolve().parent != Path(media) or not path.is_file():
                raise ValueError('image f must be a file in the room media folder (use --draw-image PATH)')
        out['f'] = f
        return out
    colour = stroke.get('c')
    if colour is not None:
        if not isinstance(colour, str) or not _COLOUR.match(colour):
            raise ValueError('c is #rrggbb')
        out['c'] = colour.lower()
    if kind == 'line':
        width = stroke.get('w', 2)
        if not isinstance(width, (int, float)) or not 1 <= width <= 100:
            raise ValueError('w is 1..100')
        out['w'] = width
    else:
        text = stroke.get('s')
        if not isinstance(text, str) or not text.strip() or len(text) > MAX_TEXT:
            raise ValueError('s is the text to draw (1..%d chars)' % MAX_TEXT)
        out['s'] = text.replace('\n', ' ')
    return out


def validate_op(op, author) -> dict:
    """del / move / style rows: the same checks as lj_canvas_append_op in c_canvas.c."""
    if author not in AUTHORS:
        raise ValueError('by must be one of ' + ', '.join(AUTHORS))
    ids = op.get('ids')
    if not isinstance(ids, list) or not 1 <= len(ids) <= 1024 or not all(isinstance(i, str) and _ID.match(i) for i in ids):
        raise ValueError('ids: 1..1024 stroke ids of [A-Za-z0-9_-] (see --canvas-list)')
    out = {'t': op['t'], 'ids': ids}
    if op['t'] == 'move':
        d = op.get('d')
        if not (isinstance(d, list) and len(d) == 2 and all(isinstance(v, (int, float)) and -1 <= v <= 1 for v in d)):
            raise ValueError('move d is [dx, dy], each in -1..1')
        out['d'] = [round(float(v), 4) for v in d]
    elif op['t'] == 'style':
        if 'c' in op:
            if not isinstance(op['c'], str) or not _COLOUR.match(op['c']):
                raise ValueError('c is #rrggbb')
            out['c'] = op['c'].lower()
        if 'w' in op:
            if not isinstance(op['w'], (int, float)) or not 1 <= op['w'] <= 100:
                raise ValueError('w is 1..100')
            out['w'] = op['w']
        if len(out) == 2:
            raise ValueError('style needs c and/or w')
    out.update({'by': author, 'at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')})
    return out


def append(root, room_id, strokes, author) -> dict:
    """Validate every stroke first, then append them all in one write."""
    media = str(media_dir(root, room_id).resolve()) if any(
        isinstance(s, dict) and s.get('t') == 'image' for s in strokes) else None
    rows = [validate(s, author, media=media) for s in strokes]
    if not rows:
        raise ValueError('nothing to draw')
    path = canvas_path(root, room_id)
    data = ''.join(json.dumps(r, ensure_ascii=False, separators=(',', ':')) + '\n' for r in rows)
    with open(path, 'a', encoding='utf-8') as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    write_scene(root, room_id)
    return {'canvas': str(path), 'appended': len(rows), 'by': author}


def read(root, room_id) -> list:
    """All strokes since the last clear with del/move/style applied in file
    order; a trailing partial line is ignored. Rows without an id get L<line>,
    exactly as c_canvas.c numbers them."""
    path = canvas_path(root, room_id)
    if not path.exists():
        return []
    strokes = []
    lines = path.read_text(encoding='utf-8').split('\n')
    for lineno, line in enumerate(lines[:-1], 1):          # the last piece is partial or empty
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if not isinstance(row, dict):
            continue
        kind = row.get('t')
        if kind == 'clear':
            strokes = []
        elif kind in OPS:
            ids = set(i for i in row.get('ids', []) if isinstance(i, str))
            if kind == 'del':
                strokes = [s for s in strokes if s['id'] not in ids]
            for s in strokes:
                if s['id'] not in ids:
                    continue
                if kind == 'move':
                    dx, dy = row.get('d', [0, 0])
                    s['p'] = [[min(1, max(0, x + dx)), min(1, max(0, y + dy))] for x, y in s['p']]
                elif kind == 'style':
                    s.update({k: row[k] for k in ('c', 'w') if k in row})
        elif kind in ('line', 'text', 'image'):
            if not (isinstance(row.get('id'), str) and _ID.match(row['id'])):
                row['id'] = 'L%d' % lineno
            strokes.append(row)
    return strokes


def listing(root, room_id) -> list:
    """What an agent needs to select: id, kind, author, bbox, and the words or file."""
    out = []
    for s in read(root, room_id):
        xs = [p[0] for p in s.get('p', [])] or [0]
        ys = [p[1] for p in s.get('p', [])] or [0]
        row = {'id': s['id'], 't': s['t'], 'by': s.get('by'), 'bbox': [min(xs), min(ys), max(xs), max(ys)]}
        for k in ('s', 'f', 'c', 'w'):
            if k in s:
                row[k] = s[k]
        out.append(row)
    return out


def undo(root, room_id, author) -> dict:
    mine = [s for s in read(root, room_id) if s.get('by') == author]
    if not mine:
        raise ValueError('nothing of %s left to undo' % author)
    return append(root, room_id, [{'t': 'del', 'ids': [mine[-1]['id']]}], author)


PNG_MAX_SIDE = 8192
PNG_MAX_PIXELS = 16_000_000


def parse_size(text) -> tuple:
    """'WxH' -> (w, h): two integers 16..8192, at most 16M pixels in total."""
    m = re.fullmatch(r'\s*(\d{1,5})\s*[xX]\s*(\d{1,5})\s*', str(text or ''))
    if not m:
        raise ValueError('--size is WxH, e.g. 1600x900')
    w, h = int(m.group(1)), int(m.group(2))
    if not (16 <= w <= PNG_MAX_SIDE and 16 <= h <= PNG_MAX_SIDE) or w * h > PNG_MAX_PIXELS:
        raise ValueError('--size sides are 16..%d and at most %d pixels in total' % (PNG_MAX_SIDE, PNG_MAX_PIXELS))
    return w, h


_AUTHOR_BIT = {'operator': 1, 'claude': 2, 'codex': 4, 'deepseek': 8}


def hidden_bits(only=None) -> int:
    """c_canvas.h lj_canvas_author_bit mask that hides everyone except `only`."""
    if not only:
        return 0
    if only not in AUTHORS:
        raise ValueError('--by is one of ' + ', '.join(AUTHORS))
    return 31 & ~_AUTHOR_BIT[only]


def png(root, room_id, out, width=1600, height=900, only=None) -> dict:
    """Rasterize the room canvas (lines, notes, images) to a PNG with lilJack's own
    renderer (c_canvas.c via the canvas-png helper), so an agent sees what the operator sees."""
    width, height = parse_size('%sx%s' % (width, height))
    canvas_path(root, room_id)
    build = Path(__file__).resolve().parent / 'build.sh'
    helper = subprocess.run(['bash', str(build), 'canvas-png'], capture_output=True, text=True)
    if helper.returncode:
        raise RuntimeError('canvas-png helper did not build: ' + helper.stderr.strip()[-400:])
    done = subprocess.run([helper.stdout.strip().splitlines()[-1], str(root), room_id, str(out), str(width), str(height),
                           str(hidden_bits(only))], capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError(done.stderr.strip() or 'canvas-png failed')
    return {'png': str(out), 'width': width, 'height': height, 'strokes': len(read(root, room_id))}


# ── Per-member views: one canvas, each model/agent reads it in its own format ──
# rooms/<room>/views.json maps a member (agent name, or exact session id, which
# wins) to a list of formats. `liljack room --canvas-view` exports the canvas
# in the caller's formats into rooms/<room>/views/. 'live' is lilJack's sixel tile.
VIEW_FORMATS = ('png', 'jpeg', 'svg', 'pdf', 'hires', 'ascii', 'unicode', 'ansi', 'live')
_AUTHOR_COLOUR = {'operator': '#ffd166', 'claude': '#33ccff', 'codex': '#7ee787', 'deepseek': '#ff7b72'}
SVG_W, SVG_H = 1600, 900


def views_path(root, room_id) -> Path:
    return canvas_path(root, room_id).parent / 'views.json'


def get_views(root, room_id) -> dict:
    try:
        data = json.loads(views_path(root, room_id).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def set_view(root, room_id, spec) -> dict:
    """'who=fmt[+fmt]'; who is an agent name or an exact session id."""
    who, _, fmts = str(spec).partition('=')
    who = who.strip()
    if not (who in AUTHORS or re.fullmatch(r's-[0-9a-f]{32}', who)):
        raise ValueError('--set-view WHO=FMT: who is one of %s or a session id (s-...)' % ', '.join(AUTHORS))
    formats = [f.strip().lower().replace('jpg', 'jpeg') for f in fmts.split('+') if f.strip()]
    if not formats or any(f not in VIEW_FORMATS for f in formats):
        raise ValueError('--set-view formats: %s (join several with +)' % ', '.join(VIEW_FORMATS))
    views = get_views(root, room_id)
    views[who] = list(dict.fromkeys(formats))
    path = views_path(root, room_id)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(views, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(tmp, path)
    return {'views': views}


def view_of(root, room_id, who, session=None) -> list:
    views = get_views(root, room_id)
    return list(views.get(session) or views.get(who) or ['png'])


def _path_d(pts):
    """The same curve c_canvas.c draws (midpoint quadratics through each inner
    point), as native SVG Q commands, so the SVG matches the tile exactly."""
    if len(pts) < 3:
        return ' '.join('%s%.1f %.1f' % ('M' if i == 0 else 'L', x, y) for i, (x, y) in enumerate(pts))
    mid = lambda a, b: ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    m = mid(pts[0], pts[1])
    d = ['M%.1f %.1f' % tuple(pts[0]), 'L%.1f %.1f' % m]
    for i in range(1, len(pts) - 1):
        m = mid(pts[i], pts[i + 1])
        d.append('Q%.1f %.1f %.1f %.1f' % (pts[i][0], pts[i][1], m[0], m[1]))
    d.append('L%.1f %.1f' % tuple(pts[-1]))
    return ' '.join(d)


def svg(root, room_id, only=None) -> str:
    """The canvas as SVG an agent can READ and Barva can EDIT: one top-level
    <g id="<agent>"> per author (Barva: one layer per agent), lines are paths,
    notes are real <text>, images are embedded <image> (data URIs). Within a
    layer strokes keep file order; across layers the order is operator, claude,
    codex, deepseek, others (the live tile keeps pure file order)."""
    import base64
    from xml.sax.saxutils import escape, quoteattr
    k = min(SVG_W, SVG_H) / 1000.0
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">' % (SVG_W, SVG_H, SVG_W, SVG_H),
           '<title>lilJack room canvas %s</title>' % escape(room_id),
           '<rect width="%d" height="%d" fill="#101418"/>' % (SVG_W, SVG_H)]
    layers = {}
    for n, row in enumerate(read(root, room_id)):
        by = str(row.get('by', ''))
        if only and by != only:
            continue
        layers.setdefault(by, []).append((n, row))
    order = [a for a in AUTHORS if a in layers] + sorted(a for a in layers if a not in AUTHORS)
    for who in order:
        out.append('<g id=%s data-author=%s>' % (quoteattr(re.sub(r'[^A-Za-z0-9_-]', '_', who) or 'unknown'), quoteattr(who)))
        for n, row in layers[who]:
            by = str(row.get('by', ''))
            colour = row.get('c') or _AUTHOR_COLOUR.get(by, '#dddddd')
            pts = [[x * SVG_W, y * SVG_H] for x, y in row.get('p', [])]
            meta = ' id=%s data-by=%s data-n="%d"' % (quoteattr('s-' + str(row.get('id', n))), quoteattr(by), n)
            if row['t'] == 'image' and len(pts) == 2:
                try:
                    data = Path(row['f']).read_bytes()
                    kind = image_kind(data)
                except (OSError, ValueError):
                    out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="#884444"%s><title>missing %s</title></rect>'
                               % (pts[0][0], pts[0][1], pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], meta, escape(row['f'])))
                    continue
                out.append('<image x="%.1f" y="%.1f" width="%.1f" height="%.1f" preserveAspectRatio="none" href="data:image/%s;base64,%s"%s><title>%s</title></image>'
                           % (pts[0][0], pts[0][1], pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], kind,
                              base64.b64encode(data).decode(), meta, escape(Path(row['f']).name)))
            elif row['t'] == 'text' and pts:
                x, y = pts[0]
                out.append('<circle cx="%.1f" cy="%.1f" r="5" fill="%s"%s/>' % (x, y, colour, meta.replace('" data-by', '-dot" data-by', 1)))
                out.append('<text x="%.1f" y="%.1f" fill="%s" font-family="sans-serif" font-size="28"%s>%s</text>'
                           % (x + 10, y + 10, colour, meta, escape(row.get('s', ''))))
            elif row['t'] == 'line' and pts:
                width = float(row.get('w', 2)) * k
                if len(pts) == 1:
                    out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"%s/>' % (pts[0][0], pts[0][1], width / 2, colour, meta))
                else:
                    d = _path_d(pts)
                    out.append('<path d="%s" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round" stroke-linejoin="round"%s/>'
                               % (d, colour, width, meta))
        out.append('</g>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


def write_scene(root, room_id) -> Path:
    """rooms/<room>/canvas.svg: the Barva-editable scene, rewritten atomically
    after every CLI write (lilJack's own drags refresh it on the next CLI write or
    --canvas-view)."""
    path = canvas_path(root, room_id).parent / 'canvas.svg'
    tmp = path.with_name('.canvas.svg.tmp')
    tmp.write_text(svg(root, room_id), encoding='utf-8')
    os.replace(tmp, path)
    return path


def text_art(png_path, kind, cols=160, rows=45, background=(0x10, 0x14, 0x18)) -> str:
    """The canvas as characters, for models and terminals without images.
    ascii: luminance ramp; unicode: braille, 2x4 dots per character; ansi:
    half blocks with 24-bit colour escapes (cat it in a terminal)."""
    from PIL import Image
    im = Image.open(png_path).convert('RGB')
    def ink(p):
        return max(abs(p[0] - background[0]), abs(p[1] - background[1]), abs(p[2] - background[2]))
    if kind == 'ascii':
        ramp = ' .:-=+*#%@'
        small = im.resize((cols, rows), Image.BOX)
        return '\n'.join(''.join(ramp[min(len(ramp) - 1, ink(small.getpixel((x, y))) * len(ramp) // 200)]
                                  for x in range(cols)) for y in range(rows)) + '\n'
    if kind == 'unicode':
        dots = ((0, 0, 0x01), (0, 1, 0x02), (0, 2, 0x04), (1, 0, 0x08), (1, 1, 0x10), (1, 2, 0x20), (0, 3, 0x40), (1, 3, 0x80))
        small = im.resize((cols * 2, rows * 4), Image.BOX)
        out = []
        for y in range(rows):
            line = ''
            for x in range(cols):
                bits = sum(b for dx, dy, b in dots if ink(small.getpixel((x * 2 + dx, y * 4 + dy))) > 40)
                line += chr(0x2800 + bits) if bits else ' '
            out.append(line)
        return '\n'.join(out) + '\n'
    if kind == 'ansi':
        small = im.resize((cols, rows * 2), Image.BOX)
        out = []
        for y in range(rows):
            cells = []
            for x in range(cols):
                t, b = small.getpixel((x, y * 2)), small.getpixel((x, y * 2 + 1))
                cells.append('\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\u2580' % (t + b))
            out.append(''.join(cells) + '\x1b[0m')
        return '\n'.join(out) + '\n'
    raise ValueError(kind)


def image_kind(data) -> str:
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return 'png'
    if data[:2] == b'\xff\xd8':
        return 'jpeg'
    if data[:4] == b'GIF8':
        return 'gif'
    if data[:2] == b'BM':
        return 'bmp'
    raise ValueError('unknown image type')


def export(root, room_id, formats, stem, only=None) -> dict:
    """Write the canvas as each format to <stem>.<ext>; returns {format: path}.
    `only` keeps one agent's layer."""
    stem = Path(stem)
    stem.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    files = {}
    raster = None
    if 'hires' in formats:
        png(root, room_id, stem.with_name(stem.name + '.hires.png'), 3840, 2160, only)
        files['hires'] = str(stem.with_name(stem.name + '.hires.png'))
    if any(f in ('png', 'jpeg', 'ascii', 'unicode', 'ansi') for f in formats) or ('pdf' in formats and not shutil.which('rsvg-convert')):
        raster = stem.with_suffix('.png')
        png(root, room_id, raster, only=only)
    for kind, ext in (('ascii', '.txt'), ('unicode', '.braille.txt'), ('ansi', '.ans')):
        if kind in formats:
            target = stem.with_name(stem.name + ext)
            target.write_text(text_art(raster, kind), encoding='utf-8')
            files[kind] = str(target)
    if 'png' in formats:
        files['png'] = str(raster)
    if 'jpeg' in formats:
        from PIL import Image
        Image.open(raster).convert('RGB').save(stem.with_suffix('.jpg'), 'JPEG', quality=90)
        files['jpeg'] = str(stem.with_suffix('.jpg'))
    if 'svg' in formats or 'pdf' in formats:
        vector = stem.with_suffix('.svg')
        vector.write_text(svg(root, room_id, only), encoding='utf-8')
        if 'svg' in formats:
            files['svg'] = str(vector)
        if 'pdf' in formats:
            pdf = stem.with_suffix('.pdf')
            if shutil.which('rsvg-convert'):                     # vector PDF: text stays text
                done = subprocess.run(['rsvg-convert', '-f', 'pdf', '-o', str(pdf), str(vector)], capture_output=True, text=True)
                if done.returncode:
                    raise RuntimeError('rsvg-convert: ' + done.stderr.strip()[-300:])
            else:
                from PIL import Image
                Image.open(raster).convert('RGB').save(pdf, 'PDF')
            files['pdf'] = str(pdf)
            if 'svg' not in formats:
                vector.unlink()
    if raster is not None and 'png' not in formats:
        raster.unlink()
    return files


def view(root, room_id, who, session=None, only=None) -> dict:
    formats = view_of(root, room_id, who, session)
    name = (session or who) + ('.' + only if only else '')
    files = export(root, room_id, [f for f in formats if f != 'live'],
                   canvas_path(root, room_id).parent / 'views' / name, only)
    scene = write_scene(root, room_id)
    out = {'who': who, 'formats': formats, 'files': files, 'scene': str(scene), 'strokes': len(read(root, room_id))}
    if only:
        out['only'] = only
    if 'live' in formats:
        out['note'] = 'live: the canvas tile in lilJack (sixel); no file'
    return out
