#define _POSIX_C_SOURCE 200809L
#include "../liljack_app/c_popup.h"
#include "../liljack_app/c_render.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static lj_popup popup;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static void cleanup(void) { lj_popup_close(&popup); lj_render_close(); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void pump(void) { struct timespec pause = {0, 5000000}; lj_popup_poll(&popup); nanosleep(&pause, NULL); }
static void child_reaped(pid_t pid) { errno = 0; CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD); }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    lj_popup_init(&popup);
    CHECK(atexit(cleanup) == 0);

    CHECK(!lj_popup_is_open(&popup));
    CHECK(!strcmp(lj_popup_status(&popup), "closed"));
    CHECK(strstr(lj_popup_window_only(), "floating video") != NULL);
    CHECK(!lj_popup_close_requested(&popup));
    lj_popup_request_close(&popup);
    CHECK(lj_popup_close_requested(&popup));

    /* Unsupported schemes are rejected at open, not left to fail at decode. */
    CHECK(!lj_popup_open(&popup, "ftp://example.invalid/video"));
    CHECK(!lj_popup_is_open(&popup));
    CHECK(!lj_popup_open(&popup, "file:///etc/passwd"));
    CHECK(!lj_popup_is_open(&popup));

    /* A missing LOCAL file starts (ffmpeg forks) but ends "unavailable", and
     * because it is not remote it must NOT retry: retries stay at max. */
    CHECK(lj_popup_open(&popup, "/nonexistent/liljack-popup-missing.mp4"));
    CHECK(lj_popup_is_open(&popup));
    double until = now() + 8;
    while (!popup.media.ended && now() < until) pump();
    CHECK(popup.media.ended);
    CHECK(strstr(lj_popup_status(&popup), "unavailable") != NULL);
    CHECK(popup.retries == LJ_POPUP_MAX_RETRIES); /* local: no re-resolve */
    lj_popup_close(&popup);
    CHECK(!lj_popup_is_open(&popup));

    /* Local video: explicit open, sustained decode to a real 640x360 frame. */
    CHECK(lj_popup_open(&popup, argv[1]));
    CHECK(lj_popup_is_open(&popup));
    pid_t decoder = popup.media.pid;
    int fd = popup.media.fd;
    CHECK(decoder > 0 && fd >= 0);
    until = now() + 8;
    while (!popup.media.pixels && !popup.media.ended && now() < until) pump();
    CHECK(popup.media.pixels && popup.media.w == 640 && popup.media.h == 360);

    uint32_t dst[4 * 4];
    for (size_t i = 0; i < 16; i++) dst[i] = 0xff000000;
    lj_popup_draw(&popup, dst, 4, 4, 0, 0, 4, 4);
    uint32_t center = popup.media.pixels[180 * 640 + 320];
    CHECK(((center >> 16) & 255) > 220); /* solid-red fixture through YUV */
    CHECK(((center >> 8) & 255) < 30);
    CHECK((center & 255) < 30);

    /* EOF cleans up the child and fd. */
    until = now() + 8;
    while (!popup.media.ended && now() < until) pump();
    CHECK(popup.media.ended);
    CHECK(popup.media.pid == 0 && popup.media.fd == -1);
    child_reaped(decoder);
    errno = 0; CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(strstr(lj_popup_status(&popup), "ended") != NULL);

    /* Repeat open works after close. */
    lj_popup_close(&popup);
    CHECK(!lj_popup_is_open(&popup));
    CHECK(lj_popup_open(&popup, argv[1]));
    CHECK(lj_popup_is_open(&popup));
    lj_popup_close(&popup);

    puts("C popup: open/close/request/status, local ended/unavailable, no local retry, "
         "sustained decode, child/fd cleanup, repeat open passed");
    return 0;
}
