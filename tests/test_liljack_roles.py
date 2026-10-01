#!/usr/bin/env python3
"""test_liljack_roles.py — the brain roles model (toolbox/liljack_roles.py).

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

What is asserted: the file round-trips through the trainer's real bmd.py; claims
sit in the document footer only (and the pasem trap provably yields zero);
every explicit `neg` the operator's brief requires is present and reads back; the
`may_control` matrix is exactly as specified, refusals included; unknown
names are refused, never answered "no"; concurrent writers lose nothing; and
the declared-not-implemented slots are visible in the digest as such.
"""
import os
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
os.environ["CUDA_VISIBLE_DEVICES"] = ""
import liljack_roles as R          # noqa: E402

PASS = FAIL = 0


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def tmp(name="roles") -> Path:
    return Path(tempfile.mkdtemp(prefix="ljroles-")) / f"{name}.bmd"


# The contract, verbatim from the brief. Rows = from, cols = to.
IMPL = ("operator", "main", "second", "third-opinion")
MATRIX = {
    "operator":      {"operator": False, "main": True,  "second": True,  "third-opinion": True},
    "main":          {"operator": True,  "main": False, "second": True,  "third-opinion": True},
    "second":        {"operator": True,  "main": True,  "second": False, "third-opinion": False},
    "third-opinion": {"operator": True,  "main": False, "second": False, "third-opinion": False},
}
AGENT = {"operator": "operator", "main": "claude", "second": "codex", "third-opinion": "deepseek"}


def main():
    B = R.board.bmd()
    # Redirect the DEFAULT path so a forgotten path= can never touch the live file.
    live = R.default_path()
    os.environ["LILJACK_ROLES"] = str(tmp("default"))
    check(R.default_path() != live, "LILJACK_ROLES redirects the default path")

    # ── seed + round-trip through the real parser ──────────────────────
    p = tmp()
    R.init(p)
    text = p.read_text()
    doc = B.parse(text, strict=True)
    check(len(doc.assertions) > 0, f"init writes a strict-parsing BMD ({len(doc.assertions)} claims)")
    check(sum(len(ps.assertions) for ps in doc.pasems) == 0,
          "claims live in the DOCUMENT footer — none inside a pasem")
    check(all(len(a.args) <= 2 for a in doc.assertions),
          "no fact/neg carries more than two args after the predicate")
    check(B.parse(B.parse(text).render(), strict=True).render() == B.parse(text).render(),
          "parse→render→parse is a fixed point (round-trip)")
    roles = R.load(p)
    check(set(roles) == {"main", "second", "third-opinion", "operator",
                         "tokenizer", "preprocessor"},
          "the six seed roles read back")
    check(R.load(None) == R._seed_roles(), "load() with no file returns the seed model")
    ag = R.by_agent(p)
    check(set(ag) == {"claude", "codex", "deepseek", "operator"},
          "by_agent() keys are the four agents; slots have none")

    # the pasem trap: a claim written inside a pasem is prose, yields ZERO
    trap = B.parse("[[pasem: x]]\nfact(role, main, claude)\n[[/pasem]]\n")
    check(len(trap.all_assertions()) == 0 and "fact(" in trap.pasem("x").text,
          "PROVEN: a claim inside a pasem parses as prose — zero assertions")

    # the prose pasem is derived from the claims
    ps = doc.pasem("brain-roles").text
    check("GENERATED" in ps and "third-opinion: deepseek" in ps
          and "tokenizer: DECLARED, NOT IMPLEMENTED" in ps,
          "the pasem is regenerated prose that restates the claims")

    # ── every required neg is present and readable ─────────────────────
    negs = {(a.pred, tuple(a.args)): a.attrs for a in doc.assertions if a.form == "neg"}
    check(("role-implemented", ("tokenizer",)) in negs,
          "neg(role-implemented, tokenizer) present")
    check(("role-implemented", ("preprocessor",)) in negs,
          "neg(role-implemented, preprocessor) present")
    check("reason" in negs.get(("role-implemented", ("tokenizer",)), {}),
          "…and it carries a reason")
    a_def = negs.get(("audit-implemented", ("third-opinion",)), {})
    check(a_def.get("reason") == "deferred by the operator 2026-09-05",
          "neg(audit-implemented, third-opinion | reason=deferred by the operator 2026-09-05)")
    check(("role-may", ("third-opinion", "execute")) in negs,
          "neg(role-may, third-opinion, execute) — the untrusted role may NOT execute")
    check(("role-may", ("third-opinion", "write-repo")) in negs
          and ("role-may", ("third-opinion", "assign")) in negs
          and ("role-may", ("third-opinion", "network")) in negs,
          "third-opinion also explicitly may NOT assign / write-repo / network")
    check(("role-may", ("second", "assign")) in negs and ("role-may", ("second", "network")) in negs,
          "second explicitly may NOT assign or network")
    check(("role-controls", ("third-opinion", "main")) in negs
          and ("role-controls", ("third-opinion", "second")) in negs
          and ("role-controls", ("second", "third-opinion")) in negs,
          "every refused handover among implemented roles is an explicit neg")
    facts = {(a.pred, tuple(a.args)) for a in doc.assertions if a.form == "fact"}
    check(all(("role-implemented", (r,)) in facts for r in IMPL),
          "the four implemented roles carry a POSITIVE role-implemented fact")
    # no slot claims anything positive
    check(not any(k[1] and k[1][0] in ("tokenizer", "preprocessor")
                  for k in facts if k[0] in ("role-may", "role-controls", "role-implemented")),
          "slots claim no capability, control or implementation")
    # every (role, capability) pair is decided one way or the other
    for r in IMPL:
        decided = all((("role-may", (r, c)) in facts) != (("role-may", (r, c)) in negs)
                      for c in R.CAPABILITIES)
        check(decided, f"{r}: every vocabulary capability is either fact or neg, never both/neither")

    # ── the may_control matrix, exactly ────────────────────────────────
    for frm in IMPL:
        for to in IMPL:
            want = MATRIX[frm][to]
            got_roles = R.may_control(frm, to, p)
            got_agents = R.may_control(AGENT[frm], AGENT[to], p)
            check(got_roles == want and got_agents == want,
                  f"may_control({frm}→{to}) == {want} (by role and by agent)")
    check(not R.may_control("main", "tokenizer", p) and not R.may_control("operator", "preprocessor", p),
          "control cannot be handed to an unimplemented slot, not even by the operator")

    # ── capabilities + policies ────────────────────────────────────────
    check(R.output_policy("deepseek", p) == "data-only", "third-opinion output_policy is data-only")
    check(R.output_policy("third-opinion", p) == "data-only", "…by role name too")
    check(R.output_policy("codex", p) == "sandboxed" and R.output_policy("claude", p) == "trusted"
          and R.output_policy("operator", p) == "trusted", "second=sandboxed, main/operator=trusted")
    check(R.may("claude", "hold-control", p) and R.may("main", "assign", p),
          "main may hold-control and assign")
    check(R.may("codex", "execute", p) and not R.may("codex", "assign", p),
          "second may execute, may not assign")
    check(not R.may("deepseek", "execute", p) and R.may("deepseek", "judge", p)
          and R.may("deepseek", "review", p) and not R.may("deepseek", "write-repo", p),
          "third-opinion judges/reviews, never executes or writes")
    check(all(R.may("operator", c, p) for c in R.CAPABILITIES), "operator may everything")
    check(not R.may("tokenizer", "read-repo", p), "an unimplemented slot may nothing")
    check(R.role_of("codex", p) == "second" and R.role_of("operator", p) == "operator",
          "role_of maps agent → role")

    # ── refusals ───────────────────────────────────────────────────────
    def raises(fn, *a, **kw):
        try:
            fn(*a, **kw)
            return False
        except ValueError:
            return True
    check(raises(R.role_of, "gpt4", p), "unknown agent is refused")
    check(raises(R.may, "nobody", "execute", p), "may(): unknown agent/role is refused, not answered False")
    check(raises(R.may, "claude", "fly", p), "may(): unknown capability is refused")
    check(raises(R.may_control, "claude", "gpt4", p) and raises(R.may_control, "gpt4", "claude", p),
          "may_control(): unknown agent on either side is refused")
    check(raises(R.output_policy, "gpt4", p), "output_policy(): unknown agent is refused")
    check(raises(R.set_role, "ghost", p, trust="trusted"), "set_role on an unknown role is refused")
    check(raises(R.set_role, "main", p, trust="godlike"), "set_role with an illegal trust is refused")
    check(raises(R.set_role, "main", p, capabilities=["fly"]), "set_role with an unknown capability is refused")
    check(raises(R.set_role, "main", p, colour="red"), "set_role with an unknown field is refused")
    check(raises(R.add_role, "main", agent="x", path=p), "add_role on an existing name is refused")
    check(raises(R.add_role, "bad|name", path=p), "a role name with a BMD separator is refused")
    check(raises(R.set_role, "tokenizer", p, agent="foo"),
          "an unimplemented slot cannot be given an agent without implementing it")

    # ── writes round-trip and negs follow the struct ───────────────────
    r = R.set_role("second", p, capabilities=["hold-control", "execute", "review", "read-repo"])
    check("write-repo" not in r["capabilities"] and "review" in r["capabilities"],
          "set_role changes a capability set and returns the re-read role")
    doc2 = B.parse(p.read_text(), strict=True)
    negs2 = {(a.pred, tuple(a.args)) for a in doc2.assertions if a.form == "neg"}
    check(("role-may", ("second", "write-repo")) in negs2,
          "a dropped capability becomes an explicit neg on the next write")
    check("write-repo" not in doc2.pasem("brain-roles").text.split("second:")[1].split("\n")[1],
          "the prose pasem follows the claims")
    r = R.set_role("second", p, note="quoted, with | pipe and 'apostrophe' (and brackets)")
    check(r["note"] == "quoted, with | pipe and 'apostrophe' (and brackets)",
          "an attribute with every separator round-trips through qattr")
    check(R.read(p)["errors"] == 0, "…and the file still parses strictly")
    # audit field + hook
    check(R.audit_hook("third-opinion", p) is None, "audit_hook is None by default (audit=none)")
    R.set_role("third-opinion", p, audit="slant-check")
    check(R.audit_hook("deepseek", p) is None, "an audit name nobody registered still yields None")
    R.register_audit("slant-check", lambda text: text)
    check(callable(R.audit_hook("deepseek", p)), "a registered audit is returned as a callable")
    check(R.read(p)["audit_deferred"].get("third-opinion") == "deferred by the operator 2026-09-05",
          "the audit-deferred neg survives set_role writes")
    # add a role
    r = R.add_role("scribe", agent="ada", trust="sandboxed",
                   capabilities=["hold-control", "read-repo", "review"],
                   may_control=["main"], output_policy="sandboxed", path=p)
    check(R.role_of("ada", p) == "scribe" and R.may("ada", "review", p) and not R.may("ada", "execute", p),
          "add_role: a new agent/role is queryable")
    check(R.may_control("ada", "main", p) and not R.may_control("ada", "second", p)
          and R.may_control("ada", "operator", p) and R.may_control("operator", "ada", p)
          and not R.may_control("claude", "ada", p),
          "add_role: the new role's control edges follow its declaration, operator both ways")
    R.set_role("scribe", p, capabilities=["read-repo", "review"])
    check(not R.may_control("operator", "ada", p) and R.may_control("ada", "main", p),
          "a role without hold-control cannot RECEIVE control, even from the operator")
    doc3 = B.parse(p.read_text(), strict=True)
    negs3 = {(a.pred, tuple(a.args)) for a in doc3.assertions if a.form == "neg"}
    check(("role-controls", ("main", "scribe")) in negs3 and ("role-controls", ("scribe", "second")) in negs3,
          "the new role's refused handovers are explicit negs both ways")
    # add a declared slot
    R.add_role("embedder", implemented=False, reason="slot only", path=p)
    check(not R.load(p)["embedder"]["implemented"] and "embedder" in R.digest(p),
          "a new declared slot is visible in the digest")
    check(raises(R.add_role, "slotless", implemented=False, path=p),
          "a declared slot without a reason is refused")

    # ── digest ─────────────────────────────────────────────────────────
    R.init(p, force=True)
    d = R.digest(p)
    check(len(d) <= R.DIGEST_BUDGET, f"digest fits the budget ({len(d)} ≤ {R.DIGEST_BUDGET})")
    check("main=claude(trusted)" in d and "third-opinion=deepseek(untrusted, output=data-only)" in d,
          "digest names who is what and what trust")
    check("second=codex(sandboxed" in d and "operator=operator(owner)" in d, "…for all four")
    check("third-opinion→nobody" in d and "second→main" in d and "operator→any" in d
          and "anyone→operator" in d, "digest states the control rules")
    check("NOT implemented: tokenizer, preprocessor" in d,
          "declared-not-implemented slots are visible in the digest AS SUCH")
    check("audit NOT implemented: third-opinion" in d, "the deferred audit is visible in the digest")
    d0 = R.digest(tmp("nofile"))
    check(len(d0) <= R.DIGEST_BUDGET and "main=claude" in d0, "digest works off the seed with no file")

    # ── concurrency: N writers, one reader, nothing lost ──────────────
    conc = tmp("conc")
    R.init(conc)
    errs, seen_bad = [], []
    fields = {"main": "note", "second": "note", "third-opinion": "note", "operator": "note"}

    def writer(role, n):
        for i in range(n):
            try:
                R.set_role(role, conc, note=f"{role} write {i}")
            except Exception as e:
                errs.append(repr(e))

    def adder(n):
        for i in range(n):
            try:
                R.add_role(f"extra-{i}", agent=f"agent{i}", trust="sandboxed",
                           capabilities=["review"], output_policy="sandboxed", path=conc)
            except Exception as e:
                errs.append(repr(e))

    def reader(stop):
        while not stop.is_set():
            try:
                if R.read(conc)["errors"]:
                    seen_bad.append("malformed")
            except Exception as e:
                seen_bad.append(repr(e))

    stop = threading.Event()
    rd = threading.Thread(target=reader, args=(stop,), daemon=True)
    rd.start()
    ts = [threading.Thread(target=writer, args=(r, 20)) for r in fields] + \
         [threading.Thread(target=adder, args=(15,))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    stop.set()
    rd.join(timeout=5)
    final = R.load(conc)
    check(not errs, f"concurrent writers raise nothing ({errs[:2]})")
    check(len(final) == 6 + 15, f"no role is lost under concurrency ({len(final)}/21)")
    check(all(final[r]["note"].startswith(f"{r} write") for r in fields),
          "each role's last write landed")
    check(final["third-opinion"]["output_policy"] == "data-only"
          and final["main"]["capabilities"] == sorted(R.CAPABILITIES),
          "fields not written keep their value under interleaving")
    check(not seen_bad, f"a concurrent reader never saw a torn file ({seen_bad[:2]})")
    check(B.parse(conc.read_text(), strict=True) is not None,
          "the file parses strictly after 95 interleaved rewrites")
    check(not list(conc.parent.glob("*.tmp")) and not list(conc.parent.glob("*.lock")),
          "no .tmp or .lock left behind")
    lock = Path(str(conc) + ".lock")
    lock.write_text("99999999\n")
    os.utime(lock, (0, 0))
    R.set_role("main", conc, note="after stale lock")
    check(R.load(conc)["main"]["note"] == "after stale lock", "a stale lock is stolen, not waited on")

    # ── the live file, if present ──────────────────────────────────────
    if live.exists():
        st = R.read(live)
        check(st["errors"] == 0 and st["stray"] == 0, "the live roles file parses strictly, no stray claims")
        lv = R.load(live)
        ok = all(R.may_control(f, t, live) == MATRIX[f][t] for f in IMPL for t in IMPL)
        check(ok, "the live file encodes the contract matrix")
        check(lv["third-opinion"]["output_policy"] == "data-only", "live third-opinion is data-only")
        check(len(R.digest(live)) <= R.DIGEST_BUDGET, "the live digest fits the budget")

    # ── negative control: a wrong expectation must FAIL ────────────────
    # Planted: "third-opinion may hand control to main". The model must say no.
    planted = R.may_control("third-opinion", "main", p)
    check(planted is False, "NEGATIVE CONTROL: asserting third-opinion→main would fail (it is False)")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
