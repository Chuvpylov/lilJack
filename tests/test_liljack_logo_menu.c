/* logo-menu — the burger opens ONE grouped dropdown (visual-polish-chrome
 * item 5, lead: B). ANSI canvas at 800x560 / 1280x720 / 1920x1080. Asserts:
 *   L1 clicking the burger opens a menu (menu_kind != 0) whose canvas shows the
 *      five group headers ROOMS SESSIONS MEDIA THEME HELP, top to bottom;
 *   L2 Quit is the last row and carries Ctrl+Q; Files carries the submenu arrow;
 *   L3 every H_MENU_ITEM hit rect lies inside the window and the rows do not
 *      overlap; no row is chopped against the right edge;
 *   L4 keyboard: Down from a fresh menu lands on the first ITEM (headers are
 *      skipped), Enter activates it (New room → the room dialog opens);
 *   L5 clicking Files opens the nested Files flyout; Esc closes the menu.
 * Fail-before: on the ribbon code the burger sets ribbon_open and no menu opens. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
static void row_text(int row,char *out,size_t cap){size_t n=0;for(int c=0;c<state.cols&&n+4<cap;c++){uint32_t cp=at(c,row)->cp;if(!cp)out[n++]=' ';else if(cp<128)out[n++]=(char)cp;else{n+=(size_t)snprintf(out+n,cap-n,"<%X>",cp);}}out[n]=0;}
static int find_row(const char *needle,int from){for(int r=from;r<state.rows;r++){char t[2048];row_text(r,t,sizeof t);if(strstr(t,needle))return r;}return -1;}
static Hit *find_hit(App *a,int kind,int index){for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==kind&&(index<0||a->hits[i].index==index))return &a->hits[i];return NULL;}
static void press(App *a,SDL_Keycode k){SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;event(a,&e);}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        lj_ansi_begin(a->w,a->h);render(a);
        Hit *b=find_hit(a,H_BURGER,-1);CHECK(b!=NULL,"%dx%d no burger hit",a->w,a->h);if(!b)continue;
        Hit burger=*b;activate(a,&burger);lj_ansi_begin(a->w,a->h);render(a);
        /* L1 */
        CHECK(a->menu_kind!=MENU_NONE,"%dx%d burger opened no menu (ribbon_open=%d)",a->w,a->h,a->ribbon_open);
        const char *hdr[5]={"ROOMS","SESSIONS","MEDIA","THEME","HELP"};int prev=-1,ok=1;
        for(int i=0;i<5;i++){int r=find_row(hdr[i],prev+1);if(r<0){ok=0;break;}prev=r;}
        CHECK(ok,"%dx%d headers not all present in order",a->w,a->h);
        /* L2 */
        int quit=find_row("Quit",0),files=find_row("Files",0),about=find_row("About lilJack",0);
        CHECK(quit>0&&about>0&&quit>about,"%dx%d Quit row %d, About row %d",a->w,a->h,quit,about);
        if(quit>0){char t[2048];row_text(quit,t,sizeof t);CHECK(strstr(t,"Ctrl+Q")!=NULL,"%dx%d Quit row lacks Ctrl+Q: %s",a->w,a->h,t);}
        if(files>0){char t[2048];row_text(files,t,sizeof t);CHECK(strstr(t,"<25B8>")!=NULL,"%dx%d Files row lacks the arrow: %s",a->w,a->h,t);}
        /* L3 */
        int items=0,bad=0;lj_rect prevr={0,0,0,0};
        for(int i=0;i<a->nhits;i++){Hit *h=&a->hits[i];if(h->kind!=H_MENU_ITEM)continue;items++;
            if(h->r.x<0||h->r.y<0||h->r.x+h->r.w>a->w||h->r.y+h->r.h>a->h)bad++;
            if(prevr.h&&h->r.y<prevr.y+prevr.h&&h->r.y+h->r.h>prevr.y&&h->r.x==prevr.x)bad++;prevr=h->r;}
        CHECK(items>=13&&bad==0,"%dx%d menu items=%d bad=%d",a->w,a->h,items,bad);
        {int chopped=0;for(int r=0;r<state.rows;r++){uint32_t cp=at(state.cols-1,r)->cp;if(cp&&cp!=' '&&r>2&&r<state.rows-2)chopped++;}
         CHECK(chopped==0,"%dx%d %d rows reach the right edge",a->w,a->h,chopped);}
        /* L4 */
        press(a,SDLK_DOWN);
        {MenuRow rows[24];int n=menu_rows(a,rows,24);
         CHECK(a->menu_sel>=0&&a->menu_sel<n&&rows[a->menu_sel].action==H_NEW_ROOM,"%dx%d Down landed on row %d",a->w,a->h,a->menu_sel);}
        press(a,SDLK_RETURN);
        CHECK(a->room_dialog&&a->menu_kind==MENU_NONE,"%dx%d Enter on New room: dialog=%d menu=%d",a->w,a->h,a->room_dialog,a->menu_kind);
        a->room_dialog=0;
        /* L5 */
        activate(a,&burger);lj_ansi_begin(a->w,a->h);render(a);
        {MenuRow rows[24];int n=menu_rows(a,rows,24),fi=-1;for(int i=0;i<n;i++)if(rows[i].action==H_FILES)fi=i;
         Hit *fh=find_hit(a,H_MENU_ITEM,fi);CHECK(fh!=NULL,"%dx%d no Files hit",a->w,a->h);
         if(fh){Hit f=*fh;activate(a,&f);CHECK(a->menu_kind==MENU_FILES,"%dx%d Files opened kind %d",a->w,a->h,a->menu_kind);}}
        menu_close(a);activate(a,&burger);press(a,SDLK_ESCAPE);
        CHECK(a->menu_kind==MENU_NONE,"%dx%d Esc left menu %d",a->w,a->h,a->menu_kind);
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();
    fprintf(stderr,"%s logo-menu: %d checks, %d failed (grouped dropdown, headers in order, Quit Ctrl+Q last, Files arrow + nested flyout, hit rects inside, keyboard Down/Enter/Esc) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
