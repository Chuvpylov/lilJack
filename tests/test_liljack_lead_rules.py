#!/usr/bin/env python3
"""Editable lead rules (the operator 2026-10-24: "i want to be able to edit rules").

One list (liljack_app/lead_rules.py) feeds both the room-folder CLAUDE.md /
AGENTS.md fragment and the agent system prompt; the NEW ROOM form edits it
through the backend actions rules_get / rules_set / rules_reset."""
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'toolbox')]
tmp = tempfile.mkdtemp(prefix='liljack-rules-')
os.environ['LILJACK_RULES_PATH'] = str(Path(tmp) / 'sub' / 'lead_rules.json')

from liljack_app import lead_rules as L                        # noqa: E402
from liljack_app import room_instructions as I, room_start as R  # noqa: E402
from liljack_app.backend import Backend                         # noqa: E402

passed = failed = 0
def check(ok, what):
    global passed, failed
    passed += bool(ok); failed += not ok
    print(('  ok     ' if ok else '  FAIL   ') + what)

def fragment_rules():
    return [l for l in I.fragment('r-1', 'n', 'p', '/f', '/p', []).splitlines() if l.startswith('- ')]

check(not L.is_custom() and L.load() == list(L.DEFAULTS), 'no file: the defaults')
check(fragment_rules() == ['- ' + r for r in L.DEFAULTS], 'fragment carries the defaults')
check(all('- ' + r in R.system_prompt_text('/p', 'r-1') for r in L.DEFAULTS), 'system prompt carries the defaults')

saved = L.save(['  - first rule ', '', 'second rule'])
check(saved == ['first rule', 'second rule'], 'save trims, drops bullets and empty rows')
check(json.loads(Path(os.environ['LILJACK_RULES_PATH']).read_text()) == saved, 'saved as a JSON list')
check(fragment_rules() == ['- first rule', '- second rule'], 'fragment follows the edit')
prompt = R.system_prompt_text('/p', 'r-1').splitlines()
check(prompt[-3:] == [R.LEAD_RULES_HEADER, '- first rule', '- second rule'], 'system prompt follows the edit')

for bad, why in ((['x\ny'], 'control'), ([], 'empty'), (['x' * 401], 'long'), (['r'] * 25, 'too many'), ('no', 'type')):
    try:
        L.save(bad); check(False, f'refuses {why}')
    except ValueError:
        check(L.load() == saved, f'refuses {why}, keeps the saved list')

Path(os.environ['LILJACK_RULES_PATH']).write_text('{broken')
check(L.load() == list(L.DEFAULTS), 'unreadable file falls back to the defaults')
L.save(saved)

b = Backend.__new__(Backend)
b.snapshot = lambda: {'snap': 1}
got = b.request({'action': 'rules_get'})
check(got.get('snap') == 1 and got['lead_rules']['rules'] == saved and got['lead_rules']['custom'], 'rules_get serves the saved list')
check(got['lead_rules']['defaults'] == list(L.DEFAULTS), 'rules_get also serves the defaults for DEFAULTS')
got = b.request({'action': 'rules_set', 'rules': ['only rule']})
check(got['lead_rules']['rules'] == ['only rule'] and L.load() == ['only rule'], 'rules_set saves')
got = b.request({'action': 'rules_reset'})
check(got['lead_rules']['rules'] == list(L.DEFAULTS) and not L.is_custom(), 'rules_reset removes the file')

# ⚠ CONTEXT AND INTRO ARE EDITABLE TOO (the operator, 2026-10-26: "all the agent rules
# and context and initial message should be editable in a menu and when
# creating as a button"). Same editor, a `section` on the same actions.
rules_file = Path(os.environ['LILJACK_RULES_PATH'])
check(L.load('context') == list(L.CONTEXT_DEFAULTS) and L.load('intro') == list(L.INTRO_DEFAULTS),
      'no files: context and intro are their defaults')
check(R._onboarding_lines()[1:-1] == ['- ' + r for r in L.CONTEXT_DEFAULTS],
      'the standing set is the context section, framed by its fixed header and closing line')
got = b.request({'action': 'rules_set', 'section': 'context', 'rules': ['ctx one', 'ctx two']})
check(got['lead_rules']['section'] == 'context' and got['lead_rules']['rules'] == ['ctx one', 'ctx two'],
      'rules_set with section=context saves and answers that section')
check(L.path('context') == rules_file.with_name('agent_context.json') and L.path('context').is_file(),
      'context is stored beside lead_rules.json')
check(L.load() == list(L.DEFAULTS), '...and leaves the lead rules alone')
prompt = R.system_prompt_text('/p', 'r-1').splitlines()
check(prompt[1:3] == ['- ctx one', '- ctx two'], 'system prompt carries the edited context')
check(R._onboarding_lines()[0].startswith('STANDING INSTRUCTION SET') and
      R._onboarding_lines()[-1].startswith('Everything above already exists'), 'header and closing line stay')
got = b.request({'action': 'rules_get', 'section': 'intro'})
check(got['lead_rules']['rules'] == list(L.INTRO_DEFAULTS) and not got['lead_rules']['custom']
      and got['lead_rules']['defaults'] == list(L.INTRO_DEFAULTS), 'rules_get serves the intro section')
b.request({'action': 'rules_set', 'section': 'intro', 'rules': ['Say hello to the lead first.']})
_ctx = {'room': {'name': 'n', 'purpose': 'p', 'folder': '/f', 'phase': 'WORK'}, 'role': 'worker',
        'last_seq': 1, 'cursor': 2, 'members': [], 'todo': [], 'other_rooms': []}
_orig = R.room_context
R.room_context = lambda *a, **k: _ctx
try:
    intro = R.intro_text(type('S', (), {'root': Path(tmp)})(), '/p', 'r-1', 's-1').splitlines()
finally:
    R.room_context = _orig
check('Say hello to the lead first.' in intro and L.INTRO_DEFAULTS[0] not in intro,
      'the initial message carries the edited intro lines instead of the defaults')
check('- ctx one' in intro, 'the initial message carries the edited context')
got = b.request({'action': 'rules_reset', 'section': 'intro'})
check(got['lead_rules']['rules'] == list(L.INTRO_DEFAULTS) and not L.is_custom('intro'), 'rules_reset per section')
try:
    L.load('nope'); check(False, 'refuses an unknown section')
except ValueError:
    check(True, 'refuses an unknown section')

print(f'\n{passed} passed, {failed} failed')
sys.exit(1 if failed else 0)
