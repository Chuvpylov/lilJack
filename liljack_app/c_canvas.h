/* Shared room canvas: rooms/<room-id>/canvas.jsonl rendered to pixels.
 * Contract: docs/codex/reports/2026-09-21-interactive-room-survey/CANVAS-FORMAT.md
 * lilJack appends the operator's mouse strokes; agents append through `liljack room --draw`.
 * The file is append-only, polled by mtime+size each frame, re-rendered on change. */
#ifndef LJ_CANVAS_H
#define LJ_CANVAS_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define LJ_CANVAS_MAX_STROKES 4096
#define LJ_CANVAS_MAX_POINTS  256   /* per stroke, kept for the operator's drags */
#define LJ_CANVAS_ASPECT (16.0f/9.0f) /* the picture main.c letterboxes into every tile */

typedef struct {
    int kind;             /* 0 line, 1 text, 2 clear, 3 image */
    uint32_t colour;      /* 0xRRGGBB */
    float width;          /* line width at a 1000px reference */
    int npoints;
    float *points;        /* npoints*2 normalized x,y in 0..1 */
    char text[64];
    char by[16];
    char file[512];       /* kind 3: absolute image path (rooms/<room>/media/...) */
    char id[24];          /* stable id: the row's "id", or L<line> for rows written before ids */
} lj_stroke;

/* Decoded images, kept across re-polls so a new stroke does not re-decode every picture. */
#define LJ_CANVAS_IMAGE_CACHE 16
typedef struct {char path[512];uint32_t *px;int w,h;unsigned long used;} lj_canvas_image;

typedef struct {
    char path[640];
    char media[640];            /* rooms/<room>/media: pasted, dropped and captured images */
    lj_stroke *strokes;int nstrokes;
    time_t mtime;long size;int loaded;
    uint32_t *pixels;int w,h;   /* last rendered frame, 0xAARRGGBB, owned */
    int dirty;                  /* strokes changed since last render */
    char status[120];
    lj_canvas_image images[LJ_CANVAS_IMAGE_CACHE];unsigned long image_clock;
    int preview_n;              /* points of the live stroke already painted into pixels */
    float damage[4];int damaged;/* normalized x0,y0,x1,y1 changed since lj_canvas_take_damage */
    unsigned hidden;            /* author layers not drawn/hit: bits from lj_canvas_author_bit */
} lj_canvas;
/* Per-agent layers: operator 1, claude 2, codex 4, deepseek 8, anyone else 16.
 * Set c->hidden then mark c->dirty=1 to show/hide an agent's strokes. */
unsigned lj_canvas_author_bit(const char *by);

void lj_canvas_init(lj_canvas *c);
/* Point the canvas at the room's file (root/rooms/<room>/canvas.jsonl); no I/O yet. */
int  lj_canvas_open(lj_canvas *c,const char *root,const char *room_id);
/* Re-read when the file changed; returns 1 when strokes changed. */
int  lj_canvas_poll(lj_canvas *c);
/* Render every stroke since the last clear into an owned w×h buffer; returns pixels. */
const uint32_t *lj_canvas_render(lj_canvas *c,int w,int h,uint32_t background);
/* Append one line stroke (normalized points) as `by`; the next poll picks it up. */
int  lj_canvas_append_line(lj_canvas *c,const float *points,int npoints,uint32_t colour,float width,const char *by);
/* Place an image: copy src into the room's media folder (unless it is already
 * there), then append {"t":"image","f":<copy>,"p":[[x0,y0],[x1,y1]]}. The rect is
 * normalized over the picture; pass x1<=x0 to get a centred rect that keeps the
 * image's aspect (at most 60% of the picture). Returns 1, or 0 with c->status set. */
int  lj_canvas_append_image(lj_canvas *c,const char *src,float x0,float y0,float x1,float y1,const char *by);
/* rooms/<room>/media (created on demand); "" before lj_canvas_open. */
const char *lj_canvas_media_dir(lj_canvas *c);
/* Copy any file into the media folder under a unique name; writes the copy's path. */
int  lj_canvas_import(lj_canvas *c,const char *src,char *out,size_t outcap);
/* Render the current strokes at w×h and write a PNG (agents' eyes, F12 companions). */
int  lj_canvas_save_png(lj_canvas *c,const char *out,int w,int h,uint32_t background);
/* Paint the stroke the operator is still dragging over the last render. INCREMENTAL:
 * only segments added since the previous call are drawn, and no full re-render
 * is queued; the stroke is redrawn smoothed once it is appended and polled. */
void lj_canvas_preview(lj_canvas *c,const float *points,int npoints,uint32_t colour,float width);
/* ── Editing (select / erase / move / restyle / undo). Ops are rows in the same
 * append-only file, applied in order:  {"t":"del","ids":[..]}
 * {"t":"move","ids":[..],"d":[dx,dy]}  {"t":"style","ids":[..],"c":"#rrggbb","w":n} */
const char *lj_canvas_stroke_id(lj_canvas *c,int i);
/* Topmost stroke within `radius` (normalized picture height units) of x,y; -1 if none.
 * Lines count their pen width; images hit inside their rect; notes near their marker. */
int  lj_canvas_hit(lj_canvas *c,float x,float y,float radius);
int  lj_canvas_bbox(lj_canvas *c,int i,float out[4]);
/* Validate one op row (JSON object text), stamp by/at, append. 0 + c->status on refusal. */
int  lj_canvas_append_op(lj_canvas *c,const char *json_row,const char *by);
/* Delete `by`'s most recent stroke still on the canvas (Ctrl+Z). 0 when none. */
int  lj_canvas_undo(lj_canvas *c,const char *by);
/* Normalized rect (x0,y0,x1,y1 in 0..1) of the picture changed since the last
 * call: the whole picture after a full render, else just the new preview
 * segments. Returns 0 when nothing changed. The presenter re-sends only this. */
int  lj_canvas_take_damage(lj_canvas *c,float out[4]);
void lj_canvas_close(lj_canvas *c);
/* Author colour when a stroke carries none. */
uint32_t lj_canvas_author_colour(const char *by);
#endif
