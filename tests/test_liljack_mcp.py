#!/usr/bin/env python3
"""tests/test_liljack_mcp.py — JSON-RPC roundtrip against a fixture mirror."""
import json, subprocess, sys, tempfile, pathlib, os, unittest
from pathlib import Path
from unittest.mock import patch
TOOLBOX = pathlib.Path(__file__).parent.parent / "toolbox"
sys.path.insert(0, str(TOOLBOX))

def _fixture():
    tmp = pathlib.Path(tempfile.mkdtemp())
    d = tmp / "mcp-projects" / "demo"; d.mkdir(parents=True)
    (d / "context.md").write_text("# demo — context\nDemo project.\n")
    (d / "sessions.jsonl").write_text(json.dumps({"session_id": "s1", "archived_at": "2026-07-26"}) + "\n")
    c = tmp / "chains" / "demo"; c.mkdir(parents=True)
    (c / "s1.chains.jsonl").write_text(json.dumps(
        {"human": "fix the login bug in auth", "final": "fixed comparison",
         "session_id": "s1", "ts": "2026-07-26", "steps": [],
         "outcome": {"files_edited": [], "tests": "pass", "turn_count": 1}}) + "\n")
    return tmp

def _rpc(proc, req):
    proc.stdin.write(json.dumps(req) + "\n"); proc.stdin.flush()
    return json.loads(proc.stdout.readline())

def test_roundtrip():
    tmp = _fixture()
    proc = subprocess.Popen(
        [sys.executable, str(TOOLBOX / "liljack_mcp.py"),
         "--mirror", str(tmp / "mcp-projects"), "--chains", str(tmp / "chains")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        assert r["result"]["serverInfo"]["name"] == "liljack"
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        names = {t["name"] for t in r["result"]["tools"]}
        # ⚠ PINNED ROSTER. Updated 2026-09-05 when the task registry landed: the
        # four task_* tools are the coordination layer's API for both harnesses.
        assert names == {"project_context", "project_todo", "search_chains", "project_history",
                 "send_message", "read_messages", "board_read", "board_update",
                 "task_list", "task_propose", "task_assign", "task_update"}
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                        "params": {"name": "project_context", "arguments": {"project": "demo"}}})
        assert "Demo project." in r["result"]["content"][0]["text"]
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                        "params": {"name": "search_chains",
                                   "arguments": {"query": "login bug auth", "project": "demo"}}})
        assert "fix the login bug" in r["result"]["content"][0]["text"]
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                        "params": {"name": "project_history", "arguments": {"project": "demo"}}})
        assert "s1" in r["result"]["content"][0]["text"]
        r = _rpc(proc, {"jsonrpc": "2.0", "id": 6, "method": "nope", "params": {}})
        assert r["error"]["code"] == -32601
    finally:
        proc.kill()

def test_bound_identity():
    import liljack_mcp as m
    # A caller must not bypass task permissions by claiming operator identity.
    for configured, detected in (("codex", ""), ("", "codex")):
        with patch.object(m, "AGENT", configured), \
             patch.object(m.liljack_mail, "detect_agent", return_value=detected), \
             patch.object(m.liljack_tasks, "propose") as propose:
            assert m._me() == "codex"
            assert m._me("codex") == "codex"
            result = m.handle({"method": "tools/call", "params": {
                "name": "task_propose", "arguments": {
                    "title": "fixture", "owner": "claude", "agent": "operator"}}})
            assert "conflicts with this harness" in result["content"][0]["text"]
            propose.assert_not_called()
    with patch.object(m, "AGENT", ""), \
         patch.object(m.liljack_mail, "detect_agent", return_value="unknown"):
        assert m._me("codex") == "codex"
        try:
            m._me("unknown")
        except ValueError:
            pass
        else:
            raise AssertionError("unknown fallback identity accepted")


class RoutingTests(unittest.TestCase):
    def test_exact_room_and_private_separation(self):
        import liljack_mcp as m
        from liljack_workspace import Workspace
        with tempfile.TemporaryDirectory() as tmp:
            project = str(Path(tmp)/'project')
            w = Workspace(Path(tmp)/'workspace')
            try:
                sid = w.observe({'harness':'tmux','session':'test/codex','agent':'codex','cwd':project})['id']
                room = w.room_create(project,'fixture',actor='operator')['id']
                other = w.room_create(project,'other',actor='operator')['id']
                w.room_move(project,sid,room,actor='operator')
                env = {'LILJACK_AGENT':'codex','LILJACK_WORKSPACE_SESSION':sid,'LILJACK_WORKSPACE_PROJECT':project,'LILJACK_WORKSPACE_ROOT':str(Path(tmp)/'workspace')}
                with patch.dict(os.environ,env), patch.object(m,'AGENT','codex'), patch.object(m.liljack_mail,'send') as mail:
                    body = 'literal `echo hi` $(echo hi)\nsecond line'
                    result = m.t_send_message(to='all',thread=room,text=body)
                    row = w.db.execute('SELECT * FROM messages').fetchone()
                    self.assertIsNotNone(row, 'room sequence did not receive the MCP message')
                    self.assertEqual(row['destination'],room)
                    self.assertEqual(row['sender'],sid)
                    self.assertEqual(row['text'],body)
                    self.assertIn(room,result)
                    mail.assert_not_called()
                    with self.assertRaises(PermissionError):
                        m.t_send_message(to='all',thread=other,text='forbidden')
                    self.assertEqual(w.db.execute('SELECT COUNT(*) FROM messages').fetchone()[0],1)
                    mail.return_value={'to':'claude','to_session':'s-private','id':'fixture','text':'private'}
                    m.t_send_message(to='claude',to_session='s-private',thread=room,text='private')
                    mail.assert_called_once()
                    mail.reset_mock()
                    m.t_send_message(to='claude',text='ordinary mailbox')
                    mail.assert_called_once()
                    self.assertEqual(w.db.execute('SELECT COUNT(*) FROM messages').fetchone()[0],1)
            finally:
                w.close()

    def test_mail_to_all_from_room_member_reaches_room_stream(self):
        """coord-a (the operator 2026-09-17, "fix the coordination bugs"): a lead's
        send_message(to='all') replied "sent to all (room r-…)" but wrote only a
        mailbox row, so the room stream never had the landing and the worker's
        review stalled. A room member's broadcast is a room post."""
        import liljack_mcp as m
        from liljack_workspace import Workspace
        with tempfile.TemporaryDirectory() as tmp:
            project = str(Path(tmp)/'project')
            w = Workspace(Path(tmp)/'workspace')
            try:
                sid = w.observe({'harness':'tmux','session':'test/claude','agent':'claude','cwd':project})['id']
                loner = w.observe({'harness':'tmux','session':'test/codex','agent':'codex','cwd':project})['id']
                room = w.room_create(project,'fixture',actor='operator')['id']
                w.room_move(project,sid,room,actor='operator')
                env = {'LILJACK_AGENT':'claude','LILJACK_WORKSPACE_SESSION':sid,'LILJACK_WORKSPACE_PROJECT':project,'LILJACK_WORKSPACE_ROOT':str(Path(tmp)/'workspace')}
                with patch.dict(os.environ,env), patch.object(m,'AGENT','claude'), patch.object(m.liljack_mail,'send') as mail:
                    result = m.t_send_message(to='all',text='LANDING ready for review')
                    row = w.db.execute('SELECT * FROM messages').fetchone()
                    self.assertIsNotNone(row,'a room member broadcast never reached the room stream')
                    self.assertEqual((row['destination'],row['sender'],row['text']),(room,sid,'LANDING ready for review'))
                    self.assertIn('seq',result)
                    mail.assert_not_called()
                    # an exact-session DM is still private mail, never a room post
                    mail.return_value={'to':'codex','to_session':'s-x','id':'fixture','text':'dm'}
                    m.t_send_message(to='codex',to_session='s-x',text='dm')
                    mail.assert_called_once()
                    self.assertEqual(w.db.execute('SELECT COUNT(*) FROM messages').fetchone()[0],1)
                # a session in no room keeps the untagged mailbox broadcast
                env2 = dict(env,LILJACK_AGENT='codex',LILJACK_WORKSPACE_SESSION=loner)
                with patch.dict(os.environ,env2), patch.object(m,'AGENT','codex'), patch.object(m.liljack_mail,'send') as mail:
                    mail.return_value={'to':'all','to_session':'','room':'','id':'fixture','text':'hello'}
                    m.t_send_message(to='all',text='hello')
                    mail.assert_called_once()
                    self.assertEqual(w.db.execute('SELECT COUNT(*) FROM messages').fetchone()[0],1)
            finally:
                w.close()


if __name__ == "__main__":
    test_roundtrip(); print("  ✓ test_roundtrip")
    test_bound_identity(); print("  ✓ test_bound_identity")
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(RoutingTests))
    if not result.wasSuccessful(): sys.exit(1)
    print("OK test_liljack_mcp")
