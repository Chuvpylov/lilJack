"""Wake an existing lead; never spawn/resume a replacement provider session."""
import subprocess

from liljack_workspace import session_key
from .runtime import Tmux


def harness_for(agent):
    return {'codex': 'codex', 'claude': 'claude', 'deepseek': 'opencode'}.get(agent)


def wake(native, notice, project, *, harness, target, tmux=None):
    if harness == 'codex':
        return subprocess.run(['codex', 'queue', '--thread', native, '--message', notice],
                              cwd=project, capture_output=True, text=True, timeout=10)
    if harness not in ('claude', 'opencode'):
        raise ValueError('No wake transport for this harness')
    # A short generated notice, not a pasted room body or shell command.
    if not notice or len(notice) > 240 or any(ord(c) < 32 or ord(c) > 126 for c in notice):
        raise ValueError('Pane delivery requires a short ASCII notice')
    tmux = tmux or Tmux()
    current = next((r for r in tmux.sessions(project) if r['id'] == target['id']), None)
    if (not current or current.get('state') != 'running' or
            harness_for(current.get('agent')) != harness or
            current.get('pane') != target.get('pane') or
            current.get('tmux_name') != target.get('tmux_name') or
            current['id'] != session_key('tmux', f"{tmux.socket}/{current.get('tmux_name')}")):
        raise RuntimeError('Exact lead terminal changed before delivery')
    if not current.get('pane'):
        raise RuntimeError('Lead has no exact pane')
    tmux.call('send-keys', '-t', current['pane'], '-l', '--', notice)
    # Never retry if the first write or this submit has an ambiguous outcome.
    return tmux.call('send-keys', '-t', current['pane'], 'Enter')
