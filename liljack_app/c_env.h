/* c_env.h — minimal child environment for every fork/exec path.
 *
 * The parent (./liljack) is launched from the operator's shell, whose environment may
 * carry API credentials (DEEPSEEK_*, ANTHROPIC_*, OPENAI_*, CLAUDE_*, ring
 * tokens, harness vars). A child must never inherit them. lj_child_env()
 * replaces the process environment with an explicit allowlist right before
 * exec, so no credential or harness variable survives into tmux attach
 * clients, the python workspace helper, or ffmpeg/yt-dlp fetchers.
 *
 * Call in the forked child, immediately before execl():
 *   lj_child_env(LJ_ENV_ATTACH); setenv("TERM", "xterm-256color", 1); execl(...);
 *
 * Baseline allowlist: PATH HOME LANG LC_ALL LC_CTYPE TZ TMPDIR
 *                     XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME
 * backend != 0 adds : LILJACK_CACHE LILJACK_WORKSPACE_ROOT
 *                     LILJACK_TMUX LILJACK_TMUX_SOCKET
 * Preserves CUDA_VISIBLE_DEVICES only when supplied by the operator.
 *
 * Only C89 primitives are used (malloc/memcpy/strlen + getenv), so the header
 * compiles under any feature-macro regime. Returns 0 on success; -1 on
 * allocation failure (environment is left empty — the caller should _exit(127)).
 * Never prints environment values.
 */
#ifndef LJ_C_ENV_H
#define LJ_C_ENV_H

#include <stdlib.h>
#include <string.h>

enum {
    LJ_ENV_NONE   = 0,
    LJ_ENV_ATTACH = 1,   /* tmux attach-session client */
    LJ_ENV_MEDIA  = 2,   /* ffmpeg / yt-dlp fetchers */
    LJ_ENV_HELPER = 3    /* python workspace helper */
};

static const char *const lj_env_base[] = {
    "PATH", "HOME", "LANG", "LC_ALL", "LC_CTYPE", "TZ", "TMPDIR",
    "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "CUDA_VISIBLE_DEVICES", NULL
};

static const char *const lj_env_extra[] = {
    "LILJACK_TOOLBOX_ROOT", "LILJACK_CACHE", "LILJACK_WORKSPACE_ROOT", "LILJACK_TMUX",
    "LILJACK_TMUX_SOCKET", NULL
};

extern char **environ;

static int lj_child_env(int backend) {
    const char *names[32];
    char *vals[32];
    size_t n = 0;

    /* snapshot allowlist values before they are destroyed */
    for (size_t pass = 0; pass < 2; pass++) {
        if (pass == 1 && !backend) break;
        const char *const *list = pass ? lj_env_extra : lj_env_base;
        for (size_t i = 0; list[i]; i++) {
            const char *v = getenv(list[i]);
            if (v && n < sizeof(names) / sizeof(names[0])) {
                size_t len = strlen(v);
                char *dup = (char *)malloc(len + 1);
                if (!dup) goto fail;
                memcpy(dup, v, len + 1);
                names[n] = list[i];
                vals[n] = dup;
                n++;
            }
        }
    }

    /* build the child environment from scratch: n pairs + NULL */
    {
        char **env = (char **)malloc((n + 1) * sizeof(char *));
        if (!env) goto fail;
        size_t e = 0;
        for (size_t i = 0; i < n; i++) {
            size_t nlen = strlen(names[i]);
            char *pair = (char *)malloc(nlen + strlen(vals[i]) + 2);
            if (!pair) {
                while (e) free(env[--e]);
                free(env);
                goto fail;
            }
            memcpy(pair, names[i], nlen);
            pair[nlen] = '=';
            memcpy(pair + nlen + 1, vals[i], strlen(vals[i]) + 1);
            env[e++] = pair;
            free(vals[i]);
        }
        env[e] = NULL;
        environ = env;
        return 0;
    }

fail:
    for (size_t i = 0; i < n; i++) free(vals[i]);
    if (environ) environ[0] = NULL;
    return -1;
}

#endif /* LJ_C_ENV_H */
