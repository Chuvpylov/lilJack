#!/usr/bin/env python3
"""Compile the real controller test; own all temporary files and PTY children.

Default: local CPU video, no network/GPU/services. --network also exercises
the built-in Rick Roll URL. --capture-dir retains terminal bytes and canvas PNGs.
"""
import argparse
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import shlex
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]


def run(binary, source, output, name, sixel):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 45, 144, 0, 0))
    env = os.environ.copy()
    env['SDL_VIDEODRIVER'] = 'dummy'
    env['SDL_AUDIODRIVER'] = 'dummy'
    env['TERM'] = 'xterm-256color'
    env.pop('TERM_PROGRAM', None)
    env.pop('LJ_SIXEL_OFF', None)
    if not sixel:
        env['LJ_SIXEL_OFF'] = '1'
    env['LJ_SIXEL_MIN_MS'] = '0'
    chunks = []
    with tempfile.TemporaryFile() as errors:
        proc = subprocess.Popen([str(binary), str(source), str(output / (name + '.png'))],
                                stdin=slave, stdout=slave, stderr=errors, env=env, cwd=ROOT)
        os.close(slave)
        pending = b''
        deadline = time.monotonic() + 90
        try:
            while time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            break
                        raise
                    if not data:
                        break
                    chunks.append(data)
                    pending += data
                    if b'\x1b[c' in pending:
                        os.write(master, b'\x1b[?6;4;2c' if sixel else b'\x1b[?6;2c')
                        pending = pending.replace(b'\x1b[c', b'')
                    if b'\x1b[16t' in pending:
                        os.write(master, b'\x1b[6;20;10t')
                        pending = pending.replace(b'\x1b[16t', b'')
                    pending = pending[-32:]
                elif proc.poll() is not None:
                    break
            code = proc.wait(timeout=3)
            errors.seek(0)
            detail = errors.read().decode(errors='replace')
            assert code == 0, detail
            wire = b''.join(chunks)
            assert b'\x1bP' in wire if sixel else b'\xe2\x96\x80' in wire
            (output / (name + '.ansi')).write_bytes(wire)
            print(name + ': ' + detail.strip())
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            os.close(master)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture-dir', type=Path)
    parser.add_argument('--network', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='liljack-popup-integration-') as tmp:
        work = Path(tmp)
        output = args.capture_dir.resolve() if args.capture_dir else work
        output.mkdir(parents=True, exist_ok=True)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                                    'sdl2', 'freetype2', 'json-c'], text=True))
        # ⚠ Must mirror what main.c links (see tests/run_liljack_native_tests.sh).
        # c_blocks landed with the block-model task and this list fell behind it —
        # the test then failed on `undefined reference to lj_block_for` and got
        # filed as "environmental". (c_ansi is NOT listed on purpose: the harness
        # source already #includes it; adding it here defines every symbol twice.)
        # ⚠ EVERY UNIT main.c CALLS MUST BE HERE. main.c's block table dispatches
        # into c_dash (the DASH home surface), so leaving it out fails at LINK
        # with `undefined reference to lj_dash_draw` — the same gap the sanitized
        # native test hit on 2026-09-12 when c_dash landed.
        modules = ['c_dock', 'c_blocks', 'c_render', 'c_media', 'c_popup', 'c_metrics',
                   'c_review', 'c_dash', 'c_sixel', 'owkterm_vt']
        binary = work / 'popup-integration'
        command = ['gcc', '-std=c11', '-O1', '-g', '-Wno-misleading-indentation',
                   '-I' + str(ROOT / '../hui'), '-I' + str(ROOT / 'liljack_app'),
                   str(ROOT / 'tests/test_liljack_popup_integration.c')]
        command += [str(ROOT / 'liljack_app' / (m + '.c')) for m in modules]
        subprocess.run(command + flags + ['-lutil', '-lm', '-ldl', '-o', str(binary)], check=True)
        fixture = work / 'red.mp4'
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                        '-threads', '2', '-filter_threads', '1', '-f', 'lavfi', '-i',
                        'color=c=red:s=64x36:r=12', '-t', '2', '-an', '-c:v', 'mpeg4',
                        '-threads', '2', '-pix_fmt', 'yuv420p', str(fixture)], check=True)
        run(binary, fixture, output, 'fixture-blocks', False)
        run(binary, fixture, output, 'fixture-sixel', True)
        if args.network:
            run(binary, 'https://www.youtube.com/watch?v=dQw4w9WgXcQ', output, 'rickroll-blocks', False)
            run(binary, 'https://www.youtube.com/watch?v=dQw4w9WgXcQ', output, 'rickroll-sixel', True)


if __name__ == '__main__':
    main()
