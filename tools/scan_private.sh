#!/usr/bin/env bash
# Guard for the public lilJack repository.
# Usage: scan_private.sh            scan every tracked file
#        scan_private.sh <rev>...   scan the added lines of each commit
# HARD patterns (private paths, LAN hosts, personal ids, credential shapes) fail
# the scan. NAME patterns (sibling project names) are only listed as warnings.
# Output is file:line plus a redacted excerpt; a token value is never printed.
set -euo pipefail
hard='/run/media/|MaShit|PRO/brain|tohache|@gmail\.com|(^|[^0-9])(10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|192\.168\.[0-9]{1,3}\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3})([^0-9.]|$)|[0-9a-f]{56,}|-----BEGIN [A-Z ]*PRIVATE KEY-----|eyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.|gh[pousr]_[A-Za-z0-9]{20,}|https?://[^/ :]+:[^@/ ]+@|(token|secret|passw|api[_-]?key)[[:space:]]*[:=][[:space:]]*["'"'"'][A-Za-z0-9_-]{12,}|\b(bibika|cerbilly|agit)\b'
names='\b(Cerebrum|Occipital|braider|owkDeck|RedIce|X-Cerber-Ring|anton-approved)\b|(?<![a-zA-Z])Jack\b|internal (app|tree|branch)|\b[srm]-[0-9a-f]{32}\b'
redact() { grep -vE 'github\.com/(tohache|Chuvpylov)|Source SHA256' | sed -E 's/[0-9a-f]{40,}/<hex>/g' | cut -c1-120; }
status=0
if (($#)); then
    for rev in "$@"; do
        # the WHOLE tree of that commit, not only its added lines
        if git grep -nE "$hard" "$rev" -- . ':!tools/scan_private.sh' | redact; then echo "scan_private: tree of $rev carries private content" >&2; status=1; fi
        git grep -cP "$names" "$rev" -- . ':!tools/scan_private.sh' | sed "s/^/scan_private: $rev name-warning: /" >&2 || true
    done
else
    if git grep -nE "$hard" -- . ':!tools/scan_private.sh' | redact; then status=1; fi
    git grep -cP "$names" -- . ':!tools/scan_private.sh' | sed 's/^/name-warning: /' >&2 || true
fi
exit $status
