#!/bin/bash
# lilJack status line, public build. Prints ONE line of fields separated by
# U+2502 (│). Every field is classified by its leading glyph in
# liljack_app/status_ribbon.py, so keep the prefixes: lilJack, ⎇, 📦, ☑, ctx[.
# All checks are local and non-blocking; the internal build adds ring, node
# and context fields from its own services.
LILJACK_CACHE="${LILJACK_CACHE:-$HOME/.cache/liljack}"
ARCHIVE="${LILJACK_ARCHIVE:-$HOME/.claude/session_archive}"
PROJECT="${LILJACK_PROJECT:-$(pwd)}"
SEP=" │ "
out="lilJack"

# git: branch and dirty count
if git -C "$PROJECT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    branch=$(git -C "$PROJECT" symbolic-ref --short HEAD 2>/dev/null || git -C "$PROJECT" rev-parse --short HEAD 2>/dev/null)
    dirty=$(git -C "$PROJECT" status --porcelain 2>/dev/null | wc -l | tr -d ' ')
    out="$out$SEP⎇ $branch"; [ "$dirty" != 0 ] && out="$out($dirty✎)"
fi

# archive: archived session count and newest age
if [ -f "$ARCHIVE/index.jsonl" ]; then
    n=$(wc -l < "$ARCHIVE/index.jsonl" | tr -d ' ')
    age=$(( $(date +%s) - $(stat -c %Y "$ARCHIVE/index.jsonl" 2>/dev/null || echo 0) ))
    out="$out$SEP📦$n $((age/3600))h$(( (age%3600)/60 ))m"
fi

# todo: open/total in the local task registry when it is a plain BMD file
reg="${LILJACK_REGISTRY:-$LILJACK_CACHE/board/agent_tasks.bmd}"
if [ -f "$reg" ]; then
    total=$(grep -c 'fact(task,' "$reg" 2>/dev/null || echo 0)
    done_n=$(grep -c 'state, *done' "$reg" 2>/dev/null || echo 0)
    out="$out$SEP☑$done_n/$total"
fi

# context: last cached session context size, if the hooks wrote one
if [ -f "$LILJACK_CACHE/context.json" ]; then
    kb=$(( $(stat -c %s "$LILJACK_CACHE/context.json" 2>/dev/null || echo 0) / 1024 ))
    out="$out${SEP}ctx[KB:$kb]"
fi
printf '%s\n' "$out"
