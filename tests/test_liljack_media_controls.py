#!/usr/bin/env python3
"""CPU-only fixtures, dummy SDL audio and unavailable-device fallback."""
import os
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='liljack-media-controls-') as tmp:
        root = Path(tmp)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        binary = root / 'controls'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-I'+str(ROOT/'../hui'),
                        str(ROOT/'tests/test_liljack_media_controls.c'), *flags, '-lm', '-o', str(binary)], check=True)
        clip = root / 'red-blue.mkv'
        subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-threads', '2', '-filter_complex_threads', '1',
                        '-f', 'lavfi', '-i', 'color=red:s=64x36:r=12:d=2',
                        '-f', 'lavfi', '-i', 'color=blue:s=64x36:r=12:d=2',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=4',
                        '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0[v]', '-map', '[v]', '-map', '2:a',
                        '-c:v', 'mpeg4', '-threads', '2', '-c:a', 'pcm_s16le', '-ac', '2', str(clip)], check=True)
        env = os.environ.copy();env['SDL_AUDIODRIVER'] = 'dummy'
        subprocess.run([str(binary), str(clip)], env=env, check=True, timeout=35)
        env['SDL_AUDIODRIVER'] = 'deliberately-unavailable';env['TEST_NO_AUDIO_DEVICE'] = '1'
        subprocess.run([str(binary), str(clip)], env=env, check=True, timeout=35)


if __name__ == '__main__':
    main()
