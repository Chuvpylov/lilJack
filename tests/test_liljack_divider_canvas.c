#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
static uint32_t reg[256];
static int reg_set[256];

static void put_sixel(uint32_t *dst, int w, int h, int *col, int band, int color, int mask) {
    for (int row = 0; row < 6; row++) {
        if (!(mask & (1 << row))) continue;
        int py = band + row, px = *col;
        if (px >= 0 && px < w && py >= 0 && py < h && reg_set[color])
            dst[py * w + px] = reg[color];
    }
    (*col)++;
}

static void decode_sixel(const char *s, int n, uint32_t *dst, int w, int h) {
    memset(dst, 0, (size_t)w * h * 4);
    memset(reg_set, 0, sizeof reg_set);
    int i = 0;
    /* find DCS ESC P ... q */
    while (i + 1 < n && !(s[i] == 0x1b && s[i + 1] == 'P')) i++;
    i += 2;
    while (i < n && s[i] != 'q') i++;
    i++;                                   /* past q */
    int col = 0, band = 0, color = 0;
    while (i < n) {
        char c = s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '\\') break;   /* ST */
        if (c == '#') {
            i++;
            int num = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') num = num * 10 + (s[i++] - '0');
            if (i + 1 < n && s[i] == ';') {
                /* colour definition #n;2;r;g;b */
                int r = 0, g = 0, b = 0;
                int mode = 0;
                while (i < n && s[i] != '#' && s[i] != '$' && s[i] != '-' && s[i] != '"') {
                    if (s[i] >= '0' && s[i] <= '9') {
                        int v = 0;
                        while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
                        if (mode == 2) r = v; else if (mode == 3) g = v; else if (mode == 4) b = v;
                    } else if (s[i] == ';') { mode++; i++; }
                    else i++;
                }
                reg[num] = (uint32_t)((r * 255 + 50) / 100) << 16
                         | (uint32_t)((g * 255 + 50) / 100) << 8
                         | (uint32_t)((b * 255 + 50) / 100);
                reg_set[num] = 1;
                continue;
            }
            color = num;
            continue;
        }
        if (c == '$') { col = 0; i++; continue; }
        if (c == '-') { band += 6; col = 0; i++; continue; }
        if (c == '"') { i++; continue; }                          /* raster attr */
        if (c == '!') {
            i++;
            int cnt = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') cnt = cnt * 10 + (s[i++] - '0');
            if (i < n) { int mask = s[i++] - 63; for (int k = 0; k < cnt; k++) put_sixel(dst, w, h, &col, band, color, mask); }
            continue;
        }
        if (c >= 63 && c <= 126) { i++; put_sixel(dst, w, h, &col, band, color, c - 63); continue; }
        i++;
    }
}
static int h6_save;
static void shot(App*a,const char*base,const char*name){
 if(!h6_save)return;
 char path[1024];snprintf(path,sizeof path,"%s-%s-%dx%d.png",base,name,a->w,a->h);
 lj_render_resize(a->w,a->h);
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
  cell*c=&state.canvas[y*state.cols+x];lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
 }
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
  cell*c=&state.canvas[y*state.cols+x];
  if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
  for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
 }
 if(state.sixel)for(int i=0;i<state.border_count;i++){
  pixel_border *b=&state.borders[i];if(!b->separator||!b->wire)continue;
  int w=b->cols*10,h=b->rows*20;uint32_t *pixels=calloc((size_t)w*h,sizeof *pixels);assert(pixels);
  decode_sixel(b->wire,(int)b->bytes,pixels,w,h);
  for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++)if(pixels[yy*w+xx])lj_render_rect(b->col*10+xx,b->row*20+yy,1,1,pixels[yy*w+xx]);
  free(pixels);
 }
 assert(lj_render_save_png(path)==0);
}

static void check_separator_wire(void){
 for(int cellsize=0;cellsize<3;cellsize++)for(int vertical=0;vertical<2;vertical++)for(int hot=0;hot<2;hot++){
  state.sixel=1;state.cellw=cellsize==2?17:cellsize?8:10;state.cellh=cellsize==2?39:cellsize?16:20;
  lj_ansi_begin(200,200);lj_ansi_rect(0,0,200,200,0x101010);
  lj_ansi_separator_hot(20,20,vertical?10:160,vertical?160:20,0xffc94a,vertical,hot);
  for(int i=0;i<state.cols*state.rows;i++)assert(state.canvas[i].cp==' ');
  assert(state.border_count==1);pixel_border *b=&state.borders[0];assert(prepare_separator(b));
  int w=b->cols*state.cellw,h=b->rows*state.cellh;uint32_t *pixels=calloc((size_t)w*h,sizeof *pixels);assert(pixels);
  decode_sixel(b->wire,(int)b->bytes,pixels,w,h);
  int thick=hot?3:1,extent=vertical?w:h,start=(extent-thick+1)/2;
  int painted=0;for(int y=0;y<h;y++)for(int x=0;x<w;x++){
   int across=vertical?x:y,expected=across>=start&&across<start+thick;
   assert((pixels[y*w+x]!=0)==expected);painted+=pixels[y*w+x]!=0;
  }
  assert(painted==thick*(vertical?h:w));
  if(extent>=thick+10){assert(start>=5);assert(extent-start-thick>=5);}
  pixel_border other=*b;other.thickness=hot?1:3;assert(!border_same_place(b,&other));
  free(pixels);
 }
 state.sixel=0;state.cellw=10;state.cellh=20;
 lj_ansi_begin(200,200);lj_ansi_separator(20,20,10,160,0xffc94a,1);
 assert(state.border_count==0);assert(state.canvas[2+state.cols].cp!=' ');
 puts("H8 decoded sixel: idle1px/hot3px, centred padding, no block glyphs, 10x20/8x16/17x39 PASS");
}
static void decode_ring(pixel_border *b,uint32_t *pixels,int cw,int ch){
 int w=b->cols*cw,h=b->rows*ch;memset(pixels,0,(size_t)w*h*sizeof *pixels);
 uint32_t *strip=calloc((size_t)w*h,sizeof *strip);assert(strip);
 for(size_t i=0;i+4<b->bytes;i++)if(b->wire[i]==27&&b->wire[i+1]=='7'&&b->wire[i+2]==27&&b->wire[i+3]=='['){
  int row,col,used=0;if(sscanf(b->wire+i+4,"%d;%dH%n",&row,&col,&used)!=2||!used)continue;
  int ox=(col-1-b->col)*cw,oy=(row-1-b->row)*ch;
  decode_sixel(b->wire+i+4+used,(int)(b->bytes-i-4-used),strip,w,h);
  for(int y=0;y<h-oy;y++)for(int x=0;x<w-ox;x++)if(strip[y*w+x])pixels[(y+oy)*w+x+ox]=strip[y*w+x];
 }
 free(strip);
}
static void check_ring_dot_invariance(void){
 for(int cellsize=0;cellsize<3;cellsize++)for(int vertical=0;vertical<2;vertical++)for(int hot=0;hot<2;hot++){
  state.opened=1;state.sixel=1;state.cellw=cellsize==2?17:cellsize?8:10;state.cellh=cellsize==2?39:cellsize?16:20;
  lj_ansi_begin(200,200);lj_ansi_rect(0,0,200,200,0x101010);
  lj_ansi_separator_hot(20,20,vertical?10:160,vertical?160:20,0xffd54a,vertical,hot);
  assert(lj_ansi_border(30,40,140,140,0xffd54a,0x5bbcff,0));
  pixel_border *ring=&state.borders[1];int w=ring->cols*state.cellw,h=ring->rows*state.cellh;
  uint32_t *base=calloc((size_t)w*h,4),*after=calloc((size_t)w*h,4);assert(base&&after);
  /* The pre-composition production path: identical dot renderer and state,
   * with no separator contributors. Only the new composition loop is inert. */
  int kind=state.borders[0].separator;state.borders[0].separator=0;
  assert(prepare_border(ring));decode_ring(ring,base,state.cellw,state.cellh);
  state.borders[0].separator=kind;assert(prepare_border(ring));decode_ring(ring,after,state.cellw,state.cellh);
  int changed=0,dots=0,thick=hot?3:1,extent=vertical?state.cellw:state.cellh,start=(extent-thick+1)/2;
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){
   size_t at=(size_t)y*w+x;if(base[at]){dots++;assert(base[at]==after[at]);}
   if(base[at]!=after[at]){int across=vertical?x:y;assert(!base[at]);assert(across>=start&&across<start+thick);changed++;}
  }
  assert(dots>0&&changed>0);free(base);free(after);
 }
 state.opened=0;lj_ansi_close();
 puts("H10 all four ring strips: identical dot coordinates+colours; changes only on divider pixels at10x20/8x16/17x39, idle+hot, both axes PASS");
}
static void check_composed_idle_output(void){
 fflush(stdout);int saved=dup(STDOUT_FILENO);FILE *sink=tmpfile();assert(saved>=0&&sink);
 assert(dup2(fileno(sink),STDOUT_FILENO)>=0);state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;
 off_t previous=0;
 for(int frame=0;frame<4;frame++){
  lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x101010);
  lj_ansi_separator(20,20,160,20,0xffd54a,0);
  assert(lj_ansi_border(30,40,140,140,0xffd54a,0x5bbcff,0));assert(lj_ansi_present());
  off_t end=lseek(STDOUT_FILENO,0,SEEK_CUR);assert(end>=0);
  if(frame)assert(end==previous);previous=end;
 }
 state.opened=0;lj_ansi_close();assert(dup2(saved,STDOUT_FILENO)>=0);close(saved);fclose(sink);
 puts("H11 intersecting ring+divider: 3 unchanged frames emit0 bytes (no ring DCS) PASS");
}
static void check_ring_separator_composition(void){
 state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;
 lj_ansi_begin(200,200);lj_ansi_rect(0,0,200,200,0x101010);
 lj_ansi_separator_hot(20,20,160,20,0xffd54a,0,1);
 assert(lj_ansi_border(30,40,140,140,0xffd54a,0x5bbcff,0));
 assert(state.border_count==2);pixel_border *ring=&state.borders[1];
 assert(prepare_border(ring));int w=ring->cols*17,h=39;
 uint32_t *pixels=calloc((size_t)w*h,sizeof *pixels);assert(pixels);
 decode_sixel(ring->wire,(int)ring->bytes,pixels,w,h);
 for(int y=18;y<=20;y++)for(int x=0;x<w;x++)assert(pixels[y*w+x]);
 int dots=0;for(int x=0;x<w;x++)dots+=pixels[38*w+x]!=0;assert(dots>0&&dots<w);
 uint64_t hot=border_surface_hash(ring);state.borders[0].thickness=1;
 assert(border_surface_hash(ring)!=hot);assert(prepare_border(ring));
 decode_sixel(ring->wire,(int)ring->bytes,pixels,w,h);
 for(int x=0;x<w;x++){assert(!pixels[18*w+x]);assert(pixels[19*w+x]);assert(!pixels[20*w+x]);}
 free(pixels);state.opened=0;state.sixel=0;
 puts("H9 ring strip retains shared-cell divider and unchanged dot band; hot/release cache invalidates PASS");
}
static void capture_state(App*a,int kind,const char*name){
 if(!a->ansi)return;
 if(kind==2&&(!strcmp(name,"hover")||!strcmp(name,"drag"))){
  lj_rect r=a->dock.dividers[0].rect;
  int row=(r.y+r.h/2)/CELLH;
  /* text cells the divider crosses keep their own colour by design (the
   * separator skips content), so require the hot colour on the free cells:
   * at least 90% of the span. */
  int span=0,green=0;for(int x=r.x/CELLW;x<(r.x+r.w)/CELLW;x++){span++;green+=state.canvas[row*state.cols+x].fg==GREEN;}
  assert(span>0&&green*10>=span*9);
 }
 printf("%s %s %dx%d\n",kind==0?"straight":kind==1?"staggered":"t-junction",name,a->w,a->h);
 for(int i=0;i<a->dock.divider_count;i++){lj_dock_divider v=a->dock.dividers[i];printf(" node=%d axis=%d group=%d logical=%d,%d,%d,%d drawn=%d,%d,%d,%d\n",v.node,a->dock.nodes[v.node].axis,v.group,v.logical.x,v.logical.y,v.logical.w,v.logical.h,v.rect.x,v.rect.y,v.rect.w,v.rect.h);}

 char base[1024];snprintf(base,sizeof base,"docs/reports/2026-09-12-divider-topology/h6-%s",kind==0?"straight":kind==1?"staggered":"t-junction");
 shot(a,base,name);
 if(h6_save&&kind==0&&a->w==1280&&!strcmp(name,"default")){
  state.sixel=1;render(a);prepare_borders();shot(a,base,"sixel");state.sixel=0;render(a);
 }
}
#define H6_CAPTURE
#define h6_capture(a,name) capture_state(a,kind,name)
static void pointer(App*a,Uint32 type,int x,int y){
 SDL_Event e={0};e.type=type;if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
 else{e.button.x=x;e.button.y=y;e.button.button=SDL_BUTTON_LEFT;}event(a,&e);
}
static void build_topology(App*a,int kind){
 lj_dock_init(&a->dock);a->split=-1;
 if(kind==2){assert(lj_dock_drop(&a->dock,"demo-0","room",LJ_DOCK_TOP));assert(lj_dock_drop(&a->dock,"demo-1","room",LJ_DOCK_LEFT));}
 else{assert(lj_dock_drop(&a->dock,"demo-0","room",LJ_DOCK_LEFT));assert(lj_dock_drop(&a->dock,"demo-1","demo-0",LJ_DOCK_BOTTOM));assert(lj_dock_drop(&a->dock,"demo-2","room",LJ_DOCK_TOP));
  if(kind==1)a->dock.nodes[a->dock.nodes[a->dock.root].a].ratio=.625;
 }
 render(a);
}
#ifndef H6_CAPTURE
#define h6_capture(a,state) ((void)0)
#endif
/* Real application/controller fixture in a newly owned terminal, no backend. */
static int terminal_lab(void){
 setlocale(LC_CTYPE,"C.UTF-8");int w,h;if(!lj_ansi_open(&w,&h))return 1;
 setenv("SDL_VIDEODRIVER","dummy",1);assert(!lj_render_init(w,h));
 App *a=calloc(1,sizeof *a);assert(a);a->w=w;a->h=h;a->ansi=1;a->demo=1;
 a->backend_in=a->backend_out=-1;a->split=-1;
 for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);build_topology(a,0);
 a->toast[0]=0;render(a);
 int cw,ch;lj_ansi_cell_pixels(&cw,&ch);
 fprintf(stderr,"ansi-lab cols=%d rows=%d cell=%dx%d divider-fixture sixel=%d\n",w/10,h/20,cw,ch,lj_ansi_images_available());fflush(stderr);
 int stage=-1,x=0,y=0;Uint32 start=SDL_GetTicks();
 while(SDL_GetTicks()-start<120000){
  unsigned elapsed=SDL_GetTicks()-start;int next=elapsed<4000?0:elapsed<8000?1:elapsed<12000?2:3;
  const char *stagefile=getenv("LJ_DIVIDER_STAGE_FILE");
  if(stagefile){FILE *f=fopen(stagefile,"r");if(f){if(fscanf(f,"%d",&next)!=1)next=0;fclose(f);}else next=0;}
  SDL_Event e;while(lj_ansi_poll(&e))if(e.type==SDL_USEREVENT)free(e.user.data1);
  if(stage!=next){
   stage=next;
   if(stage==1){lj_rect d=a->dock.dividers[1].rect;x=d.x+d.w/2;y=d.y+d.h/2;pointer(a,SDL_MOUSEMOTION,x,y);}
   if(stage==2){pointer(a,SDL_MOUSEBUTTONDOWN,x,y);pointer(a,SDL_MOUSEMOTION,x,y+40);}
   if(stage==3){pointer(a,SDL_MOUSEBUTTONUP,x,y+40);pointer(a,SDL_MOUSEMOTION,0,0);}
   render(a);fprintf(stderr,"state=%s dividers=%d\n",stage==0?"idle":stage==1?"hover":stage==2?"drag":"released",a->dock.divider_count);
   for(int i=0;i<a->dock.divider_count;i++){lj_rect d=a->dock.dividers[i].rect;fprintf(stderr,"divider=%d rect=%d,%d,%d,%d hot=%d\n",i,d.x,d.y,d.w,d.h,divider_hot(a,i));}fflush(stderr);
   if(!lj_ansi_present())break;fprintf(stderr,"presented=%d\n",stage);fflush(stderr);
  }
  SDL_Delay(100);
 }
 lj_ansi_close();lj_render_close();return 0;
}
int main(int argc,char **argv){
 for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--terminal-lab"))return terminal_lab();
 h6_save=argc>1;(void)argv;check_separator_wire();check_ring_separator_composition();check_ring_dot_invariance();check_composed_idle_output();setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(800,560));int cases=0;
 for(int size=0;size<3;size++)for(int ansi=0;ansi<3;ansi++)for(int kind=0;kind<3;kind++){
  App*a=calloc(1,sizeof *a);assert(a);a->w=size==0?800:size==1?1280:1920;a->h=size==0?560:size==1?720:1080;
  state.sixel=ansi==2;
  a->demo=1;a->ansi=ansi!=0;a->backend_in=a->backend_out=-1;a->split=-1;
  for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_media_init(&a->media);lj_popup_init(&a->popup);lj_dock_init(&a->dock);demo(a);build_topology(a,kind);
  h6_capture(a,"default");
  if(kind==0){
   /* Reconstruct the old one-node drag for before/after visual evidence. */
   lj_dock save=a->dock;lj_dock_divider v=a->dock.dividers[1];
   a->dock.nodes[v.node].ratio=(double)(v.logical.y-v.area.y+40)/(v.area.h-v.logical.h);
   render(a);h6_capture(a,"before-independent-drag");a->dock=save;render(a);
  }
  if(ansi&&kind==0){
   cell *before=malloc((size_t)state.cols*state.rows*sizeof(cell));assert(before);
   memcpy(before,state.canvas,(size_t)state.cols*state.rows*sizeof(cell));
   copy(a->drag,sizeof a->drag,"demo-1");a->dragging=1;
   lj_rect target=a->dock.tiles[0].rect;a->mousex=target.x+target.w-10;a->mousey=target.y+target.h/2;
   render(a);int checked=0;
   for(int yy=ANSI_DOCK_TOP/CELLH;yy<(a->h-ANSI_FOOTER_H)/CELLH;yy++)for(int xx=0;xx<state.cols;xx++){
    cell *old=&before[yy*state.cols+xx],*now=&state.canvas[yy*state.cols+xx];
    if(!old->content)continue;
    assert(old->cp==now->cp&&old->wide==now->wide&&!memcmp(old->marks,now->marks,sizeof old->marks));checked++;
   }
   assert(checked>100);printf("SNAP preserved %d occupied cells at %dx%d\n",checked,a->w,a->h);
   h6_capture(a,"snap-preview");free(before);a->dragging=0;a->drag[0]=0;render(a);
  }
  int selected=kind==2?0:1;lj_dock_divider v=a->dock.dividers[selected];
  int x=v.rect.x+v.rect.w/2,y=v.rect.y+v.rect.h/2;
  if(kind==2)x=v.rect.x+v.rect.w/4;
  pointer(a,SDL_MOUSEMOTION,x,y);render(a);
  assert(divider_at(a,x,y)==selected);assert(divider_hot(a,selected));
  if(kind==0)assert(divider_hot(a,2));
  if(kind==1)assert(!divider_hot(a,2));
  h6_capture(a,"hover");
  pointer(a,SDL_MOUSEBUTTONDOWN,x,y);assert(a->split==v.node);
  assert(a->split_drag.count==(kind==0?2:1));
  lj_dock_tile before[LJ_DOCK_MAX_LEAVES];int count=a->dock.tile_count;memcpy(before,a->dock.tiles,sizeof before);
  pointer(a,SDL_MOUSEMOTION,x,y);render(a);
  for(int i=0;i<count;i++)assert(!memcmp(&before[i].rect,&a->dock.tiles[i].rect,sizeof(lj_rect)));
  pointer(a,SDL_MOUSEMOTION,x,y+40);render(a);h6_capture(a,"drag");
  assert(divider_hot(a,selected));
  if(kind==0){assert(divider_hot(a,2));assert(a->dock.dividers[1].rect.y==a->dock.dividers[2].rect.y);}
  if(kind==1)for(int i=0;i<count;i++)if(!strcmp(before[i].id,"demo-2")||!strcmp(before[i].id,"room"))assert(!memcmp(&before[i].rect,&a->dock.tiles[i].rect,sizeof(lj_rect)));
  if(kind==2){int dw=a->dock.dividers[0].rect.w,want=a->w-2*CELLW;assert(dw<=want&&dw>=want-CELLW);}   /* uniform window margin; width snapped to the cell grid */
  pointer(a,SDL_MOUSEBUTTONUP,x,y+40);assert(a->split==-1);
  if(kind==2){
   /* At an overlapping snapped T-junction, the smaller local split wins. */
   lj_rect parent=hitrect(a->dock.dividers[0].rect),local=hitrect(a->dock.dividers[1].rect);
   int ix=parent.x>local.x?parent.x:local.x,iy=parent.y>local.y?parent.y:local.y;
   if(inside(parent,ix,iy)&&inside(local,ix,iy)){assert(divider_at(a,ix,iy)==1);cases++;}
  }
  cases++;for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
  if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
 }
 printf("H6 real controller: %d topology/junction cases PASS, pixel+ANSI800/1280/1920\n",cases);lj_render_close();return 0;
}
