import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))

import liljack_room_export as E  # noqa: E402
from liljack_workspace import Workspace, project_key  # noqa: E402


def _seed(root: Path, project: str):
    w = Workspace(root)
    p = project_key(project)
    w.db.execute("INSERT INTO sessions VALUES(?,?,?)", ("s-ant", p, json.dumps(
        {"agent": "shell", "harness": "tmux", "state": "idle"})))
    w.db.execute("INSERT INTO sessions VALUES(?,?,?)", ("s-cl", p, json.dumps(
        {"agent": "claude", "harness": "tmux", "state": "idle"})))
    w.db.execute("INSERT INTO rooms(id,project,name,collapsed) VALUES(?,?,?,0)", ("r-1", p, "Review"))
    w.db.execute("INSERT INTO room_members VALUES(?,?)", ("s-ant", "r-1"))
    w.db.execute("INSERT INTO room_members VALUES(?,?)", ("s-cl", "r-1"))
    w.db.execute("INSERT INTO team VALUES(?,?,?)", ("s-cl", "lead", "s-ant"))
    for i in range(3):
        mid = f"m-{i}"
        w.db.execute(
            "INSERT INTO messages(id,project,sender,destination,text,language,reply_to,"
            "created_at,request_id,seq) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (mid, p, "s-ant" if i % 2 else "s-cl",
             "r-1" if i == 0 else "room", f"hello {i}", "und", None,
             "2026-09-09T00:00:00+00:00", f"req-{i}", i + 1))
        if i == 0:
            w.db.execute("INSERT INTO message_recipients VALUES(?,?)", (mid, "operator"))
            w.db.execute("INSERT INTO message_recipients VALUES(?,?)", (mid, "s-cl"))
    w.db.execute("INSERT INTO events(project,kind,data,created_at) VALUES(?,?,?,?)",
                 (p, "message", "{}", "2026-09-09T00:00:00+00:00"))
    w.close()
    return p


class ExportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.project = "/tmp/fake-toolbox_root"
        self.p = _seed(Path(self.tmp.name), self.project)

    def tearDown(self):
        self.tmp.cleanup()

    def test_read_room_record(self):
        rec = E.read_room_record(Path(self.tmp.name), self.project)
        self.assertEqual(rec["count"], 3)
        self.assertEqual(rec["cursor"], 1)
        self.assertEqual(len(rec["rooms"]), 1)
        self.assertEqual(len(rec["members"]), 2)
        self.assertEqual(len(rec["team"]), 1)
        # message 0 is a named-room post with a frozen audience
        m0 = rec["messages"][0]
        self.assertEqual(m0["destination"], "r-1")
        self.assertEqual(m0["audience"], ["operator", "s-cl"])
        self.assertEqual(m0["agent"], "claude")  # sender s-cl -> claude

    def test_scrub_fails_closed(self):
        import liljack_room_export as mod
        orig = mod.scrub
        try:
            def broken(text):
                return "[text omitted: credential scrubber unavailable]", -1
            mod.scrub = broken
            cleaned, hits = mod.scrub("secret")
            self.assertEqual(cleaned, mod.OMISSION_MARK)
            self.assertEqual(hits, -1)
        finally:
            mod.scrub = orig

    def test_build_mirror_idempotent(self):
        rec = E.read_room_record(Path(self.tmp.name), self.project)
        first = E.build_mirror(rec, Path(self.tmp.name) / "mirror")
        self.assertEqual(first["written"], ["rooms.jsonl", "rooms.meta.json"])
        second = E.build_mirror(rec, Path(self.tmp.name) / "mirror")
        self.assertEqual(second["written"], [])       # unchanged -> skipped
        self.assertEqual(second["skipped"], ["rooms.jsonl", "rooms.meta.json"])
        out = Path(first["root"]) / "rooms.jsonl"
        lines = [json.loads(x) for x in out.read_text().splitlines() if x.strip()]
        self.assertEqual(len(lines), 3)

    def test_export_no_push(self):
        res = E.export(Path(self.tmp.name), self.project, push=False,
                       mirror=Path(self.tmp.name) / "mirror")
        self.assertEqual(res["messages"], 3)
        self.assertEqual(res["pushed"], [])
        self.assertNotIn("push_failed", res)


if __name__ == "__main__":
    unittest.main()
