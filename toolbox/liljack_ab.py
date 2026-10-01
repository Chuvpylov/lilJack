#!/usr/bin/env python3
"""
liljack_ab.py — A/B test lilJack context injection: with vs. without.

Measures BOTH sides of the ledger:
  • COST   — exact token/byte overhead the inject adds per turn (always runs).
  • QUALITY — blind LLM-judge win-rate of answers WITH inject vs WITHOUT
             (runs only if an answerer is reachable).

It imports liljack_inject.build_context() so the "with" arm is byte-identical
to what the live UserPromptSubmit hook produces. persist=False → never touches
the live session's dedup state in context.json.

Answerer auto-detection (in order):
  1. ANTHROPIC_API_KEY env  → Claude via api.anthropic.com (raw HTTP, no SDK dep)
  2. Ollama at OLLAMA_URL   → local, free
  3. neither                → cost-only report

Usage:
  python3 toolbox/liljack_ab.py                 # 8 sampled real prompts
  python3 toolbox/liljack_ab.py -n 20           # more prompts
  python3 toolbox/liljack_ab.py -p "your prompt here"
  python3 toolbox/liljack_ab.py --cost-only     # skip the LLM half
  ANSWER_MODEL=qwen3:4b JUDGE_MODEL=qwen3:4b python3 toolbox/liljack_ab.py

Honors the project's no-deps ethos: stdlib only (urllib for HTTP).
"""

import argparse
import json
import os
import pathlib
import re
import sys
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import liljack_inject as lj  # noqa: E402

PROJECT_CWD = str(lj.PROJECT_ROOT)

# ── answerer config ──────────────────────────────────────────────────────────
ANTHROPIC_KEY   = os.environ.get("ANTHROPIC_API_KEY", "")
ANTHROPIC_MODEL = os.environ.get("ANSWER_MODEL", "claude-opus-4-8")
ANTHROPIC_JUDGE = os.environ.get("JUDGE_MODEL",  "claude-opus-4-8")
OLLAMA_URL      = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL    = os.environ.get("ANSWER_MODEL", "qwen3:4b")
OLLAMA_JUDGE    = os.environ.get("JUDGE_MODEL",  "qwen3:4b")

# input price $/1M tokens for the cost estimate (Opus 4.8 default; override via env)
PRICE_IN = float(os.environ.get("PRICE_IN", "5.0"))

# A few curated dev prompts; the rest are sampled from real past intents.
CURATED = [
    "how do I start the full Den dashboard?",
    "where does the Bitterbot arena column get its checkpoint from?",
    "run the test suite",
    "what port is the arena WASM UI on and how does it auto-start?",
]


# ── token counting ───────────────────────────────────────────────────────────
def count_tokens(text: str) -> int:
    """Token count. Uses Anthropic count_tokens if a key is set, else ~4 chars/tok."""
    if not text:
        return 0
    if ANTHROPIC_KEY:
        try:
            body = json.dumps({
                "model": ANTHROPIC_MODEL,
                "messages": [{"role": "user", "content": text}],
            }).encode()
            req = urllib.request.Request(
                "https://api.anthropic.com/v1/messages/count_tokens",
                data=body, method="POST",
                headers={
                    "x-api-key": ANTHROPIC_KEY,
                    "anthropic-version": "2023-06-01",
                    "content-type": "application/json",
                })
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read())["input_tokens"]
        except Exception:
            pass
    return max(1, round(len(text) / 4))  # heuristic fallback


# ── answerers ────────────────────────────────────────────────────────────────
def answer_anthropic(prompt: str, extra: str) -> str:
    content = (f"<system-reminder>\n{extra}\n</system-reminder>\n\n{prompt}"
               if extra else prompt)
    body = json.dumps({
        "model": ANTHROPIC_MODEL,
        "max_tokens": 700,
        "messages": [{"role": "user", "content": content}],
    }).encode()
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages", data=body, method="POST",
        headers={"x-api-key": ANTHROPIC_KEY, "anthropic-version": "2023-06-01",
                 "content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = json.loads(r.read())
    return "".join(b.get("text", "") for b in data.get("content", []))


def answer_ollama(prompt: str, extra: str) -> str:
    content = (f"[context]\n{extra}\n[/context]\n\n{prompt}" if extra else prompt)
    body = json.dumps({
        "model": OLLAMA_MODEL, "prompt": content, "stream": False,
        "options": {"temperature": 0.3, "num_predict": 700},
    }).encode()
    req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", data=body,
                                 method="POST",
                                 headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.loads(r.read()).get("response", "")


def ollama_up() -> bool:
    try:
        urllib.request.urlopen(f"{OLLAMA_URL}/api/tags", timeout=2).read()
        return True
    except Exception:
        return False


# ── judge ────────────────────────────────────────────────────────────────────
JUDGE_TMPL = """You are judging two answers to the same developer question about \
the "demo" project (an inference/training dashboard). Pick the more HELPFUL, \
correct, and specific answer. Ignore length and style; reward accuracy and \
actionable detail.

QUESTION:
{q}

ANSWER A:
{a}

ANSWER B:
{b}

Reply with ONLY a JSON object: {{"winner": "A" | "B" | "tie", "why": "<8 words>"}}"""


def judge(question: str, a: str, b: str, answer_fn, model_label: str) -> dict:
    raw = answer_fn(JUDGE_TMPL.format(q=question, a=a[:2000], b=b[:2000]), "")
    m = re.search(r'\{.*\}', raw, re.DOTALL)
    if not m:
        return {"winner": "tie", "why": "unparseable judge reply"}
    try:
        out = json.loads(m.group(0))
        w = str(out.get("winner", "tie")).upper()
        return {"winner": w if w in ("A", "B") else "tie",
                "why": out.get("why", "")[:60]}
    except Exception:
        return {"winner": "tie", "why": "bad judge json"}


# ── prompt set ───────────────────────────────────────────────────────────────
def sample_prompts(n: int) -> list[str]:
    prompts = list(CURATED)
    if lj.PAIRS_FILE.exists():
        seen = set(prompts)
        # walk the pairs file, take varied real intents (deterministic stride)
        lines = lj.PAIRS_FILE.read_text(errors="replace").splitlines()
        stride = max(1, len(lines) // (n * 3 or 1))
        for i in range(0, len(lines), stride):
            try:
                intent = json.loads(lines[i]).get("human_intent", "").strip()
            except Exception:
                continue
            # only keep concrete, question-like intents
            if 15 <= len(intent) <= 160 and intent not in seen \
               and not intent.lower().startswith(("this session", "summary")):
                prompts.append(intent)
                seen.add(intent)
            if len(prompts) >= n:
                break
    return prompts[:n]


# ── main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=8, help="number of prompts")
    ap.add_argument("-p", "--prompt", help="single prompt to test")
    ap.add_argument("--cost-only", action="store_true", help="skip the LLM quality half")
    ap.add_argument("--cwd", default=PROJECT_CWD, help="cwd to simulate")
    ap.add_argument("--turns", type=int, default=20,
                    help="turns/session for the cost projection")
    args = ap.parse_args()

    prompts = [args.prompt] if args.prompt else sample_prompts(args.n)

    # pick answerer
    if not args.cost_only and ANTHROPIC_KEY:
        ans_fn, mode = answer_anthropic, f"Anthropic API ({ANTHROPIC_MODEL})"
    elif not args.cost_only and ollama_up():
        ans_fn, mode = answer_ollama, f"Ollama ({OLLAMA_MODEL})"
    else:
        ans_fn, mode = None, "cost-only (no answerer reachable)"

    print(f"\n{'='*70}\nlilJack A/B  —  mode: {mode}\n"
          f"prompts: {len(prompts)}   cwd: {args.cwd}\n{'='*70}\n")

    # ── COST half (always) ────────────────────────────────────────────────────
    overheads, first_turn_only = [], None
    for pr in prompts:
        # fresh session each prompt → measures the turn-1 (full) inject cost
        res = lj.build_context(pr, cwd=args.cwd, session_id="__ab__", persist=False)
        toks = count_tokens(res["text"])
        overheads.append(toks)
        if first_turn_only is None:
            first_turn_only = res
    avg = sum(overheads) / len(overheads) if overheads else 0
    mx  = max(overheads) if overheads else 0

    # after turn 1, stable content is deduped → only pairs/health remain.
    # estimate the steady-state turn cost by rebuilding with the same session id twice.
    lj.build_context(prompts[0], cwd=args.cwd, session_id="__abdedup__", persist=True)
    steady = lj.build_context(prompts[0], cwd=args.cwd, session_id="__abdedup__", persist=True)
    steady_toks = count_tokens(steady["text"])
    lj._save_context({k: v for k, v in lj._load_context().items()
                      if k not in ("session_id", "stable_hash")})  # don't poison live state

    tok_note = "exact (Anthropic count_tokens)" if ANTHROPIC_KEY else "≈ chars/4 heuristic"
    print(f"── COST  ({tok_note}) ──")
    print(f"  turn-1 inject   : avg {avg:.0f} tok   max {mx} tok")
    print(f"  steady-state    : {steady_toks} tok/turn  (stable content deduped)")
    sess = avg + steady_toks * (args.turns - 1)
    print(f"  per {args.turns}-turn session: ~{sess:.0f} tok "
          f"= ${sess/1e6*PRICE_IN:.4f}  @ ${PRICE_IN}/M in")
    print(f"  (old behavior re-sent ~{avg:.0f} tok EVERY turn "
          f"= ~{avg*args.turns:.0f} tok/session; dedup saves "
          f"~{(avg*args.turns - sess)/1e6*PRICE_IN:.4f})\n")

    if ans_fn is None:
        print("── QUALITY ──\n  skipped: set ANTHROPIC_API_KEY or start Ollama, "
              "then re-run without --cost-only.\n")
        print("="*70)
        return

    # ── QUALITY half ──────────────────────────────────────────────────────────
    print("── QUALITY  (blind, order-randomized judge) ──")
    wins = {"with": 0, "without": 0, "tie": 0}
    log_path = lj.CACHE / "ab_results.jsonl"
    lj.CACHE.mkdir(parents=True, exist_ok=True)
    judge_fn = answer_anthropic if (ANTHROPIC_KEY and not args.cost_only) else answer_ollama

    with open(log_path, "a") as logf:
        for i, pr in enumerate(prompts):
            extra = lj.build_context(pr, cwd=args.cwd, session_id="__ab__",
                                     persist=False)["text"]
            try:
                a_with    = ans_fn(pr, extra)
                a_without = ans_fn(pr, "")
            except Exception as e:
                print(f"  [{i+1}] answerer error: {e}")
                continue
            # randomize A/B slot without Math.random restriction (plain script)
            flip = (i % 2 == 1)
            A, B = (a_without, a_with) if flip else (a_with, a_without)
            verdict = judge(pr, A, B, judge_fn, ANTHROPIC_JUDGE)
            w = verdict["winner"]
            if w == "tie":
                key = "tie"
            else:
                with_slot = "B" if flip else "A"
                key = "with" if w == with_slot else "without"
            wins[key] += 1
            print(f"  [{i+1}/{len(prompts)}] {key:8s} ({verdict['why']})  "
                  f"« {pr[:50]}")
            logf.write(json.dumps({
                "prompt": pr, "winner": key, "why": verdict["why"],
                "inject_tokens": count_tokens(extra),
            }) + "\n")

    tot = sum(wins.values()) or 1
    print(f"\n  WITH inject won : {wins['with']:3d}  ({wins['with']/tot*100:.0f}%)")
    print(f"  WITHOUT won     : {wins['without']:3d}  ({wins['without']/tot*100:.0f}%)")
    print(f"  ties            : {wins['tie']:3d}  ({wins['tie']/tot*100:.0f}%)")
    print(f"\n  full log: {log_path}")
    print("="*70)


if __name__ == "__main__":
    main()
