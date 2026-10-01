/* popup-video-torn-wezterm (claude s-7fef91a8, 2026-09-13): with the floating
 * video popup open, WezTerm showed the popup image torn into stale bands. The
 * popup sixel was sound; the frame then emitted the centre divider strip and
 * the lead-ring side strips as sixels whose FOOTPRINT ran through the popup
 * (their pixels there were already transparent). WezTerm replaces a cell's
 * image attachment under any later sixel footprint, so the popup lost every
 * cell those strips crossed. Rule planted here: no strip DCS may have a cell
 * footprint that intersects a queued image's footprint; strips are emitted as
 * segments around images instead. Byte-level, on the presenter's own output. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>
static char *grab(size_t *len){*len=(size_t)lseek(1,0,SEEK_END);char *p=malloc(*len+1);assert(p);
    assert(pread(1,p,*len,0)==(ssize_t)*len);p[*len]=0;assert(lseek(1,0,SEEK_SET)==0);assert(ftruncate(1,0)==0);return p;}
static uint32_t px[60*32];
/* one DCS with its DECSC/CUP/DECRC wrapper → row, col, band count, max band width */
/* Portable: no memmem/sscanf-%n (an implicit memmem on an older gcc returns a
 * truncated int and the parser then dereferences garbage). The capture is
 * NUL-terminated, so plain pointer walks are enough. */
static const char *find_st(const char *q,const char *end){for(;q+1<end;q++)if(q[0]==0x1b&&q[1]=='\\')return q;return NULL;}
static int read_int(const char **pp,const char *end,int *out){const char *p=*pp;int v=0,n=0;while(p<end&&*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;n++;}*pp=p;*out=v;return n>0;}
static int next_dcs(const char *s,size_t n,size_t *at,int *row,int *col,int *bands,int *width,size_t *bytes){
    const char *p=s+*at,*end=s+n;
    for(;p+2<end;p++)if(p[0]==0x1b&&p[1]=='['){
        const char *h=p+2;int r,c;
        if(!read_int(&h,end,&r)||h>=end||*h!=';')continue;h++;
        if(!read_int(&h,end,&c)||h+2>=end||h[0]!='H'||h[1]!=0x1b||h[2]!='P')continue;
        const char *q=h+3,*st=find_st(q,end);if(!st)return 0;
        int x=0,mx=0,b=1;const char *i=q;while(i<st&&*i!='q')i++;if(i<st)i++;
        while(i<st){char ch=*i;
            if(ch=='#'){i++;while(i<st&&((*i>='0'&&*i<='9')||*i==';'))i++;continue;}
            if(ch=='$'){if(x>mx)mx=x;x=0;i++;continue;}
            if(ch=='-'){if(x>mx)mx=x;x=0;b++;i++;continue;}
            if(ch=='!'){i++;int k=0;while(i<st&&*i>='0'&&*i<='9')k=k*10+(*i++-'0');x+=k;if(i<st)i++;continue;}
            if(ch>=63&&ch<=126)x++;i++;}
        if(x>mx)mx=x;*row=r-1;*col=c-1;*bands=b;*width=mx;*bytes=(size_t)(st+2-q);*at=(size_t)(st+2-s);return 1;}
    return 0;
}
int main(void){
    int saved=dup(1);FILE *sink=tmpfile();assert(saved>=0&&sink);assert(dup2(fileno(sink),1)>=0);
    state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
    for(int i=0;i<60*32;i++)px[i]=0xff40df80;
    int bad=0,strips=0,frames=0;
    /* image: cols 35..94, rows 10..25 (the popup video body at 1280x720) */
    const int icol=35,irow=10,icols=60,irows=16;
    /* Live shape: frame 0 is the popup's "Loading video…" state — its panel is painted
     * over the strips (so their pixels there are already transparent and their wires
     * get cached) but no image is queued yet; the decoded frame arrives on frame 1.
     * A ring tick runs between frames as the app's timer does. */
    for(int frame=0;frame<4;frame++){
        lj_ansi_begin(1280,720);lj_ansi_rect(0,0,1280,720,0x08101a);
        lj_ansi_separator(650,100,10,520,0xffd54a,1);                 /* centre divider: col 65, rows 5..30 */
        lj_ansi_separator(10,240,640,20,0xffd54a,0);                  /* a tile status rule: row 12, cols 1..64 */
        lj_ansi_rect((icol-1)*10,(irow-2)*20,(icols+2)*10,(irows+5)*20,0x0d1626);   /* the popup panel, painted after the strips */
        if(frame)assert(lj_ansi_image(icol*10,irow*20,icols*10,irows*20,px,60,32));
        lj_ansi_separator(10,560,640,20,0xffd54a,0);                  /* a tile rule: row 28, cols 1..64 */
        assert(lj_ansi_border(10,300,640,320,0xffd54a,0x5bbcff,(uint32_t)frame)); /* lead ring: cols 1..64, rows 15..30 */
        assert(lj_ansi_present());
        if(frame)lj_ansi_border_tick((uint32_t)frame+7);
        size_t n;char *out=grab(&n);size_t at=0;int row,col,bands,width;size_t bytes;frames++;
        if(!frame){free(out);continue;}
        while(next_dcs(out,n,&at,&row,&col,&bands,&width,&bytes)){
            /* WezTerm sizes a raster-less sixel by its data: cols from the widest band, rows from
             * 6 px per band (a band's overhang of <20 px into the next row is the encoder's rounding). */
            int cols=(width+9)/10,rows=(bands*6)/20;if(cols<1)cols=1;if(rows<1)rows=1;(void)bytes;
            if(row==irow&&col==icol&&width>=icols*10)continue;         /* the popup image itself */
            strips++;
            int hit=col<icol+icols&&col+cols>icol&&row<irow+irows&&row+rows>irow;
            if(hit){bad++;fprintf(stderr,"frame %d: strip DCS at row %d col %d footprint %dx%d cells intersects the image (rows %d..%d, cols %d..%d); header:",frame,row,col,cols,rows,irow,irow+irows-1,icol,icol+icols-1);
                const char *hd=out+at-bytes;for(size_t k=0;k<bytes&&k<48;k++)fprintf(stderr,hd[k]>=32&&hd[k]<127?"%c":"\\x%02x",(unsigned char)hd[k]);fputc('\n',stderr);}
        }
        free(out);
    }
    dup2(saved,1);
    printf("%s strip-occlusion: %d frames, %d strip images, %d intersect a queued image footprint\n",bad?"FAIL":"PASS",frames,strips,bad);
    return bad?1:0;
}
