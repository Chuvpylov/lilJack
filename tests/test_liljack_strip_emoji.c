/* WezTerm drops a wide colour emoji whose cell lies under ANY later sixel footprint
 * (own-window experiment, docs/reports/2026-09-13-tile-header-icons/): the lead ring
 * hid the claude tile's 🧠 and the status rules hid deepseek's 🔎. Strips must skip
 * the cells of a wide glyph exactly as they skip queued-image cells. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>
/* parse the wire: every DCS placed by a CUP; return how many footprints cover (col,row) */
/* a sixel's footprint width = the widest band in cells; no raster attribute is written, so count the band data */
static int sixel_width_px(const char *d,const char *end){int w=0,col=0,rep=0;const char *q=memchr(d,'q',(size_t)(end-d));if(!q)return 0;
    for(const char *p=q+1;p<end;p++){char c=*p;if(c=='\x1b')break;
        if(c=='!'){rep=0;p++;while(p<end&&*p>='0'&&*p<='9'){rep=rep*10+(*p-'0');p++;}p--;continue;}
        if(c=='#'){while(p+1<end&&((p[1]>='0'&&p[1]<='9')||p[1]==';'))p++;rep=0;continue;}
        if(c=='$'){if(col>w)w=col;col=0;rep=0;continue;}
        if(c=='-'){if(col>w)w=col;col=0;rep=0;continue;}
        if(c>='?'&&c<='~'){col+=rep?rep:1;rep=0;}}
    if(col>w)w=col;return w;}
static int footprints_over(const char *w,size_t n,int col,int row,int cw,int ch){
    int hits=0;int crow=-1,ccol=-1;
    for(size_t i=0;i+1<n;i++){
        if(w[i]==0x1b&&w[i+1]=='['){int a=0,b=0;if(sscanf(w+i+2,"%d;%dH",&a,&b)==2){crow=a-1;ccol=b-1;}}
        if(w[i]==0x1b&&w[i+1]=='P'){const char *end=w+i;while(end+1<w+n&&!(end[0]==0x1b&&end[1]=='\\'))end++;
            int px=sixel_width_px(w+i,end);int cols=(px+cw-1)/cw;int seps=0;for(const char *p=w+i+2;p<end;p++)if(*p=='-')seps++;int rows=((seps+1)*6+ch-1)/ch;   /* one band per separator, whether or not it carries data */   /* the host's footprint: whole bands, rounded up to rows */
            if(crow>=0&&row>=crow&&row<crow+rows&&col>=ccol&&col<ccol+cols)hits++;}
    }
    return hits;
}
int main(void){
    int saved=dup(1);FILE *sink=tmpfile();assert(saved>=0&&sink);assert(dup2(fileno(sink),1)>=0);
    state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
    lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x08101a);
    lj_ansi_glyph(20,60,0x1F9E0,0xffffff,2);                     /* 🧠 at row 3, cols 2-3 */
    lj_ansi_text(40,60,"claude / mo-1",0xffffff,200);
    lj_ansi_separator(0,60,300,20,0x1c2b45,0);                    /* a horizontal rule strip across row 3, cols 0..29 */
    assert(lj_ansi_present());
    off_t end=lseek(1,0,SEEK_CUR);char *w=malloc((size_t)end+1);assert(pread(1,w,(size_t)end,0)==end);w[end]=0;dup2(saved,1);
    int over_icon=footprints_over(w,(size_t)end,2,3,10,20)+footprints_over(w,(size_t)end,3,3,10,20);
    int over_text=footprints_over(w,(size_t)end,10,3,10,20);
    int dcs=0;for(off_t i=0;i+1<end;i++)dcs+=w[i]==0x1b&&w[i+1]=='P';
    printf("strip-emoji: DCS=%d; footprints over the emoji cells=%d; over a plain text cell=%d; \n",dcs,over_icon,over_text);
    int ok=dcs>=1&&over_icon==0;printf("strip-emoji: %s\n",ok?"PASS":"FAIL");free(w);return ok?0:1;
}
