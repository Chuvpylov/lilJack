/* visual-polish-chrome mock (items 4 + 5): review-only, production source is
 * included read-only. Renders BEFORE (current code) and MOCK (proposed chrome
 * painted by this harness) for the media popup and the logo menu at 800/1280/1920.
 * No click routing, nothing lands from here. Owner: claude s-6e8dc410. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>
#include <locale.h>
#include <math.h>

static uint32_t frame_px[640*360];
/* A synthetic frame that reads as video: sky gradient, a sun, a horizon,
 * and a title card. No network, no ffmpeg. */
static void synth_frame(void){
    for(int y=0;y<360;y++)for(int x=0;x<640;x++){
        double t=y/359.0;unsigned r=(unsigned)(20+60*t),g=(unsigned)(28+40*t),b=(unsigned)(70+120*(1-t));
        if(y>250){r=18+(y-250)/4;g=40+(y-250)/3;b=30;}
        double dx=x-460,dy=y-120;if(dx*dx+dy*dy<70*70){r=255;g=205;b=90;}
        frame_px[y*640+x]=0xff000000u|(r<<16)|(g<<8)|b;
    }
}
static const char *g(uint32_t cp,const char *fallback,char *buf){
    if(!lj_render_has_codepoint(cp))return fallback;
    /* encode cp as UTF-8 */
    int n=0;if(cp<0x80)buf[n++]=(char)cp;else if(cp<0x800){buf[n++]=(char)(0xc0|cp>>6);buf[n++]=(char)(0x80|(cp&63));}
    else if(cp<0x10000){buf[n++]=(char)(0xe0|cp>>12);buf[n++]=(char)(0x80|((cp>>6)&63));buf[n++]=(char)(0x80|(cp&63));}
    else{buf[n++]=(char)(0xf0|cp>>18);buf[n++]=(char)(0x80|((cp>>12)&63));buf[n++]=(char)(0x80|((cp>>6)&63));buf[n++]=(char)(0x80|(cp&63));}
    buf[n]=0;return buf;
}
static void hline(int x,int y,int w,uint32_t c){lj_render_rect(x,y,w,1,c);}
static void vline(int x,int y,int h,uint32_t c){lj_render_rect(x,y,1,h,c);}
static void frame1(lj_rect r,uint32_t c){hline(r.x,r.y,r.w,c);hline(r.x,r.y+r.h-1,r.w,c);vline(r.x,r.y,r.h,c);vline(r.x+r.w-1,r.y,r.h,c);}

/* ── item 4: the media popup ─────────────────────────────────────────────── */
static void open_popup(App *a){
    lj_popup *p=&a->popup;lj_popup_init(p);
    p->open=1;p->media.pixels=frame_px;p->media.w=640;p->media.h=360;p->media.is_video=1;
    p->media.duration=212;p->media.clock_base=42;p->media.has_audio=1;p->media.volume=70;
    snprintf(p->media.title,sizeof p->media.title,"%s","Never Gonna Give You Up");
    snprintf(p->status,sizeof p->status,"%s","Playing");snprintf(p->media.status,sizeof p->media.status,"%s","Playing");
    a->popup_placed=0;popup_geometry(a);
}
static void close_popup(App *a){a->popup.media.pixels=NULL;a->popup.open=0;a->popup_placed=0;}

/* EVERYTHING ON THE CELL GRID. Text sits in cells in both paths; the only
 * sub-cell marks are 1 px hairlines, which are a sixel strip inside their own
 * frame/rule cell (centred: 4-5 px of padding to the neighbouring content) and
 * a box glyph on a terminal without sixel. So a frame costs one cell ring and
 * a rule costs one row — never a line drawn through a text cell. */
static void hcell_rule(int x,int y,int wcells,uint32_t c){hline(x,y+CELLH/2,wcells*CELLW,c);}
static void vcell_rule(int x,int y,int hcells,uint32_t c){vline(x+CELLW/2,y,hcells*CELLH,c);}
static void cell_frame(lj_rect r,uint32_t c){          /* r in px, on the grid */
    int wc=r.w/CELLW,hc=r.h/CELLH;
    hline(r.x+CELLW/2,r.y+CELLH/2,(wc-1)*CELLW+1,c);hline(r.x+CELLW/2,r.y+r.h-CELLH/2,(wc-1)*CELLW+1,c);
    vline(r.x+CELLW/2,r.y+CELLH/2,(hc-1)*CELLH+1,c);vline(r.x+r.w-CELLW/2,r.y+CELLH/2,(hc-1)*CELLH+1,c);
}
static void mock_popup(App *a,int hot){
    open_popup(a);lj_rect r=a->popup_rect;close_popup(a);
    char b1[8],b2[8],b3[8],b4[8],b5[8],b6[8];
    uint32_t line=hot?CYAN:EDGE;
    int wc=r.w/CELLW,hc=r.h/CELLH;
    /* rows: 0 frame · 1 title · 2 rule · 3..hc-5 video · hc-4 rule · hc-3 timeline · hc-2 controls · hc-1 frame */
    lj_render_rect(r.x,r.y,r.w,r.h,PANEL);cell_frame(r,line);
    int ix=r.x+CELLW,iw=wc-2;                          /* inner columns */
    int ty=r.y+CELLH;
    text(ix,ty,g(0x25B6,">",b1),AMBER,CELLW);
    text(ix+2*CELLW,ty,"Never Gonna Give You Up",TEXT,23*CELLW);
    if(iw>=52)text(ix+26*CELLW,ty,"· Rick Astley · 1987",DIM,20*CELLW);
    text(r.x+r.w-3*CELLW,ty,g(0x00D7,"x",b2),DIM,CELLW);           /* close: last inner cell */
    if(iw>=40){lj_render_panel(r.x+r.w-9*CELLW,ty+2,5*CELLW,CELLH-4,3,SURF2);text(r.x+r.w-9*CELLW+CELLW/2,ty,"480p",DIM,4*CELLW);}
    hcell_rule(ix,ty+CELLH,iw,EDGE);                    /* row 2 */
    lj_rect body={ix,r.y+3*CELLH,iw*CELLW,(hc-7)*CELLH};
    lj_render_rect(body.x,body.y,body.w,body.h,BG);
    lj_media m={.pixels=frame_px,.w=640,.h=360};
    lj_media_blit(&m,lj_render_pixels(),a->w,a->h,body.x,body.y,body.w,body.h);
    hcell_rule(ix,r.y+(hc-4)*CELLH,iw,EDGE);            /* row hc-4 */
    /* timeline row: times in cells, the track a 2 px sixel strip between them */
    int ay=r.y+(hc-3)*CELLH;
    text(ix,ay,"00:42",DIM,5*CELLW);text(r.x+r.w-6*CELLW,ay,"03:32",DIM,5*CELLW);
    int trk_x=ix+6*CELLW,trk_w=(iw-12)*CELLW,trk_y=ay+CELLH/2-1;
    lj_render_rect(trk_x,trk_y,trk_w,2,EDGE);int fill=(int)(trk_w*42.0/212);lj_render_rect(trk_x,trk_y,fill,2,CYAN);
    lj_render_panel(trk_x+fill-3,trk_y-2,6,6,3,hot?TEXT:CYAN);
    /* controls row: glyph buttons 4 cells each, hover = SURF3 pill (badge rule) */
    int by=ay+CELLH,bx=ix;
    const char *ctl[]={g(0x25C0,"<",b3),g(0x2016,"||",b4),g(0x25B6,">",b5)};const char *lbl[]={"10s","","10s"};
    for(int i=0;i<3;i++){
        int w=(i==1?3:5)*CELLW;
        if(i==1)lj_render_panel(bx,by+1,w,CELLH-2,4,SURF3);
        text(bx+CELLW/2,by,ctl[i],i==1?GREEN:TEXT,w);
        if(*lbl[i])text(bx+2*CELLW,by,lbl[i],DIM,3*CELLW);
        bx+=w;
    }
    bx+=CELLW;
    int spk=lj_render_has_codepoint(0x1F50A);
    text(bx,by,g(0x1F50A,"vol",b6),TEXT,spk?2*CELLW:3*CELLW);bx+=(spk?2:4)*CELLW;
    int vwc=iw>=52?10:6;lj_render_rect(bx,by+CELLH/2-1,vwc*CELLW,2,EDGE);lj_render_rect(bx,by+CELLH/2-1,vwc*CELLW*7/10,2,GREEN);
    text(bx+(vwc+1)*CELLW,by,"70%",DIM,3*CELLW);
    const char *esc="Esc closes";int ex=r.x+r.w-CELLW-(int)strlen(esc)*CELLW-2*CELLW;
    if(ex>bx+(vwc+5)*CELLW)text(ex,by,esc,DIM,(int)strlen(esc)*CELLW);
    /* resize: the frame corner cell itself (hit target = that cell), a 6 px wedge */
    for(int i=0;i<7;i++)lj_render_rect(r.x+r.w-CELLW/2-i,r.y+r.h-CELLH/2-(6-i),1,6-i+1,hot?CYAN:DIM);
}

/* ── item 5: the logo menu ───────────────────────────────────────────────── */
typedef struct{const char *label,*key;int sub,check,header;}MItem;
/* Option A — MENUBAR: the existing ribbon carries the five GROUP names; each
 * opens a flyout through menu_open/menu_rows (today's FILES mechanism), so a
 * submenu is a rows branch and nothing else. Shown: MEDIA open, Files nested. */
static void mock_menu_a(App *a){
    char b1[8];const char *arrow=g(0x25B8,">",b1);
    const char *groups[]={"ROOMS","SESSIONS","MEDIA","THEME","HELP"};int open=2;
    int x=3*CELLW,y=0;lj_render_rect(x,y,a->w/2,CELLH,SURF2);
    int gx[5];
    for(int i=0;i<5;i++){int w=((int)strlen(groups[i])+2)*CELLW;gx[i]=x;
        lj_render_panel(x,y,w,CELLH,0,i==open?SURF3:SURF2);text(x+CELLW,y,groups[i],i==open?GREEN:TEXT,w-CELLW);x+=w;}
    text(x+CELLW,y,"QUIT  Ctrl+Q",AMBER,14*CELLW);text(x+15*CELLW,y,"agents keep running",DIM,20*CELLW);
    /* dropdown under MEDIA: frame ring + rows */
    MItem it[]={{"Files",0,1,0,0},{"Review","",0,0,0},{"Play video…","",0,0,0},{"Load graph","",0,1,0}};
    int n=4,wc=22,hc=n+2,mx=gx[open],my=CELLH;
    lj_render_rect(mx,my,wc*CELLW,hc*CELLH,PANEL);cell_frame((lj_rect){mx,my,wc*CELLW,hc*CELLH},EDGE);
    int hot=0,fy=0;
    for(int i=0;i<n;i++){int cy=my+(i+1)*CELLH;
        if(i==hot){lj_render_panel(mx+CELLW,cy,(wc-2)*CELLW,CELLH,3,SURF3);fy=cy;}
        text(mx+2*CELLW,cy,it[i].label,i==hot?GREEN:TEXT,(wc-5)*CELLW);
        if(it[i].check)text(mx+CELLW,cy,g(0x2713,"*",b1),GREEN,CELLW);
        if(it[i].sub)text(mx+(wc-2)*CELLW,cy,arrow,i==hot?GREEN:DIM,CELLW);}
    /* nested Files flyout, anchored on its row, one cell overlap */
    const char *f[]={"~/projects/liljack","docs/","liljack_app/","tests/","Open folder…"};
    int fx=mx+(wc-1)*CELLW,fwc=26,fhc=7;
    lj_render_rect(fx,fy-CELLH,fwc*CELLW,fhc*CELLH,PANEL);cell_frame((lj_rect){fx,fy-CELLH,fwc*CELLW,fhc*CELLH},EDGE);
    for(int i=0;i<5;i++)text(fx+2*CELLW,fy+i*CELLH,f[i],i?TEXT:DIM,(fwc-3)*CELLW);
}
/* Option B — one grouped dropdown under the logo; header rows in CYAN separate
 * the groups (no rule rows: the fifth would push it off an 800x560 screen). */
static void mock_menu_b(App *a){
    (void)a;char b1[8],b2[8];const char *arrow=g(0x25B8,">",b1),*tick=g(0x2713,"*",b2);
    MItem items[]={
        {"ROOMS",0,0,0,1},{"New room","",0,0,0},{"Retile","",0,0,0},{"Full screen","F11",0,0,0},
        {"SESSIONS",0,0,0,1},{"Team","F7",0,0,0},{"Status","",0,0,0},{"Load graph","",0,1,0},
        {"MEDIA",0,0,0,1},{"Files",0,1,0,0},{"Review","",0,0,0},{"Play video…","",0,0,0},
        {"THEME",0,0,0,1},{"Theme editor…","",0,0,0},{"Scanline effects","",0,0,0},
        {"HELP",0,0,0,1},{"About lilJack","",0,0,0},{"Quit","Ctrl+Q",0,0,0}};
    int n=(int)(sizeof items/sizeof *items),wc=24,hc=n+2,x=0,y=CELLH,hot=9,fy=0;
    lj_render_rect(x,y,wc*CELLW,hc*CELLH,PANEL);cell_frame((lj_rect){x,y,wc*CELLW,hc*CELLH},EDGE);
    for(int i=0;i<n;i++){MItem *it=&items[i];int cy=y+(i+1)*CELLH;
        if(it->header){text(x+CELLW,cy,it->label,CYAN,(wc-2)*CELLW);continue;}
        if(i==hot){lj_render_panel(x+CELLW,cy,(wc-2)*CELLW,CELLH,3,SURF3);fy=cy;}
        text(x+2*CELLW,cy,it->label,i==hot?GREEN:TEXT,(wc-6)*CELLW);
        if(it->check)text(x+CELLW,cy,tick,GREEN,CELLW);
        if(it->key&&*it->key)text(x+(wc-1)*CELLW-(int)strlen(it->key)*CELLW,cy,it->key,DIM,(int)strlen(it->key)*CELLW);
        if(it->sub)text(x+(wc-2)*CELLW,cy,arrow,i==hot?GREEN:DIM,CELLW);}
    const char *f[]={"~/projects/liljack","docs/","liljack_app/","tests/","Open folder…"};
    int fx=x+(wc-1)*CELLW,fwc=26,fhc=7;
    lj_render_rect(fx,fy-CELLH,fwc*CELLW,fhc*CELLH,PANEL);cell_frame((lj_rect){fx,fy-CELLH,fwc*CELLW,fhc*CELLH},EDGE);
    for(int i=0;i<5;i++)text(fx+2*CELLW,fy+i*CELLH,f[i],i?TEXT:DIM,(fwc-3)*CELLW);
}


/* ── items 1-3 (cell-honest): tabs, buttons, tiles ───────────────────────────
 * CONSTRAINT: a sixel strip cannot share a cell with text (owkTerm: the pixel
 * tile replaces the glyph; WezTerm: the glyph repaint erases the image). So a
 * 1 px accent INSIDE a one-cell tab or button is not drawable on the sixel
 * path; accents live in cells (background + weight) and 1 px lines only where
 * a row/column is theirs: the tile STATUS RULE row (text in the middle, strips
 * either side) and the dividers (codex). These mocks show exactly that. */
static void mock_tabs(App *a){
    int y=CELLH;lj_render_rect(0,y,a->w,CELLH,PANEL);
    /* filter badge (as today), then tabs as flat cells: active = SURF2 + TEXT,
     * inactive = PANEL + DIM; NO 2 px accent (window-only today, invisible in
     * the terminal). Overflow = "+N ▾" badge that opens a rooms menu. */
    int x=0;text(x+CELLW/2,y,"ACTIVE ▾",CYAN,9*CELLW);x+=10*CELLW;
    const char *tabs[]={"◇ STANDALONE 1","▣ Build room 3","▣ Research 0","▣ Trio review 2"};int n=4,right=a->w-8*CELLW-6*CELLW,shown=0;
    for(int i=0;i<n;i++){int w=((int)strlen(tabs[i])-2+2)*CELLW;if(x+w>right)break;
        lj_render_rect(x,y,w,CELLH,i==1?SURF2:PANEL);text(x+CELLW,y,tabs[i],i==1?TEXT:DIM,w-2*CELLW);x+=w;shown++;}
    if(shown<n){char b[16];snprintf(b,sizeof b,"+%d ▾",n-shown);lj_render_rect(x,y,6*CELLW,CELLH,PANEL);text(x+CELLW/2,y,b,CYAN,5*CELLW);}
    text(a->w-7*CELLW,y,"+ ROOM",CYAN,7*CELLW);
    /* baseline: a 1 px strip needs its own row — there is none between the tab
     * row and the agent row, so the strip is NOT proposed; the row boundary is
     * the colour step PANEL → BG of the dock below. */
}
static void mock_buttons(App *a){
    int y=a->h-4*CELLH,x=2*CELLW;lj_render_rect(0,y-CELLH,a->w,3*CELLH,BG);
    text(x,y-CELLH,"BUTTONS · one rule via button(): pad ½ cell, states by surface + weight",DIM,a->w-4*CELLW);
    struct{const char*l;uint32_t bg,fg;}st[]={{"Default",PANEL,TEXT},{"Hover",SURF3,GREEN},{"Pressed",GREEN,BG},{"Active",GREEN,BG},{"Disabled",PANEL,lj_theme_rgb(LJ_THEME_DISABLED)}};
    for(int i=0;i<5;i++){int w=((int)strlen(st[i].l)+2)*CELLW;lj_render_panel(x,y,w,CELLH,3,st[i].bg);text(x+CELLW,y,st[i].l,st[i].fg,w-2*CELLW);x+=w+CELLW;}
}
static void mock_tile_rule(App *a,int focused,int yy){
    /* the tile status rule row: 1 px strips either side of the status text,
     * green + (window) 2 px when focused; today: ─ glyphs / hairline */
    int x=CELLW,w=a->w/2-2*CELLW,y=yy;uint32_t c=focused?GREEN:EDGE;
    lj_render_rect(x,y,w,CELLH,PANEL);
    const char *st="attached · 80x24";int tw=((int)strlen(st)+2)*CELLW,tx=x+2*CELLW;
    lj_render_rect(x,y+CELLH/2,2*CELLW,focused?2:1,c);
    text(tx,y,st,focused?GREEN:DIM,tw);
    lj_render_rect(tx+tw,y+CELLH/2,x+w-(tx+tw),focused?2:1,c);
}

int main(int argc,char **argv){
    assert(argc==2);setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));synth_frame();
    for(int size=0;size<3;size++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=size==2?1920:size?1280:800;a->h=size==2?1080:size?720:560;
        a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        char path[1024];
        /* popup: before */
        open_popup(a);render(a);snprintf(path,sizeof path,"%s/popup-before-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));close_popup(a);
        /* popup: mock idle and mock hot (dragging/hover frame) */
        render(a);mock_popup(a,0);snprintf(path,sizeof path,"%s/popup-mock-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        render(a);mock_popup(a,1);snprintf(path,sizeof path,"%s/popup-mock-hot-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        /* menu: before (ribbon fully out) */
        a->ribbon_open=1;a->ribbon=RIBBON_FULL;render(a);snprintf(path,sizeof path,"%s/menu-before-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        a->ribbon_open=0;a->ribbon=0;render(a);mock_menu_a(a);snprintf(path,sizeof path,"%s/menu-mock-a-menubar-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        render(a);mock_menu_b(a);snprintf(path,sizeof path,"%s/menu-mock-b-dropdown-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        render(a);mock_tabs(a);mock_buttons(a);mock_tile_rule(a,0,a->h-8*CELLH);mock_tile_rule(a,1,a->h-6*CELLH);
        snprintf(path,sizeof path,"%s/items123-mock-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
        printf("size %dx%d popup rect %d,%d %dx%d\n",a->w,a->h,a->popup_rect.x,a->popup_rect.y,a->popup_rect.w,a->popup_rect.h);
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();return 0;
}
