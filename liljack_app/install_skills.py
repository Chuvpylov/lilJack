"""Install the repository-owned room skills without replacing personal skills."""
from __future__ import annotations
import json
import os
from pathlib import Path
import re
import tempfile

NAMES = ('room-protocol', 'report-format', 'registry-moves', 'evidence-rules')
TOKEN = re.compile(r'\s+|//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}\[\],:]|[^\s{}\[\],:]+')


def config_with_path(text: str, source: str) -> str:
    """Find JSONC member spans, preserving comments and all unrelated bytes."""
    tokens = [(m.group(), m.start(), m.end()) for m in TOKEN.finditer(text)
              if not m.group().isspace() and not m.group().startswith(('//', '/*'))]
    spans = {}
    def parse(i, path):
        start = i
        token = tokens[i][0]
        if token in ('{', '['):
            closing = '}' if token == '{' else ']'
            value = {} if token == '{' else []
            i += 1
            while tokens[i][0] != closing:
                if token == '{':
                    key = json.loads(tokens[i][0])
                    if not isinstance(key, str) or tokens[i+1][0] != ':':
                        raise ValueError('Invalid JSONC object member')
                    if key in value:
                        raise ValueError('Duplicate JSONC member')
                    child, i = parse(i+2, path + (key,))
                    value[key] = child
                else:
                    child, i = parse(i, path + (len(value),))
                    value.append(child)
                if tokens[i][0] == ',':
                    i += 1
                elif tokens[i][0] != closing:
                    raise ValueError('Invalid JSONC separator')
            i += 1
        else:
            value = json.loads(token)
            i += 1
        spans[path] = (tokens[start][1], tokens[start][2], tokens[i-1][2])
        return value, i
    try:
        root, end = parse(0, ())
        if end != len(tokens) or not isinstance(root, dict):
            raise ValueError('Expected JSONC root object')
        skills = root.get('skills', {})
        if not isinstance(skills, dict):
            raise ValueError('Expected skills object')
        paths = skills.get('paths', [])
        if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
            raise ValueError('Expected skills.paths string array')
        if source in paths:
            return text
        if 'skills' not in root:
            offset = spans[()][1]
            addition = '\n  "skills": {"paths": [' + json.dumps(source) + ']}' + (',' if root else '')
        elif 'paths' not in skills:
            offset = spans[('skills',)][1]
            addition = '"paths": [' + json.dumps(source) + ']' + (',' if skills else '')
        else:
            offset = spans[('skills', 'paths')][1]
            addition = json.dumps(source) + (',' if paths else '')
        return text[:offset] + addition + text[offset:]
    except (IndexError, json.JSONDecodeError) as exc:
        raise ValueError('Cannot parse OpenCode JSONC; configuration left intact') from exc


def install() -> None:
    home = Path.home()
    source = Path(__file__).resolve().parents[1] / 'plugins/liljack/skills'
    codex = Path(os.environ.get('CODEX_HOME', str(home / '.codex')))
    config_dir = Path(os.environ.get('XDG_CONFIG_HOME', str(home / '.config')))
    plugin = home / '.claude/plugins/liljack-room-skills'
    bases = (codex / 'skills', home / '.claude/skills', plugin / 'skills')
    links = [(base / name, source / name) for base in bases for name in NAMES]
    # Preflight all destinations before modifying any harness configuration.
    for target, origin in links:
        if not (origin / 'SKILL.md').is_file():
            raise ValueError('Repository room skill is missing')
        if target.exists() or target.is_symlink():
            if not target.is_symlink() or target.resolve() != origin.resolve():
                raise ValueError('Existing personal skill conflicts with room skill; left intact')
    manifest = plugin / '.claude-plugin/plugin.json'
    manifest_text = json.dumps({'name': 'liljack-room-skills', 'version': '0.1.0',
                                'description': 'Shared lilJack room coordination skills'}, indent=2) + '\n'
    if manifest.exists() and manifest.read_text() != manifest_text:
        raise ValueError('Existing Claude plugin manifest differs; left intact')
    config = config_dir / 'opencode/opencode.jsonc'
    if not config.exists() and (config.parent / 'opencode.json').exists():
        config = config.parent / 'opencode.json'
    original = config.read_text() if config.exists() else '{}\n'
    updated = config_with_path(original, str(source))
    for target, origin in links:
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_symlink():
            target.symlink_to(origin, target_is_directory=True)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    if not manifest.exists():
        manifest.write_text(manifest_text)
    if updated != original:
        config.parent.mkdir(parents=True, exist_ok=True)
        # Atomic replacement, retaining permissions and never logging config data.
        fd, temp = tempfile.mkstemp(prefix='.liljack-skills-', dir=config.parent)
        try:
            with os.fdopen(fd, 'w') as stream:
                stream.write(updated)
            if config.exists():
                os.chmod(temp, config.stat().st_mode & 0o777)
            os.replace(temp, config)
        finally:
            if os.path.exists(temp):
                os.unlink(temp)
    print('Installed 4 shared lilJack skills for Claude, Codex and OpenCode')

if __name__ == '__main__':
    try:
        install()
    except (OSError, ValueError) as exc:
        # Do not expose configuration contents or credential-shaped values.
        raise SystemExit('Skill installation refused: ' + (str(exc) if isinstance(exc, ValueError) else 'filesystem error'))
