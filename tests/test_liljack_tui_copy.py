"""Exercise copy pause against a real, isolated native TUI (no provider sessions).

Usage: python3 tests/test_liljack_tui_copy.py /path/to/liljack-native
"""
import errno
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time


def read_for(fd, seconds):
    chunks = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
            try:
                chunk = os.read(fd, 65536)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            chunks.append(chunk)
    return b''.join(chunks)


def main(binary):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 50, 200, 0, 0))
    proc = subprocess.Popen([binary, '--tui', '--demo'], stdin=slave, stdout=slave,
                            stderr=slave, close_fds=True)
    os.close(slave)
    try:
        initial = read_for(master, 2)
        assert proc.poll() is None, initial[-1000:]
        assert b'\x1b[?1003h' in initial, 'Mouse capture was not enabled'
        os.write(master, b'\x1b[19~')  # F8
        paused = read_for(master, .5)
        assert b'\x1b[?1003l' in paused, 'F8 did not release mouse capture'
        assert b'Copy paused' in paused, 'Copy pause lacks a visible explanation'
        os.write(master, b'SHOULD_NOT_ENTER\x1b[200~PASTE_BLOCKED\x1b[201~')
        assert read_for(master, .3) == b'', 'Paused TUI redrew or echoed input'
        os.write(master, b'\x1b[19~')
        resumed = read_for(master, .5)
        assert b'\x1b[?1003h' in resumed, 'F8 did not restore mouse capture'
        assert b'SHOULD_NOT_ENTER' not in resumed and b'PASTE_BLOCKED' not in resumed
        os.write(master, b'\x11')  # Ctrl+Q detaches demo
        restored = read_for(master, .5)
        assert proc.wait(timeout=3) == 0
        assert b'\x1b[?1003l' in restored, 'Exit did not restore host mouse state'
        print('TUI copy: F8 mouse release, frozen display, input/paste isolation, resume and exit passed')
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master)


if __name__ == '__main__':
    main(sys.argv[1])
