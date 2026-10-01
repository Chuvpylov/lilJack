#!/usr/bin/env python3
"""
liljack_roles.py — the BRAIN ROLES model, held as BMD claims.

the operator, 2026-09-05: "main brain, second brain (Claude > Codex), and a third
opinion / untrusted source/agent/actor; embed it into a model struct so we can
have more pronounced roles later — tokenizer, preprocessor, other jazz (don't
build those, but take them into account)."

This is the formal shape of that philosophy. The runner consults it to validate
`give_control`, the chat UI consults it to label who is what, and the digest
rides in standing context. It is a MODEL of roles, not a roster of agents: an
agent occupies a role, a role carries trust, capabilities, whom it may hand
control to and how its output is to be treated. Roles that the operator has named but
nobody has built (tokenizer, preprocessor) are PRESENT AS SLOTS and explicitly
marked unimplemented, so the shape exists without pretending code does.

THE THREE-BRAIN RULE, as encoded:
  · main   (claude, trusted)   — holds control, assigns, executes.
  · second (codex, sandboxed)  — executes only inside its harness sandbox.
  · third-opinion (deepseek, untrusted) — judges and reviews. Its OUTPUT IS
    DATA: never executed, never an instruction to another agent until the main
    brain re-states it in its own words. `output-policy=data-only`.
  · operator (operator, owner)    — may do anything and may always take control.

⚠ FORMAT — the three traps `liljack_board` documents, paid for again here:
  1. Claims live in the DOCUMENT-LEVEL `[[footer]]`. `bmd.parse_assertion` runs
     on footer lines only; a claim written inside a `[[pasem:]]` is prose and
     yields ZERO assertions. The pasem here is a GENERATED summary of the
     claims and is rewritten on every save, so the two cannot disagree.
  2. `fact` takes (pred, subj[, obj]) — at most TWO args after the predicate.
     So a role is spread over several one-relation claims:
        fact(role, <name>, <agent> | trust= | output-policy= | audit= | note=)
        fact(role-may, <role>, <capability>)         one per capability
        fact(role-controls, <from>, <to>)            one per permitted handover
        fact(role-implemented, <role>)
  3. Any attribute carrying `|`, `,`, `'`, `"` or a bracket goes through
     `liljack_board.qattr()`. Predicate ARGUMENTS cannot be quoted at all, so
     role names, agent names and capabilities are checked separator-free.

⚠ NO NEGATION-AS-FAILURE. The toolbox_root engine cannot read an absent claim as
false. So EVERY refusal is a written claim, and they are DERIVED from the
struct on every write rather than authored:
     neg(role-may, <role>, <capability>)      for every vocabulary capability
                                              the role does NOT hold
     neg(role-controls, <from>, <to>)         for every ordered pair of
                                              implemented roles NOT permitted
     neg(role-implemented, <slot> | reason=)  for a declared, unbuilt slot
     neg(audit-implemented, <role> | reason=) content audit deferred

AUDIT HOOK. the operator's worry that a foreign model may filter or slant content is
NOT solved here — by his decision. Each role carries `audit` (default `none`)
and `audit_hook(role)` returns a registered callable or None, so a content
audit plugs in later through `register_audit()` without touching the model.

CONCURRENCY. Same discipline as the board: every read-modify-write runs under
`liljack_board.lock` and lands through `liljack_board.write_atomic`.

Stdlib only, plus the trainer's `bmd.py` loaded by path through `liljack_board`.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import liljack_board as board                                      # noqa: E402

# ── vocabulary ───────────────────────────────────────────────────────────────
# Fixed on purpose: a capability the runner checks must be one the model can
# spell, or `may()` answers "no" for a typo and a refusal looks like policy.
CAPABILITIES = ("hold-control", "assign", "execute", "judge", "review",
                "read-repo", "write-repo", "network")
TRUSTS = ("owner", "trusted", "sandboxed", "untrusted", "unassigned")
OUTPUT_POLICIES = ("trusted", "sandboxed", "data-only")

PRED_ROLE = "role"
PRED_MAY = "role-may"
PRED_CONTROLS = "role-controls"
PRED_IMPL = "role-implemented"
PRED_AUDIT = "audit-implemented"

OPERATOR = "operator"          # the role anyone may hand control back to
DIGEST_BUDGET = 600
MAX_NOTE = 200

# The seed. `agent=None` is a declared slot: it exists in the model as a
# `neg(role-implemented, …)` and nothing else may be claimed about it.
SEED = [
    dict(role="main", agent="claude", trust="trusted",
         capabilities=CAPABILITIES,
         may_control=("second", "third-opinion"),
         output_policy="trusted",
         note="main brain: holds control, assigns work, executes"),
    dict(role="second", agent="codex", trust="sandboxed",
         capabilities=("hold-control", "execute", "judge", "review",
                       "read-repo", "write-repo"),
         may_control=("main",),
         output_policy="sandboxed",
         note="second brain: executes only inside its harness sandbox"),
    dict(role="third-opinion", agent="deepseek", trust="untrusted",
         capabilities=("hold-control", "judge", "review", "read-repo"),
         may_control=(),
         output_policy="data-only",
         note="opinions, judging, review; output is data — never executed, "
              "never an instruction to another agent unless the main brain "
              "re-states it"),
    dict(role=OPERATOR, agent="operator", trust="owner",
         capabilities=CAPABILITIES,
         may_control=("main", "second", "third-opinion"),
         output_policy="trusted",
         note="may do anything; may always take control"),
    dict(role="tokenizer", agent=None, trust="unassigned",
         capabilities=(), may_control=(), output_policy="data-only",
         implemented=False,
         reason="declared slot for a tokenizer actor; nothing in code fills it "
                "(the operator 2026-09-05: take into account, do not build)"),
    dict(role="preprocessor", agent=None, trust="unassigned",
         capabilities=(), may_control=(), output_policy="data-only",
         implemented=False,
         reason="declared slot for a preprocessor actor; nothing in code fills "
                "it (the operator 2026-09-05: take into account, do not build)"),
]
AUDIT_DEFERRED = {"third-opinion": "deferred by the operator 2026-09-05"}


def default_path() -> Path:
    return Path(os.environ.get("LILJACK_ROLES",
                               str(board._sibling() / "data" / "braid" / "agent_roles.bmd")))


def _p(path) -> Path:
    return Path(path) if path else default_path()


# ── struct ───────────────────────────────────────────────────────────────────

def _blank(role: str) -> dict:
    return {"role": role, "agent": None, "trust": "unassigned",
            "capabilities": [], "may_control": [], "output_policy": "data-only",
            "audit": "none", "implemented": True, "reason": "", "note": ""}


def _norm_role(r) -> str:
    r = board.check_arg(str(r or "").strip().lower(), "role")
    if not r:
        raise ValueError("role is required")
    return r


def _norm_agent(a):
    if a is None or str(a).strip() in ("", "-", "none"):
        return None
    return board.check_arg(str(a).strip().lower(), "agent")


def _validate(r: dict) -> dict:
    r["role"] = _norm_role(r["role"])
    r["agent"] = _norm_agent(r.get("agent"))
    if r["trust"] not in TRUSTS:
        raise ValueError(f"trust must be one of {TRUSTS}, got {r['trust']!r}")
    if r["output_policy"] not in OUTPUT_POLICIES:
        raise ValueError(f"output_policy must be one of {OUTPUT_POLICIES}, "
                         f"got {r['output_policy']!r}")
    caps = sorted({str(c).strip().lower() for c in r["capabilities"]})
    bad = [c for c in caps if c not in CAPABILITIES]
    if bad:
        raise ValueError(f"unknown capability {bad} — vocabulary is {CAPABILITIES}")
    r["capabilities"] = caps
    # The operator is reachable from everyone BY RULE (may_control), so it is
    # never stored in a declared set; self-handover is not a handover.
    r["may_control"] = sorted({_norm_role(x) for x in r["may_control"]} - {r["role"], OPERATOR})
    r["audit"] = board.check_arg(str(r.get("audit") or "none").strip().lower(), "audit")
    r["implemented"] = bool(r.get("implemented", True))
    r["reason"] = str(r.get("reason") or "").strip()[:MAX_NOTE]
    r["note"] = str(r.get("note") or "").strip()[:MAX_NOTE]
    if not r["implemented"] and r["agent"] is not None:
        raise ValueError(f"{r['role']}: an unimplemented slot cannot carry an agent")
    if not r["implemented"] and not r["reason"]:
        raise ValueError(f"{r['role']}: an unimplemented slot needs a reason=")
    if not r["implemented"] and (r["capabilities"] or r["may_control"]):
        raise ValueError(f"{r['role']}: an unimplemented slot may not claim "
                         "capabilities or control")
    return r


def _seed_roles() -> dict:
    out = {}
    for s in SEED:
        r = _blank(s["role"])
        r.update({k: v for k, v in s.items() if k in r})
        r["audit"] = "none"
        out[r["role"]] = _validate(r)
    return out


# ── read ─────────────────────────────────────────────────────────────────────

def read(path=None) -> dict:
    """Parse the roles file. Returns {"roles": {...}, "errors", "stray",
    "header", "exists", "path", "audit_deferred"}."""
    p = _p(path)
    B = board.bmd()
    out = {"path": str(p), "exists": p.exists(), "roles": {}, "errors": 0,
           "stray": 0, "header": {}, "audit_deferred": {}, "neg_may": [],
           "neg_controls": []}
    if not p.exists():
        return out
    text = p.read_text(encoding="utf-8", errors="replace")
    try:
        doc = B.parse(text, strict=True)
    except B.BmdError:
        doc = B.parse(text, strict=False)
        out["errors"] = 1
    out["header"] = dict(doc.header)
    out["stray"] = sum(len(ps.assertions) for ps in doc.pasems)
    roles = out["roles"]

    def get(name):
        return roles.setdefault(name, _blank(name))

    for a in doc.assertions:
        args = [str(x) for x in a.args]
        if a.form == "fact" and a.pred == PRED_ROLE and args:
            r = get(args[0])
            r["agent"] = args[1] if len(args) > 1 else None
            r["trust"] = a.attrs.get("trust", r["trust"])
            r["output_policy"] = a.attrs.get("output-policy", r["output_policy"])
            r["audit"] = a.attrs.get("audit", "none")
            r["note"] = a.attrs.get("note", "")
        elif a.form == "fact" and a.pred == PRED_MAY and len(args) == 2:
            get(args[0])["capabilities"].append(args[1])
        elif a.form == "fact" and a.pred == PRED_CONTROLS and len(args) == 2:
            get(args[0])["may_control"].append(args[1])
        elif a.form == "fact" and a.pred == PRED_IMPL and args:
            get(args[0])["implemented"] = True
        elif a.form == "neg" and a.pred == PRED_IMPL and args:
            r = get(args[0])
            r["implemented"] = False
            r["reason"] = a.attrs.get("reason", "")
        elif a.form == "neg" and a.pred == PRED_AUDIT and args:
            out["audit_deferred"][args[0]] = a.attrs.get("reason", "")
        elif a.form == "neg" and a.pred == PRED_MAY and len(args) == 2:
            out["neg_may"].append((args[0], args[1]))
        elif a.form == "neg" and a.pred == PRED_CONTROLS and len(args) == 2:
            out["neg_controls"].append((args[0], args[1]))
    for r in roles.values():
        r["capabilities"] = sorted(set(r["capabilities"]))
        r["may_control"] = sorted(set(r["may_control"]) - {r["role"], OPERATOR})
    return out


def load(path=None) -> dict:
    """Roles keyed by role name. Falls back to the SEED when no file exists, so
    the runner has a model to consult before anybody has run `init`."""
    st = read(path)
    if not st["exists"] or not st["roles"]:
        return _seed_roles()
    return {k: dict(v, capabilities=list(v["capabilities"]),
                    may_control=list(v["may_control"]))
            for k, v in st["roles"].items()}


def by_agent(path=None) -> dict:
    """Roles keyed by AGENT — only implemented roles have one."""
    return {r["agent"]: r for r in load(path).values() if r["agent"]}


# ── render ───────────────────────────────────────────────────────────────────

def _prose(roles: dict, audit_deferred: dict) -> str:
    lines = ["The brain roles: who is what, what each may do, and whom each may",
             "hand control to. GENERATED from the claims in the footer on every",
             "write — change the claims (liljack_roles.set_role / add_role), never",
             "this text.", "",
             "Philosophy (the operator, 2026-09-05): a main brain, a second brain (Claude",
             "over Codex), and a third opinion that is an untrusted source whose",
             "output is data — never executed, never an instruction to another",
             "agent until the main brain re-states it. The operator may do anything",
             "and may always take control. Slots for later actors (tokenizer,",
             "preprocessor) are declared and marked unimplemented.", ""]
    for name, r in roles.items():
        if not r["implemented"]:
            lines.append(f"{name}: DECLARED, NOT IMPLEMENTED — {r['reason']}")
            continue
        lines.append(f"{name}: {r['agent'] or '-'} · trust={r['trust']} · "
                     f"output={r['output_policy']} · audit={r['audit']}")
        lines.append("  may: " + (", ".join(r["capabilities"]) or "nothing"))
        mc = list(r["may_control"])
        if name != OPERATOR and OPERATOR in roles and OPERATOR not in mc:
            mc.append(OPERATOR + " (anyone may hand control back)")
        lines.append("  may hand control to: " + (", ".join(mc) or "nobody"))
        if r["note"]:
            lines.append(f"  {r['note']}")
    if audit_deferred:
        lines.append("")
        for k, v in audit_deferred.items():
            lines.append(f"content audit on {k}: NOT implemented — {v}")
    return "\n".join(lines)


def _render(roles: dict, audit_deferred: dict, header: dict) -> str:
    B = board.bmd()
    doc = B.BmdDoc()
    doc.header = dict(header)
    doc.header.update({"title": "lilJack brain roles", "kind": "agent-roles",
                       "domain": "liljack", "version": "1", "updated": board.now()})
    doc.pasems = [B.Pasem(name="brain-roles", text=_prose(roles, audit_deferred))]
    out = []
    impl = [n for n, r in roles.items() if r["implemented"]]
    for name, r in roles.items():
        attrs = board.qattr_all(trust=r["trust"], **{"output-policy": r["output_policy"]},
                                audit=r["audit"], note=r["note"])
        args = [name] + ([r["agent"]] if r["agent"] else [])
        out.append(B.Assertion("fact", PRED_ROLE, args, attrs))
        if not r["implemented"]:
            out.append(B.Assertion("neg", PRED_IMPL, [name],
                                   board.qattr_all(reason=r["reason"])))
            continue
        out.append(B.fact(PRED_IMPL, name))
        for c in CAPABILITIES:
            if c in r["capabilities"]:
                out.append(B.fact(PRED_MAY, name, c))
            else:
                out.append(B.Assertion("neg", PRED_MAY, [name, c],
                                       board.qattr_all(reason=f"{name} does not hold {c}")))
        for other in impl:
            if other == name:
                continue
            if other in r["may_control"] or other == OPERATOR:
                out.append(B.fact(PRED_CONTROLS, name, other))
            else:
                out.append(B.Assertion("neg", PRED_CONTROLS, [name, other],
                                       board.qattr_all(reason=f"{name} may not hand control to {other}")))
    for k, v in audit_deferred.items():
        out.append(B.Assertion("neg", PRED_AUDIT, [k], board.qattr_all(reason=v)))
    doc.assertions = out
    return doc.render()


def _save(p: Path, roles: dict, audit_deferred: dict, header: dict) -> None:
    board.write_atomic(p, _render(roles, audit_deferred, header))


def init(path=None, force: bool = False) -> Path:
    """Write the seed model. Refuses to overwrite unless `force`."""
    p = _p(path)
    with board.lock(p):
        if p.exists() and not force:
            return p
        _save(p, _seed_roles(), dict(AUDIT_DEFERRED), {})
    return p


# ── write ────────────────────────────────────────────────────────────────────

_FIELDS = ("agent", "trust", "capabilities", "may_control", "output_policy",
           "audit", "implemented", "reason", "note")


def set_role(role: str, path=None, **fields) -> dict:
    """Update ONE role in place; fields not passed keep their value.
    Locked, atomic, and a full BMD round-trip: what is returned is what the
    file now says, re-read through bmd.py."""
    role = _norm_role(role)
    bad = [k for k in fields if k not in _FIELDS]
    if bad:
        raise ValueError(f"unknown role field(s) {bad}; fields are {_FIELDS}")
    p = _p(path)
    with board.lock(p):
        st = read(p)
        roles = st["roles"] if st["exists"] and st["roles"] else _seed_roles()
        deferred = st["audit_deferred"] if st["exists"] and st["roles"] else dict(AUDIT_DEFERRED)
        if role not in roles:
            raise ValueError(f"unknown role {role!r}; add_role() first")
        r = dict(roles[role])
        r.update(fields)
        roles[role] = _validate(r)
        _save(p, roles, deferred, st["header"])
    return dict(read(p)["roles"][role])


def add_role(role: str, agent=None, trust: str = "unassigned", capabilities=(),
             may_control=(), output_policy: str = "data-only", audit: str = "none",
             implemented: bool = True, reason: str = "", note: str = "",
             path=None) -> dict:
    """Add a role. Refuses an existing name — set_role() changes one."""
    role = _norm_role(role)
    p = _p(path)
    with board.lock(p):
        st = read(p)
        roles = st["roles"] if st["exists"] and st["roles"] else _seed_roles()
        deferred = st["audit_deferred"] if st["exists"] and st["roles"] else dict(AUDIT_DEFERRED)
        if role in roles:
            raise ValueError(f"role {role!r} exists; use set_role()")
        r = _blank(role)
        r.update(agent=agent, trust=trust, capabilities=list(capabilities),
                 may_control=list(may_control), output_policy=output_policy,
                 audit=audit, implemented=implemented, reason=reason, note=note)
        roles[role] = _validate(r)
        _save(p, roles, deferred, st["header"])
    return dict(read(p)["roles"][role])


# ── queries (the runner's contract) ──────────────────────────────────────────

def _resolve(name, roles: dict) -> dict:
    """A role name or an agent name → the role dict. Unknown is REFUSED, never
    treated as 'no permissions' — a typo must not look like policy."""
    n = str(name or "").strip().lower()
    if n in roles:
        return roles[n]
    for r in roles.values():
        if r["agent"] == n:
            return r
    raise ValueError(f"unknown role or agent {name!r}")


def role_of(agent: str, path=None) -> str:
    n = str(agent or "").strip().lower()
    for r in load(path).values():
        if r["agent"] == n:
            return r["role"]
    raise ValueError(f"unknown agent {agent!r}")


def may(role_or_agent: str, capability: str, path=None) -> bool:
    c = str(capability or "").strip().lower()
    if c not in CAPABILITIES:
        raise ValueError(f"unknown capability {capability!r}; vocabulary is {CAPABILITIES}")
    r = _resolve(role_or_agent, load(path))
    return r["implemented"] and c in r["capabilities"]


def may_control(from_agent: str, to_agent: str, path=None) -> bool:
    """May `from` hand control to `to`? Roles or agents on either side.
    operator→anyone; anyone→operator; otherwise the from-role's declared set,
    and the receiver must be implemented and able to hold control."""
    roles = load(path)
    a, b = _resolve(from_agent, roles), _resolve(to_agent, roles)
    if a["role"] == b["role"] or not a["implemented"] or not b["implemented"]:
        return False
    if b["role"] == OPERATOR:
        return True
    if "hold-control" not in b["capabilities"]:
        return False
    if a["role"] == OPERATOR:
        return True
    return b["role"] in a["may_control"]


def output_policy(agent: str, path=None) -> str:
    return _resolve(agent, load(path))["output_policy"]


# ── audit hook ───────────────────────────────────────────────────────────────
# `audit` on a role names an entry in this registry. Nothing is registered
# today — the operator deferred the content audit — so the hook returns None and the
# BMD says so with `neg(audit-implemented, third-opinion | reason=…)`.
_AUDITS: dict = {}


def register_audit(name: str, fn) -> None:
    if not callable(fn):
        raise TypeError("audit must be callable")
    _AUDITS[str(name).strip().lower()] = fn


def audit_hook(role: str, path=None):
    """The callable that audits this role's output, or None when the role has
    `audit=none` or names an audit nobody registered."""
    r = _resolve(role, load(path))
    return _AUDITS.get(r.get("audit") or "none")


# ── digest ───────────────────────────────────────────────────────────────────

def digest(path=None, budget: int = DIGEST_BUDGET) -> str:
    roles = load(path)
    st = read(path)
    deferred = st["audit_deferred"] if st["exists"] and st["roles"] else dict(AUDIT_DEFERRED)
    impl = [r for r in roles.values() if r["implemented"]]
    slots = [r["role"] for r in roles.values() if not r["implemented"]]
    who = " · ".join(f"{r['role']}={r['agent']}({r['trust']}"
                     + (f", output={r['output_policy']}" if r["output_policy"] != "trusted" else "")
                     + ")" for r in impl)
    ctl = []
    for r in impl:
        if r["role"] == OPERATOR:
            ctl.append("operator→any")
            continue
        tgt = ",".join(r["may_control"]) or "nobody"
        ctl.append(f"{r['role']}→{tgt}")
    lines = ["[lilJack roles] " + who,
             "control: " + " · ".join(ctl) + " · anyone→operator"]
    if slots:
        lines.append("declared, NOT implemented: " + ", ".join(slots))
    if deferred:
        lines.append("audit NOT implemented: " + ", ".join(f"{k} ({v})" for k, v in deferred.items()))
    text = "\n".join(lines)
    return text if len(text) <= budget else text[:budget - 1].rstrip() + "…"


# ── CLI ──────────────────────────────────────────────────────────────────────

def main():
    import argparse
    import json
    ap = argparse.ArgumentParser(description="lilJack brain roles (BMD)")
    ap.add_argument("--path", default="")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show")
    sub.add_parser("digest")
    i = sub.add_parser("init"); i.add_argument("--force", action="store_true")
    m = sub.add_parser("may-control"); m.add_argument("frm"); m.add_argument("to")
    a = ap.parse_args()
    p = a.path or None
    if a.cmd == "show":
        print(json.dumps(load(p), indent=2, ensure_ascii=False))
    elif a.cmd == "digest":
        print(digest(p))
    elif a.cmd == "init":
        print(init(p, force=a.force))
    elif a.cmd == "may-control":
        print(may_control(a.frm, a.to, p))


if __name__ == "__main__":
    main()
