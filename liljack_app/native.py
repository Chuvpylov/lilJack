"""Bounded native build and owkterm/HUI bindings. Single UI thread."""
from __future__ import annotations

import ctypes as C
import fcntl
import hashlib
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent


class Cell(C.Structure):
    _fields_ = [("ch", C.c_uint32), ("fg", C.c_uint32), ("bg", C.c_uint32),
                ("marks", C.c_uint32 * 3)]


def load():
    hui = Path(os.environ.get("LILJACK_HUI", str(HERE.parent / "vendor" / "hui")))
    if not (hui / "hui.h").is_file():
        raise RuntimeError("HUI source missing; set LILJACK_HUI to the directory containing hui.h")
    sources = [HERE / "owkterm_vt.c", HERE / "hui_bridge.c"]
    headers = sorted(hui.glob("*.h")) + sorted((hui / "backends").glob("*.h"))
    digest = hashlib.sha256()
    for p in sources + headers:
        digest.update(p.read_bytes())
    cache = Path(os.environ.get("LILJACK_BUILD_ROOT", str(Path.home() / ".cache/liljack/build")))
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    target = cache / ("liljack-" + digest.hexdigest()[:16] + ".so")
    with (cache / "native.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not target.exists():
            tmp = target.with_suffix(f".{os.getpid()}.tmp")
            try:
                subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-std=c11",
                                "-I", str(hui), *(str(p) for p in sources), "-lm", "-o", str(tmp)],
                               check=True, timeout=90, env=dict(os.environ))
                tmp.replace(target)
            finally:
                tmp.unlink(missing_ok=True)
    lib = C.CDLL(str(target))
    signatures = {
        "lj_vt_new": ([C.c_int, C.c_int], C.c_void_p),
        "lj_vt_free": ([C.c_void_p], None),
        "lj_vt_resize": ([C.c_void_p, C.c_int, C.c_int], None),
        "lj_vt_feed": ([C.c_void_p, C.c_char_p, C.c_int], None),
        "lj_vt_row": ([C.c_void_p, C.c_int], C.POINTER(Cell)),
        "lj_vt_scroll": ([C.c_void_p, C.c_int], None),
        "lj_vt_state": ([C.c_void_p, C.c_int], C.c_int),
        "lj_vt_reply": ([C.c_void_p], C.c_char_p),
        "lj_vt_clear_reply": ([C.c_void_p], None),
        "lj_hui_begin": ([C.c_int, C.c_int], None),
        "lj_hui_rect": ([C.c_int] * 4 + [C.c_uint, C.c_int], None),
        "lj_hui_end": ([], C.c_void_p),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result
    return lib


class Terminal:
    def __init__(self, lib, cols=80, rows=24):
        self.lib, self.cols, self.rows = lib, cols, rows
        self.ptr = lib.lj_vt_new(cols, rows)
        if not self.ptr:
            raise MemoryError("owkterm terminal allocation failed")

    def close(self):
        if self.ptr:
            self.lib.lj_vt_free(self.ptr)
            self.ptr = None

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.lib.lj_vt_feed(self.ptr, data, len(data))

    def resize(self, cols, rows):
        self.cols, self.rows = min(300, max(1, cols)), min(120, max(1, rows))
        self.lib.lj_vt_resize(self.ptr, self.cols, self.rows)

    def state(self, key):
        return self.lib.lj_vt_state(self.ptr, key)

    def row(self, index):
        return self.lib.lj_vt_row(self.ptr, index)

    def text(self, index):
        return "".join(chr(c.ch) + "".join(chr(m) for m in c.marks if m)
                       for c in self.row(index)[:self.cols] if c.ch)

    def scroll(self, delta):
        self.lib.lj_vt_scroll(self.ptr, delta)

    def reply(self):
        result = self.lib.lj_vt_reply(self.ptr)
        self.lib.lj_vt_clear_reply(self.ptr)
        return result
