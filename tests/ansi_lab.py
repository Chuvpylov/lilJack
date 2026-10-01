#!/usr/bin/env python3
"""Build the presenter-only lab and run it in the current terminal."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
def build():
    sources = [ROOT/'tests/ansi_lab.c', ROOT/'liljack_app/c_ansi.c', ROOT/'liljack_app/c_sixel.c', ROOT/'liljack_app/c_theme.c']
    headers = list((ROOT/'liljack_app').glob('*.h')) + list((ROOT.parent/'hui').glob('*.h'))
    digest=hashlib.sha256(b''.join(p.read_bytes() for p in sources+headers)).hexdigest()[:16]
    cache=Path(os.environ.get('LILJACK_BUILD_ROOT',Path.home()/'.cache/liljack/build'));cache.mkdir(parents=True,exist_ok=True)
    binary=cache/f'ansi-lab-{digest}'
    if not binary.exists():
        with tempfile.TemporaryDirectory(prefix='ansi-lab-build-', dir=cache) as tmp:
            target=Path(tmp)/'lab'
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2','json-c'],text=True))
            subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-I',str(ROOT.parent/'hui'),*flags,*map(str,sources),'-lm','-ljson-c','-o',str(target)],check=True)
            target.replace(binary)
    return binary
if __name__=='__main__':
    binary=build()
    if sys.argv[1:]==['--build-only']:print(binary)
    else:os.execv(binary,[str(binary),*sys.argv[1:]])
