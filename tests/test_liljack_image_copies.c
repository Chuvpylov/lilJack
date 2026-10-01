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

#include <assert.h>
static uint32_t *frame_raster(pixel_border*b,int *width,int *height){
 int cw,ch;lj_ansi_cell_pixels(&cw,&ch);int w=b->cols*cw,h=b->rows*ch;*width=w;*height=h;
 uint32_t *all=calloc((size_t)w*h,sizeof *all);assert(all);size_t at=0;
 for(int side=0;side<4;side++){
  while(at+1<b->bytes&&!(b->wire[at]==27&&b->wire[at+1]=='P'))at++;
  assert(at+1<b->bytes);size_t end=at+2;while(end+1<b->bytes&&!(b->wire[end]==27&&b->wire[end+1]=='\\'))end++;
  assert(end+1<b->bytes);
  int ox=side==3?w-cw:0,oy=side==1?h-ch:side>=2?ch:0;
  int sw=side<2?w:cw,sh=side<2?ch:h-2*ch;assert(sh>0);
  uint32_t *part=calloc((size_t)sw*sh,sizeof *part);assert(part);decode_sixel(b->wire+at,(int)(end+2-at),part,sw,sh);
  for(int y=0;y<sh;y++)for(int x=0;x<sw;x++)if(part[y*sw+x])all[(oy+y)*w+ox+x]=part[y*sw+x];
  free(part);at=end+2;
 }return all;
}
/* Paint ANSI cells, then decode bytes from the real deferred image emitter. */
static void setup(void){state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;}
static double us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(int argc,char **argv){
 (void)argv;setup();int saved=dup(1);FILE *wire=tmpfile();assert(wire);dup2(fileno(wire),1);
 if(argc>1){
  uint32_t *src=malloc(640*360*4);assert(src);double start=us();
  for(int frame=0;frame<100;frame++){
   for(int i=0;i<640*360;i++)src[i]=(frame&1)?0xff0000:0x00ff00;
   lj_ansi_begin(640,360);assert(lj_ansi_image(0,0,640,360,src,640,360));assert(lj_ansi_present());
  }
  double elapsed=us()-start;dup2(saved,1);printf("640x360 source fill+queue+present,100 frames: %.2f us/frame\n",elapsed/100);free(src);
#ifdef IMAGE_COPY_CAP
  size_t retained=0;for(int i=0;i<MAX_IMAGES;i++)retained+=state.images[i].capacity;
  assert(retained==640u*360u*4u);printf("Retained source pool after popup-size session: %zu bytes\n",retained);
#endif
 }else{
  uint32_t src[12*12];for(int i=0;i<144;i++)src[i]=0xff0000;
  lj_ansi_begin(20,20);assert(lj_ansi_image(0,0,20,20,src,12,12));
  for(int i=0;i<144;i++)src[i]=0x0000ff;
  assert(lj_ansi_present());fflush(wire);long n=lseek(fileno(wire),0,SEEK_END);assert(n>0);
  char *bytes=malloc((size_t)n+1);assert(bytes);rewind(wire);assert(fread(bytes,1,(size_t)n,wire)==(size_t)n);bytes[n]=0;
  uint32_t pixels[20*20];decode_sixel(bytes,(int)n,pixels,20,20);free(bytes);
  for(int i=0;i<400;i++)assert(pixels[i]==0xff0000);
#ifdef IMAGE_COPY_CAP
  lj_ansi_begin(40,40);assert(lj_ansi_image(0,0,10,20,src,12,12));
  assert(lj_ansi_image(20,0,10,20,src,12,12));
  lj_ansi_cover(0,0,10,20);assert(state.image_count==1);
  assert(lj_ansi_image(0,20,10,20,src,12,12));
  assert(state.images[0].src!=state.images[1].src);
  assert(!lj_ansi_image(0,0,10,20,src,INT_MAX,INT_MAX));
  assert(state.image_oversize_warned);
  assert(state.image_count==2);
#endif
  dup2(saved,1);puts("Stack scribble -> present -> decoded original red pixels; cover/pool separation and cap PASS");
 }
 close(saved);state.opened=0;lj_ansi_close();fclose(wire);return 0;
}
