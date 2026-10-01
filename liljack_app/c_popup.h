#ifndef LJ_POPUP_H
#define LJ_POPUP_H
/* c_popup.h — the About/rickroll popup, a thin state machine over lj_media.
 *
 * Owner: deepseek (2026-09-09, codex msg306). Wraps the single lj_media tile
 * and adds what a popup needs that raw media does not: an explicit open (no
 * auto-play), a user close request, a status line, and a bounded re-resolve
 * retry so an expired YouTube signed URL recovers instead of dying silently.
 *
 * main.c polls this state and presents lj_popup_draw's pixels through
 * lj_ansi_image (sixel or block glyphs), with draggable terminal chrome.
 */
#include <stdint.h>
#include "c_media.h"

#define LJ_POPUP_SOURCE_MAX 1024
#define LJ_POPUP_MAX_RETRIES 2

typedef struct {
    lj_media media;
    char source[LJ_POPUP_SOURCE_MAX];
    int open;             /* a popup session is active */
    int close_requested;  /* the user asked to close (hit-box in main.c) */
    int retries;          /* remaining re-resolve attempts for a failed URL */
    char status[160];
} lj_popup;

void lj_popup_init(lj_popup *p);
/* Explicit open; never auto-plays. Returns 1 when playback started or URL
 * resolution began, 0 when the source was rejected outright. */
int  lj_popup_open(lj_popup *p, const char *source);
/* Pump one decode step. Returns 1 when a new frame decoded or the state
 * changed (resolution finished, ended, retried). */
int  lj_popup_poll(lj_popup *p);
/* Blit the decoded frame (aspect-fit) into the target rect. No-op when closed
 * or before the first frame. Text chrome is the caller's. */
void lj_popup_draw(const lj_popup *p, uint32_t *dst, int dw, int dh,
                   int x, int y, int w, int h);
void lj_popup_request_close(lj_popup *p);
int  lj_popup_close_requested(const lj_popup *p);
int  lj_popup_is_open(const lj_popup *p);
void lj_popup_toggle(lj_popup *p);
int lj_popup_seek(lj_popup *p,double seconds);
void lj_popup_mute(lj_popup *p);
void lj_popup_volume(lj_popup *p,int percent);
/* Free the child decoder/fd and reset to closed. Safe to call repeatedly. */
void lj_popup_close(lj_popup *p);
const char *lj_popup_status(const lj_popup *p);
/* The one-line explanation for the terminal path. */
const char *lj_popup_window_only(void);
#endif
