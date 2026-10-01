"""Terminal hardening tests for owkterm_vt.c (exclusive to the C VT, not the app).

Builds the translation unit standalone (libc + wchar/locale only), loads it via
ctypes with the same 24-byte cell ABI the app and the C frontend bind to, and
exercises the hardened paths:

  * scrollback survives a width change (was: wiped 512 lines)
  * batched multi-line scroll (CSI S/T) is correct and bounded
  * hostile CSI (huge params, truncation) never leaves cursor bounds
  * malformed UTF-8 (overlong / surrogate / truncated / lone continuation)
    degrades to U+FFFD without OOB access
  * wide chars at the right edge do not overrun the row

ASan/UBSan evidence for the same cases is produced by the bounded C probe at the
bottom (run `python3 tests/test_liljack_vt_c.py --sanitize`).
"""
import ctypes as C
import hashlib
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
VT_C = HERE.parent / "toolbox" / "liljack_app" / "owkterm_vt.c"

MAXCOLS, MAXROWS = 300, 120


class Cell(C.Structure):
    _fields_ = [("ch", C.c_uint32), ("fg", C.c_uint32), ("bg", C.c_uint32),
                ("marks", C.c_uint32 * 3)]


def _build(flags=("-O2", "-std=c11"), out="liblj_vt.so", extra=()):
    digest = hashlib.sha256(VT_C.read_bytes() + ("".join(flags) + out).encode()).hexdigest()[:12]
    target = Path(tempfile.gettempdir()) / ("lj_vt-" + digest + "-" + out.replace(".so", ".so"))
    if not target.exists():
        subprocess.run(
            ["gcc", "-shared", "-fPIC", *flags, *extra, "-o", str(target), str(VT_C)],
            check=True, timeout=120,
            env={**os.environ, "CUDA_VISIBLE_DEVICES": ""},
        )
    lib = C.CDLL(str(target))
    lib.lj_vt_new.argtypes = [C.c_int, C.c_int]; lib.lj_vt_new.restype = C.c_void_p
    lib.lj_vt_free.argtypes = [C.c_void_p]
    lib.lj_vt_resize.argtypes = [C.c_void_p, C.c_int, C.c_int]
    lib.lj_vt_feed.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    lib.lj_vt_row.argtypes = [C.c_void_p, C.c_int]; lib.lj_vt_row.restype = C.POINTER(Cell)
    lib.lj_vt_scroll.argtypes = [C.c_void_p, C.c_int]
    lib.lj_vt_state.argtypes = [C.c_void_p, C.c_int]; lib.lj_vt_state.restype = C.c_int
    lib.lj_vt_reply.argtypes = [C.c_void_p]; lib.lj_vt_reply.restype = C.c_char_p
    lib.lj_vt_clear_reply.argtypes = [C.c_void_p]
    return lib


def _text(lib, p, row, cols):
    out = []
    for c in range(cols):
        cell = lib.lj_vt_row(p, row)[c]
        if not cell.ch:
            continue
        out.append(chr(cell.ch))
        for m in cell.marks:
            if m:
                out.append(chr(m))
    return "".join(out)


class VTHardeningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = _build()

    def setUp(self):
        self.p = self.lib.lj_vt_new(20, 5)
        self.addCleanup(self.lib.lj_vt_free, self.p)

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.lib.lj_vt_feed(self.p, data, len(data))

    # ── sixel (DCS ... q) ────────────────────────────────────────────────
    def test_dcs_is_consumed_not_printed(self):
        """⚠ ESC P WAS NOT HANDLED AT ALL — it fell through to GROUND and every
        byte of the payload was PRINTED. An agent that drew a picture sprayed
        thousands of characters over its tile; a terminfo query printed itself.
        A DCS we do not implement must vanish, and text after it must survive."""
        self.feed("\x1bP$qm\x1b\\OK")
        self.assertEqual(_text(self.lib, self.p, 0, 20).rstrip(), "OK")

    def test_sixel_draws_half_blocks_in_colour(self):
        """Two pixel rows share one cell: upper -> fg of ▀, lower -> bg. An
        image is therefore ordinary cell content — bounded by the grid, scrolled
        and cleared by the same code as text, owning no memory of its own."""
        # ⚠ Its own grid: the image is 4 columns by TWELVE pixel rows, which is
        # six CELL rows, and the shared 20x5 fixture clips the last band. A test
        # that measured 20 of 24 would be measuring the fixture, not the decoder.
        p = self.lib.lj_vt_new(20, 8)
        self.addCleanup(self.lib.lj_vt_free, p)
        data = "\x1bPq#0;2;100;0;0#0~~~~$-#1;2;0;0;100#1~~~~\x1b\\".encode()
        self.lib.lj_vt_feed(p, data, len(data))
        blocks, colours = 0, set()
        for row in range(8):
            for col in range(20):
                cell = self.lib.lj_vt_row(p, row)[col]
                if cell.ch == 0x2580:
                    blocks += 1
                    colours.add(cell.fg & 0xFFFFFF)
        self.assertEqual(blocks, 24)            # 4 columns x 12 pixel rows / 2
        # ⚠ THE FIRST PARAMETER IS THE INDEX, NOT THE FORMAT. Reading slot 0 as
        # the format made every definition fail its own test, so a two-colour
        # image drew entirely in the first colour.
        self.assertEqual(len(colours), 2)

    def test_tmux_sixel_dialect_repeats_and_colours(self):
        """⚠ THE SEAM: tmux RE-ENCODES sixel, it does not forward the bytes.

        The transport test proves tmux stops eating the DCS; the test above
        proves the parser draws half blocks from a sixel written BY HAND. Two
        verified halves need not join, and here they did not — hand-written test
        sixels use no run-length repeats, so `!n` was never exercised by
        anything until the seam was measured end to end.

        This is the EXACT byte string tmux emits for the same 4x12 two-colour
        image the test above feeds in longhand. It drew 6 half-blocks in 1
        colour instead of 24 in 2, from ONE root cause with two faces:

          * `!` read the digits BEFORE it, of which there are none — the count
            follows the introducer — so every run drew one column, not n.
          * that unconsumed count then reached sixel_colour() at the data byte
            and was taken as a colour INDEX, selecting undefined register 4.
          * and `#0!4~` lost its SELECTION too: the 0 was pending in snum and
            `!` cleared it, so runs drew in the previously selected colour.

        Longhand and tmux's dialect must agree, because they are the same image.
        """
        p = self.lib.lj_vt_new(20, 8)
        self.addCleanup(self.lib.lj_vt_free, p)
        data = b"\x1bP0;0q#0;2;100;0;0#1;2;0;0;100#0!4~-#1!4~--\x1b\\"
        self.lib.lj_vt_feed(p, data, len(data))
        blocks, colours = 0, set()
        for row in range(8):
            for col in range(20):
                cell = self.lib.lj_vt_row(p, row)[col]
                if cell.ch == 0x2580:
                    blocks += 1
                    colours.add(cell.fg & 0xFFFFFF)
        self.assertEqual(blocks, 24, "run-length repeats are not being applied")
        self.assertEqual(len(colours), 2, "a colour selection was lost before '!'")

    def test_sixel_repeat_count_follows_its_introducer(self):
        """`!` then the count, never the count then `!` — isolated from tmux."""
        p = self.lib.lj_vt_new(40, 4)
        self.addCleanup(self.lib.lj_vt_free, p)
        data = b"\x1bPq#0;2;0;100;0#0!7~\x1b\\"
        self.lib.lj_vt_feed(p, data, len(data))
        blocks = sum(1 for row in range(4) for col in range(40)
                     if self.lib.lj_vt_row(p, row)[col].ch == 0x2580)
        self.assertEqual(blocks, 21)     # 7 columns x 6 pixel rows / 2

    def test_hostile_sixel_cannot_corrupt_the_grid(self):
        """A repeat of a million, a colour index past the palette, a width past
        the row and an unterminated image: all clipped, none printed, and the
        terminal still works afterwards."""
        self.feed("\x1bPq#999;2;500;500;500!999999~~~~")
        for row in range(5):
            for col in range(20):
                ch = self.lib.lj_vt_row(self.p, row)[col].ch
                self.assertIn(ch, (0, 0x20, 0x2580), "hostile sixel printed text")
        self.feed("\x1b\\ALIVE")
        self.assertIn("ALIVE", _text(self.lib, self.p, 0, 20))

    def test_abi_basic_feed_state_reply(self):
        self.feed("hello")
        self.assertEqual(self.lib.lj_vt_row(self.p, 0)[0].ch, ord("h"))
        self.assertEqual(self.lib.lj_vt_state(self.p, 0), 5)   # cursor x
        self.lib.lj_vt_clear_reply(self.p)
        self.feed("\x1b[6n")
        self.assertEqual(self.lib.lj_vt_reply(self.p), b"\x1b[1;6R")
        self.lib.lj_vt_clear_reply(self.p)
        self.assertEqual(self.lib.lj_vt_reply(self.p), b"")

    def test_scrollback_preserved_across_width_change(self):
        p = self.lib.lj_vt_new(10, 3)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, b"a\r\nb\r\nc\r\nd\r\ne", 13)   # 2 lines scroll off
        self.lib.lj_vt_resize(p, 20, 3)                    # width change — must NOT wipe sb
        self.lib.lj_vt_scroll(p, 2)                        # browse to oldest scrollback
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, ord("a"),
                         "scrollback was dropped on resize")
        self.assertEqual(self.lib.lj_vt_row(p, 1)[0].ch, ord("b"))
        self.assertEqual(self.lib.lj_vt_row(p, 2)[0].ch, ord("c"))  # grid top follows sb

    def test_resize_grow_pads_scrollback_with_spaces(self):
        p = self.lib.lj_vt_new(4, 3)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, b"abcd\r\nefgh\r\nijkl\r\nmnop\r\n", 24)  # 'abcd','efgh' scroll off
        self.lib.lj_vt_resize(p, 8, 3)                     # grow width
        self.lib.lj_vt_scroll(p, 2)                        # view oldest scrollback line
        row = self.lib.lj_vt_row(p, 0)
        self.assertEqual(row[0].ch, ord("a"))
        self.assertEqual(row[3].ch, ord("d"))
        for c in range(4, 8):                              # exposed cells are blanks
            self.assertEqual(row[c].ch, 32, f"cell {c} not blank-padded")

    def test_batched_scroll_up_correct(self):
        p = self.lib.lj_vt_new(4, 3)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, b"aaa\r\nbbb\r\nccc", 11)   # 3 rows, no scroll yet
        self.lib.lj_vt_feed(p, b"\x1b[2S", 4)              # scroll region up 2
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, ord("c"))
        self.assertEqual(self.lib.lj_vt_row(p, 1)[0].ch, 32)  # blanked
        self.assertEqual(self.lib.lj_vt_row(p, 2)[0].ch, 32)  # blanked
        self.lib.lj_vt_scroll(p, 2)                        # 2 lines now in scrollback
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, ord("a"))
        self.assertEqual(self.lib.lj_vt_row(p, 1)[0].ch, ord("b"))

    def test_hostile_csi_stays_in_bounds(self):
        hostile = (
            "\x1b[999999999999999999999999999999H"
            "\x1b[9999999999;9999999999H"
            "\x1b[9999999999A\x1b[9999999999B\x1b[9999999999C\x1b[9999999999D"
            "\x1b[9999999999L\x1b[9999999999M\x1b[9999999999@\x1b[9999999999P"
            "\x1b[9999999999X\x1b[9999999999S\x1b[9999999999T\x1b[9999999999m"
            "\x1b[38;2;999;999;999m\x1b[48;5;999m"
            "\x1b[\x1b[38;\x1b[?9999999999999999999999h"
        )
        self.feed(hostile)
        self.assertLess(self.lib.lj_vt_state(self.p, 0), 20)   # cx within cols
        self.assertLess(self.lib.lj_vt_state(self.p, 1), 5)    # cy within rows
        # cursor position report still sane after the storm
        self.lib.lj_vt_clear_reply(self.p)
        self.feed("\x1b[6n")
        rep = self.lib.lj_vt_reply(self.p).decode()
        row, col = rep[2:-1].split(";")
        self.assertLessEqual(int(row), 5)
        self.assertLessEqual(int(col), 20)

    def test_bounded_scroll_under_hostile_params(self):
        start = time.monotonic()
        for _ in range(100):
            self.feed("\x1b[9999999999S\x1b[9999999999T")
        self.assertLess(time.monotonic() - start, 1.0)

    def test_malformed_utf8_yields_replacement(self):
        for seq in (b"\xc0\x80", b"\xe0\x80\x80", b"\xed\xa0\x80",   # overlong / surrogate
                    b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80",        # > U+10FFFF
                    b"\x80", b"\xbf"):                                # lone continuation
            p = self.lib.lj_vt_new(8, 2)
            self.addCleanup(self.lib.lj_vt_free, p)
            self.lib.lj_vt_feed(p, seq, len(seq))
            self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, 0xFFFD, repr(seq))

    def test_truncated_utf8_recovers(self):
        p = self.lib.lj_vt_new(8, 2)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, b"\xe2\x82", 2)          # truncated euro (no 3rd byte)
        self.lib.lj_vt_feed(p, b"Z", 1)                 # invalid continuation -> replacement, then Z
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, 0xFFFD)
        self.assertEqual(self.lib.lj_vt_row(p, 0)[1].ch, ord("Z"))

    def test_wide_char_at_right_edge(self):
        p = self.lib.lj_vt_new(2, 2)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, "日".encode(), 3)         # width-2 CJK at cols 0-1
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, ord("日"))
        self.assertEqual(self.lib.lj_vt_row(p, 0)[1].ch, 0)   # continuation cell
        self.lib.lj_vt_feed(p, b"A", 1)                  # wraps to next line, no overrun
        self.assertEqual(self.lib.lj_vt_row(p, 1)[0].ch, ord("A"))

    def test_combining_marks_attach_to_base(self):
        p = self.lib.lj_vt_new(8, 2)
        self.addCleanup(self.lib.lj_vt_free, p)
        self.lib.lj_vt_feed(p, "e\u0301".encode(), 4)    # e + combining acute
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].ch, ord("e"))
        self.assertEqual(self.lib.lj_vt_row(p, 0)[0].marks[0], 0x301)

    def test_alt_screen_save_restore(self):
        self.feed("normal\x1b[?1049hother\x1b[?1049l")
        self.assertEqual(_text(self.lib, self.p, 0, 20).strip(), "normal")


def _sanitize_probe():
    """Bounded ASan/UBSan C probe; returns (returncode, stderr)."""
    probe = (
        '#include <stdio.h>\n#include <string.h>\n#include "owkterm_vt.h"\n'
        'int main(void){\n'
        '  static const char *seqs[] = {'
        '    "\\x1b[999999999999999999999999999999H",'
        '    "\\x1b[9999999999;9999999999H",'
        '    "\\x1b[9999999999A\\x1b[9999999999L\\x1b[9999999999P\\x1b[9999999999X",'
        '    "\\x1b[9999999999S\\x1b[9999999999T\\x1b[9999999999m",'
        '    "\\x1b[38;2;999;999;999m\\x1b[48;5;999m\\x1b[",'
        '    "\\xC0\\x80\\xE0\\x80\\x80\\xED\\xA0\\x80\\xF4\\x90\\x80\\x80\\xF5\\x80\\x80\\x80",'
        '    "\\x80\\xBF\\xE2\\x82",'
        '    "\\xE6\\x97\\xA5A",  /* wide char + ascii */'
        '  };\n'
        '  for (int r = 0; r < 400; r++) {\n'
        '    void *v = lj_vt_new(1 + (r % 300), 1 + (r % 120));\n'
        '    for (unsigned i = 0; i < sizeof(seqs)/sizeof(*seqs); i++)\n'
        '      lj_vt_feed(v, (const unsigned char *)seqs[i], (int)strlen(seqs[i]));\n'
        '    lj_vt_resize(v, 1 + ((r * 7) % 300), 1 + ((r * 13) % 120));\n'
        '    lj_vt_scroll(v, 10); lj_vt_scroll(v, -5);\n'
        '    lj_vt_free(v);\n'
        '  }\n'
        '  puts("probe-ok");\n'
        '  return 0;\n'
        '}\n'
    )
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "probe.c").write_text(probe)
        exe = d / "probe"
        r = subprocess.run(
            ["gcc", "-std=c11", "-g", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-I", str(VT_C.parent),
             "-o", str(exe), str(d / "probe.c"), str(VT_C)],
            capture_output=True, text=True, timeout=120,
            env={**os.environ, "CUDA_VISIBLE_DEVICES": ""},
        )
        if r.returncode != 0:
            return r.returncode, "build failed:\n" + r.stderr
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=120,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1",
                 "UBSAN_OPTIONS": "print_stacktrace=1", "CUDA_VISIBLE_DEVICES": ""},
        )
        return run.returncode, run.stderr + run.stdout


if __name__ == "__main__":
    if "--sanitize" in sys.argv:
        code, out = _sanitize_probe()
        print(out)
        sys.exit(code)
    unittest.main(verbosity=2)
