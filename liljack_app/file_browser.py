"""File listing for a room's working folder — and beyond it.

The file-explorer backend: turn ``{room, path}`` into a bounded, sorted listing.
the operator (2026-09-10) wants to browse ANYWHERE and then open a room on a folder, so
browsing is UNCONFINED: the request may navigate up past the room root, or to an
absolute path. The room root is still resolved and marked in the reply
(``room_root`` + ``is_room_root``) so the renderer can draw where the room begins.

⚠ NEVER RAISES. A missing folder, a deleted directory, a dangling symlink, a
permission error — every failure returns ``error:"..."`` with ``entries:[]``, so
the C side prints a reason instead of a blank list that reads as "empty folder".

⚠ NO ALLOCATION OF SIDE EFFECTS BEYOND A READ. Read-only: no create, rename,
delete or write. Stdlib only.
"""
from __future__ import annotations

import os
from datetime import datetime, timezone

MAX_ENTRIES = 500


def _mtime_iso(ts: float) -> str:
    try:
        return datetime.fromtimestamp(ts, timezone.utc).isoformat()
    except (OSError, OverflowError, ValueError):
        return ""


def _room_root(store, project, room: str) -> str:
    """The room's absolute working folder, or the project when standalone.

    Uses the shared Workspace.room_folder resolver (Codex) so there is exactly
    one lookup. `require_exists=False` keeps the existence check here, so a
    missing folder surfaces as this module's own "folder not found" reason
    rather than the resolver's.
    """
    try:
        return store.room_folder(project, room or "", require_exists=False)
    except ValueError:
        raise ValueError("unknown room") from None


def _target(root_real: str, path: str) -> str:
    """Resolve the requested path against the room root, UNCONFINED.

    '' is the room root; a leading ``/`` is used as-is; anything else is joined
    to the root (``..`` included — going up past the root is deliberate). Returns
    the realpath of the target directory, or raises with a reason.
    """
    if not isinstance(path, str):
        raise ValueError("path must be a string")
    p = path.strip()
    if not p:
        if not os.path.exists(root_real):
            raise ValueError("folder not found")
        if not os.path.isdir(root_real):
            raise ValueError("folder is not a directory")
        return root_real
    candidate = p if p.startswith("/") else os.path.join(
        root_real, *p.replace("\\", "/").split("/"))
    target = os.path.realpath(candidate)
    if not os.path.exists(target):
        raise ValueError("path not found")
    if not os.path.isdir(target):
        raise ValueError("not a directory")
    return target


def _rel(target: str, base: str) -> str:
    rel = os.path.relpath(target, base)
    return "" if rel == "." else rel


def list_files(store, project, room: str = "", path: str = "",
               cap: int = MAX_ENTRIES) -> dict:
    """The ``files:`` packet. Never raises; ``error`` is set on every failure."""
    out = {"root": "", "path": "", "parent": None, "room_root": "",
           "is_room_root": False, "entries": [], "truncated": False, "error": ""}
    try:
        if not isinstance(cap, int) or cap < 1:
            cap = MAX_ENTRIES
        room_root = os.path.realpath(_room_root(store, project, room))
        target = _target(room_root, path)
        out["room_root"] = room_root
        out["root"] = target
        out["is_room_root"] = (target == room_root)
        out["path"] = _rel(target, room_root)
        parent_abs = os.path.dirname(target)
        out["parent"] = None if parent_abs == target else _rel(parent_abs, room_root)

        # Scan names + classification only; the dirent type makes is_dir() a
        # cached lookup, so a 200k-file folder costs one scandir, not 200k stats.
        names = []
        with os.scandir(target) as it:
            for entry in it:
                names.append((entry.name, entry.is_symlink(),
                              entry.is_dir(follow_symlinks=False)))
        names.sort(key=lambda t: (not t[2], t[0].casefold(), t[0]))
        out["truncated"] = len(names) > cap
        entries = []
        for name, is_link, is_dir in names[:cap]:
            full = os.path.join(target, name)
            try:
                st = os.lstat(full) if is_link else os.stat(full)
            except OSError:
                st = None
            entries.append({"name": name, "dir": is_dir, "symlink": is_link,
                            "size": st.st_size if st else 0,
                            "mtime": _mtime_iso(st.st_mtime) if st else ""})
        out["entries"] = entries
    except Exception as exc:                       # never raise into the backend loop
        out["error"] = str(exc) or type(exc).__name__
        out["entries"] = []
    return out
