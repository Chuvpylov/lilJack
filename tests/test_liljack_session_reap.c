/* Memory caps on the session table (audit 2026-10-15, three rooms of agents):
 *  1. A pty that hits EOF left its child a zombie until the tile was closed or
 *     reattached; poll_terminal now reaps it on EOF.
 *  2. A session that vanished from the snapshot kept its ~5.5 MB VT until all
 *     64 slots were full; once it has been gone and disconnected for
 *     LJ_GONE_FREE_MS, apply_snapshot frees its terminal state. */
#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY

static void snapshot_with(App *a, const char *id) {
    json_object *snap = json_object_new_object(), *rows = json_object_new_array();
    if (id) { json_object *row = json_object_new_object(); jadd(row, "id", id); jadd(row, "agent", "shell"); json_object_array_add(rows, row); }
    json_object_object_add(snap, "sessions", rows); json_object_object_add(snap, "messages", json_object_new_array());
    apply_snapshot(a, snap); json_object_put(snap);
}

int main(void) {
    setlocale(LC_ALL, ""); CHECK(!SDL_Init(SDL_INIT_VIDEO));
    App *a = calloc(1, sizeof *a); CHECK(a); test_app = a;
    a->w = 1280; a->h = 720; a->split = -1; a->backend_in = a->backend_out = -1;
    for (int i = 0; i < COUNT; i++) a->sessions[i].fd = -1;
    /* 1. reap on EOF */
    char script[] = "/tmp/liljack-reap-test-XXXXXX"; int fd = mkstemp(script); CHECK(fd >= 0);
    const char program[] = "#!/bin/sh\nprintf 'BYE\\n'\n";
    CHECK(write(fd, program, sizeof program - 1) == (ssize_t)(sizeof program - 1)); CHECK(fchmod(fd, 0700) == 0); close(fd);
    copy(a->tmux, sizeof a->tmux, script);
    Session *s = &a->sessions[a->nsession++]; memset(s, 0, sizeof *s); s->fd = -1;
    copy(s->id, sizeof s->id, "s-reap"); copy(s->name, sizeof s->name, "isolated"); s->managed = 1; s->vt = lj_vt_new(80, 24);
    CHECK(attach(a, s)); pid_t pid = s->pid; CHECK(pid > 0);
    for (int i = 0; i < 400 && s->connected; i++) { poll_terminal(a, s); SDL_Delay(5); }
    CHECK(!s->connected);
    errno = 0; int r = waitpid(pid, NULL, WNOHANG);
    if (!(r == -1 && errno == ECHILD)) { fprintf(stderr, "FAIL exited child %d not reaped on pty EOF (waitpid=%d)\n", (int)pid, r); return 1; }
    CHECK(s->pid == 0);
    unlink(script);
    /* 2. a vanished, disconnected session releases its VT after the grace */
    snapshot_with(a, "s-reap"); CHECK(s->vt);
    snapshot_with(a, NULL); CHECK(s->vt);                 /* just gone: kept */
    s->gone_at = SDL_GetTicks() - LJ_GONE_FREE_MS - 1000;  /* gone long enough */
    snapshot_with(a, NULL);
    if (s->vt) { fprintf(stderr, "FAIL vanished session still holds its VT\n"); return 1; }
    snapshot_with(a, "s-reap"); CHECK(s->gone_at == 0);   /* back: grace resets */
    printf("PASS session reap: EOF child reaped, vanished session VT freed\n");
    test_cleanup();
    return 0;
}
