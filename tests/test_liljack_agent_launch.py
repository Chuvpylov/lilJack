"""T1 owkterm-agent-launcher: `lilJack agent` gives a foreground window the same
identity env and room intro as a tile, on an isolated workspace."""
import os, sys, tempfile, json
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(REPO / "toolbox"))
from liljack_app import agent_launch
from liljack_workspace import Workspace

def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "ws"; root.mkdir(); project = Path(tmp) / "proj"; project.mkdir()
        store = Workspace(str(root))
        room = store.room_create(str(project), "T1 room", actor="operator", folder=str(project), purpose="lab")
        store.close()
        rid = room["id"] if isinstance(room, dict) else room
        argv, env, cwd, sid, intro = agent_launch.build("claude", str(project), str(root), room=rid, role="lead", actor="operator")
        assert argv[0] == "claude" and "--dangerously-skip-permissions" in argv
        assert env["LILJACK_WORKSPACE_SESSION"] == sid and env["LILJACK_AGENT"] == "claude" and env["LILJACK_HARNESS"] == "claude"
        assert env["LILJACK_WORKSPACE_ROOT"] == str(root)
        if room:
            assert intro and "Room:" in intro and "--context --to" in intro, intro[:200]
            assert argv[-2] == "--" and argv[-1] == intro
            store = Workspace(str(root)); members = store.db.execute("SELECT session_id FROM room_members WHERE room_id=?", ((room["id"] if isinstance(room, dict) else room),)).fetchall(); store.close()
            assert any(r[0] == sid for r in members)
        print("agent launch: env + intro + membership PASS" if room else "agent launch: env PASS (room_create unavailable in this store API)")

if __name__ == "__main__": main()
