#!/usr/bin/env python3
"""Build/run the deterministic production-header terminal fixture."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from ansi_lab import ROOT

def build():
    app=ROOT/'liljack_app'
    modules=['c_theme','c_dash','c_dock','c_blocks','c_render','c_media','c_popup','c_ansi','owkterm_vt','c_review','c_sixel']
    defines=[]
    if (app/'c_git_dag.c').exists():modules.append('c_git_dag');defines.append('-DLJ_GIT_RENDERER')
    sources=[ROOT/'tests/graph_header_lab.c',*[app/f'{m}.c' for m in modules]]
    inputs=sorted(set(sources+list(app.glob('*.c'))+list(app.glob('*.h'))+list((ROOT.parent/'hui').glob('*.h'))))
    digest=hashlib.sha256(b''.join(p.read_bytes() for p in inputs)).hexdigest()[:16]
    cache=Path(os.environ.get('LILJACK_BUILD_ROOT',Path.home()/'.cache/liljack/build'));cache.mkdir(parents=True,exist_ok=True)
    binary=cache/f'graph-header-lab-{digest}'
    if not binary.exists():
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2','freetype2','json-c'],text=True))
        libs=shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2','freetype2','json-c'],text=True))
        with tempfile.TemporaryDirectory(prefix='graph-header-',dir=cache) as tmp:
            target=Path(tmp)/'lab'
            subprocess.run(['gcc','-std=c11','-O1','-I',str(ROOT.parent/'hui'),*defines,*flags,*map(str,sources),*libs,'-lutil','-lm','-o',str(target)],check=True)
            target.replace(binary)
    return binary
if __name__=='__main__':
    binary=build()
    if sys.argv[1:]==['--build-only']:print(binary)
    else:os.execv(binary,[str(binary),*sys.argv[1:]])
