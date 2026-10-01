/* Sender half of ansi-graphs-unsynced: every presented frame is fenced with
 * synchronized output (DECSET 2026) so owkTerm commits text + graph images at once.
 * Exactly one ESC[?2026h before the first byte and one ESC[?2026l after the last,
 * per frame that writes anything; an unchanged frame writes nothing at all. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>
static char *grab(size_t *len){off_t end=lseek(1,0,SEEK_CUR);assert(end>=0);*len=(size_t)end;char *p=malloc(*len+1);assert(p);
    assert(pread(1,p,*len,0)==(ssize_t)*len);p[*len]=0;assert(lseek(1,0,SEEK_SET)==0);assert(ftruncate(1,0)==0);return p;}
static int count(const char *p,size_t n,const char *pat){size_t m=strlen(pat);int c=0;for(size_t i=0;i+m<=n;i++)if(!memcmp(p+i,pat,m))c++;return c;}
static void frame(int variant){
    lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x08101a);
    lj_ansi_text(10,0,variant?"GPU 71%":"GPU 34%",0xffffff,200);
    static uint32_t px[3][30*20];for(int k=0;k<3;k++){for(int i=0;i<30*20;i++)px[k][i]=variant?0xff0000ff:0xffff0000;assert(lj_ansi_image(120+k*40,0,30,20,px[k],30,20));}
}
int main(void){
    int saved=dup(1);FILE *sink=tmpfile();assert(saved>=0&&sink);assert(dup2(fileno(sink),1)>=0);
    state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
    const char *H="\033[?2026h",*L="\033[?2026l";int bad=0;size_t n;char *p;
    for(int f=0;f<2;f++){                       /* two changing frames, each must be fenced once */
        frame(f);assert(lj_ansi_present());p=grab(&n);
        int hs=count(p,n,H),ls=count(p,n,L),imgs=count(p,n,"\033P");
        int head=n>=8&&!memcmp(p,H,8),tail=n>=8&&!memcmp(p+n-8,L,8);
        fprintf(stderr,"frame %d: bytes=%zu images=%d 2026h=%d (first)=%d 2026l=%d (last)=%d\n",f,n,imgs,hs,head,ls,tail);
        if(!(imgs==3&&hs==1&&ls==1&&head&&tail))bad++;free(p);
    }
    frame(1);assert(lj_ansi_present());p=grab(&n);   /* unchanged: nothing on the wire, no empty fence */
    fprintf(stderr,"idle frame: bytes=%zu\n",n);if(n!=0)bad++;free(p);
    dup2(saved,1);printf("ansi-sync-fence: %s\n",bad?"FAIL":"PASS");return bad?1:0;
}
