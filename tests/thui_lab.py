#!/usr/bin/env python3
"""Build/run selected native THUI combinations; no workspace helper is started."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['tiles', 'dialog', 'popup', 'all'], default='all')
    parser.add_argument('--interactive', action='store_true', help='Open selected case in this terminal; F10 exits')
    parser.add_argument('--frames', type=int, default=300)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT/'docs/codex/reports/thui-combination-lab')
    args = parser.parse_args()
    if args.interactive and args.case == 'all':
        parser.error('--interactive requires one --case')
    if not 10 <= args.frames <= 100000:
        parser.error('--frames must be 10..100000')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2', 'freetype2', 'json-c'], text=True))
    mods = ['c_theme','c_dock','c_blocks','c_render','c_media','c_popup','owkterm_vt','c_review','c_dash','c_metrics','c_sixel','c_git_dag']
    with tempfile.TemporaryDirectory(prefix='liljack-thui-lab-') as tmp:
        binary = str(Path(tmp)/'lab')
        cmd = ['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Wno-misleading-indentation','-DLJ_GIT_RENDERER',
               '-I',os.environ.get('LILJACK_HUI',str(ROOT.parent/'hui')),str(ROOT/'tests/thui_lab.c')]
        if args.sanitize:
            cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        cmd += [str(ROOT/f'liljack_app/{m}.c') for m in mods] + flags + ['-lutil','-lm','-o',binary]
        build = subprocess.run(cmd, capture_output=True, text=True)
        (out/'build.log').write_text(build.stdout+build.stderr)
        if build.returncode:
            raise SystemExit(build.stderr)
        env = os.environ.copy()
        env['SDL_VIDEODRIVER']='dummy'
        env['LILJACK_TRACE_OVERLAYS']='0'
        cases = ['tiles','dialog','popup'] if args.case=='all' else [args.case]
        results=[]
        for case in cases:
            for w,h in ([(800,560)] if args.interactive else [(800,560),(1280,720),(1920,1080)]):
                dest=out/f'{case}-{w}x{h}';dest.mkdir(exist_ok=True)
                run = subprocess.run([binary,case,str(w),str(h),str(args.frames),str(dest),str(int(args.interactive))],
                                     env=env,capture_output=not args.interactive,text=True)
                if args.interactive:
                    raise SystemExit(run.returncode)
                (dest/'run.log').write_text((run.stdout or '')+(run.stderr or ''))
                if run.returncode:
                    raise SystemExit(f'{case} {w} failed: {run.stderr}')
                result=json.loads(run.stdout)
                result['png_sha256']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dest.glob('*.png'))}
                result['hover_differs']=result['png_sha256']['default.png']!=result['png_sha256']['hover.png']
                results.append(result); print(json.dumps(result),flush=True)
        (out/'results.json').write_text(json.dumps({'timing_scope':'CPU render/controller only; excludes terminal presentation, video decoding and live backend', 'sanitized':args.sanitize,'results':results},indent=2)+'\n')

if __name__=='__main__':
    main()
