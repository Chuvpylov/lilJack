"""context-reinjection (2026-09-13): every nudge names the room and the exact
context command, on ONE line, so a compacted or restarted agent can find its
way back without a human."""
import sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "liljack_app")); sys.path.insert(0, str(REPO / "toolbox"))
import worker_wake as w

R = "r-00000000000000000000000000000001"

def test_owner_nudge_carries_room_and_command():
    text = w.nudge_text({"kind": "owner", "task": "t1", "role": "worker", "room": R})
    assert "'t1'" in text and f"Room {R}" in text and f"--context --to {R}" in text
    assert "\n" not in text and all(ord(c) >= 32 for c in text)

def test_owner_nudge_without_room_has_no_dangling_command():
    text = w.nudge_text({"kind": "owner", "task": "t1", "role": "worker", "room": ""})
    assert "--context" not in text

def test_digest_names_rooms_and_stays_one_line():
    text = w.nudge_text({"kind": "escalate", "escalations": [
        {"owner": "codex", "task": "a", "reason": "stale", "room": R},
        {"owner": "claude", "task": "b", "reason": "x\ny", "room": ""}]})
    assert f"[room {R}]" in text and "\n" not in text and "x y" in text

if __name__ == "__main__":
    test_owner_nudge_carries_room_and_command(); test_owner_nudge_without_room_has_no_dangling_command(); test_digest_names_rooms_and_stays_one_line(); print("worker_wake context: 3 checks PASS")
