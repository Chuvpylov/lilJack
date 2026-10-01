"""`lilJack agent <claude|codex|deepseek> [--room R] [--project P] [--role worker]`

Launch an agent in THE CURRENT TERMINAL (an owkTerm window, WezTerm, a plain
shell) with exactly what a lilJack tile gives it: the harness argv from
runtime.Tmux.command, the LILJACK_* identity env, and the room intro from
room_start.intro_text — so a standalone window is not a stranger to the
workspace (agents-max-context plan T1, 2026-09-13). The session is registered
in the workspace store (backend "foreground") and attached to the room as a
member before the harness starts. Liveness for foreground sessions is not
tracked by the heartbeat's lifecycle lookup (documented limitation).
"""
import argparse
import os
import sys
import uuid
from pathlib import Path

from liljack_workspace import Workspace, project_key, utcnow
from .runtime import Tmux, app_child_env

HARNESS = {"claude": "claude", "codex": "codex", "deepseek": "opencode"}


def build(agent, project, root, room="", role="worker", actor=None, task_path=None, cwd=None, register=True):
    """Register the session (unless register=False: dry run) and return (argv, env, cwd, session_id, intro)."""
    if agent not in HARNESS:
        raise ValueError("agent must be claude, codex or deepseek")
    store = Workspace(root)
    try:
        project = project_key(project)
        actor = actor or os.environ.get("LILJACK_WORKSPACE_SESSION") or "operator"
        folder = store.room_folder(project, room) if room else project_key(cwd or project)
        if room and register:
            store.spawn_parent(project, room, role, actor=actor)
        name = "fg-" + uuid.uuid4().hex[:12]
        row = {"session": name, "harness": "foreground", "backend": "foreground", "agent": agent,
               "cwd": folder, "project": project, "tmux_name": name, "state": "running",
               "event": "AgentLaunch", "at": utcnow()}
        from liljack_workspace import session_key
        sid = session_key("foreground", name)
        intro = None
        if register:
            store.observe(row)
        if room:
            from .room_start import intro_text
            if register:
                store.attach_spawned(project, sid, room, role, actor=actor)
                intro = intro_text(store, project, room, sid, task_path=task_path)
                store.save_intro(sid, intro)
            else:
                intro = f"(dry run) room {room}: the intro is generated at launch"
    finally:
        store.close()
    argv = list(Tmux.command(agent))
    if intro:
        from liljack_workspace import clean
        intro = clean(intro, 48000)
        argv += ["--", intro] if agent in ("claude", "codex") else ["--prompt", intro]
    env = app_child_env()
    env.update({"LILJACK_WORKSPACE_SESSION": sid, "LILJACK_WORKSPACE_ROOT": str(root),
                "LILJACK_WORKSPACE_PROJECT": project, "LILJACK_AGENT": agent,
                "LILJACK_HARNESS": HARNESS[agent]})
    if agent == "deepseek":
        env["OPENCODE_PERMISSION"] = '{"*":"allow"}'
    return argv, env, folder, sid, intro


def main(argv):
    p = argparse.ArgumentParser(prog="lilJack agent", description=__doc__.split("\n")[0])
    p.add_argument("agent", choices=sorted(HARNESS))
    p.add_argument("--room", default="", help="room id (r-...) to join as a member")
    p.add_argument("--project", default=os.environ.get("LILJACK_WORKSPACE_PROJECT") or os.getcwd())
    p.add_argument("--root", default=os.environ.get("LILJACK_WORKSPACE_ROOT") or str(Path.home() / ".cache/liljack/workspace"))
    p.add_argument("--role", default="worker", choices=["lead", "worker", "reviewer"])
    p.add_argument("--dry-run", action="store_true", help="print argv/env/session and exit")
    a = p.parse_args(argv)
    cmd, env, cwd, sid, intro = build(a.agent, a.project, a.root, room=a.room, role=a.role, register=not a.dry_run)
    if a.dry_run:
        print(f"session {sid}\ncwd {cwd}\nargv {cmd[:3]}{' -- <intro %d chars>' % len(intro) if intro else ''}")
        for k in sorted(env):
            if k.startswith("LILJACK_"):
                print(f"{k}={env[k]}")
        return 0
    os.chdir(cwd)
    os.execvpe(cmd[0], cmd, env)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
