/* Image slots kept their largest-ever source buffer: one 4 MiB video frame in
 * a slot stayed resident after the slot went back to holding a 2 KB prompt
 * mark, so the pool's worst case was 32 x 4 MiB = 128 MiB, not the "<=32 MiB"
 * its comment promised. A slot whose buffer is far larger than its image now
 * shrinks to fit. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>

int main(void) {
    state.opened = 1; state.sixel = 1; state.cols = 200; state.rows = 60;
    static uint32_t big[1000 * 1000], small[16 * 16];
    assert(lj_ansi_image(0, 0, 100 * LJ_CELL_W, 20 * LJ_LINE_H, big, 1000, 1000));
    size_t high = state.images[0].capacity;
    state.image_count = 0;                                  /* next frame */
    assert(lj_ansi_image(0, 0, 2 * LJ_CELL_W, 1 * LJ_LINE_H, small, 16, 16));
    size_t after = state.images[0].capacity;
    if (after > 256u * 1024u) {
        fprintf(stderr, "FAIL slot kept %zu bytes for a %zu-byte image (high-water %zu)\n",
                after, sizeof small, high);
        return 1;
    }
    printf("PASS ansi image slot shrinks: %zu -> %zu bytes\n", high, after);
    free(state.images[0].src);
    return 0;
}
