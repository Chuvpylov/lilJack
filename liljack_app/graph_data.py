"""Bounded read-only Git topology for the native DAG tile."""
import subprocess


def snapshot(repo, limit=100):
    limit = max(1, min(int(limit), 500))
    try:
        result = subprocess.run(
            ['git', '-C', str(repo), 'log', 'HEAD', '--branches', '--tags', '--remotes', '--topo-order',
             f'--max-count={limit + 1}', '--format=%H%x00%P%x00%D%x00%cI%x00%s'],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        if result.returncode:
            return {'nodes': [], 'truncated': False, 'error': result.stderr.strip() or 'Git history unavailable'}
        nodes = []
        for line in result.stdout.splitlines():
            fields = line.split('\0', 4)
            if len(fields) != 5:
                raise ValueError('Malformed Git topology record')
            identity, parents, refs, date, subject = fields
            nodes.append({'id': identity, 'parents': parents.split(),
                          'refs': refs, 'date': date, 'subject': subject})
        return {'nodes': nodes[:limit], 'truncated': len(nodes) > limit, 'error': None}
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        return {'nodes': [], 'truncated': False, 'error': str(exc) or type(exc).__name__}
