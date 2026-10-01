"""Isolated three-agent routing and MCP identity regressions."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLBOX = Path(__file__).resolve().parents[1] / "toolbox"
sys.path.insert(0, str(TOOLBOX))
import liljack_mail as mail


class CommunicationTests(unittest.TestCase):
    def test_three_way_routing_and_receipts(self):
        with tempfile.TemporaryDirectory() as root:
            for sender in ("codex", "claude", "deepseek"):
                for recipient in ("codex", "claude", "deepseek"):
                    if sender == recipient:
                        continue
                    row = mail.send("ping", recipient, frm=sender, root=root,
                                    project="test", thread="handshake")
                    self.assertIn(row["id"], [r["id"] for r in mail.inbox(recipient, root=root)])
                    mail.ack(row["id"], recipient, root=root)
                    receipt = mail.sent(sender, root=root)[-1]
                    self.assertEqual(receipt["read_by"], [recipient])
                    self.assertIn("read by: " + recipient, mail.render([receipt]))
            mail.send("other", "claude", frm="codex", root=root, thread="other")
            rows = mail.sent("codex", 1, root=root, project="test", thread="handshake")
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["to"], "deepseek")

    def test_mcp_identity_without_host_environment(self):
        for adapter, expected in (("liljack_codex.py", "codex"),
                                  ("liljack_opencode.py", "deepseek")):
            with self.subTest(adapter=adapter), tempfile.TemporaryDirectory() as root:
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith(("CODEX_", "CLAUDE", "LILJACK_"))}
                env.update(LILJACK_CACHE=root, CUDA_VISIBLE_DEVICES="")
                requests = [
                    {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                        "name": "send_message", "arguments": {
                            "to": "claude", "text": "fixture", "thread": "test"}}},
                    {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                        "name": "read_messages", "arguments": {
                            "sent": True, "thread": "absent"}}},
                    {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                        "name": "read_messages", "arguments": {
                            "sent": True, "thread": "test"}}},
                ]
                p = subprocess.run([sys.executable, str(TOOLBOX / adapter), "mcp"],
                                   input="".join(json.dumps(r) + "\n" for r in requests),
                                   text=True, capture_output=True, env=env, timeout=20)
                self.assertEqual(p.returncode, 0, p.stderr)
                replies = [json.loads(line) for line in p.stdout.splitlines()]
                self.assertTrue(all("result" in r for r in replies), replies)
                self.assertEqual(mail.sent(expected, root=root)[0]["frm"], expected)
                self.assertIn("nothing sent", replies[1]["result"]["content"][0]["text"])
                self.assertIn("read by: nobody yet", replies[2]["result"]["content"][0]["text"])


if __name__ == "__main__":
    unittest.main()
