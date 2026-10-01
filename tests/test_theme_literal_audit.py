"""Prevent new raw UI RGB values in the five theme-extracted C modules."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]/'liljack_app'

class ThemeLiteralAudit(unittest.TestCase):
    def test_only_documented_non_theme_hex_remains(self):
        # Pixel masks, UTF-8/Unicode limits, input bound and transparent key.
        allowed={
            'main.c':{0x1000000,0xffffff,0x100000,0xff000000,0xfefefe,0xfcfcfc,0xf8f8f8},
            'c_ansi.c':{0xffffff,0x10ffff,0x010203,0xff000000},
            'c_render.c':{0x10ffff,0xffffff,0xff000000},'c_dock.c':set(),'c_blocks.c':set(),
        }
        for name,exceptions in allowed.items():
            source=(ROOT/name).read_text()
            # Retain quoted strings so comment-like text in a label is safe.
            source=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*',
                          lambda m:m[0] if m[0][0] in '\"\'' else '',source)
            found={int(m[1],16) for m in re.finditer(r'\b0x([0-9a-fA-F]{6,8})[uUlL]*\b',source)}
            self.assertEqual(found-exceptions,set(),f'{name}: add semantic tokens, not raw colours')

if __name__=='__main__':unittest.main()
