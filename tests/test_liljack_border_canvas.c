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

static int save_images;
/* Every sixel in the ring's wire is placed by its own CUP and sized by its own band data:
 * since strips are emitted as SEGMENTS (image occlusion, and the spill-aware split around
 * wide glyphs in the row below), a side may be two or more DCS — the old one-DCS-per-side
 * reconstruction stacked them at fixed offsets and read dots at the wrong x. */
static int band_width_px(const char *d,const char *end){int w=0,col=0,rep=0;const char *q=memchr(d,'q',(size_t)(end-d));if(!q)return 0;
 for(const char *p=q+1;p<end;p++){char c=*p;if(c==27)break;
  if(c=='!'){rep=0;p++;while(p<end&&*p>='0'&&*p<='9'){rep=rep*10+(*p-'0');p++;}p--;continue;}
  if(c=='#'){while(p+1<end&&((p[1]>='0'&&p[1]<='9')||p[1]==';'))p++;rep=0;continue;}
  if(c=='$'||c=='-'){if(col>w)w=col;col=0;rep=0;continue;}
  if(c>='?'&&c<='~'){col+=rep?rep:1;rep=0;}}
 if(col>w)w=col;return w;}
/* the host advances one 6 px band per '-' separator whether or not the band carried data: the ring's top
 * dots sit in the fourth band after three empty ones, so height = separators + 1 */
static int band_count(const char *d,const char *end){int seps=0,any=0;const char *q=memchr(d,'q',(size_t)(end-d));if(!q)return 0;
 for(const char *p=q+1;p<end;p++){if(*p==27)break;if(*p=='-')seps++;else if(*p>='?'&&*p<='~')any=1;}return any||seps?seps+1:0;}
static uint32_t *frame_raster(pixel_border*b,int *width,int *height){
 int cw,ch;lj_ansi_cell_pixels(&cw,&ch);int w=b->cols*cw,h=b->rows*ch;*width=w;*height=h;
 uint32_t *all=calloc((size_t)w*h,sizeof *all);assert(all);int row=-1,col=-1,n=0;
 for(size_t at=0;at+1<b->bytes;at++){
  if(b->wire[at]==27&&b->wire[at+1]=='['){int r=0,c=0;if(sscanf(b->wire+at+2,"%d;%dH",&r,&c)==2){row=r-1;col=c-1;}}
  if(b->wire[at]==27&&b->wire[at+1]=='P'){size_t end=at+2;while(end+1<b->bytes&&!(b->wire[end]==27&&b->wire[end+1]=='\\'))end++;assert(end+1<b->bytes);assert(row>=0);
   int sw=band_width_px(b->wire+at,b->wire+end),sh=band_count(b->wire+at,b->wire+end)*6;if(sw<1)sw=cw;
   int ox=(col-b->col)*cw,oy=(row-b->row)*ch;if(ox+sw>w)sw=w-ox;
   if(sh<=0||sw<=0||ox<0||oy<0||oy>=h){at=end+1;continue;}   /* an all-transparent segment carries no bands: nothing to place */
   /* decode the WHOLE band height (a 20 px strip is 24 px of bands), then keep only the rows inside the frame */
   uint32_t *part=calloc((size_t)sw*sh,sizeof *part);assert(part);decode_sixel(b->wire+at,(int)(end+2-at),part,sw,sh);
   for(int y=0;y<sh&&oy+y<h;y++)for(int x=0;x<sw;x++)if(part[y*sw+x])all[(oy+y)*w+ox+x]=part[y*sw+x];
   free(part);n++;at=end+1;}
 }
 assert(n>=4);return all;
}
static int dot_pixel(uint32_t c){int r=(c>>16)&255,g=(c>>8)&255,b=c&255;return r+g+b>200&&abs(r-b)>40;}
static void shot(App*a,const char*tag){
 if(!save_images)return;const char *dir=getenv("LILJACK_BORDER_CAPTURE_DIR");
 if(!dir)dir="docs/reports/2026-09-12-border-dots";
 char path[1024];snprintf(path,sizeof path,"%s/a6-a7-%s-%dx%d.png",dir,tag,a->w,a->h);
 lj_render_resize(a->w,a->h);
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++)lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,state.canvas[y*state.cols+x].bg);
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
  cell*c=&state.canvas[y*state.cols+x];if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
  for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
 }
 if(state.sixel)for(int i=0;i<state.border_count;i++){
  pixel_border*b=&state.borders[i];if(!b->wire)continue;
  int w,h;uint32_t *pixels;
  if(b->separator){w=b->cols*10;h=b->rows*20;pixels=calloc((size_t)w*h,sizeof *pixels);assert(pixels);decode_sixel(b->wire,(int)b->bytes,pixels,w,h);}
  else pixels=frame_raster(b,&w,&h);
  for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(pixels[y*w+x])lj_render_rect(b->col*10+x,b->row*20+y,1,1,pixels[y*w+x]);free(pixels);
 }
 assert(!lj_render_save_png(path));
}
static void edge_counts(lj_rect r,const char*tag,int require_all){
 hui_border_grid g;assert(hui_border_cells(r.x,r.y,r.w,r.h,10,20,&g));
 printf("%s %dx%d frame=%d,%d,%d,%d",tag,state.cols*10,state.rows*20,g.col,g.row,g.cols,g.rows);
 for(int edge=0;edge<4;edge++){
  int len=(edge&1)?g.rows:g.cols,count=0,prev=-1,lo=999,hi=0,blocked=0;
  for(int n=0;n<len;n++){
   int x=g.col+(edge==0?n:edge==1?g.cols-1:edge==2?g.cols-1-n:0);
   int y=g.row+(edge==0?0:edge==1?n:edge==2?g.rows-1:g.rows-1-n);
   cell*c=&state.canvas[y*state.cols+x];blocked+=c->content!=0;
   if(c->cp==0x2022){if(prev>=0){int gap=n-prev-1;if(gap<lo)lo=gap;if(gap>hi)hi=gap;}prev=n;count++;}
  }
  printf(" %s:n%d,gap%d..%d,text%d",edge==0?"top":edge==1?"right":edge==2?"bottom":"left",count,lo==999?-1:lo,hi,blocked);fflush(stdout);
  if(require_all||(edge&1))assert(count>=2);
  if((edge&1)&&g.rows>=5)assert(count>=3);
  if(!blocked){assert(count>=2&&lo>=1&&hi-lo<=1);}
 }puts("");
}
/* Allow only the declared physical divider pixels in a composed ring strip. */
static int declared_separator_pixel(pixel_border *ring,int x,int y,uint32_t pixel){
 int cw,ch;lj_ansi_cell_pixels(&cw,&ch);x+=ring->col*cw;y+=ring->row*ch;
 for(int i=0;i<state.border_count;i++){
  pixel_border *s=&state.borders[i];if(!s->separator)continue;
  int vertical=s->separator==1,width=s->cols*cw,height=s->rows*ch;
  int thick=s->thickness?s->thickness:1,extent=vertical?width:height;if(thick>extent)thick=extent;
  int left=s->col*cw+(vertical?(width-thick+1)/2:0),top=s->row*ch+(vertical?0:(height-thick+1)/2);
  if(x<left||x>=left+(vertical?thick:width)||y<top||y>=top+(vertical?height:thick))continue;
  if(border_covered(s,x/cw,y/ch))continue;
  uint32_t encoded=0;for(int shift=0;shift<=16;shift+=8){unsigned v=(s->gold>>shift)&255;encoded|=((v*100/255*255+50)/100)<<shift;}
  if(pixel==encoded)return 1;
 }
 return 0;
}
static void app_gutter_counts(void){
 for(int i=0;i<state.border_count;i++){
  pixel_border*b=&state.borders[i];if(b->separator)continue;assert(b->wire);
  int w,h;uint32_t *pixels=frame_raster(b,&w,&h),counts[4]={0},interior[4]={0};
  int left=b->inner_col?9:0,top=b->inner_row?19:0;
  int right=(b->inner_col+b->anchor_cols)*10;if(right>=w)right=w-1;
  int bottom=(b->inner_row+b->anchor_rows)*20;if(bottom>=h)bottom=h-1;
  for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(dot_pixel(pixels[y*w+x])){
   counts[0]+=y==top;counts[1]+=x==right;counts[2]+=y==bottom;counts[3]+=x==left;
   interior[0]+=y==top&&x>left&&x<right;interior[1]+=x==right&&y>top&&y<bottom;
   interior[2]+=y==bottom&&x>left&&x<right;interior[3]+=x==left&&y>top&&y<bottom;
  }
  /* Every non-divider pixel remains a dot. Divider contributions are checked
   * against their exact physical stroke and encoded colour; dot gap checks
   * below remain unchanged. */
  int solid=0,painted=0;
  for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(pixels[y*w+x]){painted++;solid+=!dot_pixel(pixels[y*w+x])&&!declared_separator_pixel(b,x,y,pixels[y*w+x]);}
  printf("FRAME dots+declared-dividers painted=%d solid=%d rows=%d\n",painted,solid,b->anchor_rows);
  fflush(stdout);assert(solid==0&&painted>0);
  /* 2x2 dot, 4px gap: along the top edge the painted runs are at most
   * LJ_BORDER_DOT long and the gaps at least one pixel (a gap can be shorter
   * than LJ_BORDER_GAP only where an edge ends). */
  {int run=0,maxrun=0,gaps=0;
   /* Skip the corners: the left/right edges paint their own dots into this row
    * there, so a run across a corner is two edges meeting, not a long dot. */
   for(int x=left+2*LJ_BORDER_DOT;x<=right-2*LJ_BORDER_DOT;x++){
     if(pixels[top*w+x]){run++;if(run>maxrun)maxrun=run;} else {gaps++;run=0;}}
   printf("FRAME top edge: longest dot run=%d, gap pixels=%d\n",maxrun,gaps);
   assert(maxrun<=LJ_BORDER_DOT&&gaps>0);}
  printf("APP SIXEL %dx%d dots=%u/%u/%u/%u\n",state.cols*10,state.rows*20,counts[0],counts[1],counts[2],counts[3]);
  for(int edge=0;edge<4;edge++){assert(counts[edge]>=2);if(((edge&1)?b->anchor_rows:b->anchor_cols)>=5)assert(interior[edge]>=1);}
  free(pixels);
 }
}
/* The test owns the rings it asks for: free every encoded wire (current and
 * cached-old) before dropping the arrays, as lj_ansi_close would. */
static void drop_rings(void){
 for(int i=0;i<state.border_count;i++){free(state.borders[i].wire);state.borders[i].wire=NULL;}
 for(int i=0;i<state.old_border_count;i++){free(state.old_borders[i].wire);state.old_borders[i].wire=NULL;}
 state.border_count=state.old_border_count=0;
}
static void isolated_checks(void){
 state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
 lj_ansi_begin(600,400);lj_ansi_rect(0,0,600,400,0x101010);
 lj_rect r={45,63,303,183};hui_border_grid g;assert(hui_border_cells(r.x,r.y,r.w,r.h,10,20,&g));
 lj_ansi_text(g.col*10,g.row*20,"title é 漢字",0xffffff,200);
 lj_ansi_text(g.col*10,(g.row+g.rows-1)*20,"status wide 界",0xffffff,200);
 cell *before=malloc((size_t)state.cols*state.rows*sizeof(cell));assert(before);memcpy(before,state.canvas,(size_t)state.cols*state.rows*sizeof(cell));
 render_ansi=1;render_anim=0;dotted_border(r,GREEN,CYAN);assert(state.border_count==1);
 pixel_border*b=&state.borders[0];b->phase=0;prepare_borders();assert(b->wire);
 for(int i=0;i<state.cols*state.rows;i++)if(before[i].content){assert(before[i].cp==state.canvas[i].cp&&before[i].wide==state.canvas[i].wide);assert(!memcmp(before[i].marks,state.canvas[i].marks,sizeof before[i].marks));}
 int w,h;uint32_t *first=frame_raster(b,&w,&h);int counts[4]={0};
 /* ring-gutter-reservation (2026-09-13): dotted_border hands lj_ansi_border the
  * rect INSET by one cell, so the sixel lines sit on r's own outer cells (the
  * ring's gutter), one cell inward from the frame of r itself. */
 hui_border_grid gi;assert(hui_border_cells(r.x+10,r.y+20,r.w-20,r.h-40,10,20,&gi));
 int left=gi.col*10-1-b->col*10,right=(gi.col+gi.cols)*10-b->col*10;
 int top=gi.row*20-1-b->row*20,bottom=(gi.row+gi.rows)*20-b->row*20;
 for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(first[y*w+x]){
  /* 2x2 dots sit on the ring line and the pixel OUTSIDE it (gutter side), never
   * on the text interior (the operator 2026-09-12: "2x2px dots"). */
  assert(x==left||x==left-1||x==right||x==right+1||y==top||y==top-1||y==bottom||y==bottom+1);
  assert(!(x>left&&x<right&&y>top&&y<bottom));  /* never paints text interior */
  if(dot_pixel(first[y*w+x])){counts[0]+=y==top;counts[1]+=x==right;counts[2]+=y==bottom;counts[3]+=x==left;}
 }
 for(int i=0;i<4;i++)assert(counts[i]>=2);
 /* MARCH: one tick later every dot has moved along its edge: same number of
  * dots, a different set of positions, still ring-only. */
 /* MARCH with the 2x2/4px pattern (the operator, live 2026-09-12): one tick later the
  * pattern has stepped one pixel along every edge, so the painted set CHANGES;
  * the count may differ by at most one dot per edge as a block enters or leaves
  * the edge, and every painted pixel is still on the ring. */
 b->phase=1;assert(prepare_border(b));uint32_t *second=frame_raster(b,&w,&h);int dots=0,dots2=0,moved=0;
 for(int i=0;i<w*h;i++){dots+=!!dot_pixel(first[i]);dots2+=!!dot_pixel(second[i]);moved+=dot_pixel(first[i])&&!dot_pixel(second[i]);
  if(second[i]){int x=i%w,y=i/w;assert(x>=left-1&&x<=right+1&&y>=top-1&&y<=bottom+1);
                assert(x<=left||x>=right||y<=top||y>=bottom);}}
 /* PERSISTENT COLOURS (the operator 2026-09-12: "don't switch colours of dots"): the
  * colour sequence along the top edge is the same after a tick, just shifted. */
 {int c1[64],c2[64],n1=0,n2=0;
  for(int x=left;x<=right&&n1<64;x++){uint32_t v=first[top*w+x];if(dot_pixel(v)&&(x==left||!dot_pixel(first[top*w+x-1])))c1[n1++]=(int)(v&0xffffff);}
  for(int x=left;x<=right&&n2<64;x++){uint32_t v=second[top*w+x];if(dot_pixel(v)&&(x==left||!dot_pixel(second[top*w+x-1])))c2[n2++]=(int)(v&0xffffff);}
  assert(n1>=3&&n2>=3);
  /* the marching pattern stepped one pixel: dot k in the first frame is dot k or k+1 in the second */
  int same=0;for(int k=0;k+1<n1&&k+1<n2;k++)same+=(c1[k]==c2[k])||(c1[k]==c2[k+1]);
  assert(same>=n1-2);}
 /* ACROSS THE WRAP (codex seq 1352): a physical dot keeps its colour when the
  * phase goes 5→6 (shift wraps 5→0). For phases 5,6 and 11,12 the colour at
  * pixel x in the first frame equals the colour at x+1 in the next frame,
  * for every lit pixel that stays inside the edge. */
 {static const uint32_t pairs[][2]={{5,6},{11,12},{17,18}};
  for(unsigned q=0;q<3;q++){
   b->phase=pairs[q][0];assert(prepare_border(b));uint32_t *f1=frame_raster(b,&w,&h);
   b->phase=pairs[q][1];assert(prepare_border(b));uint32_t *f2=frame_raster(b,&w,&h);
   int checked=0;
   for(int x=left+2*LJ_BORDER_DOT;x+1<=right-2*LJ_BORDER_DOT;x++){uint32_t v=f1[top*w+x];if(!dot_pixel(v))continue;
    uint32_t n=f2[top*w+x+1];assert(dot_pixel(n)&&(n&0xffffff)==(v&0xffffff));checked++;}
   assert(checked>=4);free(f1);free(f2);}
  b->phase=1;}
 assert(moved>0&&dots>0&&dots2>0);
 /* ICONS SURVIVE THE RING (the operator live 2026-09-12). Put a wide glyph in a cell
  * the ring crosses and re-prepare: no dot pixel may land in that cell, in
  * either half of the pair, so the presenter never re-emits and erases it. */
 {
  int cw2,ch2;lj_ansi_cell_pixels(&cw2,&ch2);
  int gx=b->col+b->cols/2, gy=b->row;                      /* on the top edge */
  lj_ansi_glyph(gx*cw2,gy*ch2,0x1F9E0,0xffffffu,2);        /* the brain icon */
  assert(prepare_border(b));
  int wcw,wch;uint32_t *third=frame_raster(b,&wcw,&wch);
  int intruded=0;
  for(int y=0;y<wch;y++)for(int x=0;x<wcw;x++){
    if(!third[y*wcw+x])continue;
    int ccx=b->col+x/cw2, ccy=b->row+y/ch2;
    if(ccy==gy&&(ccx==gx||ccx==gx+1))intruded++;
  }
  printf("ICON under the ring: dot pixels inside the wide glyph's cells = %d (expect 0)\n",intruded);
  assert(intruded==0);
  free(third);
 }
 {int d=dots-dots2; if(d<0)d=-d; assert(d<=4*(LJ_BORDER_DOT*LJ_BORDER_DOT));}
 printf("SIXEL dots=%d/%d/%d/%d; march: %d of %d pixels moved after one tick (2x2 dot, %dpx gap); ring only; text unchanged PASS\n",counts[0],counts[1],counts[2],counts[3],moved,dots,LJ_BORDER_GAP);
 free(first);free(second);free(before);state.opened=0;state.sixel=0;drop_rings();
 // A6: unaligned origins and size class use identical contained cells.
 for(int htest=79;htest<=82;htest++){
  lj_ansi_begin(200,200);lj_rect q={19,19,81,htest};hui_border_grid expected;assert(hui_border_cells(q.x,q.y,q.w,q.h,10,20,&expected));
  render_anim=0;dotted_border(q,GREEN,CYAN);edge_counts(q,"unaligned",1);
  for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++)if(state.canvas[y*state.cols+x].cp==0x2022)assert(x>=expected.col&&x<expected.col+expected.cols&&y>=expected.row&&y<expected.row+expected.rows);
 }
}
/* REDRAW STORM (s-9b350e46's audit, 2026-09-12). A ring that has not moved must
 * not dirty its perimeter: prepare_borders used to retire EVERY old border on
 * EVERY present, so an unmoved lead tile repainted its whole perimeter ~40x a
 * second. Measure it: count the cells prepare_borders marks dirty in `previous`
 * when the geometry is unchanged, then again after the ring moves. */
static int dirty_cells(void){
 int n=0;
 for(int i=0;i<state.cols*state.rows;i++){
  const unsigned char *p=(const unsigned char*)&state.previous[i];
  int all=1;for(size_t k=0;k<sizeof(cell);k++)if(p[k]!=0xff){all=0;break;}
  n+=all;
 }
 return n;
}
static void storm_check(void){
 lj_ansi_begin(600,400);state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
 lj_ansi_rect(0,0,600,400,0x101010);
 lj_rect r={45,63,303,183};
 render_ansi=1;render_anim=0;dotted_border(r,GREEN,CYAN);
 prepare_borders();
 /* pretend the frame was emitted, so the ring becomes the "old" one; the
  * old copy takes the wire (single owner), the current slot gives it up */
 for(int i=0;i<state.old_border_count;i++){free(state.old_borders[i].wire);state.old_borders[i].wire=NULL;}
 state.old_border_count=0;
 for(int i=0;i<state.border_count;i++){state.old_borders[state.old_border_count]=state.borders[i];
  state.old_border_count++;state.borders[i].wire=NULL;}   /* the old copy now owns the wire */
 for(int i=0;i<state.cols*state.rows;i++)memset(&state.previous[i],0,sizeof(cell));
 /* same place next frame */
 for(int i=0;i<state.border_count;i++){free(state.borders[i].wire);state.borders[i].wire=NULL;}
 state.border_count=0;dotted_border(r,GREEN,CYAN);prepare_borders();
 int unmoved=dirty_cells();
 /* now it moves */
 for(int i=0;i<state.cols*state.rows;i++)memset(&state.previous[i],0,sizeof(cell));
 for(int i=0;i<state.border_count;i++){free(state.borders[i].wire);state.borders[i].wire=NULL;}
 state.border_count=0;lj_rect moved={45,103,303,183};dotted_border(moved,GREEN,CYAN);prepare_borders();
 int after=dirty_cells();
 printf("REDRAW storm: unmoved ring dirties %d cells, a moved ring dirties %d PASS\n",unmoved,after);
 assert(unmoved==0&&after>0);
 state.opened=0;state.sixel=0;drop_rings();
}
int main(int argc,char**argv){
 (void)argv;save_images=argc>1||getenv("LILJACK_BORDER_CAPTURE_DIR");setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(800,560));isolated_checks();storm_check();
 App*a=calloc(1,sizeof*a);assert(a);a->demo=a->ansi=1;a->split=-1;a->backend_in=a->backend_out=-1;
 for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
 int sizes[][2]={{800,560},{1280,720},{1920,1080},{801,561},{1279,719},{1281,721},{800,579},{800,580}};
 for(unsigned z=0;z<sizeof sizes/sizeof sizes[0];z++){
  a->w=sizes[z][0];a->h=sizes[z][1];a->anim=0;copy(a->focus,sizeof a->focus,"room");render(a);
  if(z<3)shot(a,"default");copy(a->focus,sizeof a->focus,"demo-1");render(a);
  lj_rect frame={0};for(int i=0;i<a->dock.tile_count;i++)if(!strcmp(a->dock.tiles[i].id,"demo-1"))frame=a->dock.tiles[i].rect;
  /* UNIFORM WINDOW MARGIN (2026-09-12): the lead ring's outer rect is the tile
   * grown by one cell into the margin / divider cells, exactly as main.c hands
   * it to dotted_border; the placeholders and the strip live in those cells. */
  frame=(lj_rect){frame.x-CELLW,frame.y-CELLH,frame.w+2*CELLW,frame.h+2*CELLH};
  edge_counts(frame,"lead-focus",0);if(z<3){
   shot(a,"lead-focus");a->anim=4;render(a);shot(a,"lead-focus-phase-480ms");a->anim=0;
   assert(!setenv("LILJACK_ANSI_BORDER_ROWS","1",1));render(a);edge_counts(frame,"two-rows",1);shot(a,"two-rows");unsetenv("LILJACK_ANSI_BORDER_ROWS");
   state.opened=1;state.sixel=1;render(a);for(int i=0;i<state.border_count;i++)state.borders[i].phase=0;prepare_borders();app_gutter_counts();shot(a,"sixel-phase-0ms");
   for(int i=0;i<state.border_count;i++)if(!state.borders[i].separator)state.borders[i].phase=10;prepare_borders();shot(a,"sixel-phase-500ms");state.opened=0;state.sixel=0;
  }
 }
 for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);lj_render_close();puts("A6/A7 rendered border matrix PASS");return 0;
}
