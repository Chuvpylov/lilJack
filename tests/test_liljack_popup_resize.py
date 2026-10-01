#!/usr/bin/env python3
"""Isolated popup controller checks; local CPU decode, no live workspace."""
import os
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture-dir', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'sdl2', 'freetype2', 'json-c'], text=True))
    modules = ['c_dash', 'c_dock', 'c_blocks', 'c_render', 'c_media', 'c_popup',
               'c_metrics', 'c_review', 'c_sixel', 'owkterm_vt']
    with tempfile.TemporaryDirectory(prefix='liljack-popup-resize-') as tmp:
        binary = str(Path(tmp) / 'resize')
        command = ['gcc', '-std=c11', '-O1', '-g', '-Wno-misleading-indentation',
                   '-I' + str(ROOT.parent / 'hui'), str(ROOT / 'tests/test_liljack_popup_resize.c')]
        command += [str(ROOT / 'liljack_app' / (m + '.c')) for m in modules]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command + flags + ['-lutil', '-lm', '-o', binary], check=True)
        clip = str(Path(tmp) / 'red-blue.mkv')
        subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-threads', '2', '-filter_complex_threads', '1',
                        '-f', 'lavfi', '-i', 'color=red:s=64x36:r=12:d=2',
                        '-f', 'lavfi', '-i', 'color=blue:s=64x36:r=12:d=2',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=4',
                        '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0[v]', '-map', '[v]', '-map', '2:a',
                        '-c:v', 'mpeg4', '-threads', '2', '-c:a', 'pcm_s16le', '-ac', '2', clip], check=True)
        env = os.environ.copy()
        env['SDL_VIDEODRIVER'] = env['SDL_AUDIODRIVER'] = 'dummy'
        run = [binary, clip]
        if args.capture_dir:
            args.capture_dir.mkdir(parents=True, exist_ok=True)
            run.append(str(args.capture_dir.resolve()))
        subprocess.run(run, env=env, check=True, timeout=60)

if __name__ == '__main__':
    main()
