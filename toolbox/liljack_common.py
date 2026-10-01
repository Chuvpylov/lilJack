#!/usr/bin/env python3
"""
liljack_common.py — shared project/atom/memory logic for lilJack's Claude Code hooks.

Both liljack_inject.py (UserPromptSubmit) and liljack_pretool.py (PreToolUse) need the same
project cheatsheet + memory-atom filtering + session-context cache. Lives here once.
"""

import json
import pathlib

import os

# The project root: LILJACK_PROJECT, else the current directory.
PROJECT_ROOT       = pathlib.Path(os.environ.get("LILJACK_PROJECT") or os.getcwd())
DSL_FILE     = PROJECT_ROOT / "orc_data" / "liljack.dsl"
CACHE        = pathlib.Path.home() / ".cache" / "liljack"
CONTEXT_FILE = CACHE / "context.json"

_CORE_ATOM_KEYS = {"test", "posix", "ball"}
# Per-project atom keys and hints: path fragment -> keys / (name, hint).
# Fill these in for your own projects; they ship empty.
_PROJECT_ATOM_KEYS: dict[str, set[str]] = {}

# RULE: hints carry only STABLE operational facts — build/test commands, ports,
# paths, auth sources. Never live run state or anything with a date on it.
PROJECT_HINTS: dict[str, tuple[str, str]] = {}


def _load_context() -> dict:
    try:
        return json.loads(CONTEXT_FILE.read_text()) if CONTEXT_FILE.exists() else {}
    except Exception:
        return {}


def _save_context(ctx: dict) -> None:
    try:
        CACHE.mkdir(parents=True, exist_ok=True)
        CONTEXT_FILE.write_text(json.dumps(ctx, indent=2))
    except Exception:
        pass


def _filter_atoms_for_project(atoms: list[str], cwd: str) -> list[str]:
    """Return only atoms relevant to the active project."""
    active = set(_CORE_ATOM_KEYS)
    for fragment, keys in _PROJECT_ATOM_KEYS.items():
        if fragment in cwd:
            active |= keys
            break
    return [a for a in atoms if any(k in a for k in active)]


def read_memories() -> list[str]:
    """Extract memory atoms from liljack.dsl."""
    if not DSL_FILE.exists():
        return []
    try:
        import re
        mems = []
        for line in DSL_FILE.read_text(encoding="utf-8").splitlines():
            m = re.match(r'^memory\(liljack,\s*(\w+)\)\.', line)
            if m:
                mems.append(m.group(1))
        return mems
    except Exception:
        return []


def project_name_of(cwd: str) -> str:
    """Short project name for a cwd, '' if unknown."""
    if not cwd:
        return ""
    for fragment, (name, _hint) in PROJECT_HINTS.items():
        if fragment in cwd:
            return name
    return ""


def detect_project_context(cwd: str) -> str:
    """Return a compact project cheatsheet if cwd is inside a known project."""
    if not cwd:
        return ""
    for fragment, (name, hint) in PROJECT_HINTS.items():
        if fragment in cwd:
            return f"[lilJack project: {name}] {hint}"
    return ""
