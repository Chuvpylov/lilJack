#!/usr/bin/env python3
"""
liljack_pretool.py — PreToolUse hook for Claude Code

Updates lilJack's task label when superpowers skills are invoked, so
the statusline reflects what's actually happening (DESIGNING, PLANNING, etc.)

Also captures notifications of interest (large Bash commands, risky operations).

Configure in ~/.claude/settings.json:
  "hooks": {
    "PreToolUse": [{"hooks": [{"type": "command", "command": "python3 /path/liljack_pretool.py"}]}]
  }
"""

import json
import pathlib
import sys
import time

from liljack_common import (
    CACHE, CONTEXT_FILE, _load_context, _save_context,
    detect_project_context, read_memories, _filter_atoms_for_project,
)

OVERRIDE_FILE = CACHE / "task_override.txt"
SIGNAL_FILE   = CACHE / "pending_feedback.txt"

# Map superpowers skill names → task labels
SKILL_TASKS = {
    "brainstorming":                 "DESIGNING",
    "writing-plans":                 "PLANNING",
    "subagent-driven-development":   "ORCHESTRATING",
    "finishing-a-development-branch":"FINISHING",
    "executing-plans":               "EXECUTING",
    "test-driven-development":       "TESTING·TDD",
    "debugging":                     "DEBUGGING",
    "requesting-code-review":        "REVIEWING",
    "using-git-worktrees":           "BRANCHING",
    "code-simplifier":               "REFACTORING",
    "code-review":                   "REVIEWING",
    "frontend-design":               "DESIGNING-UI",
    "claude-md-management":          "DOC-MGMT",
    "skill-creator":                 "SKILL-BUILD",
}


def set_task_override(label: str, ttl_seconds: int = 600) -> None:
    """Write task override with expiry timestamp."""
    CACHE.mkdir(parents=True, exist_ok=True)
    expires = int(time.time()) + ttl_seconds
    OVERRIDE_FILE.write_text(f"{label}|{expires}")


def _plugin_context(cwd: str, trigger_kind: str, session_id: str) -> str:
    """Project cheatsheet + relevant memory atoms, deduped per (session_id, trigger_kind)."""
    ctx        = _load_context()
    new_session = ctx.get("session_id") != session_id
    seen       = set() if new_session else set(ctx.get("plugin_ctx_seen", []))
    if new_session:
        # Claiming the session id without clearing the previous session's
        # stable_hash would make liljack_inject's dedup match on turn 1 and
        # silently drop the project cheatsheet for the whole session.
        ctx.pop("stable_hash", None)
    if trigger_kind in seen:
        return ""

    project_ctx = detect_project_context(cwd)
    mems        = read_memories()
    mems_rel    = _filter_atoms_for_project(mems, cwd) if mems else []
    mems_txt    = ("[lilJack memory priors] " + " · ".join(mems_rel)) if mems_rel else ""
    parts = [p for p in [project_ctx, mems_txt] if p]
    if not parts:
        return ""

    if session_id:
        seen.add(trigger_kind)
        ctx["session_id"] = session_id
        ctx["plugin_ctx_seen"] = sorted(seen)
        _save_context(ctx)
    return "\n".join(parts)


def main():
    raw = sys.stdin.read()
    try:
        data = json.loads(raw)
    except Exception:
        sys.exit(0)

    tool_name  = data.get("tool_name", "")
    tool_input = data.get("tool_input", {})

    # ── Skill tool → update task override ────────────────────────────────────
    if tool_name == "Skill":
        skill = tool_input.get("skill", "").lower()
        for key, label in SKILL_TASKS.items():
            if key in skill:
                set_task_override(label)
                # Inject context hint for Claude
                print(json.dumps({
                    "hookSpecificOutput": {
                        "hookEventName": "PreToolUse",
                        "permissionDecision": "allow",
                        "additionalContext": f"[lilJack] task label updated to {label}"
                    }
                }))
                sys.exit(0)

    # ── Agent tool → ORCHESTRATING ────────────────────────────────────────────
    if tool_name == "Agent":
        set_task_override("ORCHESTRATING")

    # ── clangd-lsp / context7 / github plugins → labels + contextual injection ──
    label = None
    if tool_name == "LSP":
        label = "LSP-LOOKUP"
    elif tool_name.startswith("mcp__context7__"):
        label = "DOCS-LOOKUP"
    elif tool_name == "Bash" and _is_gh_command(tool_input.get("command", "")):
        label = "PR-WORK"

    if label:
        set_task_override(label)
        cwd        = data.get("cwd", "")
        session_id = data.get("session_id", "")
        extra      = _plugin_context(cwd, label, session_id)
        ctx_text   = f"[lilJack] task label updated to {label}"
        if extra:
            ctx_text += f"\n{extra}"
        print(json.dumps({
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "additionalContext": ctx_text,
            }
        }))
        sys.exit(0)

    # Always allow
    sys.exit(0)


def _is_gh_command(command: str) -> bool:
    """True if a Bash command's effective invocation starts with `gh `."""
    stripped = command.strip()
    return stripped.startswith("gh ") or stripped == "gh"


if __name__ == "__main__":
    main()
