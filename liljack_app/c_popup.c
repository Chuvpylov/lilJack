#define _POSIX_C_SOURCE 200809L
#include "c_popup.h"
#include <stdio.h>
#include <string.h>

static void set_status(lj_popup *p, const char *s) {
    snprintf(p->status, sizeof(p->status), "%s", s);
}

static int is_remote(const char *src) {
    return !strncmp(src, "http://", 7) || !strncmp(src, "https://", 8);
}

void lj_popup_init(lj_popup *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    lj_media_init(&p->media);
    p->retries = LJ_POPUP_MAX_RETRIES;
    set_status(p, "closed");
}

const char *lj_popup_window_only(void) {
    /* Legacy symbol retained for callers; both terminal paths show pixels. */
    return "Play floating video · drag its title · Esc closes";
}

int lj_popup_open(lj_popup *p, const char *source) {
    if (!p || !source || !*source) return 0;
    lj_popup_close(p);
    snprintf(p->source, sizeof(p->source), "%s", source);
    p->retries = LJ_POPUP_MAX_RETRIES;
    if (!lj_media_open(&p->media, source)) {
        set_status(p, p->media.status[0] ? p->media.status
                                         : "Media could not be opened");
        p->open = 0;
        return 0;
    }
    p->open = 1;
    p->close_requested = 0;
    set_status(p, p->media.status);
    return 1;
}

int lj_popup_poll(lj_popup *p) {
    if (!p || !p->open) return 0;
    int changed = lj_media_poll(&p->media);
    set_status(p, p->media.status);
    /* An expired YouTube signed URL surfaces as "unavailable" once ffmpeg dies
     * with a non-zero exit. Re-open re-resolves the source (yt-dlp -g), so a
     * bounded retry recovers playback instead of leaving the popup dead. Local
     * files and genuinely-bad inputs are never retried. */
    if (p->media.ended && strstr(p->media.status, "unavailable") &&
        p->retries > 0 && is_remote(p->source)) {
        p->retries--;
        int paused=p->media.paused,muted=p->media.muted,volume=p->media.volume,height=p->media.decode_h;
        double offset=lj_media_position(&p->media);
        if (lj_media_open(&p->media, p->source)) {
            lj_media_resolution(&p->media,height);
            p->media.offset=p->media.clock_base=offset;
            lj_media_pause(&p->media,paused);lj_media_mute(&p->media,muted);lj_media_volume(&p->media,volume);
            set_status(p, "Re-resolving media…");
            changed = 1;
        }
    }
    return changed;
}

void lj_popup_draw(const lj_popup *p, uint32_t *dst, int dw, int dh,
                   int x, int y, int w, int h) {
    if (!p || !p->open || !p->media.pixels) return;
    lj_media_blit(&p->media, dst, dw, dh, x, y, w, h);
}

void lj_popup_request_close(lj_popup *p) {
    if (p) p->close_requested = 1;
}

int lj_popup_close_requested(const lj_popup *p) {
    return p && p->close_requested;
}

int lj_popup_is_open(const lj_popup *p) {
    return p && p->open;
}
void lj_popup_toggle(lj_popup *p){
    if(!p||!p->open)return;
    if(p->media.ended){if(lj_media_seek(&p->media,0))lj_media_pause(&p->media,0);}
    else lj_media_pause(&p->media,!p->media.paused);
}
int lj_popup_seek(lj_popup *p,double seconds){return p&&p->open&&lj_media_seek(&p->media,seconds);}
void lj_popup_mute(lj_popup *p){if(p&&p->open)lj_media_mute(&p->media,!p->media.muted);}
void lj_popup_volume(lj_popup *p,int percent){if(p&&p->open)lj_media_volume(&p->media,percent);}

const char *lj_popup_status(const lj_popup *p) {
    return p ? p->status : "";
}

void lj_popup_close(lj_popup *p) {
    if (!p) return;
    lj_media_close(&p->media);
    p->open = 0;
    p->close_requested = 0;
    p->retries = LJ_POPUP_MAX_RETRIES;
    p->source[0] = 0;
    set_status(p, "closed");
}
