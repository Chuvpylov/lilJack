"""Room-folder instruction fragment (agents-max-context plan T5, 2026-09-13).

demo carries CLAUDE.md / AGENTS.md; brain/ and the repo do not, so rooms
whose folder is one of those get NO instruction file — only the one-shot intro
prompt, which does not survive compaction. When a room starts we write a marked
fragment into the room folder's CLAUDE.md and AGENTS.md (creating the file, or
refreshing our fragment inside an existing one); when the room resolves we
remove exactly our fragment (deleting the file only if nothing else is in it).
Never touches text outside the markers.
"""
from pathlib import Path

BEGIN = "<!-- liljack:room-fragment:begin -->"
END = "<!-- liljack:room-fragment:end -->"
FILES = ("CLAUDE.md", "AGENTS.md")

LEAD_RULES = [
    "- Announce a shared-file edit in the room BEFORE writing; re-read the file first; one editor per file at a time.",
    "- Every landing: fail-before/pass-after test output, the native suite line, the report path, and a relaunch line with the build id.",
    "- Never park a room in REVIEW: ask the operator in the room, keep WORK; the heartbeat drives owners until every todo is done.",
    "- Sixel-first: the ANSI cell path is a fallback for terminals without sixel, never the target of visual work.",
    "- Update your registry row (task_update progress) whenever you post, so the heartbeat does not hand your task to another session of your agent.",
    "- A direction to another agent is incomplete until it is a durable registry task bound to that agent's exact owner_session and the worker acknowledges or starts it.",
    "- Monitor assigned, stale and blocked rows; explicitly unblock, reassign or escalate them instead of assuming a room message was delivered.",
]


def fragment(room_id, name, purpose, folder, project, standing_lines, overview=""):
    lines = [BEGIN,
             "# lilJack room context (generated; do not edit inside the markers)",
             f"Room: {name} ({room_id})", f"Purpose: {purpose or '-'}", f"Folder: {folder}", f"Project: {project}",
             "Re-read the live context any time (after compaction or a restart):",
             f"    python3 -m liljack_app.cli room --root $LILJACK_WORKSPACE_ROOT --project {project} --context --to {room_id}",
             ""]
    lines += list(standing_lines)
    lines += ["", "Lead rules in force:"] + LEAD_RULES
    if overview:
        lines += ["", "Project overview (from PROJECTS.md):", overview.rstrip()]
    lines += [END]
    return "\n".join(lines) + "\n"


def overview_text(brain_root, max_lines=40):
    """The '### name — role' headings of PROJECTS.md: enough to orient, small enough to keep."""
    p = Path(brain_root) / "PROJECTS.md"
    if not p.exists():
        return ""
    out = []
    for line in p.read_text(errors="ignore").splitlines():
        if line.startswith("### ") and " — " in line:   # project headings only, not the ops sections
            out.append("- " + line[4:].strip())
            if len(out) >= max_lines:
                break
    return "\n".join(out)


def _split(text):
    a, b = text.find(BEGIN), text.find(END)
    if a < 0 or b < 0 or b < a:
        return None
    return text[:a], text[b + len(END):].lstrip("\n")


def ensure_fragment(folder, room_id, name, purpose, project, standing_lines, overview=""):
    """Write/refresh the fragment in CLAUDE.md and AGENTS.md under `folder`.
    Returns the list of files touched. Existing files without our markers are
    left alone (a repo that ships its own instructions keeps them)."""
    folder = Path(folder)
    body = fragment(room_id, name, purpose, str(folder), project, standing_lines, overview)
    touched = []
    for fn in FILES:
        p = folder / fn
        if not p.exists():
            p.write_text(body); touched.append(str(p)); continue
        text = p.read_text(errors="ignore")
        parts = _split(text)
        if parts is None:
            continue                      # foreign file: never touched
        new = parts[0] + body + ("\n" + parts[1] if parts[1] else "")
        if new != text:
            p.write_text(new); touched.append(str(p))
    return touched


def remove_fragment(folder):
    """Remove our fragment; delete the file only if nothing else remains."""
    folder = Path(folder); removed = []
    for fn in FILES:
        p = folder / fn
        if not p.exists():
            continue
        text = p.read_text(errors="ignore"); parts = _split(text)
        if parts is None:
            continue
        rest = (parts[0] + parts[1]).strip()
        if rest:
            p.write_text(parts[0].rstrip("\n") + ("\n" if parts[0].strip() else "") + parts[1])
        else:
            p.unlink()
        removed.append(str(p))
    return removed


def reconcile(store, project, changed_room, overview=None):
    """After a room's lifecycle changed, make its folder's fragment name the room
    that is LIVE there, or remove it when none is.

    ⚠ LAST WRITER WAS NOT THE LIVE ROOM. "ring dashboard rewiew" started twice in
    brain/orc (2026-09-17); the twin that was later removed wrote the fragment last
    and removal never rewrote it, so agents launched in orc aligned into a dead
    room. the operator: "fix the coordination bugs". Best effort: never raises."""
    try:
        from liljack_workspace import project_key
        key = project_key(project)
        folder = store.room_folder(project, changed_room, require_exists=False)
        if not Path(folder).is_dir():
            return []
        live = []
        for row in store.db.execute("SELECT id,state,state_at FROM rooms WHERE project=?", (key,)).fetchall():
            if row["state"] != "active":
                continue
            try:
                if store.room_folder(project, row["id"], require_exists=False) == folder:
                    live.append((row["state_at"] or "", row["id"]))
            except Exception:
                continue
        if not live:
            return remove_fragment(folder)
        room_id = sorted(live)[-1][1]                 # the most recently (re)activated live room
        room = store._room(key, room_id)
        from .room_start import _onboarding_lines
        if overview is None:
            overview = overview_text(Path(__file__).resolve().parents[2])
        return ensure_fragment(folder, room_id, room["name"], room.get("purpose", ""), key,
                               _onboarding_lines(), overview)
    except Exception:
        return []
