#!/usr/bin/env python3
"""Isolated production-controller folder picker checks and three-size captures."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import argparse

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'docs/codex/reports/folder-browser-popup')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2', 'freetype2', 'json-c'], text=True))
    modules = ['c_theme', 'c_dash', 'c_dock', 'c_blocks', 'c_render', 'c_media', 'c_popup', 'owkterm_vt', 'c_review', 'c_metrics', 'c_sixel', 'c_git_dag']
    with tempfile.TemporaryDirectory(prefix='liljack-folder-picker-') as tmp:
        binary = str(Path(tmp)/'fixture')
        tree = Path(tmp)/'folders'
        tree.mkdir()
        for name in ['alpha', 'beta', 'delta', 'epsilon', 'folder with spaces', 'zeta', '.hidden']:
            (tree/name).mkdir()
        for i in range(40):   # enough folders to scroll at every size
            (tree/f'many-{i:02d}').mkdir()
        (tree/'alpha'/'nested').mkdir()
        (tree/'ordinary-file.txt').write_text('not a directory')
        cmd = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-misleading-indentation', '-DLJ_GIT_RENDERER', '-I', os.environ.get('LILJACK_HUI', str(ROOT.parent/'hui')), str(ROOT/'tests/test_liljack_folder_picker_flow.c')]
        cmd += [str(ROOT/f'liljack_app/{m}.c') for m in modules] + flags + ['-lutil', '-lm', '-o', binary]
        build = subprocess.run(cmd, capture_output=True, text=True)
        (out/'build.log').write_text(build.stdout+build.stderr)
        if build.returncode:
            raise SystemExit(build.stderr)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', LILJACK_TRACE_OVERLAYS='0')
        for w, h in [(800,560), (1280,720), (1920,1080)]:
            dest = out/f'{w}x{h}'
            dest.mkdir(exist_ok=True)
            run = subprocess.run([binary,str(w),str(h),str(tree),str(dest)], env=env,capture_output=True,text=True)
            (dest/'run.log').write_text(run.stdout+run.stderr)
            if run.returncode:
                raise SystemExit(run.stderr)
            print(run.stdout, end='')

if __name__ == '__main__':
    main()
