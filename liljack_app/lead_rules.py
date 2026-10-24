"""Lead rules: ONE editable list, read by every place that tells agents the rules.

Before 2026-10-24 the rules lived as two hardcoded lists (room_instructions and
room_start) that had already drifted apart in wording, and nobody could change
them without a code edit. The operator: "i want to be able to edit rules". The NEW ROOM
form edits this list (backend actions rules_get / rules_set / rules_reset); the
room-folder CLAUDE.md/AGENTS.md fragment and the agent system prompt both read
it through load(), so an edit reaches every room started or reconciled after it.

Stored as a JSON array of strings at $LILJACK_RULES_PATH, default
~/.config/liljack/lead_rules.json. No file (or an unreadable one) = DEFAULTS.
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


def path() -> Path:
    raw = os.environ.get("LILJACK_RULES_PATH", "").strip()
    return Path(raw) if raw else Path.home() / ".config" / "liljack" / "lead_rules.json"


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


def load() -> list:
    try:
        return validate(json.loads(path().read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return list(DEFAULTS)


def is_custom() -> bool:
    return path().is_file()


def save(rules) -> list:
    clean = validate(rules)
    p = path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps(clean, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, p)
    return clean


def reset() -> list:
    try:
        path().unlink()
    except FileNotFoundError:
        pass
    return list(DEFAULTS)


def lines() -> list:
    """The rules as the bulleted lines the fragment and the prompt print."""
    return ["- " + r for r in load()]


def packet() -> dict:
    """What the backend hands the NEW ROOM form."""
    return {"rules": load(), "defaults": list(DEFAULTS), "custom": is_custom(),
            "max_rules": MAX_RULES, "max_chars": MAX_CHARS}
