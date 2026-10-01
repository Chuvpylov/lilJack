#!/usr/bin/env python3
"""tests/test_liljack_chains.py — chain extraction from session JSONL fixtures."""
import json, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent / "toolbox"))

FIX = [
  {"type": "user", "timestamp": "2026-07-26T10:00:00Z",
   "message": {"role": "user", "content": "fix the login bug"}},
  {"type": "assistant", "message": {"role": "assistant", "content": [
      {"type": "thinking", "thinking": "The bug is in auth.py — wrong comparison."},
      {"type": "tool_use", "id": "t1", "name": "Edit",
       "input": {"file_path": "/src/auth.py", "old_string": "==", "new_string": "!="}}]}},
  {"type": "user", "message": {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "t1", "content": "edit ok"}]}},
  {"type": "assistant", "message": {"role": "assistant", "content": [
      {"type": "thinking", "thinking": "Now verify with the test suite."},
      {"type": "text", "text": "Edit applied — running the auth tests to confirm."},
      {"type": "tool_use", "id": "t2", "name": "Bash",
       "input": {"command": "python3 -m pytest tests/test_auth.py"}}]}},
  {"type": "user", "message": {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "t2", "content": "1 passed"}]}},
  {"type": "assistant", "message": {"role": "assistant", "content": [
      {"type": "text", "text": "Fixed — comparison inverted, test passes."}]}},
  {"type": "user", "timestamp": "2026-07-26T10:05:00Z",
   "message": {"role": "user", "content": "thanks, now add logging"}},
  {"type": "assistant", "message": {"role": "assistant", "content": [
      {"type": "text", "text": "Sure — where should logs go?"}]}},
]

def test_extract_two_chains():
    from liljack_chains import extract_chains
    chains = extract_chains(FIX, project="demo", session_id="s1")
    assert len(chains) == 2
    c = chains[0]
    assert c["human"] == "fix the login bug"
    assert c["project"] == "demo" and c["session_id"] == "s1"
    assert [s["i"] for s in c["steps"]] == [1, 2]
    assert c["steps"][0]["thinking"].startswith("The bug is in auth.py")
    assert c["steps"][0]["tool"] == "Edit"
    assert "auth.py" in c["steps"][0]["input_digest"]
    assert c["steps"][0]["result_digest"] == "edit ok"
    assert c["steps"][1]["tool"] == "Bash"
    assert c["steps"][0]["say"] == ""
    assert c["steps"][1]["say"].startswith("Edit applied")
    assert c["final"].startswith("Fixed")
    assert c["outcome"]["files_edited"] == ["/src/auth.py"]
    assert c["outcome"]["tests"] == "pass"
    assert c["outcome"]["turn_count"] == 3
    assert chains[1]["outcome"]["tests"] == "none"
    assert chains[1]["steps"] == []

def test_digest_bounds_and_garbage():
    from liljack_chains import extract_chains, _digest
    assert len(_digest({"x": "a" * 9999}, 200)) <= 200
    garbage = [{"type": "summary"}, {"bogus": 1}, {"type": "user", "message": None}]
    assert extract_chains(garbage + FIX[:1], project="p", session_id="s") != []

PAR = [
  {"type": "user", "timestamp": "2026-09-02T20:00:00Z",
   "message": {"role": "user", "content": "check disk and the timers"}},
  # one narration, then TWO tool calls streamed as separate assistant records
  {"type": "assistant", "uuid": "u-say", "message": {"role": "assistant", "content": [
      {"type": "text", "text": "I'll check disk and timers in parallel."}]}},
  {"type": "assistant", "uuid": "u-a", "message": {"role": "assistant", "content": [
      {"type": "tool_use", "id": "a", "name": "Bash", "input": {"command": "df -h", "description": "disk"}}]}},
  {"type": "assistant", "uuid": "u-b", "message": {"role": "assistant", "content": [
      {"type": "tool_use", "id": "b", "name": "Bash", "input": {"command": "systemctl list-timers"}}]}},
  {"type": "user", "message": {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "a", "content": [{"type": "text", "text": "HEAD" + "x" * 20000 + "TAIL"}]}]}},
  {"type": "user", "message": {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "b", "content": "no timers", "is_error": True}]}},
  # a third call with NO new narration after the results → say is empty
  {"type": "assistant", "uuid": "u-c", "message": {"role": "assistant", "content": [
      {"type": "tool_use", "id": "c", "name": "Bash", "input": {"command": "python3 tests/test_x.py"}}]}},
  {"type": "user", "message": {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "c", "content": "x" * 400 + "\nFAILED 1: foo"}]}},
  {"type": "assistant", "message": {"role": "assistant", "content": [
      {"type": "text", "text": "Disk is fine; one test fails."}]}},
]

def test_parallel_narration_and_full_io():
    from liljack_chains import extract_chains, RESULT_HEAD, RESULT_TAIL
    c = extract_chains(PAR, project="demo", session_id="s2")[0]
    a, b, cc = c["steps"]
    # narration attaches to EVERY call issued before a result came back
    assert a["say"].startswith("I'll check") and b["say"] == a["say"], (a["say"], b["say"])
    assert cc["say"] == "", cc["say"]
    # full input, head+tail result, length, error flag, uuid pointer
    assert a["input"] == {"command": "df -h", "description": "disk"}
    assert a["result"].startswith("HEAD") and a["result"].endswith("TAIL")
    assert "chars elided" in a["result"] and a["result_len"] == 20008
    assert len(a["result"]) <= RESULT_HEAD + RESULT_TAIL + 40
    assert b["result"] == "no timers" and b["is_error"] is True and a["is_error"] is False
    assert a["uuid"] == "u-a" and b["uuid"] == "u-b"
    # digests unchanged for old consumers
    assert len(a["result_digest"]) == 300 and a["input_digest"].startswith('{"command"')
    # tests heuristic reads the FULL result — "FAILED" sits past the 300-char digest
    assert c["outcome"]["tests"] == "fail", c["outcome"]

def test_backfill_force():
    import tempfile, gzip
    import liljack_chains as L
    root = pathlib.Path(tempfile.mkdtemp())
    L.ARCHIVE_ROOT = root; L.CHAINS_DIR = root / "chains"
    gz = root / "2026" / "09" / "demo" / "s9.jsonl.gz"; gz.parent.mkdir(parents=True)
    with gzip.open(gz, "wt") as f:
        for r in FIX: f.write(json.dumps(r) + "\n")
    assert L.backfill() == 2
    out = L.CHAINS_DIR / "demo" / "s9.chains.jsonl"
    out.write_text("stale\n")
    assert L.backfill() == 0 and out.read_text() == "stale\n"      # existing → skipped
    assert L.backfill(force=True) == 2 and "fix the login bug" in out.read_text()

def test_write_chains():
    import tempfile
    from liljack_chains import extract_chains, write_chains
    import liljack_chains
    liljack_chains.CHAINS_DIR = pathlib.Path(tempfile.mkdtemp())
    out = write_chains(extract_chains(FIX, project="demo", session_id="s1"), "demo")
    lines = out.read_text().strip().splitlines()
    assert len(lines) == 2 and json.loads(lines[0])["human"] == "fix the login bug"

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print(f"  ✓ {name}")
    print("OK test_liljack_chains")
