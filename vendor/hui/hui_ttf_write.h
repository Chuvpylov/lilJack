/* hui_ttf_write.h — pure-C TrueType writer. Define HUI_TTF_WRITE_IMPLEMENTATION
 * in one TU (after HUI_GLYPH_IMPLEMENTATION). Depends on hui_glyph.h. */
#ifndef HUI_TTF_WRITE_H
#define HUI_TTF_WRITE_H
#include <stdint.h>
#include <stddef.h>
#include "hui_glyph.h"

typedef struct { hui_v2 ctrl, end; } hui_qseg;
int hui_cubic_to_quad(hui_v2 p0, hui_v2 p1, hui_v2 p2, hui_v2 p3,
                      hui_qseg *out, int max_out, float tol, double *err);

typedef struct { int ok; char err[128]; double max_quad_err; } hui_ttf_write_result;

/* OpenType table data (codepoint-based; writer resolves to glyph IDs internally) */
typedef struct { uint32_t left_cp, right_cp; int delta; } HuiKernPair;
typedef struct { const uint32_t *seq_cp; int seq_len; uint32_t result_cp; } HuiLigature;
typedef struct { uint32_t from_cp, to_cp; } HuiSubst;

hui_ttf_write_result hui_ttf_write(const hui_glyph_font *f, const char *path);
hui_ttf_write_result hui_ttf_write_mem(const hui_glyph_font *f, uint8_t **out, size_t *out_len);
hui_ttf_write_result hui_ttf_write_ex(const hui_glyph_font *f, const char *path,
    const HuiKernPair *kern, int nkern,
    const HuiLigature *ligs, int nligs,
    const HuiSubst *subs, int nsubs);
hui_ttf_write_result hui_ttf_write_mem_ex(const hui_glyph_font *f,
    const HuiKernPair *kern, int nkern,
    const HuiLigature *ligs, int nligs,
    const HuiSubst *subs, int nsubs,
    uint8_t **out, size_t *out_len);

#ifdef HUI_TTF_WRITE_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* sample deviation between a cubic and a single quad with control qc */
static double hui__cubic_quad_err(hui_v2 p0,hui_v2 p1,hui_v2 p2,hui_v2 p3,hui_v2 qc){
    double m=0;
    for (int k=1;k<4;k++){ double t=k/4.0,u=1-t;
        double cx=u*u*u*p0.x+3*u*u*t*p1.x+3*u*t*t*p2.x+t*t*t*p3.x;
        double cy=u*u*u*p0.y+3*u*u*t*p1.y+3*u*t*t*p2.y+t*t*t*p3.y;
        double qx=u*u*p0.x+2*u*t*qc.x+t*t*p3.x;
        double qy=u*u*p0.y+2*u*t*qc.y+t*t*p3.y;
        double d=sqrt((cx-qx)*(cx-qx)+(cy-qy)*(cy-qy)); if(d>m)m=d;
    }
    return m;
}
static int hui__c2q_rec(hui_v2 p0,hui_v2 p1,hui_v2 p2,hui_v2 p3,
                        hui_qseg *out,int max_out,int *n,float tol,double *err,int depth){
    hui_v2 qc={ (3*(p1.x+p2.x)-(p0.x+p3.x))/4, (3*(p1.y+p2.y)-(p0.y+p3.y))/4 };
    double e=hui__cubic_quad_err(p0,p1,p2,p3,qc);
    if ((e<=tol || depth>=6) && *n<max_out){ out[*n].ctrl=qc; out[*n].end=p3; (*n)++;
        if(e>*err)*err=e; return 1; }
    if (*n>=max_out) return 0;
    hui_v2 p01={(p0.x+p1.x)/2,(p0.y+p1.y)/2},p12={(p1.x+p2.x)/2,(p1.y+p2.y)/2},
           p23={(p2.x+p3.x)/2,(p2.y+p3.y)/2};
    hui_v2 a={(p01.x+p12.x)/2,(p01.y+p12.y)/2},b={(p12.x+p23.x)/2,(p12.y+p23.y)/2};
    hui_v2 m={(a.x+b.x)/2,(a.y+b.y)/2};
    return hui__c2q_rec(p0,p01,a,m,out,max_out,n,tol,err,depth+1)
        && hui__c2q_rec(m,b,p23,p3,out,max_out,n,tol,err,depth+1);
}
int hui_cubic_to_quad(hui_v2 p0,hui_v2 p1,hui_v2 p2,hui_v2 p3,
                      hui_qseg *out,int max_out,float tol,double *err){
    int n=0; double e=0; hui__c2q_rec(p0,p1,p2,p3,out,max_out,&n,tol,&e,0);
    if(err)*err=e; return n;
}

/* ---- big-endian growable byte buffer ---- */
typedef struct { uint8_t *d; size_t n, cap; } huittf_buf;
static void hb_need(huittf_buf*b,size_t k){
    if(b->n+k>b->cap){ size_t nc=b->cap?b->cap*2:1024; while(nc<b->n+k)nc*=2;
        b->d=(uint8_t*)realloc(b->d,nc); b->cap=nc; } }
static void hb_u8 (huittf_buf*b,uint32_t v){ hb_need(b,1); b->d[b->n++]=(uint8_t)(v&0xffu); }
static void hb_u16(huittf_buf*b,uint32_t v){ hb_u8(b,v>>8); hb_u8(b,v); }
static void hb_u32(huittf_buf*b,uint32_t v){ hb_u8(b,v>>24); hb_u8(b,v>>16); hb_u8(b,v>>8); hb_u8(b,v); }
static void hb_i16(huittf_buf*b,int v){ hb_u16(b,(uint32_t)(v & 0xffff)); }
static void hb_bytes(huittf_buf*b,const void*p,size_t k){ hb_need(b,k); memcpy(b->d+b->n,p,k); b->n+=k; }
static void hb_pad4(huittf_buf*b){ while(b->n & 3u) hb_u8(b,0); }
static uint32_t hb_checksum(const uint8_t*d,size_t n){
    uint32_t s=0; size_t i=0;
    for(;i+4<=n;i+=4) s+=((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|((uint32_t)d[i+2]<<8)|d[i+3];
    if(i<n){ uint32_t last=0; int sh=24; for(;i<n;i++){ last|=(uint32_t)d[i]<<sh; sh-=8; } s+=last; }
    return s;
}
static void hb_utf16be(huittf_buf*b,const char*s){ for(;*s;s++){ hb_u8(b,0); hb_u8(b,(uint8_t)*s); } }
static size_t utf16len(const char*s){ size_t n=0; while(*s++)n+=2; return n; }

#define HTW_MAXC 64
#define HTW_MAXP 8192
typedef struct { int16_t x,y; uint8_t on; } HtwPt;

static int htw_build_points(const hui_glyph*g,float tol,HtwPt*pts,int*npts,
                            uint16_t*endpts,int*ncont,double*maxerr,char*err){
    int np=0,nc=0;
    for(int ci=0;ci<g->ncontour;ci++){
        const hui_contour*c=&g->contours[ci];
        int cstart=np;
        if(np>=HTW_MAXP){ snprintf(err,128,"too many points"); return 0; }
        pts[np].x=(int16_t)lroundf(c->start.x); pts[np].y=(int16_t)lroundf(c->start.y); pts[np].on=1; np++;
        hui_v2 prev=c->start;
        for(int j=0;j<c->nseg;j++){
            hui_qseg q[64]; double e=0;
            int nq=hui_cubic_to_quad(prev,c->segs[j].c0,c->segs[j].c1,c->segs[j].a,q,64,tol,&e);
            if(e>*maxerr)*maxerr=e;
            for(int k=0;k<nq;k++){
                if(np+2>=HTW_MAXP){ snprintf(err,128,"too many points"); return 0; }
                pts[np].x=(int16_t)lroundf(q[k].ctrl.x); pts[np].y=(int16_t)lroundf(q[k].ctrl.y); pts[np].on=0; np++;
                pts[np].x=(int16_t)lroundf(q[k].end.x);  pts[np].y=(int16_t)lroundf(q[k].end.y);  pts[np].on=1; np++;
            }
            prev=c->segs[j].a;
        }
        if(np-1>cstart && pts[np-1].on && pts[np-1].x==pts[cstart].x && pts[np-1].y==pts[cstart].y) np--;
        if(nc>=HTW_MAXC){ snprintf(err,128,"too many contours"); return 0; }
        if(np-1<cstart){ continue; } /* empty contour, skip */
        endpts[nc++]=(uint16_t)(np-1);
    }
    *npts=np; *ncont=nc; return 1;
}

/* ---- codepoint -> 1-based glyph ID ---- */
static uint16_t htw_cp_to_gid(const hui_glyph_font *f, uint32_t cp) {
    for (int i = 0; i < f->nglyph; i++)
        if (f->glyphs[i].codepoint == cp) return (uint16_t)(i+1);
    return 0;
}

/* ---- GPOS PairPosFormat1: explicit kern pairs ---- */
static huittf_buf htw_build_gpos(const hui_glyph_font *f,
                                  const HuiKernPair *kern, int nkern) {
    huittf_buf res = {0};
    if (nkern == 0) return res;

    /* collect unique sorted left GIDs */
    uint16_t lgids[512]; int nl = 0;
    for (int i = 0; i < nkern && nl < 512; i++) {
        uint16_t g = htw_cp_to_gid(f, kern[i].left_cp);
        if (!g) continue;
        int dup = 0;
        for (int j = 0; j < nl; j++) if (lgids[j]==g) { dup=1; break; }
        if (!dup) lgids[nl++] = g;
    }
    for (int a=0;a<nl-1;a++) for (int b=a+1;b<nl;b++)
        if (lgids[b]<lgids[a]) { uint16_t t=lgids[a]; lgids[a]=lgids[b]; lgids[b]=t; }

    /* build one huittf_buf per unique left GID */
    huittf_buf *psets = (huittf_buf*)calloc((size_t)nl, sizeof *psets);
    typedef struct { uint16_t rgid; int16_t delta; } PRec;
    for (int li = 0; li < nl; li++) {
        PRec recs[512]; int nr = 0;
        for (int i = 0; i < nkern && nr < 512; i++) {
            if (htw_cp_to_gid(f, kern[i].left_cp) != lgids[li]) continue;
            uint16_t rg = htw_cp_to_gid(f, kern[i].right_cp);
            if (!rg) continue;
            recs[nr++] = (PRec){rg, (int16_t)kern[i].delta};
        }
        for (int a=0;a<nr-1;a++) for (int b=a+1;b<nr;b++)
            if (recs[b].rgid<recs[a].rgid) { PRec t=recs[a]; recs[a]=recs[b]; recs[b]=t; }
        hb_u16(&psets[li], (uint32_t)nr);
        for (int i = 0; i < nr; i++) {
            hb_u16(&psets[li], recs[i].rgid);
            hb_i16(&psets[li], (int)recs[i].delta);
        }
    }

    /* PairPos layout:
       header = 10 + nl*2  bytes  (PosFormat+CovOff+VF1+VF2+Count + nl offsets)
       coverage = 2+2+nl*2 bytes  (immediately after header)
       pairsets follow coverage */
    int pp_hdr  = 10 + nl*2;
    int cov_sz  = 2 + 2 + nl*2;
    int cur     = pp_hdr + cov_sz;
    int *psoff  = (int*)calloc((size_t)nl, sizeof *psoff);
    for (int i = 0; i < nl; i++) { psoff[i]=cur; cur+=(int)psets[i].n; }

    huittf_buf pp = {0};
    hb_u16(&pp, 1);               /* PosFormat */
    hb_u16(&pp, (uint32_t)pp_hdr); /* CoverageOffset */
    hb_u16(&pp, 4);               /* ValueFormat1 = XAdvance */
    hb_u16(&pp, 0);               /* ValueFormat2 = none */
    hb_u16(&pp, (uint32_t)nl);   /* PairSetCount */
    for (int i=0;i<nl;i++) hb_u16(&pp,(uint32_t)psoff[i]);
    /* Coverage format 1 */
    hb_u16(&pp,1); hb_u16(&pp,(uint32_t)nl);
    for (int i=0;i<nl;i++) hb_u16(&pp, lgids[i]);
    for (int i=0;i<nl;i++) { hb_bytes(&pp,psets[i].d,psets[i].n); free(psets[i].d); }
    free(psets); free(psoff);

    /* Lookup type 2 (PairPos): 8-byte header, subtable at offset 8 */
    huittf_buf lk = {0};
    hb_u16(&lk,2); hb_u16(&lk,0); hb_u16(&lk,1); hb_u16(&lk,8);
    hb_bytes(&lk,pp.d,pp.n); free(pp.d);

    /* LookupList: count(2)+offset(2)=4 bytes, lookup at 4 */
    huittf_buf ll = {0};
    hb_u16(&ll,1); hb_u16(&ll,4);
    hb_bytes(&ll,lk.d,lk.n); free(lk.d);

    /* FeatureTable: params(2)+count(2)+idx(2)=6 bytes */
    huittf_buf ft = {0};
    hb_u16(&ft,0); hb_u16(&ft,1); hb_u16(&ft,0);

    /* FeatureList: count(2)+record(tag4+off2)=8 bytes, feature at 8 */
    huittf_buf fl = {0};
    hb_u16(&fl,1); hb_bytes(&fl,"kern",4); hb_u16(&fl,8);
    hb_bytes(&fl,ft.d,ft.n); free(ft.d);

    /* ScriptList: count(2)+record(tag4+off2)=8 bytes;
       ScriptTable: defLS(2)+n(2)=4 bytes at +8;
       LangSys: lookupOrder(2)+reqFeat(2)+count(2)+idx(2)=8 bytes at +12 */
    huittf_buf sl = {0};
    hb_u16(&sl,1); hb_bytes(&sl,"DFLT",4); hb_u16(&sl,8);
    hb_u16(&sl,4); hb_u16(&sl,0);
    hb_u16(&sl,0); hb_u16(&sl,0xFFFF); hb_u16(&sl,1); hb_u16(&sl,0);

    /* GPOS header (10 bytes): version + scriptList + featureList + lookupList */
    int sl_off=10, fl_off=sl_off+(int)sl.n, ll_off=fl_off+(int)fl.n;
    hb_u32(&res,0x00010000);
    hb_u16(&res,(uint32_t)sl_off);
    hb_u16(&res,(uint32_t)fl_off);
    hb_u16(&res,(uint32_t)ll_off);
    hb_bytes(&res,sl.d,sl.n); free(sl.d);
    hb_bytes(&res,fl.d,fl.n); free(fl.d);
    hb_bytes(&res,ll.d,ll.n); free(ll.d);
    return res;
}

/* ---- GSUB: SingleSubst (type 1 fmt 2) + LigatureSubst (type 4) ---- */
static huittf_buf htw_build_gsub(const hui_glyph_font *f,
                                  const HuiLigature *ligs, int nligs,
                                  const HuiSubst *subs, int nsubs) {
    huittf_buf res = {0};
    if (nligs == 0 && nsubs == 0) return res;

    huittf_buf lookups[2]; int lk_n = 0;

    /* -- SingleSubst lookup (type 1, format 2) -- */
    if (nsubs > 0) {
        uint16_t fgids[512], tgids[512]; int ns2=0;
        for (int i=0;i<nsubs&&ns2<512;i++) {
            uint16_t fg=htw_cp_to_gid(f,subs[i].from_cp);
            uint16_t tg=htw_cp_to_gid(f,subs[i].to_cp);
            if (fg&&tg) { fgids[ns2]=fg; tgids[ns2]=tg; ns2++; }
        }
        for (int a=0;a<ns2-1;a++) for (int b=a+1;b<ns2;b++)
            if (fgids[b]<fgids[a]) { uint16_t t=fgids[a];fgids[a]=fgids[b];fgids[b]=t;
                                       t=tgids[a];tgids[a]=tgids[b];tgids[b]=t; }
        /* Format2: fmt(2)+cov(2)+count(2)+subst(ns2*2), then coverage */
        int cov_off_ss = 6 + ns2*2;
        huittf_buf ss = {0};
        hb_u16(&ss,2); hb_u16(&ss,(uint32_t)cov_off_ss); hb_u16(&ss,(uint32_t)ns2);
        for (int i=0;i<ns2;i++) hb_u16(&ss,tgids[i]);
        hb_u16(&ss,1); hb_u16(&ss,(uint32_t)ns2);
        for (int i=0;i<ns2;i++) hb_u16(&ss,fgids[i]);

        lookups[lk_n]=(huittf_buf){0};
        hb_u16(&lookups[lk_n],1); hb_u16(&lookups[lk_n],0);
        hb_u16(&lookups[lk_n],1); hb_u16(&lookups[lk_n],8);
        hb_bytes(&lookups[lk_n],ss.d,ss.n); free(ss.d);
        lk_n++;
    }

    /* -- LigatureSubst lookup (type 4) -- */
    if (nligs > 0) {
        uint16_t first_gids[256]; int nfirst=0;
        for (int i=0;i<nligs&&nfirst<256;i++) {
            if (ligs[i].seq_len<2) continue;
            uint16_t g=htw_cp_to_gid(f,ligs[i].seq_cp[0]);
            if (!g) continue;
            int dup=0; for (int j=0;j<nfirst;j++) if(first_gids[j]==g){dup=1;break;}
            if (!dup) first_gids[nfirst++]=g;
        }
        for (int a=0;a<nfirst-1;a++) for (int b=a+1;b<nfirst;b++)
            if (first_gids[b]<first_gids[a]) { uint16_t t=first_gids[a];first_gids[a]=first_gids[b];first_gids[b]=t; }

        huittf_buf *lsets=(huittf_buf*)calloc((size_t)nfirst,sizeof *lsets);
        for (int fi=0;fi<nfirst;fi++) {
            /* gather ligatures for this first GID, build Ligature records */
            huittf_buf lrecs[64]; int cnt=0;
            for (int i=0;i<nligs&&cnt<64;i++) {
                if (ligs[i].seq_len<2) continue;
                if (htw_cp_to_gid(f,ligs[i].seq_cp[0])!=first_gids[fi]) continue;
                uint16_t out_g=htw_cp_to_gid(f,ligs[i].result_cp);
                lrecs[cnt]=(huittf_buf){0};
                hb_u16(&lrecs[cnt],out_g?out_g:0);
                hb_u16(&lrecs[cnt],(uint32_t)(ligs[i].seq_len-1));
                for (int j=1;j<ligs[i].seq_len;j++) hb_u16(&lrecs[cnt],htw_cp_to_gid(f,ligs[i].seq_cp[j]));
                cnt++;
            }
            /* LigatureSet: count(2)+offsets(cnt*2), then records */
            int ls_hdr=2+cnt*2;
            hb_u16(&lsets[fi],(uint32_t)cnt);
            int off=ls_hdr;
            for (int i=0;i<cnt;i++) { hb_u16(&lsets[fi],(uint32_t)off); off+=(int)lrecs[i].n; }
            for (int i=0;i<cnt;i++) { hb_bytes(&lsets[fi],lrecs[i].d,lrecs[i].n); free(lrecs[i].d); }
        }

        /* LigSubstFormat1: fmt(2)+cov(2)+count(2)+offsets(nfirst*2), coverage, sets */
        int ls_hdr_sz=6+nfirst*2;
        int cov_sz_ls=2+2+nfirst*2;
        huittf_buf lssub={0};
        hb_u16(&lssub,1); hb_u16(&lssub,(uint32_t)ls_hdr_sz); hb_u16(&lssub,(uint32_t)nfirst);
        int cur=ls_hdr_sz+cov_sz_ls;
        for (int i=0;i<nfirst;i++) { hb_u16(&lssub,(uint32_t)cur); cur+=(int)lsets[i].n; }
        hb_u16(&lssub,1); hb_u16(&lssub,(uint32_t)nfirst);
        for (int i=0;i<nfirst;i++) hb_u16(&lssub,first_gids[i]);
        for (int i=0;i<nfirst;i++) { hb_bytes(&lssub,lsets[i].d,lsets[i].n); free(lsets[i].d); }
        free(lsets);

        lookups[lk_n]=(huittf_buf){0};
        hb_u16(&lookups[lk_n],4); hb_u16(&lookups[lk_n],0);
        hb_u16(&lookups[lk_n],1); hb_u16(&lookups[lk_n],8);
        hb_bytes(&lookups[lk_n],lssub.d,lssub.n); free(lssub.d);
        lk_n++;
    }

    /* FeatureTable pointing to all lookups */
    huittf_buf ft={0};
    hb_u16(&ft,0); hb_u16(&ft,(uint32_t)lk_n);
    for (int i=0;i<lk_n;i++) hb_u16(&ft,(uint32_t)i);

    huittf_buf fl={0};
    hb_u16(&fl,1); hb_bytes(&fl,"liga",4); hb_u16(&fl,8);
    hb_bytes(&fl,ft.d,ft.n); free(ft.d);

    huittf_buf sl={0};
    hb_u16(&sl,1); hb_bytes(&sl,"DFLT",4); hb_u16(&sl,8);
    hb_u16(&sl,4); hb_u16(&sl,0);
    hb_u16(&sl,0); hb_u16(&sl,0xFFFF); hb_u16(&sl,1); hb_u16(&sl,0);

    /* LookupList */
    huittf_buf ll={0};
    hb_u16(&ll,(uint32_t)lk_n);
    int lk_hdr=2+lk_n*2, lk_cur=lk_hdr;
    for (int i=0;i<lk_n;i++) { hb_u16(&ll,(uint32_t)lk_cur); lk_cur+=(int)lookups[i].n; }
    for (int i=0;i<lk_n;i++) { hb_bytes(&ll,lookups[i].d,lookups[i].n); free(lookups[i].d); }

    int sl_off=10, fl_off=sl_off+(int)sl.n, ll_off=fl_off+(int)fl.n;
    hb_u32(&res,0x00010000);
    hb_u16(&res,(uint32_t)sl_off); hb_u16(&res,(uint32_t)fl_off); hb_u16(&res,(uint32_t)ll_off);
    hb_bytes(&res,sl.d,sl.n); free(sl.d);
    hb_bytes(&res,fl.d,fl.n); free(fl.d);
    hb_bytes(&res,ll.d,ll.n); free(ll.d);
    return res;
}

hui_ttf_write_result hui_ttf_write_mem(const hui_glyph_font *f, uint8_t **out, size_t *out_len) {
    return hui_ttf_write_mem_ex(f, NULL,0,NULL,0,NULL,0, out, out_len);
}

hui_ttf_write_result hui_ttf_write_mem_ex(const hui_glyph_font *f,
    const HuiKernPair *kern, int nkern,
    const HuiLigature *ligs, int nligs,
    const HuiSubst *subs, int nsubs,
    uint8_t **out, size_t *out_len){
    hui_ttf_write_result R; R.ok=0; R.err[0]=0; R.max_quad_err=0;
    if(!f){ snprintf(R.err,sizeof R.err,"null font"); return R; }
    int upem = f->units_per_em>0?f->units_per_em:1000;
    float tol = (float)upem/200.0f;
    int numGlyphs = 1 + f->nglyph;   /* index 0 = .notdef */

    /* ---- glyf + loca (long), single pass ---- */
    huittf_buf glyf={0}; uint32_t *loca=(uint32_t*)calloc((size_t)numGlyphs+1,sizeof(uint32_t));
    int gMinX=32767,gMinY=32767,gMaxX=-32768,gMaxY=-32768;
    int maxPoints=0,maxContours=0;
    loca[0]=0; /* glyph 0 .notdef: empty (loca[0]==loca[1]) */
    for(int gi=0; gi<f->nglyph; gi++){
        loca[gi+1]=(uint32_t)glyf.n;            /* offset = current end of glyf */
        const hui_glyph*g=&f->glyphs[gi];
        HtwPt pts[HTW_MAXP]; uint16_t endpts[HTW_MAXC]; int np=0,nc=0;
        if(!htw_build_points(g,tol,pts,&np,endpts,&nc,&R.max_quad_err,R.err)){ free(loca); free(glyf.d); return R; }
        if(nc==0){ continue; }                  /* empty glyph: no glyf bytes */
        int xmn=32767,ymn=32767,xmx=-32768,ymx=-32768;
        for(int i=0;i<np;i++){ if(pts[i].x<xmn)xmn=pts[i].x; if(pts[i].y<ymn)ymn=pts[i].y;
            if(pts[i].x>xmx)xmx=pts[i].x; if(pts[i].y>ymx)ymx=pts[i].y; }
        if(xmn<gMinX)gMinX=xmn; if(ymn<gMinY)gMinY=ymn; if(xmx>gMaxX)gMaxX=xmx; if(ymx>gMaxY)gMaxY=ymx;
        if(np>maxPoints)maxPoints=np; if(nc>maxContours)maxContours=nc;
        hb_u16(&glyf,(uint32_t)nc);
        hb_i16(&glyf,xmn); hb_i16(&glyf,ymn); hb_i16(&glyf,xmx); hb_i16(&glyf,ymx);
        for(int i=0;i<nc;i++) hb_u16(&glyf,endpts[i]);
        hb_u16(&glyf,0);                        /* instructionLength */
        for(int i=0;i<np;i++) hb_u8(&glyf, pts[i].on?0x01u:0x00u); /* flags: int16 deltas */
        int prevx=0; for(int i=0;i<np;i++){ hb_i16(&glyf, pts[i].x-prevx); prevx=pts[i].x; }
        int prevy=0; for(int i=0;i<np;i++){ hb_i16(&glyf, pts[i].y-prevy); prevy=pts[i].y; }
        if(glyf.n & 1u) hb_u8(&glyf,0);         /* pad glyph to even length */
    }
    loca[numGlyphs]=(uint32_t)glyf.n;
    if(gMinX>gMaxX){ gMinX=gMinY=0; gMaxX=gMaxY=0; }

    huittf_buf locab={0};
    for(int i=0;i<=numGlyphs;i++) hb_u32(&locab, loca[i]);

    /* ---- hmtx + advance stats ---- */
    huittf_buf hmtx={0}; int advMax=0; long advSum=0;
    hb_u16(&hmtx,(uint32_t)(upem/2)); hb_i16(&hmtx,0); /* .notdef */
    advMax=upem/2; advSum=upem/2;
    for(int gi=0;gi<f->nglyph;gi++){
        int adv=f->glyphs[gi].advance, lsb=f->glyphs[gi].lsb;
        hb_u16(&hmtx,(uint32_t)adv); hb_i16(&hmtx,lsb);
        if(adv>advMax)advMax=adv; advSum+=adv;
    }
    int xAvg=(int)(advSum/numGlyphs);

    /* ---- cmap (format 4) ---- */
    huittf_buf cmap={0};
    /* collect sorted (cp,gid) pairs */
    typedef struct { uint32_t cp; uint16_t gid; } Pair;
    Pair *pr=(Pair*)calloc((size_t)f->nglyph+1,sizeof(Pair)); int npr=0;
    for(int gi=0;gi<f->nglyph;gi++){ uint32_t cp=f->glyphs[gi].codepoint;
        if(cp==0||cp>0xFFFE) continue; pr[npr].cp=cp; pr[npr].gid=(uint16_t)(gi+1); npr++; }
    for(int a=0;a<npr-1;a++) for(int b=a+1;b<npr;b++) if(pr[b].cp<pr[a].cp){ Pair t=pr[a];pr[a]=pr[b];pr[b]=t; }
    int firstCP = npr?(int)pr[0].cp:0, lastCP = npr?(int)pr[npr-1].cp:0;
    int segCount = npr+1; /* +final 0xFFFF */
    /* searchRange = 2*2^floor(log2 segCount); entrySelector = floor(log2 segCount) */
    int sr=1, es=0; while(sr*2<=segCount){ sr*=2; es++; } sr*=2;
    int searchRange4=sr, entrySel4=es, rangeShift4=segCount*2-searchRange4;
    hb_u16(&cmap,0); hb_u16(&cmap,1);              /* version, numTables */
    hb_u16(&cmap,3); hb_u16(&cmap,1); hb_u32(&cmap,12); /* platform3 enc1 offset12 */
    /* format4 subtable */
    huittf_buf sub={0};
    hb_u16(&sub,4); /* format */
    /* length: header(14) + reservedPad(2) + 4 arrays * segCount * 2 */
    int subLen = 14 + 2 + segCount*8;
    hb_u16(&sub,(uint32_t)subLen);
    hb_u16(&sub,0); /* language */
    hb_u16(&sub,(uint32_t)(segCount*2));
    hb_u16(&sub,(uint32_t)searchRange4); hb_u16(&sub,(uint32_t)entrySel4); hb_u16(&sub,(uint32_t)rangeShift4);
    for(int i=0;i<npr;i++) hb_u16(&sub,pr[i].cp); hb_u16(&sub,0xFFFF);   /* endCode + final */
    hb_u16(&sub,0); /* reservedPad */
    for(int i=0;i<npr;i++) hb_u16(&sub,pr[i].cp); hb_u16(&sub,0xFFFF);   /* startCode */
    for(int i=0;i<npr;i++) hb_i16(&sub,(int)((pr[i].gid - pr[i].cp) & 0xffff)); hb_u16(&sub,1); /* idDelta */
    for(int i=0;i<segCount;i++) hb_u16(&sub,0);                          /* idRangeOffset */
    hb_bytes(&cmap,sub.d,sub.n); free(sub.d); free(pr);

    /* ---- maxp ---- */
    huittf_buf maxp={0};
    hb_u32(&maxp,0x00010000); hb_u16(&maxp,(uint32_t)numGlyphs);
    hb_u16(&maxp,(uint32_t)maxPoints); hb_u16(&maxp,(uint32_t)maxContours);
    hb_u16(&maxp,0); hb_u16(&maxp,0); hb_u16(&maxp,2); hb_u16(&maxp,0);
    hb_u16(&maxp,0); hb_u16(&maxp,0); hb_u16(&maxp,0); hb_u16(&maxp,0);
    hb_u16(&maxp,0); hb_u16(&maxp,0); hb_u16(&maxp,0);

    /* ---- head (checkSumAdjustment patched later) ---- */
    huittf_buf head={0};
    hb_u32(&head,0x00010000); hb_u32(&head,0x00010000); /* version, fontRevision */
    hb_u32(&head,0);                 /* checkSumAdjustment (patched) */
    hb_u32(&head,0x5F0F3CF5);        /* magic */
    hb_u16(&head,0x000B);            /* flags */
    hb_u16(&head,(uint32_t)upem);
    hb_u32(&head,0); hb_u32(&head,0); /* created (LONGDATETIME=0) */
    hb_u32(&head,0); hb_u32(&head,0); /* modified */
    hb_i16(&head,gMinX); hb_i16(&head,gMinY); hb_i16(&head,gMaxX); hb_i16(&head,gMaxY);
    hb_u16(&head,0);                 /* macStyle */
    hb_u16(&head,8);                 /* lowestRecPPEM */
    hb_u16(&head,2);                 /* fontDirectionHint */
    hb_u16(&head,1);                 /* indexToLocFormat = long */
    hb_u16(&head,0);                 /* glyphDataFormat */

    /* ---- hhea ---- */
    huittf_buf hhea={0};
    hb_u32(&hhea,0x00010000);
    hb_i16(&hhea,f->ascent); hb_i16(&hhea,f->descent); hb_i16(&hhea,f->line_gap);
    hb_u16(&hhea,(uint32_t)advMax);
    hb_i16(&hhea,0); hb_i16(&hhea,0); hb_i16(&hhea,gMaxX); /* minLSB,minRSB,xMaxExtent */
    hb_i16(&hhea,1); hb_i16(&hhea,0); hb_i16(&hhea,0);     /* caretSlopeRise/Run/Offset */
    hb_i16(&hhea,0); hb_i16(&hhea,0); hb_i16(&hhea,0); hb_i16(&hhea,0); /* reserved */
    hb_i16(&hhea,0);                 /* metricDataFormat */
    hb_u16(&hhea,(uint32_t)numGlyphs); /* numberOfHMetrics */

    /* ---- name ---- */
    huittf_buf name={0};
    char full[160]; snprintf(full,sizeof full,"%s %s",f->family,f->style);
    char ps[160]; { int o=0; for(const char*s=f->family;*s&&o<158;s++) if(*s!=' ')ps[o++]=*s;
        if(o<158)ps[o++]='-'; for(const char*s=f->style;*s&&o<159;s++) if(*s!=' ')ps[o++]=*s; ps[o]=0; }
    struct { uint16_t id; const char*s; } nr[]={{1,f->family},{2,f->style},{4,full},{6,ps}};
    int nn=4;
    hb_u16(&name,0); hb_u16(&name,(uint32_t)nn); hb_u16(&name,(uint32_t)(6+12*nn));
    size_t soff=0;
    for(int i=0;i<nn;i++){ size_t L=utf16len(nr[i].s);
        hb_u16(&name,3); hb_u16(&name,1); hb_u16(&name,0x409); hb_u16(&name,nr[i].id);
        hb_u16(&name,(uint32_t)L); hb_u16(&name,(uint32_t)soff); soff+=L; }
    for(int i=0;i<nn;i++) hb_utf16be(&name,nr[i].s);

    /* ---- post v3.0 ---- */
    huittf_buf post={0};
    hb_u32(&post,0x00030000); hb_u32(&post,0); /* version, italicAngle */
    hb_i16(&post,-100); hb_u16(&post,50);       /* underlinePos, underlineThickness */
    hb_u32(&post, f->is_monospace?1u:0u);       /* isFixedPitch */
    hb_u32(&post,0); hb_u32(&post,0); hb_u32(&post,0); hb_u32(&post,0);

    /* ---- OS/2 v4 ---- */
    huittf_buf os2={0};
    hb_u16(&os2,4);                       /* version */
    hb_i16(&os2,xAvg);                    /* xAvgCharWidth */
    hb_u16(&os2,400);                     /* usWeightClass */
    hb_u16(&os2,5);                       /* usWidthClass */
    hb_u16(&os2,0);                       /* fsType */
    hb_i16(&os2,650); hb_i16(&os2,600); hb_i16(&os2,0); hb_i16(&os2,75);    /* subscript */
    hb_i16(&os2,650); hb_i16(&os2,600); hb_i16(&os2,0); hb_i16(&os2,350);   /* superscript */
    hb_i16(&os2,50); hb_i16(&os2,250);    /* strikeout size/pos */
    hb_i16(&os2,0);                       /* sFamilyClass */
    for(int i=0;i<10;i++) hb_u8(&os2,0);  /* panose */
    hb_u32(&os2,0); hb_u32(&os2,0); hb_u32(&os2,0); hb_u32(&os2,0); /* unicode ranges */
    hb_bytes(&os2,"OWK ",4);              /* achVendID */
    hb_u16(&os2,0x40);                    /* fsSelection = regular */
    hb_u16(&os2,(uint32_t)firstCP); hb_u16(&os2,(uint32_t)lastCP);
    hb_i16(&os2,f->ascent); hb_i16(&os2,f->descent); hb_i16(&os2,f->line_gap); /* typo */
    hb_u16(&os2,(uint32_t)f->ascent); hb_u16(&os2,(uint32_t)(-f->descent));    /* win asc/desc */
    hb_u32(&os2,1); hb_u32(&os2,0);       /* codepage ranges (Latin1) */
    hb_i16(&os2,500); hb_i16(&os2,700);   /* sxHeight, sCapHeight */
    hb_u16(&os2,0); hb_u16(&os2,32);      /* defaultChar, breakChar */
    hb_u16(&os2,0);                       /* usMaxContext */

    /* ---- GPOS + GSUB (optional) ---- */
    huittf_buf gpos_b = htw_build_gpos(f, kern, nkern);
    huittf_buf gsub_b = htw_build_gsub(f, ligs, nligs, subs, nsubs);

    /* ---- assemble (filter empty optional tables) ---- */
    typedef struct { char tag[4]; huittf_buf *b; } Tbl;
    Tbl all_tabs[]={ {{'G','P','O','S'},&gpos_b}, {{'G','S','U','B'},&gsub_b},
                     {{'O','S','/','2'},&os2}, {{'c','m','a','p'},&cmap}, {{'g','l','y','f'},&glyf},
                     {{'h','e','a','d'},&head}, {{'h','h','e','a'},&hhea}, {{'h','m','t','x'},&hmtx},
                     {{'l','o','c','a'},&locab}, {{'m','a','x','p'},&maxp}, {{'n','a','m','e'},&name},
                     {{'p','o','s','t'},&post} };
    Tbl tabs[12]; int nt=0;
    for(int i=0;i<12;i++) if(all_tabs[i].b->n>0) tabs[nt++]=all_tabs[i];
    for(int a=0;a<nt-1;a++) for(int b=a+1;b<nt;b++)
        if(memcmp(tabs[b].tag,tabs[a].tag,4)<0){ Tbl t=tabs[a]; tabs[a]=tabs[b]; tabs[b]=t; }
    uint32_t off=(uint32_t)(12+16*nt);
    uint32_t toff[12]; int head_idx=-1;
    for(int i=0;i<nt;i++){ toff[i]=off; if(memcmp(tabs[i].tag,"head",4)==0) head_idx=i;
        off+=(uint32_t)((tabs[i].b->n+3)&~(size_t)3); }

    huittf_buf file={0};
    hb_u32(&file,0x00010000); hb_u16(&file,(uint32_t)nt);
    int sr2=1,es2=0; while(sr2*2<=nt){ sr2*=2; es2++; }
    hb_u16(&file,(uint32_t)(sr2*16)); hb_u16(&file,(uint32_t)es2); hb_u16(&file,(uint32_t)(nt*16-sr2*16));
    for(int i=0;i<nt;i++){ hb_bytes(&file,tabs[i].tag,4);
        hb_u32(&file,hb_checksum(tabs[i].b->d,tabs[i].b->n));
        hb_u32(&file,toff[i]); hb_u32(&file,(uint32_t)tabs[i].b->n); }
    uint32_t head_off=0;
    for(int i=0;i<nt;i++){ if(i==head_idx) head_off=(uint32_t)file.n;
        hb_bytes(&file,tabs[i].b->d,tabs[i].b->n); hb_pad4(&file); }

    uint32_t adj = 0xB1B0AFBAu - hb_checksum(file.d,file.n);
    file.d[head_off+8]=(uint8_t)(adj>>24); file.d[head_off+9]=(uint8_t)(adj>>16);
    file.d[head_off+10]=(uint8_t)(adj>>8); file.d[head_off+11]=(uint8_t)adj;

    free(glyf.d); free(loca); free(locab.d); free(hmtx.d); free(cmap.d);
    free(maxp.d); free(head.d); free(hhea.d); free(name.d); free(post.d); free(os2.d);
    free(gpos_b.d); free(gsub_b.d);

    *out=file.d; *out_len=file.n; R.ok=1; return R;
}

hui_ttf_write_result hui_ttf_write(const hui_glyph_font *f, const char *path){
    return hui_ttf_write_ex(f, path, NULL,0,NULL,0,NULL,0);
}

hui_ttf_write_result hui_ttf_write_ex(const hui_glyph_font *f, const char *path,
    const HuiKernPair *kern, int nkern,
    const HuiLigature *ligs, int nligs,
    const HuiSubst *subs, int nsubs) {
    uint8_t *buf=NULL; size_t len=0;
    hui_ttf_write_result r = hui_ttf_write_mem_ex(f,kern,nkern,ligs,nligs,subs,nsubs,&buf,&len);
    if(!r.ok) return r;
    FILE *fp=fopen(path,"wb");
    if(!fp){ free(buf); r.ok=0; snprintf(r.err,sizeof r.err,"cannot open %s",path); return r; }
    fwrite(buf,1,len,fp); fclose(fp); free(buf);
    return r;
}
#endif /* HUI_TTF_WRITE_IMPLEMENTATION */
#endif /* HUI_TTF_WRITE_H */
