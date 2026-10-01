#!/usr/bin/env python3
"""opencode lifecycle adapter — reuses the Codex harness machinery under a
different identity.

Two names, on purpose:
  · harness  = "opencode"  (LILJACK_HARNESS) — the CLI; drives the local
    lifecycle store (`~/.opencode/liljack/lifecycle.sqlite3`), archive paths
    (`opencode/…`), chains filenames (`opencode-*.chains.jsonl`) and the MCP
    tool names (`opencode_history` / `opencode_status`).
  · agent    = "deepseek"  (LILJACK_AGENT) — the coordination identity in the
    shared mailbox / attention board / task registry (third-opinion, untrusted).

The environment is set before importing liljack_codex because that module reads
LILJACK_HARNESS / LILJACK_AGENT at import time.
"""
import os
import sys
from pathlib import Path

os.environ.setdefault("LILJACK_HARNESS", "opencode")
os.environ.setdefault("LILJACK_AGENT", "deepseek")

sys.path.insert(0, str(Path(__file__).resolve().parent))

import liljack_codex as _codex  # noqa: E402  (reads the env above at import)


def _score_stop():
    """At turn end, write the objective session anchors (read-only sources).

    Deliberately silent and non-fatal: scoring must never block or break a
    turn. The self-rating (the voice) is a separate, conscious act via
    `liljack_scores.py self-rate`.
    """
    try:
        import liljack_scores
        scores = liljack_scores.Scores()
        try:
            return scores.compute_anchors()
        finally:
            scores.close()
    except Exception:
        return {}


def main():
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("hook", "context", "mcp", "history", "status", "sync"))
    parser.add_argument("--transcript", default="")
    args = parser.parse_args()
    if args.mode == "mcp":
        sys.argv = [sys.argv[0]]
        _codex.mcp_main()
    elif args.mode == "hook":
        try:
            data = json.load(sys.stdin)
            if isinstance(data, dict) and not data.get("transcript_path") and \
                    data.get("hook_event_name") in ("Stop", "SessionEnd"):
                # opencode has no codex-format JSONL, so export one now and let
                # record_event queue it for the shared archive (the capture gap).
                try:
                    import liljack_opencode_transcript as _transcript
                    sid = data.get("session_id")
                    if isinstance(sid, str) and sid:
                        path = _transcript.export(sid)
                        if path:
                            data["transcript_path"] = str(path)
                except Exception:
                    pass
            print(json.dumps(_codex.hook(data)))
            if isinstance(data, dict) and data.get("hook_event_name") in (
                    _codex.CONTEXT_EVENTS |
                    {"Stop", "Interrupt", "PreCompact", "SessionEnd", "SubagentStop"}):
                _codex.start_sync_worker()
            if isinstance(data, dict) and data.get("hook_event_name") == "Stop":
                _score_stop()
        except Exception as exc:
            print("liljack: hook unavailable (" + type(exc).__name__ + ")",
                  file=sys.stderr)
            print("{}")
    elif args.mode == "context":
        try:
            data = json.load(sys.stdin)
            data.setdefault("hook_event_name", "UserPromptSubmit")
            print(_codex.build_context(data))
        except Exception as exc:
            print("", file=sys.stderr)
            print("")
    elif args.mode == "status":
        print(_codex.status()["ribbon"])
    elif args.mode == "sync":
        from liljack_codex_archive import sync_pending
        print(json.dumps(sync_pending()))
    else:
        print(json.dumps(_codex.history(), indent=2))


if __name__ == "__main__":
    main()
