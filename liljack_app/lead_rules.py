"""Lead rules: ONE editable list, read by every place that tells agents the rules.

Before 2026-10-24 the rules lived as two hardcoded lists (room_instructions and
room_start) that had already drifted apart in wording, and nobody could change
them without a code edit. The operator: "i want to be able to edit rules". The NEW ROOM
form edits this list (backend actions rules_get / rules_set / rules_reset); the
room-folder CLAUDE.md/AGENTS.md fragment and the agent system prompt both read
it through load(), so an edit reaches every room started or reconciled after it.

Stored as a JSON array of strings at $LILJACK_RULES_PATH, default
~/.config/liljack/lead_rules.json. No file (or an unreadable one) = DEFAULTS.

⚠ THREE SECTIONS, ONE EDITOR (the operator, 2026-10-26: "all the agent rules and
context and initial message should be editable in a menu and when creating as
a button"). `rules` is the list above; `context` is the STANDING INSTRUCTION SET
every intro, system prompt and room-folder fragment carries; `intro` is the
guidance that closes every agent's first message. Each lives in its own JSON
file BESIDE lead_rules.json, so a custom $LILJACK_RULES_PATH moves all three.
"""
import json
import os
from pathlib import Path

MAX_RULES = 24
MAX_CHARS = 400

DEFAULTS = (
    "Announce a shared-file edit in the room BEFORE writing; re-read the file first; one editor per file at a time.",
    "Every landing: fail-before/pass-after test output, the native suite line, the report path, and a relaunch line with the build id.",
    "Never park a room in REVIEW: ask the operator in the room, keep WORK; the heartbeat drives owners until every todo is done.",
    "Sixel-first: the ANSI cell path is a fallback for terminals without sixel, never the target of visual work.",
    "Update your registry row (task_update progress) whenever you post, so the heartbeat does not hand your task to another session of your agent.",
    "A direction to another agent is incomplete until it is a durable registry task bound to that agent's exact owner_session and the worker acknowledges or starts it.",
    "Monitor assigned, stale and blocked rows; explicitly unblock, reassign or escalate them instead of assuming a room message was delivered.",
)


CONTEXT_DEFAULTS = (
    "One task registry (the durable work queue). Move YOUR tasks with task_update "
    "(start/block/complete/abandon/progress). Reading a message never consumes or finishes a task.",
    "One shared board for standing context. board_update writes ONE item (task+state, "
    "or a topic+value note). Owner writes; lead appends.",
    "One mailbox for direct messages (claude | codex | deepseek | operator | all). "
    "Private DMs stay separate from rooms.",
    "One room protocol: ALIGN -> PLAN -> WORK -> REVIEW -> RESOLVED. The lead drives phases. "
    "Distinguish the message sequence (chronological) from the event cursor (snapshot); "
    "state which you last read.",
    "Room prose is NOT task assignment. Before directing a worker, the lead must create or "
    "assign one durable registry task with that worker's exact owner_session, acceptance "
    "criteria, and avoid scope. The worker starts that row before work and records progress "
    "whenever posting.",
    "The lead owns delivery: confirm each assigned worker acknowledges or starts its exact "
    "task, monitor stale/blocked rows, and explicitly unblock, reassign, or escalate them. "
    "Workers must not silently substitute room prose for an owned task or mark work complete "
    "by merely reading a message.",
    "Post room text with --text-file PATH or --text - (stdin); never put backticks or $( ) "
    "inside a double-quoted --text — the shell substitutes them (2026-09-12: a `git stash` ran "
    "in the demo tree). MCP-capable agents: prefer the send_message tool, which takes the "
    "body as a parameter and never goes through a shell.",
    "Report what was checked, what was found with evidence, and what was NOT verified, to "
    "docs/codex/reports/. Then stop.",
    "Announce shared-file edits and get an ownership handoff before touching another agent's "
    "files; re-read before writing. Do not infer permissions from another room.",
    "Scrub credentials; never print a ring token or credential-shaped value.",
)

INTRO_DEFAULTS = (
    "Coordinate and report in this room using its exact room ID. Keep private messages separate.",
    "Follow the working folder instructions. Do not infer permissions from another room.",
)

SECTIONS = {"rules": DEFAULTS, "context": CONTEXT_DEFAULTS, "intro": INTRO_DEFAULTS}
_FILES = {"context": "agent_context.json", "intro": "agent_intro.json"}


def _section(section) -> str:
    section = str(section or "rules").strip().lower()
    if section not in SECTIONS:
        raise ValueError(f"section must be one of {', '.join(SECTIONS)}")
    return section


def path(section="rules") -> Path:
    raw = os.environ.get("LILJACK_RULES_PATH", "").strip()
    rules = Path(raw) if raw else Path.home() / ".config" / "liljack" / "lead_rules.json"
    section = _section(section)
    return rules if section == "rules" else rules.with_name(_FILES[section])


def validate(rules) -> list:
    """The cleaned list, or ValueError saying what is wrong with it."""
    if not isinstance(rules, (list, tuple)):
        raise ValueError("rules must be a list of strings")
    out = []
    for item in rules:
        if not isinstance(item, str):
            raise ValueError("each rule must be a string")
        text = item.strip()
        if text.startswith("- "):
            text = text[2:].strip()
        if not text:
            continue                      # an emptied row in the editor is a deletion
        if any(ord(c) < 32 or 127 <= ord(c) < 160 for c in text):
            raise ValueError("a rule must be one line without control characters")
        if len(text) > MAX_CHARS:
            raise ValueError(f"a rule is limited to {MAX_CHARS} characters")
        out.append(text)
    if not out:
        raise ValueError("keep at least one rule (or reset to the defaults)")
    if len(out) > MAX_RULES:
        raise ValueError(f"at most {MAX_RULES} rules")
    return out


def load(section="rules") -> list:
    section = _section(section)
    try:
        return validate(json.loads(path(section).read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return list(SECTIONS[section])


def is_custom(section="rules") -> bool:
    return path(section).is_file()


def save(rules, section="rules") -> list:
    clean = validate(rules)
    p = path(section)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps(clean, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, p)
    return clean


def reset(section="rules") -> list:
    section = _section(section)
    try:
        path(section).unlink()
    except FileNotFoundError:
        pass
    return list(SECTIONS[section])


def lines(section="rules") -> list:
    """The section as the bulleted lines the fragment and the prompt print."""
    return ["- " + r for r in load(section)]


def packet(section="rules") -> dict:
    """What the backend hands the rules editor (NEW ROOM form or logo menu)."""
    section = _section(section)
    return {"section": section, "rules": load(section), "defaults": list(SECTIONS[section]),
            "custom": is_custom(section), "max_rules": MAX_RULES, "max_chars": MAX_CHARS}
