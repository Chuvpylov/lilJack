/* Validate the owkterm-native (sessiond) frame protocol from the C side:
 * 4-byte big-endian length + tag + body, against a live liljack_sessiond.
 *
 * Not a full UI test — this pins the byte layout the UI's native_attach/native_parse
 * use (frame_write / 4-byte BE length / 'C' control / 'O' output / 'I' input)
 * so a Python-supervisor <-> C-client mismatch cannot ship silently.
 *
 * Usage: test_liljack_sessiond SOCK_PATH
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int ok = 0, fail = 0;
#define CHECK(c, msg) do { if (c) { ok++; printf("  ok     %s\n", msg); } \
                           else { fail++; printf("  FAIL   %s\n", msg); } } while (0)

/* Same frame_write the UI uses: 4-byte BE length + tag + body, write loop. */
static int frame_write(int fd, unsigned char tag, const void *body, size_t n) {
    unsigned char *frame = malloc(5 + n);
    if (!frame) return -1;
    uint32_t len = 1 + (uint32_t)n;
    frame[0] = (unsigned char)(len >> 24); frame[1] = (unsigned char)(len >> 16);
    frame[2] = (unsigned char)(len >> 8);  frame[3] = (unsigned char)len; frame[4] = tag;
    if (n) memcpy(frame + 5, body, n);
    size_t total = 5 + n, sent = 0;
    while (sent < total) { ssize_t w = write(fd, frame + sent, total - sent);
        if (w < 0) { if (errno == EINTR) continue; free(frame); return -1; } sent += (size_t)w; }
    free(frame); return 0;
}

/* Poll the socket (non-blocking) for a complete frame. Returns tag, body into
 * caller-supplied buffer (capped), or -1 on timeout. */
static int read_frame(int fd, unsigned char *tag, unsigned char *body, size_t cap,
                      size_t *blen, int timeout_ms) {
    static unsigned char hdr[4];
    static int have_hdr = 0;
    int waited = 0;
    while (have_hdr < 4) {
        ssize_t n = read(fd, hdr + have_hdr, 4 - have_hdr);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { if (waited >= timeout_ms) return -1; usleep(5000); waited += 5; continue; }
        if (n <= 0) return -1;
        have_hdr += (int)n;
    }
    uint32_t len = ((uint32_t)hdr[0]<<24)|((uint32_t)hdr[1]<<16)|((uint32_t)hdr[2]<<8)|hdr[3];
    if (len < 1 || len > 0x100000) { have_hdr = 0; return -1; }
    unsigned char tmp[1];
    while (1) { ssize_t n = read(fd, tmp, 1); if (n == 1) break;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { if (waited >= timeout_ms) return -1; usleep(5000); waited += 5; continue; }
        if (n <= 0) return -1; }
    *tag = tmp[0];
    size_t bl = (size_t)(len - 1); if (bl > cap) bl = cap;
    size_t got = 0;
    while (got < bl) {
        ssize_t n = read(fd, body + got, bl - got);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { if (waited >= timeout_ms) return -1; usleep(5000); waited += 5; continue; }
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    if (bl < (size_t)(len - 1)) { /* discard overflow */ size_t rest = (size_t)(len - 1) - bl;
        unsigned char drop[4096]; while (rest) { size_t c = rest < sizeof(drop) ? rest : sizeof(drop);
            if (read(fd, drop, c) <= 0) break; rest -= c; } }
    *blen = bl; have_hdr = 0;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s SOCK\n", argv[0]); return 2; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(addr.sun_path)) { fprintf(stderr, "sock too long\n"); return 2; }
    strcpy(addr.sun_path, argv[1]);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 2; }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    const char *create_json = "{\"op\":\"create\",\"cmd\":[\"sh\",\"-c\",\"echo C_MARKER; sleep 60\"],\"cwd\":\"/tmp\",\"cols\":80,\"rows\":24}";
    CHECK(frame_write(fd, 'C', create_json, strlen(create_json)) == 0, "create frame sent");

    unsigned char tag; unsigned char body[8192]; size_t blen;
    CHECK(read_frame(fd, &tag, body, sizeof(body), &blen, 4000) == 0 && tag == 'C',
          "create answered");
    body[blen] = 0;
    char sid[64] = {0};
    const char *k = strstr((char*)body, "\"session_id\"");
    if (k) { const char *colon = strchr(k, ':'); if (colon) { const char *q = strchr(colon, '"');
        if (q) { q++; size_t i = 0; while (q[i] && q[i] != '"' && i < 63) { sid[i] = q[i]; i++; } } } }
    CHECK(sid[0] != 0, "parsed the supervisor's session id");

    char attach_json[128]; snprintf(attach_json, sizeof(attach_json), "{\"op\":\"attach\",\"session\":\"%s\"}", sid);
    CHECK(frame_write(fd, 'C', attach_json, strlen(attach_json)) == 0, "attach frame sent");

    int saw_marker = 0;
    for (int i = 0; i < 400 && !saw_marker; i++) {
        unsigned char t; unsigned char b[8192]; size_t n;
        if (read_frame(fd, &t, b, sizeof(b), &n, 4000) < 0) break;
        if (t == 'O' && memmem(b, n, "C_MARKER", 8)) saw_marker = 1;
    }
    CHECK(saw_marker, "the agent's output streamed through the C frame parser");

    const char *input = "echo DONE\r";
    CHECK(frame_write(fd, 'I', input, strlen(input)) == 0, "input frame sent");
    int saw_done = 0;
    for (int i = 0; i < 400 && !saw_done; i++) {
        unsigned char t; unsigned char b[8192]; size_t n;
        if (read_frame(fd, &t, b, sizeof(b), &n, 4000) < 0) break;
        if (t == 'O' && memmem(b, n, "DONE", 4)) saw_done = 1;
    }
    CHECK(saw_done, "typed input reached the agent and echoed back");

    close(fd);
    printf("\n%d checks passed, %d failed\n", ok, fail);
    return fail ? 1 : 0;
}
