#!/usr/bin/env python3
"""Compile and run the isolated tab/chip state-flip rectangle regression."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2', 'freetype2', 'json-c'], text=True))
    modules = ['c_dash','c_dock','c_blocks','c_render','c_media','c_popup','owkterm_vt','c_review','c_metrics','c_sixel']
    with tempfile.TemporaryDirectory(prefix='liljack-label-form-') as tmp:
        binary = str(Path(tmp)/'fixture')
        cmd = ['gcc','-std=c11','-O1','-g','-I'+str(ROOT.parent/'hui'),str(ROOT/'tests/test_liljack_label_form_stability.c')]
        cmd += [str(ROOT/f'liljack_app/{m}.c') for m in modules] + flags + ['-lutil','-lm','-o',binary]
        subprocess.run(cmd, check=True)
        subprocess.run([binary],env=dict(os.environ,SDL_VIDEODRIVER='dummy'),check=True)

if __name__ == '__main__':
    main()
