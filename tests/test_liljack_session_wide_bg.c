/* Real session painter: a continuation-cell fill must preserve wide ink. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>

int main(void){
    assert(setlocale(LC_CTYPE,"C.UTF-8"));
    const int sizes[][2]={{800,560},{1280,720},{1920,1080}};
    const char *samples[]={"中","💬"};
    int failed=0;
    for(unsigned z=0;z<3;z++){
        int w=sizes[z][0],h=sizes[z][1];
        assert(!lj_render_init(w,h));render_ansi=0;
        App *a=calloc(1,sizeof *a);assert(a);a->demo=1;a->w=w;a->h=h;
        a->backend_in=a->backend_out=-1;a->nsession=1;
        Session *s=&a->sessions[0];s->fd=-1;s->cols=80;s->rows=24;
        copy(s->id,sizeof s->id,"wide-test");copy(s->agent,sizeof s->agent,"shell");
        copy(s->state,sizeof s->state,"demo");
        lj_dock_tile tile={0};copy(tile.id,sizeof tile.id,s->id);
        BlockCtx ctx={.r={20,40,w-40,h-80},.tile=&tile,.headtext=40};
        for(unsigned sample=0;sample<2;sample++){
            s->vt=lj_vt_new(s->cols,s->rows);assert(s->vt);
            char input[100];snprintf(input,sizeof input,"\033[48;2;18;52;86m%s\033[0m",samples[sample]);
            lj_vt_feed(s->vt,(const unsigned char*)input,(int)strlen(input));
            uint32_t cp=lj_vt_row(s->vt,0)[0].ch;
            assert(lj_render_cells_for(cp)==2&&lj_vt_row(s->vt,0)[1].ch==0);
            draw_block_session(a,&ctx);
            uint32_t actual[2*LJ_CELL_W*LJ_LINE_H];
            const uint32_t *px=lj_render_pixels();
            for(int y=0;y<CELLH;y++)for(int x=0;x<2*CELLW;x++)
                actual[y*2*CELLW+x]=px[(s->grid.y+y)*w+s->grid.x+x];
            /* demo is one whole glyph over its full cell background. */
            lj_render_rect(s->grid.x,s->grid.y,2*CELLW,CELLH,0x123456);
            lj_render_text_trim(2);
            lj_render_glyph(s->grid.x,s->grid.y,cp,cellcolor(lj_vt_row(s->vt,0)[0].fg),2);
            lj_render_text_trim(0);
            px=lj_render_pixels();int mismatch=0,right_ink=0;
            for(int y=0;y<CELLH;y++)for(int x=0;x<2*CELLW;x++){
                uint32_t expected=px[(s->grid.y+y)*w+s->grid.x+x];
                mismatch+=expected!=actual[y*2*CELLW+x];
                right_ink+=x>=CELLW&&(expected&0xffffff)!=0x123456;
            }
            printf("wide-bg %dx%d U+%04X: mismatched=%d right-ink=%d %s\n",
                   w,h,cp,mismatch,right_ink,!mismatch&&right_ink?"PASS":"FAIL");
            failed|=mismatch!=0||right_ink==0;
            lj_vt_free(s->vt);s->vt=NULL;
        }
        free(a);lj_render_close();
    }
    return failed;
}
