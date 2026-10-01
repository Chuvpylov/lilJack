"""Env-allowlist probe for c_env.h (lj_child_env).

Compiles a small C harness against liljack_app/c_env.h and, for each
backend (NONE/ATTACH/MEDIA/HELPER), forks a child that applies lj_child_env()
then reports only booleans — a planted secret's presence/absence, allowlist
presence, extra-var presence — never the values themselves.

Run:  python3 tests/test_liljack_c_env.py
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = HERE.parent / "toolbox" / "liljack_app"

PROBE = r'''
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "c_env.h"

static int fails;
static void chk(const char *label, int cond) {
    printf("  %s: %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}
static void run_child(int backend, const char *expected) {
    if (lj_child_env(backend) != 0) { printf("  backend=%d lj_child_env: FAIL\n", backend); _exit(1); }
    chk("PATH present",          getenv("PATH") != NULL);
    chk("PATH value preserved",  strcmp(getenv("PATH"), "/bin") == 0);
    chk("HOME present",          getenv("HOME") != NULL);
    chk("DEEPSEEK_API_KEY absent", getenv("DEEPSEEK_API_KEY") == NULL);
    chk("ANTHROPIC_API_KEY absent", getenv("ANTHROPIC_API_KEY") == NULL);
    chk("CLAUDE_CODE_SSH_PASS absent", getenv("CLAUDE_CODE_SSH_PASS") == NULL);
    const char *c = getenv("CUDA_VISIBLE_DEVICES");
    chk("CUDA visibility preserved", expected ? (c && strcmp(c, expected) == 0) : c == NULL);
    if (backend) {
        chk("LILJACK_WORKSPACE_ROOT present", getenv("LILJACK_WORKSPACE_ROOT") != NULL);
        chk("LILJACK_WORKSPACE_ROOT value preserved", strcmp(getenv("LILJACK_WORKSPACE_ROOT"), "/tmp/ws") == 0);
        chk("LILJACK_TMUX present",          getenv("LILJACK_TMUX") != NULL);
    } else {
        chk("LILJACK_WORKSPACE_ROOT absent", getenv("LILJACK_WORKSPACE_ROOT") == NULL);
        chk("LILJACK_TMUX absent",          getenv("LILJACK_TMUX") == NULL);
    }
    fflush(stdout);
    _exit(fails ? 1 : 0);
}
int main(void) {
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/tmp", 1);
    setenv("LILJACK_WORKSPACE_ROOT", "/tmp/ws", 1);
    setenv("LILJACK_TMUX", "/usr/bin/tmux", 1);
    setenv("DEEPSEEK_API_KEY", "super-secret", 1);
    setenv("ANTHROPIC_API_KEY", "also-secret", 1);
    setenv("CLAUDE_CODE_SSH_PASS", "leaked-marker", 1);
    int backends[] = { LJ_ENV_NONE, LJ_ENV_ATTACH, LJ_ENV_MEDIA, LJ_ENV_HELPER };
    int bad = 0;
    const char *cases[] = {NULL, "", "2"};
    for (size_t j = 0; j < 3; j++) {
    if (cases[j]) setenv("CUDA_VISIBLE_DEVICES", cases[j], 1); else unsetenv("CUDA_VISIBLE_DEVICES");
    for (size_t i = 0; i < sizeof(backends)/sizeof(backends[0]); i++) {
        printf("backend=%d\n", backends[i]);
        pid_t pid = fork();
        if (pid == 0) run_child(backends[i], cases[j]);
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad = 1;
    }
    }
    printf(bad ? "probe-FAIL\n" : "probe-ok\n");
    return bad;
}
'''


class EnvHelperProbe(unittest.TestCase):
    def test_child_env_allowlist(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "probe.c").write_text(PROBE)
            exe = d / "probe"
            build = subprocess.run(
                ["gcc", "-std=c11", "-I", str(APP), "-o", str(exe),
                 str(d / "probe.c")],
                capture_output=True, text=True, timeout=120,
                env=dict(os.environ),
            )
            self.assertEqual(build.returncode, 0, "compile failed:\n" + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            out = run.stdout
            self.assertIn("probe-ok", out, out)
            self.assertNotIn("probe-FAIL", out)
            self.assertNotIn("FAIL", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
