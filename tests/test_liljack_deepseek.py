#!/usr/bin/env python3
"""test_liljack_deepseek.py — DeepSeek as lilJack's third family.

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

⚠ NO NETWORK. Every call goes through an injected transport, so the suite is
deterministic, free, and runnable with the key absent. `LILJACK_PROVIDERS`
points at a temp provider file with a FAKE key, so a real credential is never
read, let alone reachable from a test artifact.
"""
import importlib
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SIBLING = ROOT.parent / "the trainer"
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(SIBLING / "scripts"))

TMP = Path(tempfile.mkdtemp(prefix="ljds-"))
FAKE_KEY = "sk-TESTONLY0000000000000000000000000"
PROV = TMP / "providers.json"
PROV.write_text(json.dumps({"deepseek_api_key": FAKE_KEY,
                            "deepseek_base_url": "https://api.example.invalid"}))
os.environ["LILJACK_PROVIDERS"] = str(PROV)
os.environ["LILJACK_CACHE"] = str(TMP / "cache")

import liljack_deepseek as D          # noqa: E402
import liljack_sessions as S          # noqa: E402

PASS = FAIL = 0


def check(cond, label, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}" + (f"  [{extra}]" if extra else ""))


# ── fake transports ──────────────────────────────────────────────────────
def reply(content="ok", reasoning="thinking…", finish="stop", pt=100, ct=20, rt=15,
          model="deepseek-v4-pro"):
    return json.dumps({
        "model": model,
        "choices": [{"finish_reason": finish,
                     "message": {"content": content, "reasoning_content": reasoning}}],
        "usage": {"prompt_tokens": pt, "completion_tokens": ct,
                  "completion_tokens_details": {"reasoning_tokens": rt}},
    }).encode()


class Tx:
    """Records every call so the suite can inspect what went on the wire."""

    def __init__(self, script):
        self.script = list(script)        # [(status, body_bytes) | Exception]
        self.calls = []

    def __call__(self, url, headers, body, timeout):
        self.calls.append({"url": url, "headers": headers,
                           "body": json.loads(body.decode()), "timeout": timeout})
        step = self.script.pop(0) if len(self.script) > 1 else self.script[0]
        if isinstance(step, Exception):
            raise step
        return step


def naps():
    out = []
    return out, (lambda s: out.append(s))


def code_mentions(path, name: str) -> bool:
    """Is `name` referenced by CODE in `path` (comments and strings ignored)?"""
    import io
    import tokenize
    src = Path(path).read_text(encoding="utf-8")
    for tok in tokenize.generate_tokens(io.StringIO(src).readline):
        if tok.type in (tokenize.COMMENT, tokenize.STRING):
            continue
        if name in tok.string:
            return True
    return False


def main():
    usage = TMP / "u.jsonl"

    # ═══ 1. credentials ═══════════════════════════════════════════════════
    key, base = D.load_credentials()
    check(key == FAKE_KEY and base == "https://api.example.invalid",
          "the key and base url are read from providers.json")

    bad = TMP / "missing.json"
    try:
        D.load_credentials(bad); ok = False
    except D.DeepSeekError as e:
        ok = "no provider file" in str(e)
    check(ok, "a missing provider file raises, never returns an empty key")

    empty = TMP / "empty.json"
    empty.write_text(json.dumps({"deepseek_api_key": ""}))
    try:
        D.load_credentials(empty); ok = False
    except D.DeepSeekError as e:
        ok = "empty" in str(e)
    check(ok, "an empty key raises rather than producing a 401 later")

    check(D.load_credentials(TMP / "nobase.json" if False else PROV)[1].endswith("invalid"),
          "base url has no trailing slash")

    # ═══ 2. the key never leaks ═══════════════════════════════════════════
    tx = Tx([(200, reply())])
    r = D.ask("hi", transport=tx, usage_path=usage, purpose="unit")
    check(tx.calls[0]["headers"]["Authorization"] == f"Bearer {FAKE_KEY}",
          "the key travels in the Authorization header (and only there)")
    check(FAKE_KEY not in json.dumps(tx.calls[0]["body"]),
          "the key is never in the request body")
    check(FAKE_KEY not in usage.read_text(), "the key is never in the usage log")
    row = json.loads(usage.read_text().splitlines()[0])
    check(set(row) == {"ts", "model", "prompt_tokens", "completion_tokens",
                       "reasoning_tokens", "seconds", "finish_reason", "purpose"},
          "the usage row carries counts and a purpose — no text, no key", row)
    check("hi" not in usage.read_text() and "thinking" not in usage.read_text(),
          "neither prompt nor completion text is written to the usage log")
    check(D.assert_no_exec() == [],
          "the module contains no execution primitive and imports none",
          D.assert_no_exec())
    check(FAKE_KEY not in D.redact(f"boom {FAKE_KEY} boom"),
          "redact() removes the literal key from any surfaced text")
    check("sk-" not in D.redact("leaked sk-abcdefghijklmnop"),
          "redact() also removes an unknown sk- shaped literal")

    # ⚠ Both of these are AST checks, not greps. A grep for "os.environ.copy()"
    # matched the COMMENT explaining why it is banned — a check that fails on
    # the prose promising it will pass is worse than no check.
    check("os.environ.copy" in D.BANNED_CALLS and "subprocess" in D.BANNED_IMPORTS,
          "copying the environment and spawning a process are both banned calls")
    planted = TMP / "planted.py"
    planted.write_text("import subprocess\n"
                       "def f(env):\n"
                       "    e = os.environ.copy()\n"
                       "    return eval('1+1')\n")
    found = D.assert_no_exec(planted)
    check(len(found) == 3 and any("subprocess" in f for f in found)
          and any("os.environ.copy" in f for f in found) and any("eval" in f for f in found),
          "…and the checker actually catches them when they are present", found)

    # ═══ 3. reasoning models: both fields, and truncation is loud ═════════
    check(r["content"] == "ok" and r["reasoning"] == "thinking…",
          "content and reasoning_content are returned SEPARATELY")
    check(r["text"] == r["content"], "`text` is an alias of content, never of reasoning")
    check(r["usage"]["reasoning_tokens"] == 15,
          "reasoning tokens are surfaced from completion_tokens_details")
    check(r["finish_reason"] == "stop" and isinstance(r["seconds"], float),
          "finish_reason and latency come back with the answer")
    check(D.DEFAULT_MODEL == "deepseek-v4-pro",
          "the default model is the strongest tier, not the cheap alias")
    check(D.MAX_TOKENS[D.PRO] == 8000 and D.MAX_TOKENS[D.FLASH] == 4000,
          "per-model budgets are in the thousands (measured, not guessed)")
    tx = Tx([(200, reply())])
    D.ask("hi", transport=tx, usage_path=usage)
    check(tx.calls[0]["body"]["max_tokens"] == 8000,
          "max_tokens=None resolves to this model's measured budget")
    tx = Tx([(200, reply(model="deepseek-v4-flash"))])
    D.ask("hi", model=D.FLASH, transport=tx, usage_path=usage)
    check(tx.calls[0]["body"]["max_tokens"] == 4000, "flash gets its own budget")

    u2 = TMP / "trunc.jsonl"
    tx = Tx([(200, reply(content="", finish="length", ct=2000, rt=2000))])
    try:
        D.ask("hi", transport=tx, usage_path=u2); ok = False; exc = None
    except D.DeepSeekTruncated as e:
        ok = True; exc = e
    check(ok, "empty content + finish_reason=length RAISES, never returns an answer")
    check(exc and exc.usage["reasoning_tokens"] == 2000 and exc.max_tokens == 8000,
          "the truncation carries the usage and the budget that was exhausted")
    check(u2.exists() and json.loads(u2.read_text().splitlines()[0])["completion_tokens"] == 2000,
          "a truncated call is still BILLED, so it is still recorded")
    check(len(tx.calls) == 1, "truncation is not retried by default (each retry is billed)")

    # a non-empty content with finish=length is a real (if clipped) answer
    tx = Tx([(200, reply(content="ok", finish="length"))])
    check(D.ask("hi", transport=tx, usage_path=usage)["content"] == "ok",
          "finish=length WITH content is an answer, not a budget failure")

    # opt-in escalation doubles the budget once, then succeeds
    tx = Tx([(200, reply(content="", finish="length")), (200, reply(content="yes"))])
    got = D.ask("hi", transport=tx, usage_path=usage, escalate=1)
    check(got["content"] == "yes", "escalate=1 retries at a larger budget")
    check([c["body"]["max_tokens"] for c in tx.calls] == [8000, 16000],
          "the escalation doubles, capped at TRUNCATION_CAP",
          [c["body"]["max_tokens"] for c in tx.calls])
    tx = Tx([(200, reply(content="", finish="length"))])
    try:
        D.ask("hi", transport=tx, usage_path=usage, escalate=1); ok = False
    except D.DeepSeekTruncated:
        ok = len(tx.calls) == 2
    check(ok, "an escalation that still truncates gives up loudly, bounded")

    # ═══ 4. error, timeout and rate-limit paths ═══════════════════════════
    tx = Tx([(400, b'{"error":{"message":"bad request"}}')])
    try:
        D.ask("hi", transport=tx, usage_path=usage); ok = False; e4 = None
    except D.DeepSeekError as e:
        ok = True; e4 = e
    check(ok and e4.status == 400 and "bad request" in e4.body,
          "an HTTP error carries the status AND the body")
    check(len(tx.calls) == 1, "a non-retryable 4xx is not retried")

    slept, nap = naps()
    tx = Tx([(429, b"slow down")])
    try:
        D.ask("hi", transport=tx, usage_path=usage, retries=3, sleep=nap); ok = False
    except D.DeepSeekError as e:
        ok = e.status == 429
    check(ok and len(tx.calls) == 3, "429 is retried up to `retries` times", len(tx.calls))
    check(slept == [1.0, 2.0], "backoff is bounded and exponential", slept)

    slept, nap = naps()
    tx = Tx([TimeoutError("timed out")])
    try:
        D.ask("hi", transport=tx, usage_path=usage, retries=2, sleep=nap); ok = False
    except D.DeepSeekError as e:
        ok = "TimeoutError" in str(e)
    check(ok and len(tx.calls) == 2, "a timeout is retried and then reported by name")

    tx = Tx([(429, b"x"), (200, reply(content="recovered"))])
    slept, nap = naps()
    check(D.ask("hi", transport=tx, usage_path=usage, sleep=nap)["content"] == "recovered",
          "a retry that succeeds returns the answer")

    tx = Tx([(200, b"not json at all")])
    try:
        D.ask("hi", transport=tx, usage_path=usage, retries=1); ok = False
    except D.DeepSeekError as e:
        ok = "not JSON" in str(e)
    check(ok, "a 200 with a non-JSON body is an error, not an empty answer")

    tx = Tx([(500, ("leak " + FAKE_KEY).encode())])
    try:
        D.ask("hi", transport=tx, usage_path=usage, retries=1)
    except D.DeepSeekError as e:
        check(FAKE_KEY not in str(e) and FAKE_KEY not in e.body,
              "an error body that echoes the key is redacted before it is raised")

    try:
        D.ask("   ", transport=Tx([(200, reply())])); ok = False
    except ValueError:
        ok = True
    check(ok, "an empty prompt is refused before a call is spent")

    # ═══ 5. usage accounting and spend() ══════════════════════════════════
    u3 = TMP / "spend.jsonl"
    D.record_usage("deepseek-v4-pro", 100, 200, "judge", path=u3, reasoning_tokens=180,
                   seconds=43.0, finish_reason="stop")
    D.record_usage("deepseek-v4-flash", 10, 5, "smoke", path=u3, reasoning_tokens=4,
                   seconds=2.0)
    sp = D.spend(path=u3)
    check(sp["calls"] == 2 and sp["prompt_tokens"] == 110 and sp["completion_tokens"] == 205,
          "spend() totals prompt and completion tokens", sp)
    check(sp["reasoning_tokens"] == 184 and sp["seconds"] == 45.0,
          "spend() totals reasoning tokens and latency separately")
    check(sp["usd"] is None and sp["priced"] is False,
          "usd is None when no rate is supplied — never a fabricated number")
    sp2 = D.spend(path=u3, price_in=0.5, price_out=2.0)
    check(abs(sp2["usd"] - (110 / 1e6 * 0.5 + 205 / 1e6 * 2.0)) < 1e-12,
          "a supplied rate prices the exact token counts", sp2["usd"])
    check(sp["per_model"]["deepseek-v4-pro"]["reasoning"] == 180
          and sp["per_purpose"]["judge"]["calls"] == 1,
          "spend attributes per model and per purpose")
    check(D.spend(path=TMP / "nope.jsonl")["calls"] == 0,
          "spend() on a missing log is zero, not an error")
    (TMP / "torn.jsonl").write_text('{"prompt_tokens": 5}\n{ this is torn\n')
    check(D.spend(path=TMP / "torn.jsonl")["calls"] == 1,
          "a torn line is skipped, never fatal")
    check(D.spend(path=u3, balance={"total": "49.99"})["balance"]["total"] == "49.99",
          "spend() carries an INJECTED balance; it never fetches one itself")

    # balance() parses the account endpoint through an injected transport
    def bal_tx(url, headers, timeout):
        check(url.endswith("/user/balance"), "balance uses GET /user/balance")
        check(headers["Authorization"] == f"Bearer {FAKE_KEY}", "balance sends the bearer")
        return 200, json.dumps({"is_available": True, "balance_infos": [
            {"currency": "USD", "total_balance": "49.99", "granted_balance": "49.99",
             "topped_up_balance": "0.00"}]}).encode()
    b = D.balance(transport=bal_tx)
    check(b["total"] == "49.99" and b["currency"] == "USD" and b["is_available"],
          "balance() returns the account total as ground truth", b)

    # ═══ 6. the reply is DATA, never executed ═════════════════════════════
    nasty = "__import__('os').system('touch /tmp/pwned_by_deepseek')"
    tx = Tx([(200, reply(content=nasty))])
    got = D.ask("hi", transport=tx, usage_path=usage)
    check(isinstance(got["content"], str) and got["content"] == nasty,
          "a reply containing code is returned verbatim as a STRING")
    check(not Path("/tmp/pwned_by_deepseek").exists(),
          "…and nothing about returning it executed it")

    # ═══ 7. no inherited environment ══════════════════════════════════════
    env = D.minimal_env(environ={"PATH": "/usr/bin", "HOME": "/h", "APOLLO_RING": "SECRET",
                                 "DEEPSEEK_API_KEY": FAKE_KEY, "CLAUDECODE": "1",
                                 "CODEX_HOME": "/c", "LANG": "C"})
    check(set(env) == {"PATH", "HOME", "LANG", "CUDA_VISIBLE_DEVICES"},
          "minimal_env is an ALLOWLIST — six names, not a filtered copy", sorted(env))
    check("APOLLO_RING" not in env and FAKE_KEY not in json.dumps(env),
          "the ring token and the provider key cannot reach a child")
    check("CLAUDECODE" not in env and "CODEX_HOME" not in env,
          "harness identity markers do not leak into a child")
    check(env["CUDA_VISIBLE_DEVICES"] == "", "the GPU is pinned off for any child")
    check(D.minimal_env(extra={"X": 1}, environ={})["X"] == "1",
          "explicit extras are allowed and stringified")
    try:
        D.minimal_env(extra={"A=B": "1"}, environ={}); ok = False
    except ValueError:
        ok = True
    check(ok, "a malformed env name is refused")

    # ═══ 8. the session registry: available, and NOT deliverable ══════════
    src = D.session_source()
    check(src["ok"] and src["rows"][0]["event"] == D.AVAILABLE_EVENT
          and src["rows"][0]["agent"] == "deepseek",
          "the deepseek source reports one row with event Available")
    st = S.collapse(src["rows"])
    check(st[0]["state"] == "available", "…which collapses to state `available`", st)
    check("available" in S.STATES and "available" not in S.DELIVERABLE_STATES,
          "`available` is a real state and is deliberately NOT deliverable")
    check(S.pick_target(st) is None,
          "the watcher will never try to inject a message into DeepSeek")
    check(S.pick_target(st + [{"state": "idle", "seq": 9, "agent": "codex"}])["agent"] == "codex",
          "a real idle session is still chosen when one exists")
    D.register()
    check("deepseek" in S.SOURCES, "register() adds the source to the registry")
    snap = S.snapshot(sources=["deepseek"])
    check(snap["sessions"] and snap["sessions"][0]["state"] == "available"
          and not snap["errors"], "snapshot() sees deepseek through the registry", snap)
    miss = D.session_source(providers=TMP / "nope.json")
    check(not miss["ok"] and "no provider file" in miss["error"],
          "no credential is REPORTED as an error, not silently absent")
    check(S.agents_live({"sessions": [dict(st[0], agent="deepseek")]})["deepseek"]["state"]
          == "available", "agents_live ranks an available agent as reachable")

    # ⚠ the autoload is GENERIC: sessions.py must never import deepseek by name.
    # Checked on CODE tokens with comments and strings stripped — a plain grep
    # flagged the comment that explains the rule, which is the third time in
    # this session a text scan has matched its own prose.
    check(not code_mentions(ROOT / "toolbox" / "liljack_sessions.py", "liljack_deepseek"),
          "liljack_sessions has no code reference to DeepSeek — the extension point holds")
    check(code_mentions(ROOT / "toolbox" / "liljack_deepseek.py", "liljack_sessions"),
          "…while the dependency runs the other way, as it must")
    check(S.autoload(spec="liljack_deepseek") == {} and "deepseek" in S.SOURCES,
          "autoload imports a named module and lets it register itself")
    errs = S.autoload(spec="liljack_no_such_module_xyz")
    check("liljack_no_such_module_xyz" in errs and "Error" in errs["liljack_no_such_module_xyz"],
          "a source module that cannot be imported is REPORTED, never raised", errs)
    check(S.autoload(spec="liljack_no_such_module_xyz") == errs,
          "…and is attempted once per process, not once per tick")
    check(S.autoload(spec="") == {}, "autoload with nothing configured is a no-op")
    # codex behaviour is untouched
    check(S.state_of("Stop", 999) == "idle" and S.state_of("SessionEnd", 999) == "ended"
          and S.state_of("PreToolUse", 5) == "busy",
          "the existing codex state mapping is unchanged")

    # ═══ 9. the family vocabulary ═════════════════════════════════════════
    import trio_judge as J
    import build_trio_core as B
    sch = J.load_schema()
    check("deepseek" in J.FAMILIES, "trio_judge.FAMILIES contains deepseek")
    check("deepseek" in sch["by"]["families"], "schema.json lists deepseek as a family")
    check(J.by_ok("deepseek-school-a", sch), "a deepseek-* `by` name passes the pattern")
    check(J.family_of("deepseek-school-a") == "deepseek", "family_of resolves it")
    check(B.model_family("deepseek-school-a") == "deepseek",
          "build_trio_core.model_family agrees — one vocabulary, three files")
    check(not J.by_ok("mistral-school-a", sch),
          "a genuinely unknown family is still refused by the pattern")
    check(not J.by_ok("deepseek", sch) and not J.by_ok("deepseek-", sch),
          "a bare family name is not a valid `by`")
    # every pre-existing family still behaves identically
    check(all(J.by_ok(f"{f}-t-a", sch) for f in
              ("human", "operator", "claude", "codex", "gpt", "gemini", "qwen")),
          "every pre-existing family still validates")
    check(J.family_of("operator-school-en") == "human" and J.family_of("claude-qa-k0") == "claude"
          and B.model_family("codex-d2-judge-b") == "codex",
          "existing family resolution is byte-for-byte unchanged")
    check(J.family_of("mistral-x") == "mistral",
          "an unknown head still reports itself, never 'deepseek'")

    # ═══ 10. the judge bridge treats the reply as untrusted input ═════════
    import trio_judge_deepseek as JD
    units = [{"id": "u1", "hash": "0" * 16, "shard": "uk", "text": "hello",
              "sample": {"text": "hello", "language": "uk"}},
             {"id": "u2", "hash": "1" * 16, "shard": "uk", "text": "world",
              "sample": {"text": "world", "language": "uk"}}]
    good = json.dumps([{"id": "u1", "verdict": "ok", "issues": []},
                       {"id": "u2", "verdict": "label", "issues": ["short"],
                        "note": "n" * 400}])
    rows, rej = JD.parse_verdicts(good, units, "deepseek-school-a")
    check(len(rows) == 2 and not rej, "a well-formed batch reply parses to rows", rej)
    check(all(r["by"] == "deepseek-school-a" for r in rows),
          "`by` is forced from OUR argument, never taken from the reply")
    check(rows[0]["hash"] == "0" * 16 and rows[1]["hash"] == "1" * 16,
          "`hash` is re-derived from our unit — a reply cannot claim other text")
    check(len(rows[1]["note"]) == 160, "an over-long note is clamped, not rejected")

    spoof = json.dumps([{"id": "u1", "hash": "f" * 16, "verdict": "ok", "issues": [],
                         "by": "claude-school-a", "extra": "ignored"}])
    rows, rej = JD.parse_verdicts(spoof, units, "deepseek-school-a")
    check(rows[0]["hash"] == "0" * 16 and rows[0]["by"] == "deepseek-school-a",
          "a reply that spoofs hash and family is overwritten with the truth")
    check("extra" not in rows[0], "unknown fields in the reply are dropped")

    rows, rej = JD.parse_verdicts(json.dumps([{"id": "nope", "verdict": "ok"}]),
                                  units, "deepseek-school-a")
    check(not rows and any("not in this batch" in r[2] for r in rej),
          "a verdict on an id we never sent is REJECTED, never created", rej)
    check(sum(1 for r in rej if "no verdict returned" in r[2]) == 2,
          "units the model skipped are reported as missing")

    dupe = json.dumps([{"id": "u1", "verdict": "ok"}, {"id": "u1", "verdict": "exclude"}])
    rows, rej = JD.parse_verdicts(dupe, units, "deepseek-school-a")
    check(len(rows) == 1 and any("duplicate" in r[2] for r in rej),
          "two verdicts for one unit: the second is rejected, not silently applied")

    fenced = "here you go:\n```json\n" + good + "\n```\nhope that helps"
    check(len(JD.parse_verdicts(fenced, units, "deepseek-school-a")[0]) == 2,
          "a fenced reply with prose around it is unwrapped")
    try:
        JD.parse_verdicts("I refuse to judge this.", units, "deepseek-school-a"); ok = False
    except ValueError:
        ok = True
    check(ok, "a reply with no JSON is refused WHOLE, never partially salvaged")
    try:
        JD.parse_verdicts("[{bad json]", units, "deepseek-school-a"); ok = False
    except ValueError:
        ok = True
    check(ok, "malformed JSON is refused rather than hand-repaired")
    check(JD.extract_json('{"verdicts": [{"id": "u1"}]}') == [{"id": "u1"}],
          "a {verdicts: [...]} wrapper is accepted")

    # ⚠ the batch budget must grow with the batch — measured: 4 units at 8000
    # burned the whole budget on reasoning and returned nothing.
    check(JD.budget_for("deepseek-v4-pro", 1) == 8000,
          "one unit gets the model's own floor")
    check(JD.budget_for("deepseek-v4-pro", 4) == 12000,
          "four units get 3000/unit, above the floor", JD.budget_for("deepseek-v4-pro", 4))
    check(JD.budget_for("deepseek-v4-flash", 1) == 4000
          and JD.budget_for("deepseek-v4-flash", 10) == 30000,
          "the floor is per model and the per-unit term wins once it is bigger")
    check(JD.budget_for("deepseek-v4-pro", 0) >= 8000, "a zero batch never asks for zero")

    sp_ = JD.system_prompt("school", sch)
    check("exclude" in sp_ and "frontmatter" in sp_ and "kindergarten" in sp_,
          "the system prompt reads its vocabulary out of schema.json")
    check("mistranslation" not in sp_,
          "…and carries only the enums of the kind being judged")

    # end to end through append(), on a TEMP store — never a live one
    store = TMP / "verdicts.deepseek-school-a.jsonl"
    called = {}

    def fake_ask(prompt, **kw):
        called.update(kw)
        called["prompt"] = prompt
        return {"text": json.dumps([{"id": "u1", "verdict": "ok", "issues": []}]),
                "usage": {"prompt_tokens": 900, "completion_tokens": 300,
                          "reasoning_tokens": 280}, "seconds": 41.2,
                "model": "deepseek-v4-pro", "finish_reason": "stop"}

    rep = JD.judge("school", units[:1], "deepseek-school-a", ask=fake_ask, schema=sch)
    check(rep["rows"] == [{"id": "u1", "hash": "0" * 16, "by": "deepseek-school-a",
                           "verdict": "ok", "issues": []}],
          "judge() produces exactly the row append() expects", rep["rows"])
    check(called["model"] == "deepseek-v4-pro" and called["purpose"].startswith("judge:"),
          "the pass runs on the strong model and tags its purpose for spend()")
    check("hello" in called["prompt"] and "u1" in called["prompt"],
          "the rendered unit and its id are in the prompt")

    ap = J.append("school", "deepseek-school-a", rep["rows"], schema=sch, units=units,
                  store=store)
    check(ap["counts"]["added"] == 1 and not ap["rejected"],
          "append() accepts a deepseek verdict into a temp store", ap)
    check(ap["family"] == "deepseek", "…and records it under the deepseek family")
    check(json.loads(store.read_text().strip())["by"] == "deepseek-school-a",
          "the store holds the verdict verbatim")
    # the hash gate still bites
    ap2 = J.append("school", "deepseek-school-a",
                   [{"id": "u1", "hash": "e" * 16, "verdict": "ok", "issues": []}],
                   schema=sch, units=units, store=TMP / "v2.jsonl")
    check(ap2["counts"].get("added", 0) == 0 and ap2["rejected"],
          "a verdict whose hash does not match the sampled text is still refused")
    try:
        J.append("school", "mistral-school-a", rep["rows"], schema=sch, units=units,
                 store=TMP / "v3.jsonl"); ok = False
    except SystemExit:
        ok = True
    check(ok, "append() still refuses a genuinely unknown family outright")

    check(not (TMP / "v3.jsonl").exists(), "a refused append writes nothing")

    # ═══ 11. the task roster ══════════════════════════════════════════════
    # deepseek is now a PERMANENT member of liljack_mail.AGENTS (2026-09-06), so
    # the env-var extension is tested with a genuine non-member ("chad"). The
    # invariant is unchanged: the mail roster leads, an env agent appends after
    # it, and a model may never assign.
    import liljack_mail as mail
    os.environ["LILJACK_AGENTS"] = "chad"
    import liljack_tasks
    T = importlib.reload(liljack_tasks)
    check("chad" in T.AGENTS and "chad" in T.ACTORS,
          "LILJACK_AGENTS=chad makes it a legal owner — no code change needed")
    check(T.AGENTS[:len(mail.AGENTS)] == mail.AGENTS,
          "the fixed mail roster is unchanged and still first")
    check("deepseek" in T.AGENTS, "deepseek is a permanent roster member now")
    check("chad" not in T.ASSIGNERS, "a model may not assign work to anyone")
    del os.environ["LILJACK_AGENTS"]
    T = importlib.reload(liljack_tasks)
    check("chad" not in T.AGENTS,
          "without the env var the roster is exactly the mail roster")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
