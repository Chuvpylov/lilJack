#!/usr/bin/env python3
"""
liljack_deepseek.py — DeepSeek over the HTTP API, as lilJack's third family.

⚠ READ THIS FIRST — WHAT DEEPSEEK IS HERE.
DeepSeek is reached over an OpenAI-compatible `/chat/completions` endpoint. It
is a MODEL, not an agent: it has no filesystem, no shell, no MCP, no session
and no tools. Every piece of machinery this repo built for *agents* — sandbox
profiles, worktrees, tool allowlists, a dedicated unix user — answers a threat
("the agent holds a shell") that does not exist on this path. Building it here
would be theatre. See the report and `SKIPPED` below.

The two things that DO carry over, and are implemented:

  (a) NEVER INHERIT THE ENVIRONMENT. `minimal_env()` builds an explicit
      allowlist env for any subprocess. This module spawns none — but the
      helper is here so the next caller does not reach for `os.environ.copy()`,
      which would hand a child the ring token, the provider key and every
      CLAUDE_*/CODEX_* marker the harness keys identity off.

  (b) THE MODEL'S OUTPUT IS UNTRUSTED INPUT. It is returned as a `str` and
      nothing more. It is never eval'd, exec'd, shelled out, imported, or
      written into a store something else executes. It is data to be JUDGED,
      exactly like corpus text — the caller parses it strictly and re-derives
      every identifying field (id, hash, by) from OUR record, never from the
      reply. `assert_no_exec()` makes that a testable property of this file.

⚠ THE KEY. `demo/.apollo/providers.json`, mode 0600, registered in
`the trainer/corpus/credentials.py::CREDENTIAL_FILES` so the scrubber knows the
literal and the ingester refuses the file. It is read at CALL TIME and:
  · never passed on a command line (argv is world-readable in /proc and the
    Codex lifecycle DB records command lines),
  · never written to the usage log, an exception message, or a report,
  · never placed in an environment variable this module constructs.
`--echo` prints a completion to the local terminal only, and prints nothing to
any archive.

⚠ THESE ARE REASONING MODELS, AND THAT IS THE TRAP. The response carries
`message.reasoning_content` SEPARATELY from `message.content`, and the thinking
is billed as completion tokens (`usage.completion_tokens_details.
reasoning_tokens`). Measured 2026-09-05 on a ONE-WORD judging probe:
`deepseek-v4-pro` at `max_tokens=300` AND at `2000` returned **empty content**
with `finish_reason: "length"` — every token went to reasoning; it only
answered at `max_tokens=8000` (2,243 reasoning tokens, 43 s). `deepseek-v4-flash`
failed the same way at 200. Consequences, all implemented here:
  · `ask` returns `content`, `reasoning` AND `finish_reason` — never content alone.
  · `finish_reason == "length"` with empty content is a BUDGET FAILURE, raised
    as `DeepSeekTruncated`, never returned as an answer. A judge that silently
    yields "" at scale is worse than one that fails loudly. The spent tokens are
    still recorded — we paid for them.
  · `MAX_TOKENS` is per model and in the THOUSANDS (pro 8000, flash 4000).
  · 43 s for a trivial call means a 600-unit pass is HOURS. Batch units per
    request where the protocol allows; `seconds` is in every usage row.

COST. Every call appends one line to `~/.cache/liljack/deepseek_usage.jsonl`:
`{ts, model, prompt_tokens, completion_tokens, reasoning_tokens, seconds,
finish_reason, purpose}` — counts and a purpose tag, never prompt or completion
TEXT. `spend()` totals it so the budget is observed rather than guessed, and
`balance()` reads DeepSeek's own `/user/balance` as GROUND TRUTH. ⚠ The balance
is coarse (it moved $50.00 → $49.99 over ~6 probes), so it bounds the spend
while the local log attributes it.

⚠ USD IS UNPRICED BY DEFAULT, DELIBERATELY. `PRICES` is empty and `spend()`
returns `usd: None` unless a rate is supplied (`--price-in/--price-out`, or
`LILJACK_DEEPSEEK_PRICE_IN`/`_OUT`, USD per 1M tokens). A fabricated price is
worse than no price: it would read as measured. Tokens are always exact.

Stdlib only (`urllib`), like every other module in this toolbox.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
PROVIDERS = Path(os.environ.get("LILJACK_PROVIDERS", str(PROJECT_ROOT / ".apollo" / "providers.json")))
CACHE = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))
USAGE = CACHE / "deepseek_usage.jsonl"

# ⚠ `GET /models` on 2026-09-05: deepseek-v4-flash · deepseek-v4-pro ·
# deepseek-v4-flash-vision-exp. `deepseek-chat` is an ALIAS for flash — it is
# accepted here but never the default, because a name that silently means "the
# cheap tier" is exactly the kind of thing that gets a judging pass run on the
# wrong model. the operator wants the strongest for coding/reasoning/judging.
PRO = "deepseek-v4-pro"
FLASH = "deepseek-v4-flash"
DEFAULT_MODEL = PRO

# ⚠ Per model, in the thousands, and MEASURED — not a guess. See the docstring.
MAX_TOKENS = {PRO: 8000, FLASH: 4000, "deepseek-chat": 4000}
FALLBACK_MAX_TOKENS = 4000
DEFAULT_TIMEOUT = 180                # 43 s measured for a one-word answer
DEFAULT_RETRIES = 3
RETRY_STATUS = (408, 409, 425, 429, 500, 502, 503, 504)
TRUNCATION_CAP = 16000               # ceiling for an opt-in budget escalation
BACKOFF_BASE = 1.0            # seconds; doubled per attempt, capped
BACKOFF_CAP = 20.0
BODY_SNIPPET = 400            # of an error body, kept for diagnosis

# ⚠ EMPTY ON PURPOSE — see the module docstring. USD per 1,000,000 tokens.
PRICES: dict = {}

# Parts of the doc written for Claude-Code-as-agent that this file deliberately
# does NOT implement, with the reason. Kept in code because a skipped control
# that is only mentioned in a report gets re-litigated every quarter.
SKIPPED = {
    "bubblewrap sandbox": "no process is spawned; there is nothing to confine",
    "dedicated unix user": "same — the API call runs in-process as the caller",
    "ephemeral git worktree": "the model cannot see, let alone write, a filesystem",
    "tool allowlist / MCP gating": "the model is given no tools; the reply is text",
    "network egress policy": "the ONE egress is this endpoint, in this file",
}


class DeepSeekTruncated(RuntimeError):
    """The model spent its whole budget thinking and said nothing.

    ⚠ A SEPARATE class because it is a separate fact: the call SUCCEEDED (200,
    tokens billed) and produced no answer. Returning "" here would put an empty
    verdict in a judge store and it would look like a judgement.
    """

    def __init__(self, msg: str, usage=None, reasoning: str = "", model: str = "",
                 max_tokens: int = 0):
        super().__init__(msg)
        self.usage = usage or {}
        self.reasoning = reasoning
        self.model = model
        self.max_tokens = max_tokens


class DeepSeekError(RuntimeError):
    """A call that could not be completed. Carries `status` when HTTP said so."""

    def __init__(self, msg: str, status: int | None = None, body: str = ""):
        super().__init__(msg)
        self.status = status
        self.body = body


# ══════════════════════════════════════════════════════════════════════════
# credentials — read at call time, never held longer than the call
# ══════════════════════════════════════════════════════════════════════════

def load_credentials(path=None) -> tuple:
    """(api_key, base_url) from providers.json.

    ⚠ Raises rather than returning a partial pair: a call with an empty key
    fails at the endpoint with a 401 whose body is less informative than this.
    """
    p = Path(path or PROVIDERS)
    if not p.exists():
        raise DeepSeekError(f"no provider file at {p} (expected keys "
                            f"deepseek_api_key, deepseek_base_url)")
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except Exception as exc:
        raise DeepSeekError(f"{p}: unreadable provider file: {type(exc).__name__}")
    key = (d.get("deepseek_api_key") or "").strip()
    base = (d.get("deepseek_base_url") or "https://api.deepseek.com").strip().rstrip("/")
    if not key:
        raise DeepSeekError(f"{p}: deepseek_api_key is empty")
    return key, base


def redact(text: str, key: str = "") -> str:
    """Remove the literal key from any text this module is about to surface.

    Belt and braces: nothing here is *supposed* to put the key in an error
    body, but an endpoint that echoes the Authorization header would, and the
    body goes into an exception message a caller may well log.
    """
    s = str(text or "")
    if key and len(key) >= 8 and key in s:
        s = s.replace(key, "[REDACTED]")
    return re.sub(r"(sk-[A-Za-z0-9_\-]{8,})", "[REDACTED]", s)


# ══════════════════════════════════════════════════════════════════════════
# (a) no inherited environment
# ══════════════════════════════════════════════════════════════════════════

SAFE_ENV = ("PATH", "HOME", "LANG", "LC_ALL", "TZ", "TMPDIR")


def minimal_env(extra=None, environ=None) -> dict:
    """An explicit env for a child process. ALLOWLIST, never a filtered copy.

    ⚠ The difference matters. A denylist over `os.environ.copy()` ships every
    variable nobody thought to name — APOLLO_RING, CLAUDE_CODE_*, CODEX_HOME,
    AWS_*, and whatever the next tool invents. An allowlist ships six.

    `CUDA_VISIBLE_DEVICES=""` is pinned: nothing on this path may take the card
    out from under a trainer.
    """
    src = os.environ if environ is None else environ
    env = {k: str(src[k]) for k in SAFE_ENV if k in src and src[k] is not None}
    env.setdefault("PATH", "/usr/bin:/bin")
    env["CUDA_VISIBLE_DEVICES"] = ""
    for k, v in (extra or {}).items():
        k = str(k)
        if not k or "=" in k:
            raise ValueError(f"bad env name {k!r}")
        env[k] = str(v)
    return env


# ══════════════════════════════════════════════════════════════════════════
# (b) the reply is data — provable about THIS file
# ══════════════════════════════════════════════════════════════════════════

# ⚠ AST, NOT A TEXT SCAN. The first cut grepped this file for "exec(" and
# "subprocess" and flagged its OWN DOCSTRING — a check that fails on the prose
# promising it will pass is worthless. Parsing asks the real question: does any
# CALL in this module reach an execution primitive, and is one imported at all?
BANNED_CALLS = {"eval", "exec", "compile", "__import__",
                # (a): a child must never receive the parent's environment
                "os.environ.copy", "os.putenv",
                "os.system", "os.popen", "os.execv", "os.execve", "os.spawnv",
                "os.fork", "os.posix_spawn",
                "pickle.loads", "pickle.load", "marshal.loads",
                "subprocess.run", "subprocess.Popen", "subprocess.call",
                "subprocess.check_output", "subprocess.check_call"}
BANNED_IMPORTS = {"subprocess", "pickle", "marshal", "ctypes", "shlex", "pty"}


def _dotted(node) -> str:
    import ast
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = _dotted(node.value)
        return f"{base}.{node.attr}" if base else node.attr
    return ""


def assert_no_exec(path=None) -> list:
    """Execution primitives reachable from this module's own source.

    Empty list is the invariant: nothing here can turn a completion into a
    running thing. Asserted on the PARSE TREE, so a future edit that reaches
    for `subprocess` trips the suite — and a docstring that merely mentions it
    does not.
    """
    import ast
    src = Path(path or __file__).read_text(encoding="utf-8")
    tree = ast.parse(src)
    bad = []
    for n in ast.walk(tree):
        if isinstance(n, ast.Call):
            name = _dotted(n.func)
            if name in BANNED_CALLS:
                bad.append(f"call {name}() line {n.lineno}")
        elif isinstance(n, ast.Import):
            for a in n.names:
                if a.name.split(".")[0] in BANNED_IMPORTS:
                    bad.append(f"import {a.name} line {n.lineno}")
        elif isinstance(n, ast.ImportFrom):
            if (n.module or "").split(".")[0] in BANNED_IMPORTS:
                bad.append(f"from {n.module} line {n.lineno}")
    return bad


# ══════════════════════════════════════════════════════════════════════════
# usage accounting
# ══════════════════════════════════════════════════════════════════════════

def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def record_usage(model: str, prompt_tokens: int, completion_tokens: int,
                 purpose: str = "", path=None, reasoning_tokens: int = 0,
                 seconds: float = 0.0, finish_reason: str = "") -> dict:
    """One append-only line per call. NO prompt, NO completion, NO key.

    ⚠ `reasoning_tokens` is broken out because on these models it IS the bill:
    a one-word answer cost 2,243 of them. Rolled into `completion_tokens` it
    would be invisible, and "why did 600 units cost that much" unanswerable.
    ⚠ Written even when the call produced no usable answer — a truncated call
    is billed, so leaving it out would make the local log understate the spend
    exactly where it matters.

    Append-only for the same reason the mailbox is: several processes may call
    concurrently and none of them takes a lock.
    """
    row = {"ts": utcnow(), "model": str(model or ""),
           "prompt_tokens": int(prompt_tokens or 0),
           "completion_tokens": int(completion_tokens or 0),
           "reasoning_tokens": int(reasoning_tokens or 0),
           "seconds": round(float(seconds or 0.0), 2),
           "finish_reason": str(finish_reason or ""),
           "purpose": str(purpose or "")[:80]}
    p = Path(path or USAGE)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    return row


def spend(path=None, price_in=None, price_out=None, balance=None) -> dict:
    """Totals over the usage log. `usd` is None unless a rate is supplied.

    Rates are USD per 1,000,000 tokens, from the arguments or from
    LILJACK_DEEPSEEK_PRICE_IN / _OUT. Tokens are always exact.

    ⚠ `balance` is INJECTED, never fetched here: this function must stay
    offline so the suite can exercise it, and because the local log answers a
    different question. The log ATTRIBUTES (which purpose, which model, how
    long); the account balance is GROUND TRUTH for the dollars and is coarse
    ($50.00 → $49.99 over ~6 probes). Pass `balance=balance()` to have both.
    """
    p = Path(path or USAGE)
    calls = 0
    pin = pout = preason = 0
    seconds = 0.0
    per_model, per_purpose = {}, {}
    if p.exists():
        for ln in p.read_text(encoding="utf-8", errors="replace").splitlines():
            ln = ln.strip()
            if not ln:
                continue
            try:
                r = json.loads(ln)
            except Exception:
                continue                       # a torn line is skipped, never fatal
            if not isinstance(r, dict):
                continue
            calls += 1
            a = int(r.get("prompt_tokens") or 0)
            b = int(r.get("completion_tokens") or 0)
            c = int(r.get("reasoning_tokens") or 0)
            pin += a
            pout += b
            preason += c
            seconds += float(r.get("seconds") or 0.0)
            for bucket, name in ((per_model, r.get("model") or "?"),
                                 (per_purpose, r.get("purpose") or "")):
                v = bucket.setdefault(name, {"calls": 0, "in": 0, "out": 0,
                                             "reasoning": 0, "seconds": 0.0})
                v["calls"] += 1; v["in"] += a; v["out"] += b
                v["reasoning"] += c; v["seconds"] = round(v["seconds"] + float(r.get("seconds") or 0.0), 2)

    def _rate(v, env):
        if v is None:
            v = os.environ.get(env, "").strip()
        try:
            return float(v) if v not in (None, "") else None
        except ValueError:
            return None

    ri = _rate(price_in, "LILJACK_DEEPSEEK_PRICE_IN")
    ro = _rate(price_out, "LILJACK_DEEPSEEK_PRICE_OUT")
    usd = None
    if ri is not None and ro is not None:
        usd = round(pin / 1e6 * ri + pout / 1e6 * ro, 6)
    return {"calls": calls, "prompt_tokens": pin, "completion_tokens": pout,
            "reasoning_tokens": preason, "seconds": round(seconds, 2),
            "total_tokens": pin + pout, "usd": usd, "priced": usd is not None,
            "balance": balance,
            "per_model": per_model, "per_purpose": per_purpose, "path": str(p)}


# ══════════════════════════════════════════════════════════════════════════
# transport — injected in tests, so the suite never touches the network
# ══════════════════════════════════════════════════════════════════════════

def urllib_transport(url: str, headers: dict, body: bytes, timeout: float) -> tuple:
    """(status, body_bytes). The ONLY egress in this file.

    An HTTPError is a RESPONSE, not a failure of transport — its status and
    body are what tell `ask` whether to retry, so it is returned, not raised.
    """
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        try:
            b = e.read()
        except Exception:
            b = b""
        return e.code, b


def urllib_get_transport(url: str, headers: dict, timeout: float) -> tuple:
    """(status, body_bytes) for a GET. Used only by `balance`."""
    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        try:
            b = e.read()
        except Exception:
            b = b""
        return e.code, b


def balance(providers=None, transport=None, timeout: float = 30) -> dict:
    """DeepSeek's own `GET /user/balance` — GROUND TRUTH for the budget.

    ⚠ Coarse: it moved $50.00 → $49.99 across ~6 probes, so it bounds the spend
    to a cent and cannot attribute it. That is exactly why the local usage log
    exists beside it, and why neither replaces the other.
    """
    key, base = load_credentials(providers)
    tx = transport or urllib_get_transport
    status, raw = tx(f"{base}/user/balance",
                     {"Authorization": f"Bearer {key}", "Accept": "application/json"},
                     timeout)
    if status != 200:
        snippet = redact(raw[:BODY_SNIPPET].decode("utf-8", "replace")
                         if isinstance(raw, (bytes, bytearray)) else raw, key)
        raise DeepSeekError(f"HTTP {status}: {snippet}", status=status, body=snippet)
    try:
        d = json.loads(raw.decode("utf-8", "replace"))
    except Exception:
        raise DeepSeekError("balance: 200 with a body that is not JSON")
    infos = d.get("balance_infos") or []
    first = infos[0] if infos else {}
    return {"is_available": bool(d.get("is_available")),
            "currency": first.get("currency") or "",
            "total": first.get("total_balance"),
            "granted": first.get("granted_balance"),
            "topped_up": first.get("topped_up_balance")}


def _sleep(sec: float) -> None:
    time.sleep(sec)


def ask(prompt: str, *, system=None, model: str = DEFAULT_MODEL,
        max_tokens: int | None = None, temperature: float = 0.0,
        timeout: float = DEFAULT_TIMEOUT, retries: int = DEFAULT_RETRIES,
        purpose: str = "", transport=None, providers=None, usage_path=None,
        sleep=None, response_format=None, escalate: int = 0, clock=None) -> dict:
    """One completion.

    Returns `{text, content, reasoning, model, usage, attempts, finish_reason,
    seconds}`. `text` is an alias of `content` kept for callers that only want
    the answer; `reasoning` is the model's thinking, returned so a caller can
    see WHY, and never confused with the answer.

    ⚠ `max_tokens=None` means "this model's measured budget" (`MAX_TOKENS`),
    not a small default. A small default on a reasoning model returns nothing.

    ⚠ TRUNCATION IS RAISED, NOT RETURNED. `finish_reason == "length"` with
    empty content means the whole budget went to reasoning: `DeepSeekTruncated`.
    `escalate=N` opts into N budget doublings (capped at TRUNCATION_CAP) before
    giving up — off by default, because each retry is billed again and a silent
    doubling loop is how a budget disappears.

    Retries cover timeouts, transport errors and the retryable statuses (429
    included) with bounded exponential backoff; a 4xx that is not retryable
    fails immediately, because retrying a bad request just spends the budget on
    the same error.
    """
    if not isinstance(prompt, str) or not prompt.strip():
        raise ValueError("prompt must be a non-empty string")
    if max_tokens is None:
        max_tokens = MAX_TOKENS.get(model, FALLBACK_MAX_TOKENS)
    now = clock or time.monotonic
    key, base = load_credentials(providers)
    tx = transport or urllib_transport
    nap = sleep or _sleep
    url = f"{base}/chat/completions"

    msgs = []
    if system:
        msgs.append({"role": "system", "content": str(system)})
    msgs.append({"role": "user", "content": prompt})
    # ⚠ the key lives HERE and only here: one header, one call, no argv, no env.
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json",
               "Accept": "application/json"}

    budget = int(max_tokens)
    rounds = max(1, int(escalate) + 1)
    trunc = last = None
    for round_ in range(rounds):
        payload = {"model": model, "messages": msgs, "stream": False,
                   "max_tokens": budget, "temperature": float(temperature)}
        if response_format:
            payload["response_format"] = response_format
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")

        last = trunc = None
        attempts = max(1, int(retries))
        for attempt in range(1, attempts + 1):
            t0 = now()
            try:
                status, raw = tx(url, dict(headers), body, timeout)
            except Exception as exc:                       # timeout, DNS, reset …
                last = DeepSeekError(f"transport {type(exc).__name__}: "
                                     f"{redact(exc, key)}")
                status = None
            else:
                elapsed = float(now() - t0)
                text_body = (raw.decode("utf-8", "replace")
                             if isinstance(raw, (bytes, bytearray)) else str(raw))
                if status == 200:
                    try:
                        d = json.loads(text_body)
                    except Exception:
                        last = DeepSeekError("200 with a body that is not JSON: "
                                             + redact(text_body[:BODY_SNIPPET], key))
                    else:
                        ch = (d.get("choices") or [{}])[0]
                        msg = ch.get("message") or {}
                        content = str(msg.get("content") or "")
                        # ⚠ SEPARATE field. Merging them would let a chain of
                        # thought be parsed as the answer.
                        reasoning = str(msg.get("reasoning_content") or "")
                        finish = ch.get("finish_reason") or ""
                        u = d.get("usage") or {}
                        det = u.get("completion_tokens_details") or {}
                        rec = record_usage(
                            d.get("model") or model, u.get("prompt_tokens"),
                            u.get("completion_tokens"), purpose, path=usage_path,
                            reasoning_tokens=det.get("reasoning_tokens") or 0,
                            seconds=elapsed, finish_reason=finish)
                        usage = {"prompt_tokens": rec["prompt_tokens"],
                                 "completion_tokens": rec["completion_tokens"],
                                 "reasoning_tokens": rec["reasoning_tokens"],
                                 "total_tokens": rec["prompt_tokens"] + rec["completion_tokens"]}
                        if finish == "length" and not content.strip():
                            # billed, and no answer — a budget failure
                            trunc = DeepSeekTruncated(
                                f"{rec['model']}: max_tokens={budget} was exhausted by "
                                f"reasoning ({usage['reasoning_tokens']} reasoning tokens, "
                                f"{elapsed:.1f}s); content is empty and finish_reason is "
                                f"'length'. That is a budget failure, not an answer.",
                                usage=usage, reasoning=reasoning,
                                model=rec["model"], max_tokens=budget)
                            break
                        return {"text": content, "content": content,
                                "reasoning": reasoning, "model": rec["model"],
                                "usage": usage, "attempts": attempt,
                                "finish_reason": finish, "seconds": round(elapsed, 2)}
                else:
                    snippet = redact(text_body[:BODY_SNIPPET], key)
                    last = DeepSeekError(f"HTTP {status}: {snippet}", status=status,
                                         body=snippet)

            retryable = status is None or status in RETRY_STATUS
            if not retryable or attempt == attempts:
                break
            nap(min(BACKOFF_CAP, BACKOFF_BASE * (2 ** (attempt - 1))))

        if trunc is None:
            raise last or DeepSeekError("no attempt was made")
        nxt = min(TRUNCATION_CAP, budget * 2)
        if round_ >= rounds - 1 or nxt <= budget:
            break
        budget = nxt
    raise trunc


# ══════════════════════════════════════════════════════════════════════════
# session registry adapter — DeepSeek is `available`, never `idle`
# ══════════════════════════════════════════════════════════════════════════

AVAILABLE_EVENT = "Available"


def session_source(providers=None, now=None, **_) -> dict:
    """`liljack_sessions` adapter. One row, event `Available`.

    ⚠ NOT `idle`. `idle` is a claim about a live terminal that has finished a
    turn and can be handed a message — the heartbeat's whole delivery path
    reads it that way. DeepSeek has no terminal and nothing to inject into: a
    row saying `idle` would make the watcher try to wake a session that does
    not exist. `available` says the true thing — a caller may spend a call on
    it right now — and is deliberately absent from `DELIVERABLE_STATES`.

    Reports NOT ok when the credential is missing, because "no key configured"
    and "configured and reachable" are different facts.
    """
    try:
        load_credentials(providers)
    except DeepSeekError as exc:
        return {"ok": False, "error": redact(exc), "rows": []}
    at = now or utcnow()
    return {"ok": True, "rows": [{
        "seq": 1, "at": at, "session": "deepseek-api", "cwd": str(PROJECT_ROOT),
        "event": AVAILABLE_EVENT, "agent": "deepseek", "harness": "deepseek"}]}


def register(sessions=None) -> None:
    """Register the adapter. Idempotent; called at import of this module by
    `liljack_sessions` consumers that ask for the `deepseek` source."""
    if sessions is None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import liljack_sessions as sessions            # noqa: PLC0415
    sessions.register_source("deepseek", session_source)


# ══════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════

def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="DeepSeek client (lilJack)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("ask", help="one completion; the prompt is read from stdin or --prompt")
    a.add_argument("--prompt", default="")
    a.add_argument("--system", default=None)
    a.add_argument("--model", default=DEFAULT_MODEL,
                   help=f"{PRO} (default, strongest) | {FLASH} (cheap tier)")
    a.add_argument("--max-tokens", type=int, default=None,
                   help="default: this model's measured budget (%s)" % MAX_TOKENS)
    a.add_argument("--escalate", type=int, default=0,
                   help="budget doublings to allow on a truncated answer (billed each)")
    a.add_argument("--temperature", type=float, default=0.0)
    a.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    a.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    a.add_argument("--purpose", default="cli")
    a.add_argument("--echo", action="store_true",
                   help="print the completion to THIS terminal (never archived)")

    s = sub.add_parser("spend", help="token totals from the usage log")
    s.add_argument("--price-in", default=None, help="USD per 1M prompt tokens")
    s.add_argument("--price-out", default=None, help="USD per 1M completion tokens")
    s.add_argument("--json", action="store_true")
    s.add_argument("--balance", action="store_true",
                   help="also fetch DeepSeek's own balance (one network call)")

    sub.add_parser("balance", help="account balance from GET /user/balance")
    sub.add_parser("status", help="is the credential present, and what is skipped")

    ns = ap.parse_args(argv)

    if ns.cmd == "balance":
        try:
            b = balance()
        except DeepSeekError as exc:
            print(f"✗ {exc}", file=sys.stderr)
            return 1
        print(json.dumps(b, indent=2))
        return 0

    if ns.cmd == "spend":
        bal = None
        if ns.balance:
            try:
                bal = balance()
            except DeepSeekError as exc:
                bal = {"error": str(exc)}
        rep = spend(price_in=ns.price_in, price_out=ns.price_out, balance=bal)
        if ns.json:
            print(json.dumps(rep, indent=2))
        else:
            print(f"calls {rep['calls']}  in {rep['prompt_tokens']}  out {rep['completion_tokens']} "
                  f"(reasoning {rep['reasoning_tokens']})  total {rep['total_tokens']}  "
                  f"{rep['seconds']:.0f}s  usd "
                  + (f"{rep['usd']:.4f}" if rep["priced"] else "— (unpriced; pass --price-in/--price-out)"))
            for m, v in sorted(rep["per_model"].items()):
                print(f"  {m:<26} calls {v['calls']:>4}  in {v['in']:>8}  out {v['out']:>8}  "
                      f"reasoning {v['reasoning']:>8}  {v['seconds']:>8.1f}s")
            if rep["balance"]:
                print(f"  account balance (ground truth): {rep['balance']}")
        return 0

    if ns.cmd == "status":
        try:
            key, base = load_credentials()
            print(f"✓ credential present ({len(key)} chars, not shown) · base {base}")
        except DeepSeekError as exc:
            print(f"✗ {redact(exc)}")
            return 1
        print("deliberately NOT implemented (agent-shaped controls that do not apply):")
        for k, why in SKIPPED.items():
            print(f"  · {k}: {why}")
        return 0

    prompt = ns.prompt or sys.stdin.read()
    try:
        r = ask(prompt, system=ns.system, model=ns.model, max_tokens=ns.max_tokens,
                temperature=ns.temperature, timeout=ns.timeout, retries=ns.retries,
                purpose=ns.purpose, escalate=ns.escalate)
    except DeepSeekTruncated as exc:
        # loud, and never mistaken for an answer
        print(f"✗ TRUNCATED {exc}", file=sys.stderr)
        return 2
    except DeepSeekError as exc:
        print(f"✗ {exc}", file=sys.stderr)
        return 1
    u = r["usage"]
    print(f"model={r['model']} in={u['prompt_tokens']} out={u['completion_tokens']} "
          f"reasoning={u['reasoning_tokens']} {r['seconds']}s "
          f"attempts={r['attempts']} finish={r['finish_reason']}", file=sys.stderr)
    if ns.echo:
        print(r["text"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
