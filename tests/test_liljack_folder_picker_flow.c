/* folder-browser-popup — the operator 2026-09-16: "improve file browser popup window with
 * proper folder browser controls and folder icons, I can't really browse in a room
 * folder selector. Make it a proper popup floating window."
 *
 * The production controller, driven only through event(): SDL key, text, wheel and
 * pointer events, never by poking browser state. The harness builds a real tree
 * (46 visible folders, one hidden, one nested, one plain file). Asserts:
 *   F1 the New Room dialog has no inline 4-row picker; FOLDER has a BROWSE control
 *   F2 BROWSE opens a floating window inside the screen, listing every subfolder
 *      (sorted, hidden excluded, files excluded), each row with a folder icon
 *   F3 keyboard: Down/End/Home move the selection and the list scrolls to it;
 *      a letter jumps to the first folder starting with it; Enter opens; Left goes
 *      up and re-selects the folder you came from
 *   F4 mouse: wheel scrolls without moving the selection; a click selects, a second
 *      click opens; the chevron opens; a breadcrumb segment navigates
 *   F5 Ctrl+H shows and hides dot-folders
 *   F6 PATH: "/" starts editing, Enter goes to a typed folder, a bad path stays in
 *      edit with the folder unchanged, Esc leaves edit
 *   F7 the title bar drags and the grip resizes, both snapped to cells
 *   F8 modal: a click outside the window does not reach the dialog
 *   F9 USE FOLDER fills FOLDER and the create payload; Esc and CANCEL change nothing;
 *      Ctrl+O reopens at the chosen folder; FILES browser path untouched
 * Fail-before: the build has no H_ROOM_FOLDER_BROWSE / fb_* browser at all. */
#define main liljack_application_main
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <sys/resource.h>

static void lab_pointer(App *a, Uint32 type, int x, int y) {
    SDL_Event e = {0}; e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x=x; e.motion.y=y; }
    else { e.button.button=SDL_BUTTON_LEFT; e.button.x=x; e.button.y=y; }
    event(a,&e);
}
static void lab_key(App *a,SDL_Keycode k,Uint16 mod){SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;e.key.keysym.mod=mod;event(a,&e);render(a);}
/* SDL_TEXTINPUT carries at most 31 bytes: long paths arrive as several events, as they do live. */
static void lab_text(App *a,const char *s){size_t n=strlen(s);for(size_t o=0;o<n;o+=20){SDL_Event e={0};e.type=SDL_TEXTINPUT;snprintf(e.text.text,sizeof e.text.text,"%.20s",s+o);event(a,&e);}render(a);}
static void lab_wheel(App *a,int dy){SDL_Event e={0};e.type=SDL_MOUSEWHEEL;e.wheel.y=dy;event(a,&e);render(a);}
static void lab_png(App *a, const char *path) {
    /* Headless evidence is explicitly the cell fallback, never sixel proof. */
    lj_render_resize(a->w,a->h);
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++) {
        cell *c=&state.canvas[y*state.cols+x];
        lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
    }
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++) {
        cell *c=&state.canvas[y*state.cols+x];
        if(c->cp==0x2580)lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH/2,c->fg);
        else if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
        for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
    }
    assert(lj_render_save_png(path)==0);
}
static void lab_capture(App *a,const char *dir,const char *name) {
    char path[PATH_MAX];snprintf(path,sizeof path,"%s/%s.png",dir,name);render(a);lab_png(a,path);
}
static Hit *find(App *a,int kind,int index){render(a);for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&a->hits[i].index==index)return &a->hits[i];return NULL;}
static void click(App *a,int kind,int index){
    Hit *h=find(a,kind,index);
    if(!h){fprintf(stderr,"Missing control %d/%d\n",kind,index);abort();}
    lj_rect r=h->r;int x=r.x+r.w/2,y=r.y+r.h/2;
    assert(r.x>=0&&r.y>=0&&r.x+r.w<=a->w&&r.y+r.h<=a->h);
    lab_pointer(a,SDL_MOUSEMOTION,x,y);lab_pointer(a,SDL_MOUSEBUTTONDOWN,x,y);lab_pointer(a,SDL_MOUSEBUTTONUP,x,y);render(a);
}
static int entry_index(App *a,const char *name){for(int i=0;i<a->room_folder_count;i++)if(!strcmp(a->room_folder_entries[i],name))return i;return -1;}
static int count_cp(uint32_t cp){int n=0;for(int i=0;i<state.rows*state.cols;i++)if(state.canvas[i].cp==cp)n++;return n;}
int main(int argc,char **argv){
    assert(argc==5);setlocale(LC_CTYPE,"C.UTF-8");
    App *a=calloc(1,sizeof *a);assert(a);
    a->w=atoi(argv[1]);a->h=atoi(argv[2]);a->demo=a->ansi=a->running=1;
    a->split=-1;a->backend_in=a->backend_out=-1;a->mousex=a->mousey=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);
    assert(!lj_render_init(a->w,a->h));demo(a);a->toast[0]=0;
    const char *tree=argv[3];char alpha[PATH_MAX],nested[PATH_MAX];
    snprintf(alpha,sizeof alpha,"%s/alpha",tree);snprintf(nested,sizeof nested,"%s/alpha/nested",tree);
    copy(a->project,sizeof a->project,tree);
    copy(a->files_path,sizeof a->files_path,"unchanged-browser-path");
    room_dialog_open(a);copy(a->room_name,sizeof a->room_name,"Folder picker proof");
    /* F1 */
    click(a,H_ROOM_FIELD,RF_FOLDER);
    assert(!find(a,H_ROOM_FOLDER_ENTRY,0)&&"the inline picker is gone");
    assert(find(a,H_ROOM_FOLDER_BROWSE,0));
    /* F2 */
    click(a,H_ROOM_FOLDER_BROWSE,0);
    assert(a->fb_open&&!strcmp(a->room_folder_base,tree));
    assert(a->room_folder_count==46&&!strcmp(a->room_folder_entries[0],"alpha")&&entry_index(a,".hidden")<0&&entry_index(a,"ordinary-file.txt")<0);
    lj_rect r=a->fb_rect;assert(r.x>=0&&r.y>=0&&r.x+r.w<=a->w&&r.y+r.h<=a->h&&r.w<a->w);
    int kinds[]={H_ROOM_FOLDER_UP,H_FB_HOME,H_FB_PROJECT,H_FB_HIDDEN,H_FB_CANCEL,H_ROOM_FOLDER_USE,H_FB_PATH,H_FB_CLOSE};
    for(size_t k=0;k<sizeof kinds/sizeof *kinds;k++){Hit *h=find(a,kinds[k],0);assert(h);assert(h->r.x>=r.x&&h->r.y>=r.y&&h->r.x+h->r.w<=r.x+r.w&&h->r.y+h->r.h<=r.y+r.h);}
    int shown=0;render(a);for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_ROOM_FOLDER_ENTRY)shown++;
    assert(shown==a->fb_rows&&shown>=3);
    assert(count_cp(0x1F4C1)>=shown);
    lab_capture(a,argv[4],"browser");
    /* F3 */
    lab_key(a,SDLK_DOWN,0);lab_key(a,SDLK_DOWN,0);assert(a->fb_sel==2);
    lab_key(a,SDLK_END,0);assert(a->fb_sel==45&&a->fb_scroll>0&&find(a,H_ROOM_FOLDER_ENTRY,45));
    lab_capture(a,argv[4],"scrolled-end");
    lab_key(a,SDLK_HOME,0);assert(a->fb_sel==0&&a->fb_scroll==0);
    lab_text(a,"z");assert(!strcmp(a->room_folder_entries[a->fb_sel],"zeta"));
    lab_text(a,"f");assert(!strcmp(a->room_folder_entries[a->fb_sel],"folder with spaces"));
    lab_key(a,SDLK_HOME,0);lab_key(a,SDLK_RETURN,0);assert(!strcmp(a->room_folder_base,alpha)&&a->room_folder_count==1);
    lab_key(a,SDLK_LEFT,0);assert(!strcmp(a->room_folder_base,tree)&&!strcmp(a->room_folder_entries[a->fb_sel],"alpha"));
    /* F4 */
    {Hit *row=find(a,H_ROOM_FOLDER_ENTRY,0);assert(row);lab_pointer(a,SDL_MOUSEMOTION,row->r.x+CELLW,row->r.y+CELLH/2);}
    int sel=a->fb_sel;lab_wheel(a,-2);assert(a->fb_scroll>0&&a->fb_sel==sel);
    lab_wheel(a,10);assert(a->fb_scroll==0);
    click(a,H_ROOM_FOLDER_ENTRY,1);assert(a->fb_sel==1&&!strcmp(a->room_folder_base,tree));
    click(a,H_ROOM_FOLDER_ENTRY,1);assert(strcmp(a->room_folder_base,tree)&&"second click opens");
    lab_key(a,SDLK_BACKSPACE,0);assert(!strcmp(a->room_folder_base,tree));
    click(a,H_FB_OPEN,entry_index(a,"alpha"));assert(!strcmp(a->room_folder_base,alpha));
    click(a,H_ROOM_FOLDER_ENTRY,0);click(a,H_ROOM_FOLDER_ENTRY,0);assert(!strcmp(a->room_folder_base,nested)&&a->room_folder_count==0);
    click(a,H_FB_CRUMB,(int)strlen(tree));assert(!strcmp(a->room_folder_base,tree));
    /* F5 */
    lab_key(a,SDLK_h,KMOD_LCTRL);assert(a->room_folder_count==47&&entry_index(a,".hidden")>=0);
    lab_capture(a,argv[4],"hidden-shown");
    lab_key(a,SDLK_h,KMOD_LCTRL);assert(a->room_folder_count==46);
    /* F6 */
    lab_text(a,"/");assert(a->fb_edit);
    lab_key(a,SDLK_u,KMOD_LCTRL);lab_text(a,nested);lab_capture(a,argv[4],"path-edit");
    lab_key(a,SDLK_RETURN,0);assert(!a->fb_edit&&!strcmp(a->room_folder_base,nested));
    lab_key(a,SDLK_l,KMOD_LCTRL);assert(a->fb_edit);lab_key(a,SDLK_u,KMOD_LCTRL);lab_text(a,"/nonexistent/liljack-folder-picker");
    lab_key(a,SDLK_RETURN,0);assert(a->fb_edit&&!strcmp(a->room_folder_base,nested)&&a->toast[0]);
    lab_key(a,SDLK_ESCAPE,0);assert(!a->fb_edit&&a->fb_open);
    lab_key(a,SDLK_u,KMOD_LCTRL);   /* not editing: Ctrl+U is ignored, never clears FOLDER */
    lab_key(a,SDLK_LEFT,0);lab_key(a,SDLK_LEFT,0);assert(!strcmp(a->room_folder_base,tree));
    /* F7 */
    render(a);r=a->fb_rect;
    lab_pointer(a,SDL_MOUSEMOTION,r.x+5*CELLW+3,r.y+CELLH+5);lab_pointer(a,SDL_MOUSEBUTTONDOWN,r.x+5*CELLW+3,r.y+CELLH+5);
    int mx=r.x>=3*CELLW?-3:3,my=r.y>=2*CELLH?-2:2;
    lab_pointer(a,SDL_MOUSEMOTION,r.x+5*CELLW+3+mx*CELLW,r.y+CELLH+5+my*CELLH);lab_pointer(a,SDL_MOUSEBUTTONUP,r.x+5*CELLW+3+mx*CELLW,r.y+CELLH+5+my*CELLH);render(a);
    assert(a->fb_rect.x==r.x+mx*CELLW&&a->fb_rect.y==r.y+my*CELLH&&a->fb_rect.w==r.w&&!a->fb_dragging);
    r=a->fb_rect;{int gx=r.x+r.w-CELLW/2,gy=r.y+r.h-CELLH/2;
    lab_pointer(a,SDL_MOUSEMOTION,gx,gy);lab_pointer(a,SDL_MOUSEBUTTONDOWN,gx,gy);lab_pointer(a,SDL_MOUSEMOTION,gx-4*CELLW,gy-2*CELLH);lab_pointer(a,SDL_MOUSEBUTTONUP,gx-4*CELLW,gy-2*CELLH);}
    render(a);assert(a->fb_rect.w==r.w-4*CELLW&&a->fb_rect.h==r.h-2*CELLH&&a->fb_rect.x==r.x&&!a->fb_resizing);
    lab_capture(a,argv[4],"moved-resized");
    /* F8 */
    r=a->fb_rect;{int ox=r.x>CELLW?1:r.x+r.w+1,oy=r.y>CELLH?1:r.y+r.h+1;
    lab_pointer(a,SDL_MOUSEBUTTONDOWN,ox,oy);lab_pointer(a,SDL_MOUSEBUTTONUP,ox,oy);render(a);}
    assert(a->fb_open&&a->room_dialog);
    /* F9 */
    click(a,H_FB_OPEN,entry_index(a,"alpha"));click(a,H_ROOM_FOLDER_USE,0);
    assert(!a->fb_open&&a->room_field==RF_PURPOSE&&!strcmp(a->room_folder,alpha));
    lab_key(a,SDLK_o,KMOD_LCTRL);assert(a->fb_open&&!strcmp(a->room_folder_base,alpha));
    lab_key(a,SDLK_LEFT,0);lab_key(a,SDLK_ESCAPE,0);assert(!a->fb_open&&!strcmp(a->room_folder,alpha));
    click(a,H_ROOM_FOLDER_BROWSE,0);lab_key(a,SDLK_LEFT,0);click(a,H_FB_CANCEL,0);assert(!a->fb_open&&!strcmp(a->room_folder,alpha));
    click(a,H_ROOM_FOLDER_BROWSE,0);lab_key(a,SDLK_LEFT,0);lab_key(a,SDLK_RETURN,KMOD_LCTRL);assert(!a->fb_open&&!strcmp(a->room_folder,tree));
    click(a,H_ROOM_FOLDER_BROWSE,0);lab_key(a,SDLK_RETURN,0);click(a,H_FB_CLOSE,0);assert(!a->fb_open&&!strcmp(a->room_folder,tree));
    click(a,H_ROOM_FOLDER_BROWSE,0);lab_text(a,"a");lab_key(a,SDLK_RIGHT,0);lab_key(a,SDLK_RETURN,KMOD_LCTRL);assert(!strcmp(a->room_folder,alpha));
    int fds[2];assert(pipe(fds)==0);a->backend_in=fds[1];a->demo=0;a->busy=0;
    click(a,H_ROOM_CREATE,0);close(fds[1]);a->backend_in=-1;
    char payload[8192];ssize_t n=read(fds[0],payload,sizeof payload-1);assert(n>0);payload[n]=0;close(fds[0]);
    json_object *o=json_tokener_parse(payload);assert(o);
    assert(!strcmp(jstr(o,"action"),"room_start")&&!strcmp(jstr(o,"folder"),alpha));json_object_put(o);
    assert(!strcmp(a->files_path,"unchanged-browser-path"));
    printf("PASS %dx%d folder-browser popup: F1 no inline picker · F2 floating list+icons · F3 keys · F4 wheel/click/chevron/crumb · F5 hidden · F6 path edit · F7 drag/resize · F8 modal · F9 use/cancel/esc/ctrl+o/payload\n",a->w,a->h);
    for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
    if(a->messages)json_object_put(a->messages);drafts_close(a);lj_popup_close(&a->popup);lj_media_close(&a->media);
    free(a->pending);free(a);lj_render_close();lj_ansi_close();SDL_Quit();return 0;
}
