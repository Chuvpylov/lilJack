"""Scrub full Codex rollouts, index useful turns, and sync via Liljack's Apollo auth."""
from contextlib import closing
from datetime import datetime, timezone
import fcntl
import gzip
import hashlib
import json
from pathlib import Path
import re


def _text(content):
    if isinstance(content, str):
        return content
    return "\n".join(b.get("text", "") for b in (content or [])
                     if isinstance(b, dict) and isinstance(b.get("text"), str))


def extract_turns(records, session_id, project, harness="codex"):
    """Map observed public messages/calls to Liljack chains; leave unknowns unknown.

    event_msg user/assistant mirrors are deliberately skipped to avoid duplication.
    Tool arguments remain their native Codex representation, including code-mode JS.
    Opaque reasoning is retained only in the source archive, never interpreted here.
    """
    chains, current, pending = [], None, {}
    turn_id = None
    for rec in records:
        p = rec.get("payload", {})
        if not isinstance(p, dict):
            continue
        if rec.get("type") == "turn_context":
            turn_id = p.get("turn_id")
            if current:
                current["turn_id"] = turn_id
            continue
        if rec.get("type") == "event_msg" and p.get("type") == "task_started":
            turn_id = p.get("turn_id")
            continue
        if rec.get("type") == "event_msg" and p.get("type") in ("task_complete", "turn_aborted"):
            if current:
                current["lifecycle"] = p["type"]
            continue
        if rec.get("type") != "response_item":
            continue
        kind = p.get("type")
        if kind == "message" and p.get("role") == "user":
            human = _text(p.get("content"))
            if human.lstrip().startswith(("<environment_context>", "<permissions instructions>", "# AGENTS.md")):
                continue
            current = {"harness": harness, "source": "human", "project": project,
                       "session_id": session_id, "turn_id": turn_id,
                       "ts": rec.get("timestamp", ""), "human": human,
                       "steps": [], "final": "", "lifecycle": "unknown",
                       "outcome": {"tests": "unknown", "files_edited": [], "turn_count": 1}}
            chains.append(current)
            pending = {}
        elif current and kind == "message" and p.get("role") == "assistant":
            if p.get("phase") == "final_answer" or p.get("channel") == "final":
                current["final"] = _text(p.get("content"))
        elif current and kind in ("function_call", "custom_tool_call"):
            inp = p.get("arguments", p.get("input", ""))
            step = {"tool": p.get("name", "unknown"), "tool_use_id": p.get("call_id"),
                    "input": inp, "input_digest": str(inp)[:300],
                    "result_digest": "", "result": "", "is_error": None}
            current["steps"].append(step)
            pending[p.get("call_id")] = step
        elif kind in ("function_call_output", "custom_tool_call_output"):
            step = pending.pop(p.get("call_id"), None)
            if step is not None:
                result = p.get("output", "")
                result = result if isinstance(result, str) else json.dumps(result)
                step.update(result=result[:2000], result_digest=result[:300], result_len=len(result))
    return chains


def load_index(root=None, harness="codex"):
    from liljack_codex import state_root
    root = Path(root or state_root()) / "archives"
    result = []
    for path in root.glob("*/*.meta.json"):
        try:
            data = json.loads(path.read_text())
            if data.get("harness") == harness:
                result.append(data)
        except (OSError, ValueError):
            continue
    return result


def sync_pending(root=None, push=True):
    """Acknowledge only after all blobs succeed; failed/offline work stays queued."""
    from liljack_codex import state_root, _connect, rollout_metadata, HARNESS
    from session_archive import _live_secrets, _copy_scrubbed, _apollo_url, _apollo_push_file
    from liljack_common import project_name_of
    import liljack_chains
    import liljack_projects

    root = Path(root or state_root())
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    report = {"archived": 0, "synced": 0, "pending": 0}
    with (root / "sync.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return {"busy": True}
        with closing(_connect(root, write=True)) as db:
            queue = db.execute("SELECT session_id,cwd,transcript,revision FROM archive_queue").fetchall()
            for sid, cwd, transcript, revision in queue:
                try:
                    metadata = rollout_metadata(transcript, sid)
                    if metadata.get("availability") != "sampled":
                        continue
                    _, gate = _live_secrets()
                    if gate.startswith("unavailable"):
                        continue
                    project = project_name_of(cwd) or Path(cwd).name
                    project = re.sub(r"[^A-Za-z0-9_.-]", "_", project).strip(".") or "unknown"
                    safe_sid = hashlib.sha256(sid.encode()).hexdigest()[:32]
                    folder = root / "archives" / project
                    folder.mkdir(parents=True, exist_ok=True)
                    target = folder / f"{safe_sid}.jsonl.gz"
                    temporary = target.with_suffix(".tmp")
                    hits, gate = _copy_scrubbed(Path(transcript), temporary)
                    temporary.replace(target)
                    target.chmod(0o600)
                    records = []
                    with gzip.open(target, "rt") as stream:
                        for line in stream:
                            try:
                                rec = json.loads(line)
                                if isinstance(rec, dict):
                                    records.append(rec)
                            except ValueError:
                                continue
                    chains = extract_turns(records, sid, project, harness=HARNESS)
                    # Harness in filename avoids collisions with Claude session IDs.
                    chain_dir = liljack_chains.CHAINS_DIR / project
                    chain_file = chain_dir / f"{HARNESS}-{safe_sid}.chains.jsonl"
                    # This dir is rglobbed by corpus ingest; a session with no turns
                    # must leave no file behind (a bare newline is litter, not a chain).
                    chain_blob = b""
                    if chains:
                        chain_dir.mkdir(parents=True, exist_ok=True)
                        chain_blob = ("\n".join(json.dumps(c, ensure_ascii=False) for c in chains) + "\n").encode()
                        chain_file.write_bytes(chain_blob)
                    elif chain_file.exists():
                        chain_file.unlink()
                    repo_path = f"{HARNESS}/{project}/{safe_sid}.jsonl.gz"
                    meta = {"harness": HARNESS, "session_id": sid, "project_slug": project,
                            "project_path": cwd, "repo_path": repo_path,
                            "message_count": sum(r.get("type") == "response_item" and
                                                 r.get("payload", {}).get("type") == "message" for r in records),
                            "archived_at": datetime.now(timezone.utc).isoformat(),
                            "redactions": hits, "credential_gate": gate, "turn_count": len(chains),
                            "metadata": metadata}
                    meta_blob = json.dumps(meta, ensure_ascii=False).encode()
                    (folder / f"{safe_sid}.meta.json").write_bytes(meta_blob)
                    report["archived"] += 1
                    # Publish shared project context/TODO/chains; this function merges Codex index.
                    project_result = liljack_projects.sync_project(project, cwd, push=push)
                    url = _apollo_url() if push else ""
                    blobs = [("liljack-sessions", repo_path, target.read_bytes()),
                             ("liljack-sessions", f"{HARNESS}/{project}/{safe_sid}.meta.json", meta_blob)]
                    if chain_blob:
                        blobs.append(("mcp-projects", f"{project}/{HARNESS}/{safe_sid}.chains.jsonl", chain_blob))
                    success = bool(url)
                    if url:
                        for repo, path, blob in blobs:
                            if not _apollo_push_file(url, repo, path, blob):
                                success = False
                        # Project sync retries internally by digest; ensure current project index is published.
                        for name in ("context.md", "sessions.jsonl"):
                            local = liljack_projects.MIRROR / project / name
                            if local.exists() and not _apollo_push_file(url, "mcp-projects", f"{project}/{name}", local.read_bytes()):
                                success = False
                    if success:
                        with db:
                            db.execute("DELETE FROM archive_queue WHERE session_id=? AND revision=?", (sid, revision))
                        report["synced"] += 1
                except Exception as exc:
                    report["last_error"] = type(exc).__name__
            report["pending"] = db.execute("SELECT count(*) FROM archive_queue").fetchone()[0]
    (root / "sync_status.json").write_text(json.dumps(report))
    return report
