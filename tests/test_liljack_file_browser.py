#!/usr/bin/env python3
"""Unconfined browsing, room-root marking, sorting, cap and error paths.

The contract this pins is Claude's files_list shape:
  files: {root, path, parent, room_root, is_room_root,
          entries[{name,dir,size,mtime,symlink}], truncated, error}

the operator (2026-09-10) reversed the earlier confinement: browsing is UNCONFINED —
you may go up past the room root or jump to an absolute path. The room root is
still resolved and marked (room_root + is_room_root) so the renderer can draw
where the room begins. Every failure must come back as a non-empty `error` with
`entries:[]`, never an exception and never a blank list that reads as empty.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

from liljack_app import file_browser as F  # noqa: E402


class Store:
    """Minimal stand-in for Workspace: resolves room -> folder."""
    def __init__(self, rooms):
        self.rooms = rooms

    def room_folder(self, project, room_id="", require_exists=True):
        if not room_id:
            return project
        if room_id not in self.rooms:
            raise ValueError("room does not belong to this project")
        return self.rooms[room_id].get("folder") or project


class FileBrowserTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.base = base
        self.folder = base / "room-folder"
        self.folder.mkdir()
        (self.folder / "alpha.txt").write_text("a")
        (self.folder / "Bravo.txt").write_text("b")
        (self.folder / "sub").mkdir()
        (self.folder / "sub" / "inner.txt").write_text("i")
        self.outside = base / "outside"
        self.outside.mkdir()
        (self.outside / "secret.txt").write_text("s")
        self.project = str(base / "project")
        self.store = Store({"r-1": {"folder": str(self.folder)}})

    def test_root_lists_room_folder_sorted_dirs_first(self):
        result = F.list_files(self.store, self.project, room="r-1")
        self.assertEqual(result["error"], "")
        self.assertEqual(result["root"], os.path.realpath(str(self.folder)))
        self.assertEqual(result["room_root"], os.path.realpath(str(self.folder)))
        self.assertTrue(result["is_room_root"])
        self.assertEqual(result["path"], "")
        self.assertEqual(result["parent"], "..")   # unconfined: up goes past the room
        names = [e["name"] for e in result["entries"]]
        self.assertEqual(names, ["sub", "alpha.txt", "Bravo.txt"])   # dirs first
        self.assertFalse(result["truncated"])
        by = {e["name"]: e for e in result["entries"]}
        self.assertTrue(by["sub"]["dir"] and not by["sub"]["symlink"])
        self.assertFalse(by["alpha.txt"]["dir"])
        self.assertTrue(by["alpha.txt"]["size"] > 0 and by["alpha.txt"]["mtime"])

    def test_nested_path_and_parent(self):
        result = F.list_files(self.store, self.project, room="r-1", path="sub")
        self.assertEqual(result["path"], "sub")
        self.assertEqual(result["parent"], "")
        self.assertEqual(result["root"], os.path.realpath(str(self.folder / "sub")))
        self.assertFalse(result["is_room_root"])
        self.assertEqual([e["name"] for e in result["entries"]], ["inner.txt"])

    def test_standalone_uses_project_as_root(self):
        Path(self.project).mkdir()
        result = F.list_files(self.store, self.project, room="")
        self.assertEqual(result["root"], os.path.realpath(self.project))
        self.assertTrue(result["is_room_root"])
        self.assertEqual(result["error"], "")
        self.assertEqual(result["entries"], [])  # empty project dir, honestly empty

    # ── unconfined browsing ──────────────────────────────────────────────
    def test_dotdot_goes_up_past_room_root(self):
        result = F.list_files(self.store, self.project, room="r-1", path="..")
        self.assertEqual(result["error"], "")
        self.assertEqual(result["root"], os.path.realpath(str(self.base)))
        self.assertEqual(result["path"], "..")
        self.assertFalse(result["is_room_root"])
        names = [e["name"] for e in result["entries"]]
        self.assertIn("outside", names)
        self.assertIn("room-folder", names)

    def test_absolute_path_browses_anywhere(self):
        result = F.list_files(self.store, self.project, room="r-1",
                              path=str(self.outside))
        self.assertEqual(result["error"], "")
        self.assertEqual(result["root"], os.path.realpath(str(self.outside)))
        self.assertFalse(result["is_room_root"])
        self.assertEqual([e["name"] for e in result["entries"]], ["secret.txt"])

    def test_escaping_symlink_now_browses_and_stays_flagged(self):
        link = self.folder / "escape"
        link.symlink_to(self.outside, target_is_directory=True)
        parent = F.list_files(self.store, self.project, room="r-1")
        entry = next(e for e in parent["entries"] if e["name"] == "escape")
        self.assertTrue(entry["symlink"])
        # unconfined: navigating into the escaping link is allowed now
        into = F.list_files(self.store, self.project, room="r-1", path="escape")
        self.assertEqual(into["error"], "")
        self.assertEqual(into["root"], os.path.realpath(str(self.outside)))
        self.assertFalse(into["is_room_root"])

    def test_symlink_to_etc_is_flagged_but_now_browsable(self):
        link = self.folder / "etc-link"
        link.symlink_to("/etc", target_is_directory=True)
        result = F.list_files(self.store, self.project, room="r-1")
        entry = next(e for e in result["entries"] if e["name"] == "etc-link")
        self.assertTrue(entry["symlink"])
        self.assertFalse(entry["dir"])
        into = F.list_files(self.store, self.project, room="r-1", path="etc-link")
        self.assertEqual(into["error"], "")
        self.assertEqual(into["root"], os.path.realpath("/etc"))
        self.assertFalse(into["is_room_root"])

    def test_symlink_inside_root_is_flagged_but_navigable(self):
        link = self.folder / "sub-link"
        link.symlink_to(self.folder / "sub", target_is_directory=True)
        result = F.list_files(self.store, self.project, room="r-1")
        entry = next(e for e in result["entries"] if e["name"] == "sub-link")
        self.assertTrue(entry["symlink"])
        self.assertFalse(entry["dir"])
        into = F.list_files(self.store, self.project, room="r-1", path="sub-link")
        self.assertEqual(into["error"], "")
        self.assertEqual([e["name"] for e in into["entries"]], ["inner.txt"])

    # ── cap and truncation ───────────────────────────────────────────────
    def test_cap_truncates_and_sorts_dirs_first(self):
        many = Path(self.tmp.name) / "many"
        many.mkdir()
        for i in range(300):
            (many / f"f-{i:04d}.txt").write_text("x")
        for i in range(300):
            (many / f"d-{i:04d}").mkdir()
        store = Store({"r-1": {"folder": str(many)}})
        result = F.list_files(store, self.project, room="r-1", cap=500)
        self.assertTrue(result["truncated"])
        self.assertEqual(len(result["entries"]), 500)
        self.assertTrue(all(e["dir"] for e in result["entries"][:300]))
        self.assertFalse(any(e["dir"] for e in result["entries"][-10:]))

    # ── error paths: a reason, never a blank ─────────────────────────────
    def test_deleted_directory_says_so(self):
        result = F.list_files(self.store, self.project, room="r-1", path="ghost")
        self.assertIn("not found", result["error"])
        self.assertEqual(result["entries"], [])

    def test_unknown_room_says_so(self):
        result = F.list_files(self.store, self.project, room="r-none")
        self.assertIn("unknown room", result["error"])
        self.assertEqual(result["entries"], [])

    def test_path_through_a_file_is_not_a_directory(self):
        result = F.list_files(self.store, self.project, room="r-1", path="alpha.txt")
        self.assertIn("not a directory", result["error"])

    def test_dangling_symlink_is_an_error_not_a_blank(self):
        link = self.folder / "dangling"
        link.symlink_to(self.folder / "never-existed", target_is_directory=True)
        result = F.list_files(self.store, self.project, room="r-1", path="dangling")
        self.assertTrue(result["error"])
        self.assertEqual(result["entries"], [])
        parent = F.list_files(self.store, self.project, room="r-1")
        entry = next(e for e in parent["entries"] if e["name"] == "dangling")
        self.assertTrue(entry["symlink"] and not entry["dir"])

    def test_permission_error_becomes_a_reason_never_raises(self):
        with patch("liljack_app.file_browser.os.scandir",
                   side_effect=PermissionError("EACCES")):
            result = F.list_files(self.store, self.project, room="r-1")
        self.assertIn("EACCES", result["error"])
        self.assertEqual(result["entries"], [])

    def test_a_missing_root_is_an_error_not_an_empty_list(self):
        store = Store({"r-1": {"folder": str(Path(self.tmp.name) / "gone")}})
        result = F.list_files(store, self.project, room="r-1")
        self.assertIn("folder not found", result["error"])
        self.assertEqual(result["entries"], [])


if __name__ == "__main__":
    unittest.main()
