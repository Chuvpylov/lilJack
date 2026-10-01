/* hui_glyph.h — editable vector glyph model + live renderer (cubic contours).
 * Define HUI_GLYPH_IMPLEMENTATION in exactly one TU. Depends on hui_math.h. */
#ifndef HUI_GLYPH_H
#define HUI_GLYPH_H
#include <stdint.h>
#include "hui_math.h"

typedef struct { hui_v2 c0, c1, a; } hui_seg;          /* cubic: prev->a */
typedef struct { hui_v2 start; hui_seg *segs; int nseg, cap; } hui_contour;
typedef struct {
    uint32_t codepoint; hui_contour *contours; int ncontour, cap; int advance, lsb;
} hui_glyph;
typedef struct {
    char family[64], style[32];
    int units_per_em, ascent, descent, line_gap, is_monospace;
    hui_glyph *glyphs; int nglyph, cap;
} hui_glyph_font;

hui_glyph_font *hui_glyph_font_new(const char *family, int units_per_em);
void            hui_glyph_font_free(hui_glyph_font *f);
hui_glyph      *hui_glyph_add(hui_glyph_font *f, uint32_t cp);
hui_glyph      *hui_glyph_find(hui_glyph_font *f, uint32_t cp);
hui_contour    *hui_glyph_add_contour(hui_glyph *g);
void            hui_glyph_add_seg(hui_contour *c, hui_v2 c0, hui_v2 c1, hui_v2 a);
hui_rect        hui_glyph_bbox(const hui_glyph *g);

typedef struct hui_glyph_warp {
    float jitter_amp;    /* max point displacement, em units */
    float jitter_freq;   /* spatial frequency of the noise field (reserved) */
    float baseline_wob;  /* per-glyph vertical wobble amplitude (em) */
    float slant_jit;     /* per-glyph shear jitter, radians (reserved) */
    int   enabled;
} hui_glyph_warp;
void hui_glyph_draw(const hui_glyph_font *f, uint32_t cp, int x, int baseline_y,
                    float size, hui_color col, const struct hui_glyph_warp *warp, uint32_t seed);
int  hui_glyph_text(const hui_glyph_font *f, int x, int baseline_y, float size,
                    const char *utf8, hui_color col, const struct hui_glyph_warp *warp, uint32_t seed);

int             hui_glyph_font_save(const hui_glyph_font *f, const char *path);
hui_glyph_font *hui_glyph_font_load(const char *path);

/* TTF import — requires stb_truetype.h already included in the TU.
 * Enable by defining HUI_GLYPH_IMPORT_TTF before including this header. */
#ifdef HUI_GLYPH_IMPORT_TTF
hui_glyph_font *hui_glyph_import_ttf_mem(const unsigned char *data, int n, const char *family);
hui_glyph_font *hui_glyph_import_ttf_file(const char *path);
#endif

#ifdef HUI_GLYPH_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

hui_glyph_font *hui_glyph_font_new(const char *family, int upem) {
    hui_glyph_font *f = (hui_glyph_font*)calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->family, sizeof f->family, "%s", family ? family : "owk");
    snprintf(f->style, sizeof f->style, "Regular");
    f->units_per_em = upem > 0 ? upem : 1000;
    f->ascent = (int)(f->units_per_em * 0.8);
    f->descent = -(int)(f->units_per_em * 0.2);
    f->line_gap = 0;
    return f;
}
void hui_glyph_font_free(hui_glyph_font *f) {
    if (!f) return;
    for (int i = 0; i < f->nglyph; i++) {
        hui_glyph *g = &f->glyphs[i];
        for (int j = 0; j < g->ncontour; j++) free(g->contours[j].segs);
        free(g->contours);
    }
    free(f->glyphs); free(f);
}
hui_glyph *hui_glyph_add(hui_glyph_font *f, uint32_t cp) {
    if (f->nglyph == f->cap) {
        int nc = f->cap ? f->cap * 2 : 16;
        hui_glyph *ng = (hui_glyph*)realloc(f->glyphs, nc * sizeof *ng);
        if (!ng) return NULL;
        f->glyphs = ng; f->cap = nc;
    }
    hui_glyph *g = &f->glyphs[f->nglyph++];
    memset(g, 0, sizeof *g);
    g->codepoint = cp;
    g->advance = f->units_per_em / 2;   /* default half-em advance */
    return g;
}
hui_glyph *hui_glyph_find(hui_glyph_font *f, uint32_t cp) {
    for (int i = 0; i < f->nglyph; i++) if (f->glyphs[i].codepoint == cp) return &f->glyphs[i];
    return NULL;
}
hui_contour *hui_glyph_add_contour(hui_glyph *g) {
    if (g->ncontour == g->cap) {
        int nc = g->cap ? g->cap * 2 : 4;
        hui_contour *ncs = (hui_contour*)realloc(g->contours, nc * sizeof *ncs);
        if (!ncs) return NULL;
        g->contours = ncs; g->cap = nc;
    }
    hui_contour *c = &g->contours[g->ncontour++];
    memset(c, 0, sizeof *c);
    return c;
}
void hui_glyph_add_seg(hui_contour *c, hui_v2 c0, hui_v2 c1, hui_v2 a) {
    if (c->nseg == c->cap) {
        int nc = c->cap ? c->cap * 2 : 8;
        hui_seg *ns = (hui_seg*)realloc(c->segs, nc * sizeof *ns);
        if (!ns) return;
        c->segs = ns; c->cap = nc;
    }
    c->segs[c->nseg++] = (hui_seg){c0, c1, a};
}
hui_rect hui_glyph_bbox(const hui_glyph *g) {
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f; int any=0;
    for (int i=0;i<g->ncontour;i++){
        const hui_contour *c=&g->contours[i];
        hui_v2 pts_seed = c->start;
        #define UPD(p) do{ if((p).x<minx)minx=(p).x; if((p).y<miny)miny=(p).y; \
                           if((p).x>maxx)maxx=(p).x; if((p).y>maxy)maxy=(p).y; any=1; }while(0)
        UPD(pts_seed);
        for(int j=0;j<c->nseg;j++){ UPD(c->segs[j].c0); UPD(c->segs[j].c1); UPD(c->segs[j].a); }
        #undef UPD
    }
    if(!any) return hui_rect_make(0,0,0,0);
    return hui_rect_make((int)minx,(int)miny,(int)(maxx-minx),(int)(maxy-miny));
}

static uint32_t hui__hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static float hui__hashf(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = hui__hash32(a*73856093u ^ b*19349663u ^ c*83492791u);
    return (float)(h & 0xffffffu) / (float)0xffffffu * 2.0f - 1.0f;   /* [-1,1] */
}
/* Deterministic per-point displacement. Bounded by jitter_amp (x) and
 * jitter_amp + baseline_wob (y). slant_jit/jitter_freq reserved for the editor. */
static void hui__glyph_warp_pt(const struct hui_glyph_warp *w, uint32_t seed,
                               uint32_t cp, int idx, float *x, float *y) {
    if (!w || !w->enabled) return;
    float nx = hui__hashf(seed, cp*131u + (uint32_t)idx, 0x11u);
    float ny = hui__hashf(seed, cp*131u + (uint32_t)idx, 0x22u);
    float wob = hui__hashf(seed, cp, 0x33u) * w->baseline_wob;
    *x += nx * w->jitter_amp;
    *y += ny * w->jitter_amp + wob;
}

/* Flatten one cubic into line points appended to (px,py,*n). Depth-bounded. */
static void hui__cubic_flat(hui_v2 p0, hui_v2 p1, hui_v2 p2, hui_v2 p3,
                            float *px, float *py, int *n, int cap, int depth) {
    if (depth > 6 || *n >= cap-1) {
        if (*n < cap) { px[*n]=p3.x; py[*n]=p3.y; (*n)++; }  /* strict: never write past cap */
        return;
    }
    float dx=p3.x-p0.x, dy=p3.y-p0.y;
    float d1=fabsf((p1.x-p3.x)*dy-(p1.y-p3.y)*dx);
    float d2=fabsf((p2.x-p3.x)*dy-(p2.y-p3.y)*dx);
    if ((d1+d2)*(d1+d2) < 0.1f*(dx*dx+dy*dy)) { px[*n]=p3.x; py[*n]=p3.y; (*n)++; return; }
    hui_v2 p01={(p0.x+p1.x)/2,(p0.y+p1.y)/2}, p12={(p1.x+p2.x)/2,(p1.y+p2.y)/2},
           p23={(p2.x+p3.x)/2,(p2.y+p3.y)/2};
    hui_v2 a={(p01.x+p12.x)/2,(p01.y+p12.y)/2}, b={(p12.x+p23.x)/2,(p12.y+p23.y)/2};
    hui_v2 m={(a.x+b.x)/2,(a.y+b.y)/2};
    hui__cubic_flat(p0,p01,a,m,px,py,n,cap,depth+1);
    hui__cubic_flat(m,b,p23,p3,px,py,n,cap,depth+1);
}

void hui_glyph_draw(const hui_glyph_font *f, uint32_t cp, int x, int baseline_y,
                    float size, hui_color col, const struct hui_glyph_warp *warp, uint32_t seed) {
    hui_glyph *g = hui_glyph_find((hui_glyph_font*)f, cp);
    if (!g) return;
    float s = size / (float)f->units_per_em;
    enum { MAXE = 2048 };
    typedef struct { float x0,y0,x1,y1; } Edge;
    static Edge edges[MAXE]; int ne=0;
    float minY=1e9f, maxY=-1e9f;
    for (int ci=0; ci<g->ncontour; ci++) {
        const hui_contour *c=&g->contours[ci];
        enum { FLAT_CAP = 1024 };
        float fx[FLAT_CAP], fy[FLAT_CAP]; int fn=0;
        fx[fn]=c->start.x; fy[fn]=c->start.y; fn++;
        hui_v2 prev=c->start;
        for (int j=0;j<c->nseg;j++) {
            hui_v2 p1=c->segs[j].c0,p2=c->segs[j].c1,p3=c->segs[j].a;
            if (fn<FLAT_CAP-1) hui__cubic_flat(prev,p1,p2,p3,fx,fy,&fn,FLAT_CAP,0);
            prev=p3;
        }
        for (int j=0;j<fn;j++) {
            float ex0=fx[j], ey0=fy[j];
            float ex1=fx[(j+1)%fn], ey1=fy[(j+1)%fn];
            if (warp) hui__glyph_warp_pt(warp, seed, cp, j, &ex0, &ey0);
            if (warp) hui__glyph_warp_pt(warp, seed, cp, (j+1)%fn, &ex1, &ey1);
            float sx0=x+ex0*s, sy0=baseline_y-ey0*s;
            float sx1=x+ex1*s, sy1=baseline_y-ey1*s;
            if (ne<MAXE) edges[ne++]=(Edge){sx0,sy0,sx1,sy1};
            if (sy0<minY)minY=sy0; if (sy0>maxY)maxY=sy0;
        }
    }
    if (ne == 0) return;
    /* Clamp the scanline range to the screen. Without this, a glyph rendered
     * far larger than the viewport (e.g. a font editor zoomed deep in) would
     * loop over tens of thousands of off-screen rows — millions of edge tests
     * per frame — which reads as a hang/crash. Off-screen rows draw nothing
     * anyway (the backend scissors per pixel), so skipping them is lossless.
     * Margins of 1px keep things simple; X spans are clamped below. */
    int sh = hui_g ? hui_g->screen_h : 4096;
    int sw = hui_g ? hui_g->screen_w : 4096;
    int ylo = (int)minY, yhi = (int)maxY;
    if (ylo < 0)  ylo = 0;
    if (yhi > sh) yhi = sh;
    /* Anti-aliased coverage fill: per pixel row, sample HUI_GLYPH_AA_SUB
     * sub-scanlines and accumulate exact horizontal span coverage into a row
     * buffer, then emit run-length alpha-modulated 1px fills. This fixes the
     * two artifacts that made small vector text look janky: stems snapping
     * between 1px and 2px (horizontal rounding) and stair-stepped curves
     * (no vertical coverage). Alpha is quantized to 16 levels so smooth
     * gradients still run-length compress. */
    #ifndef HUI_GLYPH_AA_SUB
    #  define HUI_GLYPH_AA_SUB 3
    #endif
    enum { COVW = 8192 };
    static float cov[COVW];
    int cw = sw < COVW ? sw : COVW;
    for (int y=ylo; y<=yhi; y++) {
        int rxa = cw, rxb = -1;                    /* touched x-range this row */
        for (int sub=0; sub<HUI_GLYPH_AA_SUB; sub++) {
            float xs[128]; int nx=0;
            float yc=(float)y + ((float)sub + 0.5f) / (float)HUI_GLYPH_AA_SUB;
            for (int e=0;e<ne;e++) {
                float y0=edges[e].y0,y1=edges[e].y1;
                if ((y0<=yc && y1>yc) || (y1<=yc && y0>yc)) {
                    float dy=y1-y0;
                    if (dy>-1e-4f && dy<1e-4f) continue;   /* skip near-horizontal: avoids huge/NaN x */
                    float t=(yc-y0)/dy;
                    if (nx<128) xs[nx++]=edges[e].x0+t*(edges[e].x1-edges[e].x0);
                }
            }
            for (int a=0;a<nx-1;a++) for (int b=a+1;b<nx;b++) if (xs[b]<xs[a]){float t=xs[a];xs[a]=xs[b];xs[b]=t;}
            for (int p=0;p+1<nx;p+=2) {
                float fa=xs[p], fb=xs[p+1];
                if (fa < 0.0f)        fa = 0.0f;
                if (fb > (float)cw)   fb = (float)cw;
                if (fb <= fa) continue;
                int ia=(int)fa, ib=(int)fb;
                if (ia >= cw) continue;
                float w = 1.0f / (float)HUI_GLYPH_AA_SUB;
                if (ia == ib) {
                    cov[ia] += (fb - fa) * w;
                } else {
                    cov[ia] += ((float)(ia+1) - fa) * w;
                    for (int px=ia+1; px<ib && px<cw; px++) cov[px] += w;
                    if (ib < cw) cov[ib] += (fb - (float)ib) * w;
                }
                if (ia < rxa) rxa = ia;
                if (ib > rxb) rxb = ib < cw ? ib : cw-1;
            }
        }
        /* emit runs of equal (quantized) alpha, clearing the buffer as we go */
        int px = rxa;
        while (px <= rxb) {
            float cv = cov[px];
            int a8 = (int)(cv * 255.0f + 0.5f);
            if (a8 > 255) a8 = 255;
            a8 = (a8 + 8) & ~15;                    /* 16-level quantize */
            if (a8 > 255) a8 = 255;
            int run = px;
            while (run <= rxb) {
                int b8 = (int)(cov[run] * 255.0f + 0.5f);
                if (b8 > 255) b8 = 255;
                b8 = (b8 + 8) & ~15;
                if (b8 > 255) b8 = 255;
                if (b8 != a8) break;
                cov[run] = 0.0f;
                run++;
            }
            if (a8 > 0) {
                hui_color c2 = col;
                c2.a = (uint8_t)((int)col.a * a8 / 255);
                hui_rect_fill(hui_rect_make(px, y, run - px, 1), c2, 0);
            }
            px = run;
        }
    }
}

int hui_glyph_text(const hui_glyph_font *f, int x, int baseline_y, float size,
                   const char *utf8, hui_color col, const struct hui_glyph_warp *warp, uint32_t seed) {
    float s = size/(float)f->units_per_em;
    const char *p=utf8; int pen=x;
    while (*p) {
        uint32_t cp=hui_utf8_next(&p);
        hui_glyph *g=hui_glyph_find((hui_glyph_font*)f,cp);
        int adv = g ? g->advance : f->units_per_em/2;
        hui_glyph_draw(f, cp, pen, baseline_y, size, col, warp, seed);
        pen += (int)(adv*s);
    }
    return pen;
}

int hui_glyph_font_save(const hui_glyph_font *f, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "font %s %s\n", f->family, f->style);
    fprintf(fp, "upem %d\n", f->units_per_em);
    fprintf(fp, "metrics ascent=%d descent=%d linegap=%d mono=%d\n",
            f->ascent, f->descent, f->line_gap, f->is_monospace);
    for (int i=0;i<f->nglyph;i++) {
        const hui_glyph *g=&f->glyphs[i];
        fprintf(fp, "glyph U+%04X advance=%d lsb=%d\n", g->codepoint, g->advance, g->lsb);
        for (int j=0;j<g->ncontour;j++) {
            const hui_contour *c=&g->contours[j];
            fprintf(fp, "  contour\n");
            fprintf(fp, "    move %g %g\n", (double)c->start.x, (double)c->start.y);
            for (int k=0;k<c->nseg;k++)
                fprintf(fp, "    curve %g %g %g %g %g %g\n",
                        (double)c->segs[k].c0.x,(double)c->segs[k].c0.y,
                        (double)c->segs[k].c1.x,(double)c->segs[k].c1.y,
                        (double)c->segs[k].a.x, (double)c->segs[k].a.y);
            fprintf(fp, "    close\n");
        }
    }
    fclose(fp);
    return 0;
}

hui_glyph_font *hui_glyph_font_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char line[512], fam[64]="owk", sty[32]="Regular", kw[32];
    if (!fgets(line, sizeof line, fp)) { fclose(fp); return NULL; }
    if (sscanf(line, "font %63s %31s", fam, sty) < 1) { fclose(fp); return NULL; }
    hui_glyph_font *f = hui_glyph_font_new(fam, 1000);
    snprintf(f->style, sizeof f->style, "%s", sty);
    hui_glyph *g=NULL; hui_contour *c=NULL;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, " %31s", kw) != 1) continue;
        if (strcmp(kw,"upem")==0) { sscanf(line, " upem %d", &f->units_per_em); }
        else if (strcmp(kw,"metrics")==0) {
            int as,de,lg,mo;
            if (sscanf(line," metrics ascent=%d descent=%d linegap=%d mono=%d",&as,&de,&lg,&mo)==4)
                { f->ascent=as; f->descent=de; f->line_gap=lg; f->is_monospace=mo; }
        }
        else if (strcmp(kw,"glyph")==0) {
            unsigned cp=0; int adv=-1,lsb=0;
            int got=sscanf(line," glyph U+%X advance=%d lsb=%d",&cp,&adv,&lsb);
            if (got>=1){ g=hui_glyph_add(f,cp); if(got>=2)g->advance=adv; if(got>=3)g->lsb=lsb; c=NULL; }
        }
        else if (strcmp(kw,"contour")==0) { if (g) c=hui_glyph_add_contour(g); }
        else if (strcmp(kw,"move")==0) {
            float x,y; if (c && sscanf(line," move %f %f",&x,&y)==2) c->start=(hui_v2){x,y};
        }
        else if (strcmp(kw,"curve")==0) {
            float v[6];
            if (c && sscanf(line," curve %f %f %f %f %f %f",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6)
                hui_glyph_add_seg(c,(hui_v2){v[0],v[1]},(hui_v2){v[2],v[3]},(hui_v2){v[4],v[5]});
        }
        /* "close" is implicit; no-op */
    }
    fclose(fp);
    return f;
}

#ifdef HUI_GLYPH_IMPORT_TTF
/* Convert stb glyph outlines into our cubic-contour model. Imports ASCII 32..126. */
hui_glyph_font *hui_glyph_import_ttf_mem(const unsigned char *data, int n, const char *family) {
    (void)n;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) return NULL;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, 1.0f);   /* = 1/unitsPerEm */
    int upem = scale > 0.0f ? (int)(1.0f/scale + 0.5f) : 1000;
    hui_glyph_font *f = hui_glyph_font_new(family ? family : "imported", upem);
    int asc=0, desc=0, gap=0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->ascent=asc; f->descent=desc; f->line_gap=gap;

    for (uint32_t cp=32; cp<=126; cp++) {
        int gi = stbtt_FindGlyphIndex(&info, (int)cp);
        if (gi <= 0 && cp != 32) continue;
        hui_glyph *g = hui_glyph_add(f, cp);
        int adv=0, lsb=0; stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        g->advance=adv; g->lsb=lsb;

        stbtt_vertex *verts=NULL;
        int nv = stbtt_GetGlyphShape(&info, gi, &verts);
        hui_contour *c=NULL; hui_v2 prev={0,0};
        for (int i=0;i<nv;i++) {
            hui_v2 to = {(float)verts[i].x, (float)verts[i].y};
            switch (verts[i].type) {
            case STBTT_vmove:
                c = hui_glyph_add_contour(g); c->start = to; prev = to; break;
            case STBTT_vline:
                if (c) hui_glyph_add_seg(c, prev, to, to);   /* straight cubic */
                prev = to; break;
            case STBTT_vcurve: {                              /* quadratic -> cubic */
                hui_v2 q = {(float)verts[i].cx, (float)verts[i].cy};
                hui_v2 c0 = {prev.x + 2.0f/3.0f*(q.x-prev.x), prev.y + 2.0f/3.0f*(q.y-prev.y)};
                hui_v2 c1 = {to.x   + 2.0f/3.0f*(q.x-to.x),   to.y   + 2.0f/3.0f*(q.y-to.y)};
                if (c) hui_glyph_add_seg(c, c0, c1, to);
                prev = to; break;
            }
            case STBTT_vcubic: {
                hui_v2 c0 = {(float)verts[i].cx,  (float)verts[i].cy};
                hui_v2 c1 = {(float)verts[i].cx1, (float)verts[i].cy1};
                if (c) hui_glyph_add_seg(c, c0, c1, to);
                prev = to; break;
            }
            default: break;
            }
        }
        if (verts) stbtt_FreeShape(&info, verts);
    }
    return f;
}
hui_glyph_font *hui_glyph_import_ttf_file(const char *path) {
    FILE *fp=fopen(path,"rb"); if(!fp) return NULL;
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    if (sz<=0){ fclose(fp); return NULL; }
    unsigned char *buf=(unsigned char*)malloc((size_t)sz);
    if(!buf){ fclose(fp); return NULL; }
    size_t rd=fread(buf,1,(size_t)sz,fp); fclose(fp);
    hui_glyph_font *f = (rd==(size_t)sz) ? hui_glyph_import_ttf_mem(buf,(int)sz,path) : NULL;
    free(buf);
    return f;
}
#endif /* HUI_GLYPH_IMPORT_TTF */
#endif /* HUI_GLYPH_IMPLEMENTATION */
#endif /* HUI_GLYPH_H */
