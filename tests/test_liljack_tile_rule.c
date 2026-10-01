/* tile-rule — visual-polish-chrome item 3: the tile STATUS RULE on the sixel
 * path is a 1 px separator strip in the tile's own bottom row, either side of
 * the status text; EDGE idle, GREEN on the focused tile; never in the ring or
 * divider cells. The tree already draws it this way since the hairline helper
 * routes through lj_ansi_separator (sixel-dividers-hairline); this is the
 * planted proof so it cannot regress. Run with LJ_TILE_RULE_NO_SIXEL=1 to see
 * the test detect the glyph fallback (the checks then FAIL by design). */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    int sixel=getenv("LJ_TILE_RULE_NO_SIXEL")?0:1;
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        state.sixel=sixel;state.cellw=10;state.cellh=20;
        lj_ansi_begin(a->w,a->h);render(a);
        int tiles=0;
        for(int t=0;t<a->dock.tile_count;t++){
            lj_dock_tile *tile=&a->dock.tiles[t];Session *ss=session(a,tile->id);if(!ss||!terminal_tile(ss))continue;tiles++;
            lj_rect r=tile->rect;int row=(r.y+r.h)/CELLH-1,c0=r.x/CELLW,c1=(r.x+r.w)/CELLW;
            /* the status text is in that row */
            char txt[512]={0};size_t n=0;for(int c=c0;c<c1&&n<500;c++){uint32_t cp=at(c,row)->cp;txt[n++]=cp&&cp<128?(char)cp:cp?'?':' ';}
            CHECK(strstr(txt,"demo replay")!=NULL,"%dx%d tile %s row %d has no status text: '%s'",a->w,a->h,tile->id,row,txt);
            /* a horizontal separator strip owns the rest of that row, inside the tile's columns */
            int strips=0,outside=0,thick=0;uint32_t colour=0;
            for(int i=0;i<state.border_count;i++){pixel_border *b=&state.borders[i];if(b->separator!=2||b->row!=row)continue;
                if(b->col<c0||b->col+b->cols>c1){/* another tile's rule on the same row */ if(b->col+b->cols<=c0||b->col>=c1)continue;outside++;}
                strips++;colour=b->gold;if(b->thickness>1)thick++;}
            CHECK(strips>=1,"%dx%d tile %s: no separator strip on its status row %d (sixel=%d)",a->w,a->h,tile->id,row,sixel);
            CHECK(outside==0,"%dx%d tile %s: %d strips cross the tile edge (ring/divider cells)",a->w,a->h,tile->id,outside);
            CHECK(thick==0,"%dx%d tile %s: status rule thickened (%d)",a->w,a->h,tile->id,thick);
            int sel=!strcmp(a->focus,tile->id);
            if(strips)CHECK((colour&0xffffff)==((sel?GREEN:EDGE)&0xffffff),"%dx%d tile %s: rule colour %06x, wanted %06x (focused=%d)",a->w,a->h,tile->id,colour&0xffffff,(sel?GREEN:EDGE)&0xffffff,sel);
            /* no ─ glyph replaced a status cell: every non-space cell in the row is text or a line glyph outside the label */
            int rules=0;for(int c=c0;c<c1;c++){uint32_t cp=at(c,row)->cp;if(cp==0x2500)rules++;}
            CHECK(rules==0,"%dx%d tile %s: %d U+2500 glyphs on the status row (glyph fallback while sixel=%d)",a->w,a->h,tile->id,rules,sixel);
        }
        CHECK(tiles>=2,"%dx%d only %d session tiles",a->w,a->h,tiles);
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();
    fprintf(stderr,"%s tile-rule: %d checks, %d failed (status rule = 1 px separator strip inside the tile row beside the status text, no glyph fallback, not thickened) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
