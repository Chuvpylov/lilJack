#!/usr/bin/env python3
"""
toolbox/session_archive.py — Claude Code session archiver (liljack)

Archives every Claude Code session from ~/.claude/projects/ to:
  1. LOCAL cache  — ~/.claude/session_archive/  (always, immediate)
  2. APOLLO store — apollo://liljack-sessions repo  (when Apollo is reachable)

Apollo is the permanent, cross-machine store. Local cache is the fast offline
fallback. When Apollo comes online after being offline, --sync pushes everything
in the local cache that hasn't been synced yet.

Archive layout (local + Apollo repo mirror):
  <year>/<month>/<project-slug>/<session-id>.jsonl.gz         — compressed session
  <year>/<month>/<project-slug>/<session-id>.subagents.tar.gz — subagents (compaction summaries, etc.)
  index.jsonl                                                  — lightweight metadata index

Compaction summaries are stored by Claude Code in:
  ~/.claude/projects/<project>/<session-id>/subagents/agent-acompact-*.jsonl
The last assistant message in each compaction file contains the full summary text.
liljack archives these alongside the session so no context is ever lost across compactions.

Usage:
  python3 toolbox/session_archive.py              # archive new sessions + sync to Apollo
  python3 toolbox/session_archive.py --stats      # show archive stats
  python3 toolbox/session_archive.py --list       # list archived sessions
  python3 toolbox/session_archive.py --mine       # extract NLP facts from sessions
  python3 toolbox/session_archive.py --sync       # push local cache to Apollo (retry)
  python3 toolbox/session_archive.py --no-apollo  # skip Apollo sync (offline mode)

Called automatically via Claude Code Stop hook after each assistant turn.
Remote store config read from: <project>/.apollo/config (LILJACK_PROJECT)
"""

import os
import io
import sys
import json
import gzip
import shutil
import hashlib
import subprocess
import tarfile
import fcntl
import urllib.request
import urllib.error
from pathlib import Path
from datetime import datetime, timezone

CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"
ARCHIVE_ROOT    = Path.home() / ".claude" / "session_archive"
INDEX_FILE      = ARCHIVE_ROOT / "index.jsonl"
LOCK_FILE       = ARCHIVE_ROOT / ".liljack.lock"   # exclusive lock for multi-instance safety
CONV_DIR        = ARCHIVE_ROOT / "conv"         # compact user/claude dialog logs

# Apollo integration
ORACLE_ROOT     = Path(os.environ.get("LILJACK_PROJECT") or Path(__file__).parent.parent)
APOLLO_ROOT     = Path(os.environ.get("LILJACK_APOLLO_ROOT") or ORACLE_ROOT)
SECRETS_SCRIPTS = os.environ.get("LILJACK_SECRETS_SCRIPTS", "")   # dir holding scrub_archives.py (optional)
JACK_REPO       = "liljack-sessions"          # Apollo repo name for session archive
APOLLO_SYNC_LOG = ARCHIVE_ROOT / "apollo_sync.jsonl"  # tracks what's been pushed

# ── Secret gate ───────────────────────────────────────────────────────────────
# Transcripts are pushed to the remote store verbatim, so this archiver is the
# last point at which a live credential can be stopped. Known literals only,
# never entropy: entropy gating a dialogue corpus shreds legitimate hashes and
# base64 while adding nothing a literal list does not already catch.
_SECRETS_CACHE = None


def _live_secrets() -> tuple[list, str]:
    """(known-live secret literals, gate status). Cached per process."""
    global _SECRETS_CACHE
    if _SECRETS_CACHE is None:
        try:
            if not SECRETS_SCRIPTS:
                # No live-secret source configured: nothing known to redact.
                _SECRETS_CACHE = ([], "empty")
                return _SECRETS_CACHE
            sys.path.insert(0, SECRETS_SCRIPTS)
            from scrub_archives import collect_live_secrets
            vals = [s for s in collect_live_secrets() if len(s) >= 16]
            _SECRETS_CACHE = (vals, "ok" if vals else "empty")
        except Exception as e:                       # never block archiving
            _SECRETS_CACHE = ([], f"unavailable:{type(e).__name__}")
    return _SECRETS_CACHE


def _copy_scrubbed(src_path: Path, dest_gz: Path) -> tuple[int, str]:
    """Compress src → dest_gz, redacting known-live credentials. (hits, status).

    Line-oriented because the source is JSONL: a secret cannot straddle records,
    and REDACT contains no quote or backslash, so each line stays valid JSON.
    """
    secrets, status = _live_secrets()
    if not secrets:
        with open(src_path, "rb") as src, gzip.open(dest_gz, "wb") as gz:
            shutil.copyfileobj(src, gz)
        return 0, status
    hits = 0
    with open(src_path, "rb") as src, gzip.open(dest_gz, "wb") as gz:
        for raw in src:
            line = raw.decode("utf-8", errors="replace")
            for s in secrets:
                if s in line:
                    hits += line.count(s)
                    line = line.replace(s, "[REDACTED]")
            gz.write(line.encode("utf-8"))
    return hits, status


def _scrub_bytes(data: bytes, secrets: list) -> tuple[bytes, int]:
    text, hits = data.decode("utf-8", errors="replace"), 0
    for s in secrets:
        if s in text:
            hits += text.count(s)
            text = text.replace(s, "[REDACTED]")
    return text.encode("utf-8"), hits


def _tar_scrubbed(src_dir: Path, dest_tar: Path, arcname: str) -> int:
    """Tar src_dir with the same credential gate as the transcript.

    Compaction summaries live here and are a summary OF the conversation, so
    they carry the same exposure as the transcript itself.
    """
    secrets, _status = _live_secrets()
    if not secrets:
        with tarfile.open(dest_tar, "w:gz") as tar:
            tar.add(src_dir, arcname=arcname)
        return 0
    hits = 0
    with tarfile.open(dest_tar, "w:gz") as tar:
        for path in sorted(src_dir.rglob("*")):
            rel = Path(arcname) / path.relative_to(src_dir)
            if path.is_dir():
                tar.addfile(tar.gettarinfo(str(path), arcname=str(rel)))
                continue
            if not path.is_file():
                continue
            clean, n = _scrub_bytes(path.read_bytes(), secrets)
            hits += n
            info = tar.gettarinfo(str(path), arcname=str(rel))
            info.size = len(clean)
            tar.addfile(info, io.BytesIO(clean))
    return hits


def _read_apollo_config() -> dict:
    """Read the remote store URL + ring token from <project>/.apollo/config (fallback: LILJACK_APOLLO_ROOT)."""
    for config_path in [ORACLE_ROOT / ".apollo" / "config",
                        APOLLO_ROOT  / ".apollo" / "config"]:
        if not config_path.exists():
            continue
        cfg: dict = {}
        section = ""
        for line in config_path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
            elif "=" in line:
                k, _, v = line.partition("=")
                cfg[f"{section}.{k.strip()}"] = v.strip()
        return cfg
    return {}


def _apollo_url() -> str:
    cfg = _read_apollo_config()
    return cfg.get("remote.url", "").rstrip("/")


def _apollo_online(url: str, timeout: float = 1.5) -> bool:
    """Quick health check — returns True if Apollo responds."""
    if not url:
        return False
    try:
        req = urllib.request.Request(f"{url}/api/status", method="GET")
        with urllib.request.urlopen(req, timeout=timeout):
            return True
    except Exception:
        return False


def _apollo_ring_token() -> str:
    """Return the cerber ring token from .apollo/config, env, or empty string."""
    # Check env first
    tok = os.environ.get("APOLLO_RING", "").strip()
    if tok:
        return tok
    cfg = _read_apollo_config()
    return cfg.get("remote.token", "").strip()


def _apollo_push_file(url: str, repo: str, path_in_repo: str,
                       data: bytes, ring: int = 1) -> bool:
    """
    Push a single blob to Apollo repo via REST API.
    PUT /api/repos/{repo}/files/{path}  with raw bytes body.
    Returns True on success.
    """
    try:
        api = f"{url}/api/repos/{repo}/files/{path_in_repo.lstrip('/')}"
        headers: dict = {"Content-Type": "application/octet-stream"}
        tok = _apollo_ring_token()
        if tok:
            headers[os.environ.get("LILJACK_ARCHIVE_AUTH_HEADER", "X-Ring-Token")] = tok
        else:
            headers["X-Ring"] = str(ring)
        req = urllib.request.Request(api, data=data, method="PUT", headers=headers)
        with urllib.request.urlopen(req, timeout=10):
            return True
    except Exception:
        return False


def _apollo_push_index(url: str, ring: int = 1) -> bool:
    """Push the full index.jsonl to Apollo."""
    if not INDEX_FILE.exists():
        return True
    return _apollo_push_file(url, JACK_REPO, "index.jsonl",
                              INDEX_FILE.read_bytes(), ring)


def _load_sync_log() -> dict:
    """session_id → its LAST sync record ({path, ts, msgs, size}).

    Was a set of ids, and a session was pushed exactly once — the first time
    the 5-min sync saw it. A live session keeps growing for hours after that
    and the local archive is refreshed every pass, so Apollo held the FIRST
    snapshot of every session, never the last: measured 2026-09-02, a session
    at 272 KB locally was 26 KB on Apollo. The record now carries the
    message count and size that were pushed; sync_to_apollo re-pushes when
    the index says the archive has grown. Records without those fields
    (pre-2026-09-02) compare unequal and are re-pushed once.
    """
    if not APOLLO_SYNC_LOG.exists():
        return {}
    synced: dict = {}
    for ln in APOLLO_SYNC_LOG.read_text().splitlines():
        try:
            rec = json.loads(ln)
            synced[rec["session_id"]] = rec
        except Exception:
            pass
    return synced


def _synced_ids() -> set:
    return set(_load_sync_log())


def _mark_synced(session_id: str, path_in_repo: str, msgs=None, size=None):
    ARCHIVE_ROOT.mkdir(parents=True, exist_ok=True)
    with open(APOLLO_SYNC_LOG, "a") as f:
        f.write(json.dumps({"session_id": session_id,
                             "path": path_in_repo,
                             "ts": _ts(), "msgs": msgs, "size": size}) + "\n")


def _needs_push(meta: dict, rec) -> bool:
    """True when the index says the archive differs from what was pushed."""
    if not rec:
        return True
    return (rec.get("msgs") != meta.get("message_count")
            or rec.get("size") != meta.get("size_bytes"))


def _spawn_detached_sync() -> None:
    """Fire-and-forget Apollo sync so the Stop hook never blocks on network I/O."""
    try:
        subprocess.Popen(
            [sys.executable, str(Path(__file__)), "--sync"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL, start_new_session=True,
        )
    except Exception:
        pass


def sync_to_apollo(verbose: bool = False) -> tuple[int, int]:
    """
    Push all locally-archived sessions not yet in Apollo sync log.
    Returns (pushed, skipped).
    """
    url = _apollo_url()
    if not url:
        return 0, 0
    if not _apollo_online(url):
        if verbose:
            print(f"[liljack] Apollo offline ({url}) — skipping sync")
        return 0, 0

    cfg  = _read_apollo_config()
    ring = int(cfg.get("remote.ring", 1))

    synced  = _load_sync_log()
    index   = _load_index()
    pushed  = skipped = 0

    for meta in index.values():
        sid = meta["session_id"]
        if not _needs_push(meta, synced.get(sid)):
            skipped += 1
            continue
        arc = ARCHIVE_ROOT / meta.get("archive_path", "")
        if not arc.exists():
            continue
        path_in_repo = meta["archive_path"]
        ok = _apollo_push_file(url, JACK_REPO, path_in_repo, arc.read_bytes(), ring)
        if ok:
            # Also push subagents archive if present
            sub_rel = meta.get("subagents_archive_path", "")
            if sub_rel:
                sub_arc = ARCHIVE_ROOT / sub_rel
                if sub_arc.exists():
                    _apollo_push_file(url, JACK_REPO, sub_rel, sub_arc.read_bytes(), ring)
            _mark_synced(sid, path_in_repo, meta.get("message_count"), meta.get("size_bytes"))
            pushed += 1
            if verbose:
                compact_n = meta.get("compaction_count", 0)
                compact_tag = f" [{compact_n}✂]" if compact_n else ""
                print(f"  → apollo  {meta['project_slug']}/{sid[:8]}{compact_tag}")
        # else: Apollo online but push failed — will retry next time

    # Always push latest index
    _apollo_push_index(url, ring)

    # Per-project mcp-projects mirror push (context/TODO/chains/session index)
    try:
        from liljack_projects import sync_current
        sync_current(push=True)
    except Exception:
        pass

    # Room export rides the same 5-minute sync (D2): scrubbed rooms.jsonl + meta
    # into mcp-projects/<slug>/rooms/. Explicit the project project — this worker runs
    # from $HOME, so os.getcwd() would export the wrong tree.
    try:
        from liljack_room_export import export as export_rooms
        export_rooms(project=str(Path(__file__).resolve().parent.parent), push=True)
    except Exception:
        pass

    if verbose and pushed:
        print(f"[liljack] synced {pushed} session(s) to apollo://{JACK_REPO}")
    return pushed, skipped


class _ArchiveLock:
    """
    Non-blocking exclusive file lock for the archive index.
    Multiple Claude Code instances (IDE, terminal) may run Stop hooks concurrently.
    If another instance holds the lock, this one skips gracefully — it will
    catch up on the next turn.
    """
    def __init__(self):
        ARCHIVE_ROOT.mkdir(parents=True, exist_ok=True)
        self._fh = None

    def __enter__(self):
        self._fh = open(LOCK_FILE, "w")
        try:
            fcntl.flock(self._fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self._fh.close()
            self._fh = None
            raise
        return self

    def __exit__(self, *_):
        if self._fh:
            fcntl.flock(self._fh, fcntl.LOCK_UN)
            self._fh.close()


def _ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _project_slug(project_path: str) -> str:
    """Convert project dir hash to human-readable slug from path."""
    # ~/.claude/projects/-home-user-src-myproject → myproject
    parts = project_path.replace("-", "/").strip("/").split("/")
    return parts[-1] if parts else project_path[:16]


def _extract_compaction_summary(subagents_dir: Path) -> tuple[int, str]:
    """
    Scan subagents dir for agent-acompact-*.jsonl files.
    Returns (count, latest_summary_text).
    The last assistant text message in each compact file is the summary.
    """
    compact_files = sorted(subagents_dir.glob("agent-acompact-*.jsonl")) if subagents_dir.exists() else []
    if not compact_files:
        return 0, ""

    latest_summary = ""
    # Read the most recently modified compact file for the summary text
    most_recent = max(compact_files, key=lambda p: p.stat().st_mtime)
    try:
        lines = most_recent.read_text(encoding="utf-8", errors="replace").splitlines()
        for ln in reversed(lines):
            try:
                obj = json.loads(ln)
                msg = obj.get("message", {})
                if msg.get("role") == "assistant":
                    for block in (msg.get("content") or []):
                        if isinstance(block, dict) and block.get("type") == "text":
                            text = block.get("text", "").strip()
                            if len(text) > 100:
                                latest_summary = text
                                break
                if latest_summary:
                    break
            except json.JSONDecodeError:
                pass
    except OSError:
        pass

    return len(compact_files), latest_summary


def _session_meta(jsonl_path: Path) -> dict:
    """Extract lightweight metadata from a session JSONL file."""
    lines = []
    try:
        with open(jsonl_path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return {}

    session_id = jsonl_path.stem
    message_count = 0
    first_ts = ""
    project_path = jsonl_path.parent.name

    for ln in lines:
        try:
            obj = json.loads(ln)
        except json.JSONDecodeError:
            continue
        t = obj.get("type", "")
        if t in ("user", "assistant") or obj.get("message", {}).get("role"):
            message_count += 1
        if not first_ts:
            ts = obj.get("timestamp") or obj.get("created_at") or ""
            if ts:
                first_ts = ts

    # Check for compaction summaries in session subdir
    session_dir = jsonl_path.parent / session_id
    compaction_count, latest_summary = _extract_compaction_summary(
        session_dir / "subagents" if session_dir.exists() else session_dir
    )

    meta: dict = {
        "session_id":    session_id,
        "project_path":  project_path,
        "project_slug":  _project_slug(project_path),
        "message_count": message_count,
        "size_bytes":    jsonl_path.stat().st_size,
        "first_ts":      first_ts,
        "archived_at":   _ts(),
    }
    if compaction_count:
        meta["compaction_count"] = compaction_count
        # Store first 500 chars of summary for quick inspection
        if latest_summary:
            meta["latest_summary_head"] = latest_summary[:500]
    return meta


def _load_index() -> dict[str, dict]:
    """Load index as {session_id: meta}."""
    if not INDEX_FILE.exists():
        return {}
    idx = {}
    for ln in INDEX_FILE.read_text(encoding="utf-8").splitlines():
        try:
            rec = json.loads(ln)
            idx[rec["session_id"]] = rec
        except (json.JSONDecodeError, KeyError):
            pass
    return idx


def _append_index(meta: dict):
    ARCHIVE_ROOT.mkdir(parents=True, exist_ok=True)
    with open(INDEX_FILE, "a", encoding="utf-8") as f:
        f.write(json.dumps(meta, ensure_ascii=False) + "\n")


def archive_all(verbose: bool = False) -> int:
    """Archive all session JSONL files not yet in the index. Returns count archived."""
    if not CLAUDE_PROJECTS.exists():
        return 0

    try:
        lock = _ArchiveLock()
        lock.__enter__()
    except OSError:
        # Another instance is archiving — skip silently, will catch up next turn
        return 0

    try:
        return _archive_all_locked(verbose=verbose)
    finally:
        lock.__exit__()


def _archive_all_locked(verbose: bool = False) -> int:
    """Inner archive logic — caller must hold _ArchiveLock."""
    index = _load_index()
    archived = 0

    for project_dir in sorted(CLAUDE_PROJECTS.iterdir()):
        if not project_dir.is_dir():
            continue

        for jsonl_file in sorted(project_dir.glob("*.jsonl")):
            session_id = jsonl_file.stem

            # Skip if already archived and file hasn't grown
            if session_id in index:
                prev_size = index[session_id].get("size_bytes", 0)
                curr_size = jsonl_file.stat().st_size
                if curr_size <= prev_size:
                    continue  # no new data

            meta = _session_meta(jsonl_file)
            if not meta:
                continue

            # Determine archive path: <year>/<month>/<project-slug>/<session-id>.jsonl.gz
            now = datetime.now(timezone.utc)
            dest_dir = (ARCHIVE_ROOT / str(now.year) / f"{now.month:02d}"
                        / meta["project_slug"])
            dest_dir.mkdir(parents=True, exist_ok=True)
            dest = dest_dir / f"{session_id}.jsonl.gz"

            # Compress and write main session file, redacting live credentials
            redacted, gate = _copy_scrubbed(jsonl_file, dest)
            meta["secret_gate"] = gate
            if redacted:
                meta["secrets_redacted"] = redacted

            # Archive subagents dir (contains compaction summaries + subagent traces)
            session_dir = project_dir / session_id
            if session_dir.is_dir():
                subagents_archive = dest_dir / f"{session_id}.subagents.tar.gz"
                redacted += _tar_scrubbed(session_dir, subagents_archive, session_id)
                if redacted:
                    meta["secrets_redacted"] = redacted
                meta["subagents_archive_path"] = str(subagents_archive.relative_to(ARCHIVE_ROOT))
                meta["compaction_count"] = meta.get("compaction_count", 0)

            # Update index (overwrite if re-archiving)
            meta["archive_path"] = str(dest.relative_to(ARCHIVE_ROOT))
            if session_id in index:
                # Re-archive: update entry in place by rewriting (simple approach)
                lines = INDEX_FILE.read_text().splitlines()
                updated = []
                for ln in lines:
                    try:
                        r = json.loads(ln)
                        if r.get("session_id") == session_id:
                            updated.append(json.dumps(meta, ensure_ascii=False))
                        else:
                            updated.append(ln)
                    except json.JSONDecodeError:
                        updated.append(ln)
                INDEX_FILE.write_text("\n".join(updated) + "\n", encoding="utf-8")
            else:
                _append_index(meta)
                index[session_id] = meta

            if verbose:
                print(f"  archived  {meta['project_slug']}/{session_id[:8]}  "
                      f"msgs={meta['message_count']}  "
                      f"{meta['size_bytes']//1024}KB → {dest}")
            archived += 1

    return archived


def archive_and_sync(verbose: bool = False, no_apollo: bool = False) -> tuple[int, int, int]:
    """Archive new sessions + history locally, then sync to Apollo. Returns (archived, pushed, skipped)."""
    archived = archive_all(verbose=verbose)
    archive_history(verbose=verbose)   # always archive history too — fast, non-blocking
    pushed = skipped = 0
    if not no_apollo:
        pushed, skipped = sync_to_apollo(verbose=verbose)

    # Auto-update liljack personality (fast, no network)
    try:
        import sys as _sys
        _sys.path.insert(0, str(Path(__file__).parent))
        from liljack import update_quick, auto_mine_if_due
        update_quick()
        auto_mine_if_due(verbose=verbose)
    except Exception:
        pass  # never break archival if liljack update fails

    return archived, pushed, skipped


HISTORY_FILE = Path.home() / ".claude" / "history.jsonl"


def archive_history(verbose: bool = False) -> int:
    """
    Archive ~/.claude/history.jsonl — Claude Code prompt-history entries.
    Each entry: {display, pastedContents, timestamp, project, sessionId}
    Stored month-by-month as history/<year>/<month>.jsonl.gz.
    Returns number of new entries written.
    """
    if not HISTORY_FILE.exists():
        return 0

    try:
        lines = HISTORY_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return 0

    # Group by year/month
    by_month: dict[str, list[str]] = {}
    for ln in lines:
        try:
            obj = json.loads(ln)
            ts_ms = obj.get("timestamp", 0)
            dt = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc)
            key = f"{dt.year}/{dt.month:02d}"
            by_month.setdefault(key, []).append(ln)
        except Exception:
            pass

    written = 0
    for month_key, month_lines in sorted(by_month.items()):
        dest_dir = ARCHIVE_ROOT / "history" / month_key.split("/")[0]
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / f"{month_key.split('/')[1]}.jsonl.gz"

        # Check if we need to update (naive: re-write if count changed)
        existing_count = 0
        if dest.exists():
            try:
                with gzip.open(dest, "rt", encoding="utf-8") as gz:
                    existing_count = sum(1 for _ in gz)
            except Exception:
                pass
        if len(month_lines) <= existing_count:
            continue

        with gzip.open(dest, "wt", encoding="utf-8") as gz:
            gz.write("\n".join(month_lines) + "\n")
        written += len(month_lines) - existing_count
        if verbose:
            year, month = month_key.split("/")
            print(f"  history   {year}/{month}  {len(month_lines)} entries → {dest}")

    return written


def pc_scan(verbose: bool = False) -> dict:
    """
    Scan this machine for AI-assistant and developer metadata.
    Produces structured JSONL at ~/.claude/session_archive/pc_scan.jsonl.
    Non-destructive, read-only. Returns summary dict.
    """
    ARCHIVE_ROOT.mkdir(parents=True, exist_ok=True)
    out_path = ARCHIVE_ROOT / "pc_scan.jsonl"
    scan_ts  = _ts()
    records  = []

    def _record(source: str, data: dict):
        records.append(json.dumps({"ts": scan_ts, "source": source, **data},
                                   ensure_ascii=False))

    # ── Claude Code history stats ─────────────────────────────────────────
    if HISTORY_FILE.exists():
        lines = HISTORY_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
        projects: dict = {}
        for ln in lines:
            try:
                obj = json.loads(ln)
                p = obj.get("project", "?")
                projects[p] = projects.get(p, 0) + 1
            except Exception:
                pass
        _record("claude_history", {"total_entries": len(lines), "projects": projects})

    # ── Shell history vocabulary ──────────────────────────────────────────
    for hist_file in [Path.home() / ".bash_history", Path.home() / ".zsh_history"]:
        if not hist_file.exists():
            continue
        try:
            cmds = hist_file.read_text(encoding="utf-8", errors="replace").splitlines()
            # Count top command prefixes (first word only)
            from collections import Counter
            tops = Counter(c.split()[0] for c in cmds if c and not c.startswith(":")).most_common(20)
            _record("shell_history", {"file": str(hist_file), "total": len(cmds),
                                       "top_commands": dict(tops)})
        except Exception:
            pass

    # ── Git repos on this machine ─────────────────────────────────────────
    import subprocess as _sp
    scan_roots = [Path(os.environ.get("LILJACK_SCAN_ROOT", str(Path.home() / "Projects")))]
    for brain_root in scan_roots:
        if not brain_root.exists():
            continue
        for repo_dir in sorted(brain_root.iterdir()):
            git_dir = repo_dir / ".git"
            if not git_dir.exists():
                continue
            try:
                result = _sp.run(
                    ["git", "-C", str(repo_dir), "log", "--all", "--oneline", "-30"],
                    capture_output=True, text=True, timeout=5
                )
                commits = [ln.strip() for ln in result.stdout.splitlines() if ln.strip()]
                # Also grab branch list
                br = _sp.run(
                    ["git", "-C", str(repo_dir), "branch", "--all", "--format=%(refname:short)"],
                    capture_output=True, text=True, timeout=3
                )
                branches = [b.strip() for b in br.stdout.splitlines() if b.strip()]
                _record("git_repo", {"repo": repo_dir.name, "root": str(brain_root),
                                     "recent_commits": commits, "branches": branches})
            except Exception:
                pass

    # ── VS Code extensions ────────────────────────────────────────────────
    vscode_ext = Path.home() / ".vscode" / "extensions"
    if vscode_ext.exists():
        exts = [d.name for d in vscode_ext.iterdir() if d.is_dir() and not d.name.startswith(".")]
        _record("vscode_extensions", {"count": len(exts), "extensions": sorted(exts)})

    # ── Recently opened files/workspaces (XDG recent-files) ──────────────
    xbel = Path.home() / ".local" / "share" / "recently-used.xbel"
    if xbel.exists():
        try:
            import xml.etree.ElementTree as ET
            tree = ET.parse(str(xbel))
            root = tree.getroot()
            items = []
            # namespace-agnostic: match both plain and namespaced bookmark tags
            ns = "{http://www.freedesktop.org/standards/desktop-bookmarks}"
            bms = list(root.iter(f"{ns}bookmark")) or list(root.iter("bookmark"))
            for bm in bms:
                href = bm.get("href", "")
                modified = bm.get("modified", "")
                if href.startswith("file://"):
                    items.append({"modified": modified, "path": href[7:]})
            items.sort(key=lambda x: x["modified"], reverse=True)
            _record("recent_files", {"count": len(items), "recent": items[:30]})
        except Exception:
            pass

    # ── Cursor AI / Continue.dev / Copilot logs ───────────────────────────
    for ai_tool, log_globs in [
        ("cursor",   [Path.home() / ".cursor" / "logs"]),
        ("continue", [Path.home() / ".continue"]),
        ("copilot",  [Path.home() / ".config" / "github-copilot"]),
    ]:
        for base in log_globs:
            if base.exists():
                try:
                    files = list(base.rglob("*.log"))[:5] + list(base.rglob("*.jsonl"))[:5]
                    _record(f"{ai_tool}_logs", {
                        "base": str(base),
                        "log_files": [str(f.relative_to(base)) for f in files],
                        "count": len(files),
                    })
                except Exception:
                    pass

    # Write output
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(records) + "\n")

    if verbose:
        print(f"[liljack] pc_scan → {len(records)} sources → {out_path}")

    # Push to Apollo oracle-kb/pc_scan/ if reachable
    url = _apollo_url()
    if url and _apollo_online(url):
        tok = _apollo_ring_token()
        ok = _apollo_push_file(url, "oracle-kb",
                               f"pc_scan/{scan_ts[:10]}.jsonl",
                               out_path.read_bytes())
        if verbose:
            print(f"[liljack] pc_scan → apollo {'ok' if ok else 'push failed'}")

    return {"sources": len(records), "output": str(out_path)}


def stats() -> dict:
    index = _load_index()
    if not index:
        return {}
    total_msgs  = sum(v.get("message_count", 0) for v in index.values())
    total_bytes = sum(v.get("size_bytes", 0) for v in index.values())
    by_project: dict = {}
    for v in index.values():
        p = v.get("project_slug", "?")
        by_project.setdefault(p, 0)
        by_project[p] += 1
    return {
        "total_sessions": len(index),
        "total_messages": total_msgs,
        "total_bytes":    total_bytes,
        "by_project":     by_project,
    }


def mine_sessions(output_tsv: str = "/tmp/session_mine.tsv",
                  project_filter: str = ""):
    """
    Extract NLP-extractable text from archived sessions (assistant messages)
    and write as plain text for den_scan / nlp_extract to process.
    """
    index = _load_index()
    out_path = Path(output_tsv)
    count = 0
    with open(out_path, "w", encoding="utf-8") as out:
        for meta in index.values():
            if project_filter and project_filter not in meta.get("project_slug", ""):
                continue
            arc = ARCHIVE_ROOT / meta.get("archive_path", "")
            if not arc.exists():
                continue
            try:
                with gzip.open(arc, "rt", encoding="utf-8", errors="replace") as gz:
                    for ln in gz:
                        try:
                            obj = json.loads(ln)
                        except json.JSONDecodeError:
                            continue
                        # Extract assistant text content
                        msg = obj.get("message", {})
                        if msg.get("role") == "assistant":
                            content = msg.get("content", "")
                            if isinstance(content, list):
                                for block in content:
                                    if isinstance(block, dict) and block.get("type") == "text":
                                        text = block.get("text", "").strip()
                                        if text and len(text) > 20:
                                            out.write(text[:2000] + "\n")
                                            count += 1
                            elif isinstance(content, str) and len(content) > 20:
                                out.write(content[:2000] + "\n")
                                count += 1
            except Exception:
                pass
    print(f"[session_archive] mined {count} assistant messages → {out_path}")
    return count


def extract_pairs(project_filter: str = "", limit: int = 0, verbose: bool = False) -> int:
    """
    Extract (human_intent, action_type, tools_used, outcome) tuples from sessions.
    Writes to oracle/orc_data/session_pairs.jsonl — fed to oracle KB on next liljack update.

    Each pair record:
      {session_id, project, ts, human_intent, action_type, tools, tool_count, outcome_len}

    action_type classification (by dominant tool):
      DEPLOY | TEST | BUILD | CODE_EDIT | QUERY_KB | RESEARCH | GIT | ANALYZE | READ
    """
    PAIRS_OUT = Path(__file__).parent.parent / "orc_data" / "session_pairs.jsonl"
    index = _load_index()
    seen_sessions = set()
    if PAIRS_OUT.exists():
        for ln in PAIRS_OUT.read_text(errors="replace").splitlines():
            try:
                seen_sessions.add(json.loads(ln)["session_id"])
            except Exception:
                pass

    written = 0
    with open(PAIRS_OUT, "a", encoding="utf-8") as out:
        for meta in index.values():
            sid = meta.get("session_id", "")
            if sid in seen_sessions:
                continue
            if project_filter and project_filter not in meta.get("project_slug", ""):
                continue

            arc = ARCHIVE_ROOT / meta.get("archive_path", "")
            if not arc.exists():
                continue

            try:
                messages = []
                opener = gzip.open if str(arc).endswith(".gz") else open
                with opener(arc, "rt", encoding="utf-8", errors="replace") as f:
                    for ln in f:
                        try:
                            messages.append(json.loads(ln))
                        except Exception:
                            pass

                # Walk paired (human → assistant) turns
                i = 0
                while i < len(messages):
                    msg = messages[i]
                    role = msg.get("message", {}).get("role", "")
                    if role != "user":
                        i += 1
                        continue

                    # Extract human text (skip tool_result-only turns)
                    human_text = ""
                    content = msg.get("message", {}).get("content", [])
                    if isinstance(content, str):
                        human_text = content.strip()
                    elif isinstance(content, list):
                        for block in content:
                            if isinstance(block, dict) and block.get("type") == "text":
                                human_text += block.get("text", "").strip() + " "
                    human_text = human_text.strip()[:200]

                    # Collect all tool_uses from consecutive assistant turns
                    tools = []
                    total_output_len = 0
                    j = i + 1
                    while j < len(messages):
                        next_role = messages[j].get("message", {}).get("role", "")
                        if next_role == "user":
                            break
                        if next_role == "assistant":
                            ac = messages[j].get("message", {}).get("content", [])
                            if isinstance(ac, list):
                                for block in ac:
                                    if block.get("type") == "tool_use":
                                        tools.append(block.get("name", ""))
                                    elif block.get("type") == "text":
                                        total_output_len += len(block.get("text", ""))
                        j += 1
                    i = j

                    if not human_text or not tools:
                        continue

                    # Classify action type
                    tool_set = set(tools)
                    bash_dom = tools.count("Bash") > 2
                    edit_dom = tools.count("Edit") + tools.count("Write") > 1
                    if "mcp__apollo__" in " ".join(tools) or any("oracle" in t for t in tools):
                        action_type = "QUERY_KB"
                    elif bash_dom:
                        action_type = "ANALYZE"  # refined below
                    elif edit_dom:
                        action_type = "CODE_EDIT"
                    elif "Agent" in tool_set:
                        action_type = "ORCHESTRATE"
                    elif "WebFetch" in tool_set or "WebSearch" in tool_set:
                        action_type = "RESEARCH"
                    elif "Read" in tool_set and len(tool_set) <= 2:
                        action_type = "READ"
                    else:
                        action_type = "ANALYZE"

                    record = {
                        "session_id":   sid,
                        "project":      meta.get("project_slug", ""),
                        "ts":           msg.get("timestamp", ""),
                        "human_intent": human_text,
                        "action_type":  action_type,
                        "tools":        list(dict.fromkeys(tools)),  # unique, ordered
                        "tool_count":   len(tools),
                        "outcome_len":  total_output_len,
                    }
                    out.write(json.dumps(record) + "\n")
                    written += 1

                seen_sessions.add(sid)
                if limit and written >= limit:
                    break

            except Exception as e:
                if verbose:
                    print(f"  [pairs] skip {sid}: {e}")

    if verbose:
        print(f"[session_archive] pairs: {written} new records → {PAIRS_OUT}")
    return written


def _extract_conv_pairs(messages: list) -> list:
    """
    Walk session messages and return compact exchange pairs.
    User text: string content that is not system/hook-injected (doesn't start with '<' or '[lil').
    Claude text: concatenated text blocks from assistant messages (skip thinking blocks).
    Returns list of {"ts":str, "u":str, "a":str}.
    """
    pairs = []
    i = 0
    while i < len(messages):
        try:
            msg = messages[i]
            role = msg.get("message", {}).get("role", "")
        except Exception:
            i += 1
            continue

        if role != "user":
            i += 1
            continue

        # Extract user text — skip system/hook-injected messages
        content = msg.get("message", {}).get("content", "")
        ts = (msg.get("timestamp") or "")[:16]
        user_text = ""

        if isinstance(content, str):
            c = content.strip()
            if c and not c.startswith("<") and not c.startswith("[lil"):
                user_text = c
        elif isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("type") == "text":
                    t = block.get("text", "").strip()
                    if t and not t.startswith("<") and not t.startswith("[lil"):
                        user_text = t
                        break

        if not user_text:
            i += 1
            continue

        # Collect assistant text + thinking from following assistant messages
        asst_parts = []
        think_parts = []
        j = i + 1
        while j < len(messages):
            try:
                next_role = messages[j].get("message", {}).get("role", "")
            except Exception:
                j += 1
                continue
            if next_role == "user":
                break
            if next_role == "assistant":
                ac = messages[j].get("message", {}).get("content", "")
                if isinstance(ac, str):
                    ac = ac.strip()
                    if ac:
                        asst_parts.append(ac)
                elif isinstance(ac, list):
                    for block in ac:
                        if not isinstance(block, dict):
                            continue
                        btype = block.get("type", "")
                        if btype == "text":
                            t = block.get("text", "").strip()
                            if t:
                                asst_parts.append(t)
                        elif btype == "thinking":
                            t = block.get("thinking", "").strip()
                            if t:
                                think_parts.append(t)
            j += 1

        i = j
        asst_text = " ".join(asst_parts).strip()
        if not asst_text:
            continue  # no assistant response yet — skip incomplete exchange

        record: dict = {"ts": ts, "u": user_text[:400], "a": asst_text[:800]}
        if think_parts:
            record["r"] = " ".join(think_parts)[:600]  # reasoning/thinking
        pairs.append(record)

    return pairs


def record_conv(session_path: Path, verbose: bool = False) -> int:
    """
    Extract compact conversation from session JSONL → conv/<project>/<session>.jsonl.
    Each line: {"ts":"...","u":"user typed text","a":"claude response text"}.
    User and assistant stored as separate keys — query either independently.
    Idempotent: rewrites the conv file on every call.
    Returns number of exchanges written.
    """
    try:
        lines = session_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return 0

    messages = []
    for ln in lines:
        try:
            messages.append(json.loads(ln))
        except json.JSONDecodeError:
            pass

    if not messages:
        return 0

    pairs = _extract_conv_pairs(messages)
    if not pairs:
        return 0

    project = session_path.parent.name
    session_id = session_path.stem
    slug = _project_slug(project)

    dest_dir = CONV_DIR / slug
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / f"{session_id}.jsonl"

    with open(dest, "w", encoding="utf-8") as f:
        for pair in pairs:
            f.write(json.dumps(pair, ensure_ascii=False) + "\n")

    if verbose:
        print(f"  conv  {slug}/{session_id[:8]}  {len(pairs)} exchanges → {dest}")

    return len(pairs)


def record_current_session(verbose: bool = False) -> int:
    """Find the most recently modified Claude Code session and record its conversation."""
    if not CLAUDE_PROJECTS.exists():
        return 0
    sessions = sorted(CLAUDE_PROJECTS.glob("*/*.jsonl"),
                      key=lambda p: p.stat().st_mtime, reverse=True)
    if not sessions:
        return 0
    return record_conv(sessions[0], verbose=verbose)


def main():
    import argparse
    ap = argparse.ArgumentParser(description="liljack — Claude Code session archiver")
    ap.add_argument("--stats",     action="store_true", help="Show archive stats")
    ap.add_argument("--list",      action="store_true", help="List archived sessions")
    ap.add_argument("--mine",      action="store_true", help="Extract text for NLP mining")
    ap.add_argument("--pairs",     action="store_true", help="Extract (human→action) pairs for oracle KB")
    ap.add_argument("--sync",      action="store_true", help="Push local cache to Apollo (retry)")
    ap.add_argument("--no-apollo", action="store_true", help="Skip Apollo sync (offline mode)")
    ap.add_argument("--history",   action="store_true", help="Archive ~/.claude/history.jsonl prompt history")
    ap.add_argument("--scan",      action="store_true", help="PC-wide log scan (shell history, git repos, vscode)")
    ap.add_argument("--project",   default="",          help="Filter by project slug (for --mine/--pairs)")
    ap.add_argument("--output",    default="/tmp/session_mine.tsv", help="Output TSV for --mine")
    ap.add_argument("--promote-memories", action="store_true",
                    help="After --mine, promote recurring facts to liljack.dsl")
    ap.add_argument("--record", action="store_true",
                    help="Record current session conversation (Stop hook: fast)")
    ap.add_argument("--stop",   action="store_true",
                    help="Stop hook: record + archive local (no Apollo sync)")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    if args.record:
        n = record_current_session(verbose=args.verbose)
        if args.verbose:
            print(f"[liljack] conv: {n} exchange(s) recorded")
        return

    if args.stop:
        record_current_session()
        archive_all()
        # Keep session_pairs.jsonl (the inject hook's "similar past work") in
        # step with the archive. It was only ever mined by hand, froze on
        # 2026-04-07, and with MAX_PAIR_AGE_DAYS=180 would have gone silent
        # in October. Incremental: sessions already mined cost ~0.1s total.
        try:
            extract_pairs()
        except Exception:
            pass
        try:
            sys.path.insert(0, str(Path(__file__).parent))
            from liljack_chains import extract_current
            extract_current()
        except Exception:
            pass
        try:
            from liljack_projects import sync_current
            sync_current(push=False)   # fast local mirror; push rides the detached --sync
        except Exception:
            pass
        try:
            import sys as _sys
            _sys.path.insert(0, str(Path(__file__).parent))
            from liljack import update_quick
            update_quick()
        except Exception:
            pass
        _spawn_detached_sync()
        return

    if args.history:
        n = archive_history(verbose=True)
        print(f"[liljack] history: {n} new entries archived")
        return

    if args.scan:
        result = pc_scan(verbose=True)
        print(f"[liljack] pc_scan: {result['sources']} sources → {result['output']}")
        return

    if args.stats:
        s = stats()
        if not s:
            print("[liljack] No archived sessions yet.")
            return
        print(f"[liljack] Archive: {ARCHIVE_ROOT}")
        print(f"  Sessions : {s['total_sessions']}")
        print(f"  Messages : {s['total_messages']}")
        print(f"  Size     : {s['total_bytes'] // 1024} KB (uncompressed)")
        print("\n  By project:")
        for p, n in sorted(s["by_project"].items(), key=lambda kv: -kv[1]):
            print(f"    {n:4d}  {p}")
        return

    if args.list:
        index = _load_index()
        for meta in sorted(index.values(), key=lambda m: m.get("archived_at", ""), reverse=True)[:30]:
            compact_tag = f" [{meta['compaction_count']}✂]" if meta.get("compaction_count") else ""
            print(f"  {meta.get('archived_at','?')[:10]}  {meta['project_slug']:20s}  "
                  f"{meta['session_id'][:8]}  msgs={meta.get('message_count',0)}{compact_tag}")
        return

    if args.pairs:
        n = extract_pairs(project_filter=args.project, verbose=True)
        print(f"[liljack] pairs: {n} new records extracted")
        return

    if args.mine:
        mine_sessions(args.output, args.project)
        if args.promote_memories:
            import sys as _sys
            _sys.path.insert(0, str(Path(__file__).parent))
            from liljack import mine_memories
            result = mine_memories(verbose=True)
            promoted = result["promoted"]
            if promoted:
                print(f"[liljack] promoted {len(promoted)} memory atom(s): "
                      + ", ".join(promoted))
        return

    if args.sync:
        # Archive BEFORE pushing. The Stop hook is the normal archiver, but a
        # session that crashes, freezes with the box (incidents #1-#5) or is
        # killed never reaches it; liljack-sync.timer runs this every 5 min,
        # so a transcript is at most 5 min behind its archived copy.
        try:
            archive_all()
        except Exception:
            pass
        pushed, skipped = sync_to_apollo(verbose=True)
        if not pushed and not skipped:
            url = _apollo_url()
            print(f"[liljack] Apollo unreachable ({url or 'no URL configured'})")
        return

    # Default: archive new/updated sessions + sync to Apollo
    n, pushed, _ = archive_and_sync(
        verbose=args.verbose or True,
        no_apollo=args.no_apollo,
    )
    if n > 0:
        print(f"[liljack] archived {n} session(s) → {ARCHIVE_ROOT}")
    if pushed > 0:
        url = _apollo_url()
        print(f"[liljack] synced {pushed} session(s) → apollo://{url}/{JACK_REPO}")


if __name__ == "__main__":
    main()
