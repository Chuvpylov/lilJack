#!/usr/bin/env python3
"""Durable room bootstrap and exact-session delivery form one contract.

The incident behind this proof was a lead instruction posted only as room prose.
No registry row meant no owner work entered the heartbeat, so an idle worker
could not be woken.  The bootstrap must make that boundary explicit, while the
existing worker router must deliver an assigned row only to its exact session.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "liljack_app"))

from liljack_app.room_instructions import fragment  # noqa: E402
from liljack_app.room_start import _onboarding_lines, system_prompt_text  # noqa: E402
import worker_wake as wake  # noqa: E402


def check(value, message):
    if not value:
        raise AssertionError(message)


standing = _onboarding_lines()
joined = "\n".join(standing)
for required in (
    "Room prose is NOT task assignment",
    "exact owner_session",
    "acceptance criteria",
    "The lead owns delivery",
    "The worker starts that row before work",
    "unblock, reassign, or escalate",
):
    check(required in joined, f"standing bootstrap carries {required!r}")

system = system_prompt_text("/project", "r-room")
check("Room prose is NOT task assignment" in system,
      "system prompt retains the cooperation contract after compaction")
check("incomplete until it is a registry task bound" in system,
      "system prompt carries the lead's exact-session delivery duty")

folder_fragment = fragment("r-room", "Room", "purpose", "/folder", "/project", standing)
check("Room prose is NOT task assignment" in folder_fragment,
      "AGENTS/CLAUDE room fragment retains the cooperation contract")
check("exact owner_session" in folder_fragment,
      "room fragment binds lead directions to exact sessions")

people = [
    {"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"},
    {"id": "s-codex-a", "agent": "codex", "harness": "codex", "role": "worker"},
    {"id": "s-codex-b", "agent": "codex", "harness": "codex", "role": "worker"},
]
wake.candidates = lambda project, root=None, live=None: people
wake.room_allows = lambda root, session, room: True
idle = lambda session, project, harness: {"state": "idle"}

# Fail-before reproducer: room prose has no registry task, therefore the
# heartbeat has no open owner item and cannot invent an assignment.
check(wake.pick("/project", idle, {}, owner_sessions={}, task_rooms={}) is None,
      "unassigned room prose produces no worker wake")

# Pass-after contract: once the lead creates the durable assigned row, the
# exact owner_session wins even when two live sessions share one agent name.
choice = wake.pick(
    "/project",
    idle,
    {"codex": ["lead-issued-task"]},
    owner_sessions={"lead-issued-task": "s-codex-b"},
    task_rooms={"lead-issued-task": "r-room"},
    task_states={"lead-issued-task": "assigned"},
)
check(choice and choice["session"] == "s-codex-b",
      "lead-issued assigned work reaches its exact worker session")
check(choice["task"] == "lead-issued-task" and choice["room"] == "r-room",
      "the exact-session wake retains task and room identity")

print("cooperation contract: bootstrap + exact-session delivery PASS")
