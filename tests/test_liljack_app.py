#!/usr/bin/env python3
"""Store/VT/tmux tests. Native UI/docking tests are the adjacent C tests."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
from liljack_workspace import Workspace, Selection, session_key
from liljack_app.native import load, Terminal
from liljack_app.runtime import Tmux, AttachedTerminal, room_instruction


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.w = Workspace(self.tmp.name)
        self.project = str(Path(self.tmp.name) / "project")
        self.a = self.observe("a")
        self.b = self.observe("b")
        self.c = self.observe("c")

    def tearDown(self):
        self.w.close()
        self.tmp.cleanup()

    def observe(self, native, **extra):
        return self.w.observe({"session": native, "harness": "codex", "agent": "codex",
                               "cwd": self.project, "state": "busy", **extra})["id"]

    def test_same_provider_and_cross_harness_identity(self):
        self.assertNotEqual(self.a, self.b)
        self.assertNotEqual(session_key("codex", "a"), session_key("claude", "a"))
        self.assertEqual(len(self.w.snapshot(self.project)["sessions"]), 3)

    def test_roles_survive_observation_cycles_and_cross_project(self):
        # One lead per project (the handoff fix): a=lead, with b=worker and c=reviewer under it.
        self.w.assign(self.a, "lead", actor="operator")
        self.w.assign(self.b, "worker", self.a, actor="operator")
        self.w.assign(self.c, "reviewer", self.a, actor="operator")
        self.observe("a", state="idle")
        self.assertTrue(self.w.may_control(self.a, self.b))
        self.assertTrue(self.w.may_control(self.a, self.c))
        self.assertFalse(self.w.may_control(self.b, self.c))
        self.assertFalse(self.w.may_control(self.c, self.a))
        with self.assertRaises(ValueError): self.w.assign(self.a, "lead", self.b, actor="operator")
        with self.assertRaises(PermissionError): self.w.assign(self.c, "lead", actor=self.c)
        with self.assertRaises(ValueError): self.w.assign(self.a, "worker", actor="operator")
        other = self.observe("other", cwd=self.tmp.name)
        with self.assertRaises(ValueError): self.w.assign(other, "worker", self.a, actor="operator")
        self.assertFalse(self.w.may_control(self.a, other))

    def test_binding_visible_after_refresh_and_expires(self):
        self.w.bind_terminal(self.a, "socket", "session", "%1", actor="operator")
        self.observe("a", state="idle")
        a = next(s for s in self.w.snapshot(self.project)["sessions"] if s["id"] == self.a)
        self.assertIn("terminal", a["capabilities"])
        with patch("liljack_workspace.time.time", return_value=time.time()+30):
            a = next(s for s in self.w.snapshot(self.project)["sessions"] if s["id"] == self.a)
            self.assertNotIn("terminal", a["capabilities"])

    def test_unicode_retry_history_and_private_events(self):
        before = self.w.snapshot(self.project, viewer=self.a)["cursor"]
        m = self.w.post(self.project, self.a, "Привіт 日本語 🌿", "request-1", language="uk")
        again = self.w.post(self.project, self.a, "Привіт 日本語 🌿", "request-1", language="uk")
        self.assertEqual(m, again)
        with self.assertRaises(ValueError): self.w.post(self.project, self.a, "different", "request-1")
        private = self.w.post(self.project, self.b, "Private review", "request-2", destination=self.c)
        messages = self.w.snapshot(self.project, viewer=self.a)["messages"]
        self.assertEqual([r["id"] for r in messages], [m["id"]])
        events = self.w.events(self.project, before, viewer=self.a)
        self.assertEqual([e["data"]["id"] for e in events["events"]], [m["id"]])
        self.assertEqual(self.w.events(self.project, events["cursor"], viewer=self.a)["events"], [])
        with self.assertRaises(ValueError):
            self.w.post(self.project, self.c, "leak", "bad", reply_to=private["id"])
        self.w.close()
        self.w = Workspace(self.tmp.name)
        self.assertEqual(len(self.w.snapshot(self.project)["messages"]), 2)

    def test_focus_does_not_follow_order(self):
        sel = Selection()
        rows = self.w.snapshot(self.project)["sessions"]
        sel.select(self.b, rows)
        sel.reconcile(list(reversed(rows)))
        self.assertEqual(sel.session_id, self.b)
        with self.assertRaises(ValueError): sel.focus_terminal(rows)
        sel.reconcile([r for r in rows if r["id"] != self.b])
        self.assertIsNone(sel.session_id)


class VTTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.lib = load()
    def setUp(self): self.vt = Terminal(self.lib, 20, 5)
    def tearDown(self): self.vt.close()

    def test_utf8_fragmentation_width_and_combining(self):
        raw = "Привіт 日本🌿e\u0301".encode()
        for byte in raw: self.vt.feed(bytes([byte]))
        self.assertTrue(self.vt.text(0).startswith("Привіт 日本🌿e\u0301"))
        self.assertEqual(self.vt.state(0), 14)
        self.assertEqual(self.vt.row(0)[8].ch, 0)  # wide continuation
        self.vt.feed(b"\xF0\x80\x80\x80")
        self.assertIn("�", self.vt.text(0))

    def test_alt_screen_resize_modes_and_colors(self):
        self.vt.feed("normal\x1b[?1049hother\x1b[?1002;1006;2004h")
        self.assertTrue(self.vt.text(0).startswith("other"))
        self.assertEqual(self.vt.state(4),1002)
        self.assertEqual(self.vt.state(5),1)
        self.vt.resize(30,8)
        self.vt.feed("\x1b[?1049l")
        self.assertTrue(self.vt.text(0).startswith("normal"))
        self.vt.feed("\x1b[H\x1b[38;2;1;2;3mX")
        self.assertEqual(self.vt.row(0)[0].fg,0x1010203)

    def test_independent_terminals_scroll_and_hostile_csi(self):
        other = Terminal(self.lib,20,5)
        try:
            self.vt.feed("one")
            other.feed("two")
            self.assertTrue(self.vt.text(0).startswith("one"))
            self.assertTrue(other.text(0).startswith("two"))
            self.vt.feed("\r\n".join(f"line {i}" for i in range(15)))
            self.vt.scroll(3)
            self.assertEqual(self.vt.state(8),3)
            self.vt.feed("\x1b[999999999999999999999999H\x1b[999999999999L\x1b[9999999999P")
            self.assertLess(self.vt.state(0),20)
            self.assertLess(self.vt.state(1),5)
        finally: other.close()

    def test_room_instruction_controls_removed(self):
        text = room_instruction({"id":"m1","project":"/tmp/project","sender":"operator",
                                 "text":"hello\x1b[201~\rworld\x07"},"/tmp")
        self.assertNotIn("\x1b",text)
        self.assertNotIn("\r",text)

    def test_wide_erase_and_bounded_scroll(self):
        self.vt.feed("日本語\x1b[1;2H\x1b[X")
        self.assertEqual(self.vt.row(0)[0].ch,32)
        self.assertEqual(self.vt.row(0)[1].ch,32)
        start=time.monotonic()
        for _ in range(100): self.vt.feed("\x1b[9999999999S\x1b[9999999999T")
        self.assertLess(time.monotonic()-start,1)


class PTYTests(unittest.TestCase):
    def test_two_real_pty_clients_resize_and_detach(self):
        tmux = Tmux("liljack-test-" + str(os.getpid()))
        if not tmux.binary:
            self.skipTest("tmux unavailable")
        # Owned FOREGROUND server: no daemonization, no live app socket touched.
        server = subprocess.Popen([tmux.binary,"-L",tmux.socket,"-f","/dev/null","-D"],
                                  stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        clients=[]
        try:
            with tempfile.TemporaryDirectory() as tmp:
                for _ in range(100):
                    r=tmux.call("show-options","-g",check=False)
                    if r.returncode==0:break
                    time.sleep(.02)
                self.assertIsNone(server.poll())
                ids=[tmux.create("shell",tmp,tmp) for _ in range(2)]
                rows=tmux.sessions(tmp)
                self.assertEqual(set(ids),{r["id"] for r in rows})
                lib=load()
                for row in rows: clients.append(AttachedTerminal(lib,tmux,row,60,12))
                deadline=time.monotonic()+2
                while time.monotonic()<deadline:
                    for c in clients:c.poll()
                    time.sleep(.02)
                for i,c in enumerate(clients):
                    c.send(f"printf 'ONLY_{i}\\n'\r")
                deadline=time.monotonic()+2
                while time.monotonic()<deadline:
                    for c in clients:c.poll()
                    time.sleep(.02)
                for i,c in enumerate(clients):
                    text="\n".join(c.vt.text(r) for r in range(c.vt.rows))
                    self.assertIn(f"ONLY_{i}",text)
                    self.assertNotIn(f"ONLY_{1-i}",text)
                clients[0].resize(77,19)
                time.sleep(.1)
                size=tmux.call("display-message","-p","-t",rows[0]["pane"],"#{pane_width} #{pane_height}").stdout.strip()
                self.assertEqual(size,"77 19")
                for c in clients:c.close()
                clients=[]
                self.assertEqual(len(tmux.sessions(tmp)),2)
        finally:
            for c in clients:c.close()
            server.terminate()
            try:server.wait(timeout=3)
            except subprocess.TimeoutExpired:server.kill();server.wait()


if __name__ == "__main__":
    unittest.main(verbosity=2)
