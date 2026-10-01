/* OSC 52 clipboard set — owkterm-osc52-clipboard. lilJack's copy emits
 * ESC ] 52 ; c ; <base64> ST (or BEL); the VT must hand the decoded text to the
 * app exactly once, across split feeds, and ignore queries and garbage.
 * Build: gcc -std=c11 -Wall -Wextra -I liljack_app tests/test_liljack_vt_osc52.c liljack_app/owkterm_vt.c -o vt-osc52 && ./vt-osc52 */
#include "owkterm_vt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void feed(void *vt, const char *s) { lj_vt_feed(vt, (const unsigned char *)s, (int)strlen(s)); }
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void *vt = lj_vt_new(40, 5); int bad = 0;
    /* "sudo pacman -S --needed kitty" in base64, split across two feeds, ST terminator */
    feed(vt, "\033]52;c;c3VkbyBwYWNtYW4gLVMg");
    char *t = lj_vt_take_clipboard(vt); if (t) { printf("osc52: text handed out before the terminator\n"); bad++; free(t); }
    feed(vt, "LS1uZWVkZWQga2l0dHk=\033\\");
    t = lj_vt_take_clipboard(vt);
    printf("osc52: after ST: %s\n", t ? t : "(null)");
    if (!t || strcmp(t, "sudo pacman -S --needed kitty")) bad++;
    free(t);
    if (lj_vt_take_clipboard(vt)) { printf("osc52: handed out twice\n"); bad++; }
    /* BEL terminator, selection 'p', and the OSC must not print into the grid */
    feed(vt, "\033[H\033]52;p;aGk=\007X");
    t = lj_vt_take_clipboard(vt); printf("osc52: BEL form: %s; cell(0,0)=%c\n", t ? t : "(null)", (int)lj_vt_row(vt, 0)[0].ch);
    if (!t || strcmp(t, "hi") || lj_vt_row(vt, 0)[0].ch != 'X') bad++; free(t);
    /* query and garbage yield nothing */
    feed(vt, "\033]52;c;?\033\\"); if (lj_vt_take_clipboard(vt)) { printf("osc52: query leaked\n"); bad++; }
    feed(vt, "\033]52;c;@@@@\033\\"); if (lj_vt_take_clipboard(vt)) { printf("osc52: garbage accepted\n"); bad++; }
    feed(vt, "\033]0;title\033\\"); if (lj_vt_take_clipboard(vt)) { printf("osc52: other OSC accepted\n"); bad++; }
    lj_vt_free(vt);
    printf("vt-osc52: %s\n", bad ? "FAIL" : "PASS"); return bad ? 1 : 0;
}
