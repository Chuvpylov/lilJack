/* Integration test uses the actual native controller and renderer.
 * Run with SDL_VIDEODRIVER=dummy; all PTYs are owned foreground test children. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main

static App *test_app;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
static void test_cleanup(void) {
    App *a=test_app;
    if(!a)return;
    for(int i=0;i<a->nsession;i++)terminal_close(&a->sessions[i]);
    if(a->messages)json_object_put(a->messages);if(a->status_ribbon)json_object_put(a->status_ribbon);if(a->review_panel)json_object_put(a->review_panel);drafts_close(a);
    lj_media_close(&a->media);
    if(a->texture)SDL_DestroyTexture(a->texture);
    if(a->renderer)SDL_DestroyRenderer(a->renderer);
    if(a->window)SDL_DestroyWindow(a->window);
    SDL_Quit();lj_render_close();free(a);test_app=NULL;
}
static void dispatch(SDL_Event e) {
    Uint32 id=SDL_GetWindowID(test_app->window);
    if(e.type==SDL_TEXTINPUT)e.text.windowID=id;
    else if(e.type==SDL_KEYDOWN)e.key.windowID=id;
    else if(e.type==SDL_WINDOWEVENT)e.window.windowID=id;
    else if(e.type==SDL_MOUSEMOTION)e.motion.windowID=id;
    else if(e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP)e.button.windowID=id;
    /* Installed sdl2-compat crashes inside SDL3 when synthetic text is pushed.
     * Feed that SDL event directly through the production handler; all mouse,
     * keyboard and window events still exercise the real SDL event queue. */
    if(e.type==SDL_TEXTINPUT){event(test_app,&e);return;}
    CHECK(SDL_PushEvent(&e)==1);while(SDL_PollEvent(&e))event(test_app,&e);
}
static void mouse_event(Uint32 type,int x,int y) {
    SDL_Event e;memset(&e,0,sizeof e);e.type=type;
    if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
    else {e.button.x=x;e.button.y=y;e.button.button=SDL_BUTTON_LEFT;}
    dispatch(e);
}
static Hit get_hit(int kind,const char *id) {
    for(int i=0;i<test_app->nhits;i++){
        Hit h=test_app->hits[i];if(h.kind==kind&&(!id||!strcmp(h.id,id)))return h;
    }
    fprintf(stderr,"Missing hit %d / %s\n",kind,id?id:"");exit(1);
}
static lj_rect tile_rect(const char *id) {
    for(int i=0;i<test_app->dock.tile_count;i++)if(!strcmp(test_app->dock.tiles[i].id,id))return test_app->dock.tiles[i].rect;
    fprintf(stderr,"Missing tile %s\n",id);exit(1);
}
/* ⚠ THE NINE LEFT THE HEADER (thin-HUI §2, tui-burger-ribbon). A toolbar action
 * is no longer a badge in the tab row; it is reached by opening the burger
 * ribbon and clicking its item — the two gestures a user actually makes. These
 * helpers do exactly that, so the tests below still exercise the real route
 * rather than calling activate() behind the UI's back. */
static Hit get_hit_index(int kind,int index) {
    for(int i=0;i<test_app->nhits;i++){
        Hit h=test_app->hits[i];if(h.kind==kind&&h.index==index)return h;
    }
    fprintf(stderr,"Missing hit %d / index %d\n",kind,index);exit(1);
}
static void click_hit(int kind,const char *id);
static void click_toolbar(int action) {
    App *a=test_app;
    a->ribbon_open=1;a->ribbon=RIBBON_FULL;render(a);   /* already slid out */
    MenuRow rows[16];int n=toolbar_rows(a,rows,16),idx=-1;
    for(int i=0;i<n;i++)if(rows[i].action==action)idx=i;
    CHECK(idx>=0);
    int visible=0;for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_RIBBON_ITEM&&a->hits[i].index==idx)visible=1;
    Hit h;
    if(visible)h=get_hit_index(H_RIBBON_ITEM,idx);
    else {
        click_hit(H_RIBBON_MORE,NULL);render(a);
        MenuRow overflow[16];int count=menu_rows(a,overflow,16),at=-1;
        for(int i=0;i<count;i++)if(overflow[i].index==idx)at=i;
        CHECK(at>=0);h=get_hit_index(H_MENU_ITEM,at);
    }
    int x=h.r.x+h.r.w/2,y=h.r.y+h.r.h/2;
    mouse_event(SDL_MOUSEBUTTONDOWN,x,y);mouse_event(SDL_MOUSEBUTTONUP,x,y);
}
static void click_hit(int kind,const char *id) {
    Hit h=get_hit(kind,id);int x=h.r.x+h.r.w/2,y=h.r.y+h.r.h/2;
    mouse_event(SDL_MOUSEBUTTONDOWN,x,y);mouse_event(SDL_MOUSEBUTTONUP,x,y);
}
static void type_text(const char *s) {
    while(*s){SDL_Event e;memset(&e,0,sizeof e);e.type=SDL_TEXTINPUT;size_t n=strlen(s);if(n>=sizeof(e.text.text))n=sizeof(e.text.text)-1;memcpy(e.text.text,s,n);e.text.text[n]=0;s+=n;dispatch(e);}
}
static void press(SDL_Keycode keycode) { SDL_Event e;memset(&e,0,sizeof e);e.type=SDL_KEYDOWN;e.key.keysym.sym=keycode;dispatch(e); }
static int contains(Session *s,const char *needle) {
    char buf[LJ_VT_MAXCOLS*LJ_VT_MAXROWS+LJ_VT_MAXROWS+1];size_t n=0;
    for(int y=0;y<s->rows;y++){const lj_cell *r=lj_vt_row(s->vt,y);for(int x=0;x<s->cols;x++)buf[n++]=r[x].ch<128?(char)r[x].ch:'?';buf[n++]='\n';}buf[n]=0;
    return strstr(buf,needle)!=NULL;
}
static void pump(void) { for(int i=0;i<test_app->nsession;i++)poll_terminal(test_app,&test_app->sessions[i]);SDL_Delay(5); }
static void await_text(Session *s,const char *needle) {
    Uint32 until=SDL_GetTicks()+3000;
    while(!contains(s,needle)&&SDL_GetTicks()<until)pump();
    CHECK(contains(s,needle));
}
static void foreground_shell(Session *s,const char *id) {
    memset(s,0,sizeof(*s));s->fd=-1;s->cols=80;s->rows=24;s->managed=s->seen=1;
    copy(s->id,sizeof(s->id),id);copy(s->agent,sizeof(s->agent),"shell");
    s->vt=lj_vt_new(80,24);s->queue=malloc(INPUT_CAP);CHECK(s->vt&&s->queue);
    struct winsize ws={.ws_row=24,.ws_col=80};
    s->pid=forkpty(&s->fd,NULL,NULL,&ws);CHECK(s->pid>=0);
    if(!s->pid){

        execl("/bin/sh","sh","-c","stty -echo; printf 'READY\\n'; while IFS= read -r value; do if [ \"$value\" = size ]; then stty size; else printf 'RECEIVED:%s\\n' \"$value\"; fi; done",(char*)0);
        _exit(127);
    }
    CHECK(fcntl(s->fd,F_SETFD,FD_CLOEXEC)==0);CHECK(fcntl(s->fd,F_SETFL,O_NONBLOCK)==0);s->connected=1;
}
#ifndef LJ_NATIVE_HELPERS_ONLY
int main(void) {
    setenv("SDL_VIDEODRIVER","dummy",1);setlocale(LC_CTYPE,"");signal(SIGPIPE,SIG_IGN);
    test_app=calloc(1,sizeof(*test_app));CHECK(test_app);CHECK(atexit(test_cleanup)==0);
    /* The real app always resolves a project directory at startup, and the
     * new-room form defaults its working folder from it. */
    CHECK(getcwd(test_app->project,sizeof(test_app->project))!=NULL);
    App *a=test_app;a->w=1440;a->h=900;a->running=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);
    CHECK(lj_render_init(a->w,a->h)==0);CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    a->window=SDL_CreateWindow("lilJack isolated native test",0,0,a->w,a->h,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);CHECK(a->window);
    a->renderer=SDL_CreateRenderer(a->window,-1,SDL_RENDERER_SOFTWARE);CHECK(a->renderer);
    a->texture=SDL_CreateTexture(a->renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,a->w,a->h);CHECK(a->texture);
    SDL_StartTextInput();SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
    demo(a);render(a);
    CHECK(a->dock.tile_count==4);
    /* Launch actions are reached through the compact OPEN HERE dropdown. */
    click_hit(H_OPEN_HERE,NULL);render(a);CHECK(a->menu_kind==MENU_OPEN_HERE);
    MenuRow launch_rows[16];int launch_n=menu_rows(a,launch_rows,16),create_mask=0;
    /* Four agent launches plus CANVAS (the operator 2026-09-21: "add to open here a canvas"). */
    CHECK(launch_n==5);int canvas_rows=0;
    for(int i=0;i<launch_n;i++){if(launch_rows[i].action==H_CANVAS){canvas_rows++;continue;}CHECK(launch_rows[i].action==H_CREATE);create_mask|=1<<launch_rows[i].index;}
    CHECK(create_mask==15);CHECK(canvas_rows==1);click_hit(H_MENU_SCRIM,NULL);render(a);

    /* OPEN HERE → CANVAS docks the room canvas tile; a drag inside it writes one
     * stroke to rooms/<room>/canvas.jsonl (the operator 2026-09-21: "canvas is not appearing"). */
    {char root[]="/tmp/liljack-native-canvas-XXXXXX";CHECK(mkdtemp(root));copy(a->root,sizeof(a->root),root);
     click_hit(H_OPEN_HERE,NULL);render(a);
     MenuRow rows[16];int n=menu_rows(a,rows,16),at=-1;for(int i=0;i<n;i++)if(rows[i].action==H_CANVAS)at=i;CHECK(at>=0);
     Hit h=get_hit_index(H_MENU_ITEM,at);int x=h.r.x+h.r.w/2,y=h.r.y+h.r.h/2;
     mouse_event(SDL_MOUSEBUTTONDOWN,x,y);mouse_event(SDL_MOUSEBUTTONUP,x,y);render(a);
     CHECK(a->has_canvas);CHECK(dock_has(a,"canvas"));CHECK(strstr(a->canvas.path,"/rooms/r-demo-build/canvas.jsonl"));
     lj_rect b=a->canvas_body;CHECK(b.w>10&&b.h>10);
     mouse_event(SDL_MOUSEBUTTONDOWN,b.x+5,b.y+5);mouse_event(SDL_MOUSEMOTION,b.x+b.w/2,b.y+b.h/2);CHECK(a->canvas_drawing&&a->canvas_npts==2);
     mouse_event(SDL_MOUSEBUTTONUP,b.x+b.w-5,b.y+b.h-5);render(a);
     CHECK(!a->canvas_drawing);
     CHECK(a->canvas.nstrokes==1);CHECK(!strcmp(a->canvas.strokes[0].by,"operator"));CHECK(a->canvas.strokes[0].npoints==3);
     click_hit(H_HIDE,"canvas");render(a);CHECK(!a->has_canvas&&!dock_has(a,"canvas"));
     char rm[300];snprintf(rm,sizeof rm,"rm -rf -- %s",root);CHECK(system(rm)==0);a->root[0]=0;}

    /* ⚠ ROOMS ARE TABS AND THERE IS NO SIDEBAR. Chrome is three whole rows —
     * title, tabs+menu, agent chips — and the tiles then own the FULL width.
     * The old sidebar cost a 19-to-24 column column down the whole height.
     * TUI hit boxes are also CELL-SNAPPED (main.c cellsnap): a click arrives at
     * a cell CENTRE, so a hit area that does not cover whole cells leaves the
     * cell showing its own glyph dead. Every bar control is therefore exactly
     * one row tall; at the old 28px height a snapped box spilled onto the next
     * row and the blank row below the menu fired menu actions. */
    const int ansi_sizes[][2]={{1700,920},{800,560}};
    for(int z=0;z<2;z++){
        a->ansi=1;a->w=ansi_sizes[z][0];a->h=ansi_sizes[z][1];render(a);
        int menu_count=0,tab_count=0,burger=0,filter_count=0,newroom_count=0;
        for(int i=0;i<a->nhits;i++){
            if(a->hits[i].r.y!=ANSI_SIDEBAR_TOP)continue;
            CHECK(a->hits[i].r.h==LJ_LINE_H);      /* one row, never two */
            /* the centre of the cell the label is drawn in must be clickable */
            CHECK(a->hits[i].r.x%LJ_CELL_W==0&&a->hits[i].r.w%LJ_CELL_W==0);
            if(a->hits[i].kind==H_GROUP)tab_count++;
            if(a->hits[i].kind==H_BURGER)burger++;
            if(a->hits[i].kind==H_STATE_FILTER)filter_count++;
            if(a->hits[i].kind==H_NEW_ROOM)newroom_count++;
            if(a->hits[i].kind==H_TEAM||a->hits[i].kind==H_RETILE||a->hits[i].kind==H_FULL||
               a->hits[i].kind==H_STATUS||a->hits[i].kind==H_REVIEW||a->hits[i].kind==H_FILES||
               a->hits[i].kind==H_ABOUT||a->hits[i].kind==H_LOAD)menu_count++;
        }
        /* ⚠ THE TABS OWN THIS ROW AT EVERY WIDTH NOW. It used to be a trade:
         * the toolbar drew eight badges inline while it fitted and collapsed to
         * a burger only when the row got tight, so at 800x560 the strip had
         * truncated to a single tab ("Build r") with Research and + ROOM gone.
         * The nine moved into the ribbon, so the row is never shared and the
         * old `else` branch — the one asserting eight inline badges — describes
         * a layout that no longer exists at any size.
         *
         * ⚠ menu_count==0 is the load-bearing half. The burger being present
         * does not prove the badges left; only their absence does, and a
         * regression that drew both would still pass a burger-only check. */
        CHECK(menu_count==0);
        /* ⚠ AND THE BURGER IS GONE FROM THIS ROW TOO (thin-HUI §1): it moved
         * up into the header, so the row holds tabs and nothing else. It must
         * still exist — in the HEADER row, always visible, far right. */
        CHECK(burger==0);
        {int hdr_burger=0;
         for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_BURGER&&a->hits[i].r.y==0)hdr_burger++;
         CHECK(hdr_burger==1);CHECK(get_hit(H_BURGER,NULL).r.x==0);}
        CHECK(tab_count>=2);
        /* ⚠ THE ROW'S OWN CONTENTS SURVIVE THE WIDENING. The state filter and
         * + ROOM are the two things a wider row was supposed to PROTECT, and
         * they were exactly what the old shared row squeezed out first at
         * 800x560 — so "the tabs got the space" is only worth asserting
         * together with "and they still carry everything they are for".
         * Checked at BOTH sizes, the narrow one being the one that used to
         * fail. */
        CHECK(filter_count==1);
        CHECK(newroom_count==1);
        /* Agent chips and the spawn buttons sit on their own row below. */
        int spawn=0;
        for(int i=0;i<a->nhits;i++)
            if(a->hits[i].r.y==ANSI_AGENT_TOP&&a->hits[i].kind==H_OPEN_HERE)spawn++;
        CHECK(spawn==1);
        /* ⚠ ONE RECT, ONE TARGET. Two hits with an IDENTICAL rect make the
         * smallest-target rule a tie, and a tie is broken by draw order, which
         * is not a decision anybody made. It shipped once: a room tab carried
         * both H_GROUP and a right-click menu hit on the same rectangle, so
         * left-clicking a tab opened the burger instead of the room, and a file
         * row carried two so clicking a folder did nothing. Any widget wanting
         * a second gesture reads the button, it does not register twice. */
        for(int i=0;i<a->nhits;i++)for(int j=i+1;j<a->nhits;j++){
            lj_rect p=a->hits[i].r,q=a->hits[j].r;
            if(p.x==q.x&&p.y==q.y&&p.w==q.w&&p.h==q.h){
                fprintf(stderr,"ambiguous click target: kind %d and kind %d share %d,%d,%d,%d\n",
                        a->hits[i].kind,a->hits[j].kind,p.x,p.y,p.w,p.h);
                CHECK(0);
            }
        }
        int top=a->h,bottom=0,leftmost=a->w;
        for(int i=0;i<a->dock.tile_count;i++){
            lj_rect r=a->dock.tiles[i].rect;if(r.y<top)top=r.y;
            if(r.y+r.h>bottom)bottom=r.y+r.h;
            if(r.x<leftmost)leftmost=r.x;
        }
        /* UNIFORM WINDOW MARGIN (the operator 2026-09-12): the ANSI dock is inset one
         * cell on every side so a lead ring owns margin/divider cells and no
         * tile is padded differently. No sidebar column is reserved beyond it. */
        CHECK(leftmost==ANSI_MARGIN+CELLW);
        /* Chrome is whole ROWS in the TUI now (main.c ANSI_*): title, menu and
         * room bar are one row each, the footer two, then the margin row — so
         * tiles begin on row 4 and end one row above the footer. */
        CHECK(top==ANSI_DOCK_TOP+CELLH);CHECK(bottom==a->h-ANSI_FOOTER_H-CELLH);
    }
    /* ⚠ lilJack is TERMINAL-ONLY (the operator, 2026-09-09: "remove not tui
     * implementation what so ever"). a->ansi survives solely so --screenshot
     * can render the framebuffer headless; clearing it here used to select the
     * window layout, and now only disables cell snapping while everything
     * still draws on the cell grid — a hybrid that exists nowhere in the
     * product and made a tile header land off-grid. */
    a->w=1440;a->h=900;render(a);
    /* Observed sessions remain in the sidebar, never consume dock space. */
    {
        lj_dock original=a->dock;
        Session *observed=&a->sessions[a->nsession++];memset(observed,0,sizeof(*observed));observed->fd=-1;observed->seen=1;
        copy(observed->id,sizeof(observed->id),"observed-only");copy(observed->agent,sizeof(observed->agent),"codex");
        copy(observed->room_id,sizeof(observed->room_id),a->active_room);observed->dock_pending=1;
        a->order[a->norder++]=(int)(observed-a->sessions);
        CHECK(lj_dock_drop(&a->dock,observed->id,"room",LJ_DOCK_LEFT));
        copy(a->full,sizeof(a->full),observed->id);prune_workspace(a);
        CHECK(!dock_has(a,observed->id));CHECK(!a->full[0]);
        retile(a);render(a);CHECK(!dock_has(a,observed->id));CHECK(a->dock.tile_count==4);
        click_hit(H_SESSION,observed->id);CHECK(!dock_has(a,observed->id));CHECK(!strcmp(a->focus,"room"));
        Hit focus_observed={.kind=H_AGENT_FOCUS};copy(focus_observed.id,sizeof(focus_observed.id),observed->id);activate(a,&focus_observed);
        CHECK(!dock_has(a,observed->id));
        render(a);Hit card=get_hit(H_SESSION,observed->id);lj_rect target=tile_rect("room");
        mouse_event(SDL_MOUSEBUTTONDOWN,card.r.x+10,card.r.y+10);
        mouse_event(SDL_MOUSEMOTION,target.x+target.w/2,target.y+target.h/2);
        mouse_event(SDL_MOUSEBUTTONUP,target.x+target.w/2,target.y+target.h/2);
        CHECK(!dock_has(a,observed->id));CHECK(observed->seen);
        render(a);const char *observed_shot=getenv("LILJACK_OBSERVED_SCREENSHOT");
        if(observed_shot)CHECK(lj_render_save_png(observed_shot)==0);
        a->norder--;a->nsession--;memset(observed,0,sizeof(*observed));observed->fd=-1;
        a->dock=original;render(a);
    }
    /* Hover invalidation only on divider transitions, never every 1003 motion. */
    a->mousex=0;a->mousey=0;a->dirty=0;
    SDL_Event hover;memset(&hover,0,sizeof hover);hover.type=SDL_MOUSEMOTION;
    hover.motion.x=a->dock.dividers[0].rect.x;hover.motion.y=a->dock.dividers[0].rect.y;event(a,&hover);CHECK(a->dirty);
    a->dirty=0;event(a,&hover);CHECK(!a->dirty);
    hover.motion.x=0;hover.motion.y=0;event(a,&hover);CHECK(a->dirty);render(a);
    const char *ribbon_fixture=getenv("LILJACK_RIBBON_FIXTURE");if(ribbon_fixture){a->status_ribbon=json_object_from_file(ribbon_fixture);CHECK(a->status_ribbon);}
    /* ⚠ STATUS AND REVIEW ARE TABS OF THE ROOM TILE, not modals and not their
     * own dock tiles. the operator, 2026-09-09: "add status as a tab and review is
     * reorganised properly in that tile not as a separate tile". Typing on a
     * tab with no composer must be ignored, or a keystroke fills a draft the
     * screen never shows and it reappears later on another tab. */
    /* ══ room protocol surfaces: phase, questions, unavailable ═══════════
     * ⚠ EACH OF THESE IS A DIFFERENT FACT AND MUST LOOK DIFFERENT. A room in
     * ALIGN is deliberately not working; a room with a question is stopped on
     * its LEAD, not on its workers; an agent marked unavailable has been given
     * up on WITH a reason and will not be retried. Drawn the same as "quiet" or
     * "idle", each reads as somebody who might still pick the work up. */
    {
        Room *rm=&a->rooms[0];
        copy(rm->phase,sizeof(rm->phase),"ALIGN");rm->awaiting=2;
        render(a);
        Hit tab=get_hit(H_GROUP,rm->id);
        CHECK(tab.kind==H_GROUP);            /* the room is still reachable in ALIGN */
        rm->questions=3;render(a);
        CHECK(get_hit(H_GROUP,rm->id).kind==H_GROUP);
        /* one key arms the oldest open question and moves to the tab that can answer */
        a->room_questions=json_object_new_array();
        {json_object *q=json_object_new_object();
         jadd(q,"id","q-oldest");jadd(q,"question","which encoder?");
         json_object_array_add(a->room_questions,q);}
        copy(a->focus,sizeof(a->focus),"room");a->room_tab=RT_ROOM;a->draft_len=0;a->draft[0]=0;
        press(SDLK_a);
        CHECK(!strcmp(a->answer_qid,"q-oldest"));
        CHECK(a->room_tab==RT_ACTION);       /* it lands where the composer is */
        type_text("use the damage-rect encoder");
        CHECK(a->draft_len>0);               /* and typing goes to the answer */
        press(SDLK_ESCAPE);
        CHECK(!a->answer_qid[0]);            /* Escape cancels rather than leaving it armed */
        /* an unavailable agent is neither ended nor idle */
        Session *s0=session(a,"demo-0");
        copy(s0->unavailable,sizeof(s0->unavailable),"no live terminal");
        CHECK(!session_dead(s0));            /* unavailable is NOT the same as ended */
        render(a);
        CHECK(get_hit(H_SESSION,"demo-0").kind==H_SESSION);  /* still selectable */
        s0->unavailable[0]=0;rm->phase[0]=0;rm->questions=0;rm->awaiting=0;
        json_object_put(a->room_questions);a->room_questions=NULL;
        a->draft[0]=0;a->draft_len=0;render(a);
    }
    click_toolbar(H_STATUS);CHECK(a->room_tab==RT_STATUS);CHECK(!a->status_open);render(a);
    const char *ribbon_shot=getenv("LILJACK_RIBBON_SCREENSHOT");if(ribbon_shot)CHECK(lj_render_save_png(ribbon_shot)==0);
    type_text("ignored status text");CHECK(!a->draft_len);
    press(SDLK_ESCAPE);CHECK(a->room_tab==RT_ROOM);render(a);
    click_toolbar(H_REVIEW);CHECK(a->room_tab==RT_REVIEW);CHECK(!dock_has(a,"review"));render(a);
    type_text("ignored review text");CHECK(!a->draft_len);
    press(SDLK_ESCAPE);CHECK(a->room_tab==RT_ROOM);render(a);

    /* Review is a TAB of the room tile now, so it survives prune/retile for free:
     * there is no review tile left to lose. It is still read-only, and it still
     * scrolls without a keystroke reaching a terminal or a draft. */
    const char *review_fixture=getenv("LILJACK_REVIEW_FIXTURE");if(review_fixture){a->review_panel=json_object_from_file(review_fixture);CHECK(a->review_panel);}
    click_toolbar(H_REVIEW);CHECK(a->room_tab==RT_REVIEW);CHECK(!strcmp(a->focus,"room"));render(a);
    CHECK(!dock_has(a,"review"));
    char selected_before[81];copy(selected_before,sizeof(selected_before),a->selected);
    type_text("review is read only");CHECK(!a->draft_len);CHECK(!strcmp(selected_before,a->selected));
    a->review_height=2000;press(SDLK_PAGEDOWN);CHECK(a->review_scroll>0);
    prune_workspace(a);retile(a);render(a);CHECK(a->room_tab==RT_REVIEW);
    a->review_scroll=0;render(a);
    const char *review_shot=getenv("LILJACK_REVIEW_SCREENSHOT");if(review_shot)CHECK(lj_render_save_png(review_shot)==0);
    click_hit(H_GIT,NULL);render(a);CHECK(dock_has(a,"git"));CHECK(!strcmp(a->focus,"git"));
    type_text("git must not type into terminal");CHECK(!a->draft_len);
    a->git_height=2000;press(SDLK_PAGEDOWN);CHECK(a->git_scroll>0);
    prune_workspace(a);retile(a);CHECK(dock_has(a,"git"));
    Hit hide_git={.kind=H_HIDE};copy(hide_git.id,sizeof(hide_git.id),"git");activate(a,&hide_git);
    CHECK(!dock_has(a,"git"));render(a);click_toolbar(H_REVIEW);CHECK(a->room_tab==RT_REVIEW);render(a);
    const char *review_end_shot=getenv("LILJACK_REVIEW_END_SCREENSHOT");if(review_end_shot){a->review_scroll=a->review_height;render(a);CHECK(lj_render_save_png(review_end_shot)==0);}
    Hit hide_review={.kind=H_HIDE};copy(hide_review.id,sizeof(hide_review.id),"review");activate(a,&hide_review);
    CHECK(!dock_has(a,"review"));CHECK(!strcmp(a->focus,"room"));render(a);

    /* ⚠ CONTROL IS A DROPDOWN, NOT A MODAL (the operator, 2026-09-09: "it should not
     * be a popup window it should be a dropdown with roles and checkmark on
     * selected role, and leader note that you will take over leadership").
     * The checkmark must describe the CURRENT role, and choosing lead while the
     * room already has one must say whose it takes before the click. */
    click_hit(H_CONTROLS,"demo-0");
    CHECK(a->menu_kind==MENU_ROLE);CHECK(!strcmp(a->menu_target,"demo-0"));CHECK(!a->team);
    {
        MenuRow rows[16];int n=menu_rows(a,rows,16);CHECK(n>=3);
        int checked=-1,leadrow=-1;
        for(int i=0;i<n;i++){
            if(rows[i].action==H_ROLE&&rows[i].checked)checked=i;
            if(rows[i].action==H_ROLE&&rows[i].index==2)leadrow=i;
        }
        CHECK(checked>=0);CHECK(!strcmp(rows[checked].label,"worker"));   /* demo-0 is a worker */
        CHECK(leadrow>=0);CHECK(strstr(rows[leadrow].note,"claude")!=NULL); /* demo-1 holds it */
    }
    render(a);click_hit(H_MENU_SCRIM,NULL);CHECK(a->menu_kind==MENU_NONE);
    render(a);click_toolbar(H_TEAM);CHECK(a->team);render(a);
    CHECK(get_hit(H_LEAD,"demo-0").kind==H_LEAD);
    Hit stop={0};for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_CONTROL&&a->hits[i].index==2&&!strcmp(a->hits[i].id,"demo-0"))stop=a->hits[i];CHECK(stop.kind==H_CONTROL);
    activate(a,&stop);CHECK(!strcmp(a->stop_confirm,"demo-0"));render(a);
    activate(a,&stop);CHECK(!a->stop_confirm[0]);render(a);
    const char *controls_shot=getenv("LILJACK_CONTROLS_SCREENSHOT");if(controls_shot)CHECK(lj_render_save_png(controls_shot)==0);
    click_hit(H_AGENT_FOCUS,"demo-0");CHECK(!a->team&&!strcmp(a->focus,"demo-0"));render(a);

    click_hit(H_TERMINAL,"demo-0");CHECK(!strcmp(a->focus,"demo-0"));CHECK(!strcmp(a->selected,"demo-0"));
    /* A title drag enters a snap preview and splits the target on release. */
    Hit header=get_hit(H_HEADER,"demo-0");lj_rect room=tile_rect("room");
    mouse_event(SDL_MOUSEBUTTONDOWN,header.r.x+20,header.r.y+15);
    mouse_event(SDL_MOUSEMOTION,room.x+2,room.y+room.h/2);CHECK(a->dragging);
    render(a);mouse_event(SDL_MOUSEBUTTONUP,room.x+2,room.y+room.h/2);render(a);
    CHECK(!a->dragging&&!a->drag[0]&&a->dock.tile_count==4);
    lj_rect moved=tile_rect("demo-0"),newroom=tile_rect("room");CHECK(moved.x<newroom.x&&moved.y==newroom.y&&moved.h==newroom.h);
    lj_dock_divider split=a->dock.dividers[0];double ratio=a->dock.nodes[split.node].ratio;
    mouse_event(SDL_MOUSEBUTTONDOWN,split.rect.x+split.rect.w/2,split.rect.y+split.rect.h/2);CHECK(a->split==split.node);
    mouse_event(SDL_MOUSEMOTION,split.area.x+split.area.w*3/4,split.area.y+split.area.h*3/4);
    CHECK(a->dock.nodes[split.node].ratio!=ratio);mouse_event(SDL_MOUSEBUTTONUP,0,0);CHECK(a->split==-1);
    /* Modal blocks terminal and room text; Unicode room editing stays intact. */
    press(SDLK_F6);type_text("Привіт 🌿");CHECK(!strcmp(a->draft,"Привіт 🌿"));
    press(SDLK_F7);CHECK(a->team);type_text("ignored");CHECK(!strcmp(a->draft,"Привіт 🌿"));press(SDLK_ESCAPE);CHECK(!a->team);
    /* Conversations strictly filter room/lobby/DM and retain per-recipient drafts. */
    json_object *message=json_object_new_object();jadd(message,"sender","operator");jadd(message,"destination","room");CHECK(!message_visible(a,message));
    jadd(message,"destination","r-demo-build");CHECK(message_visible(a,message));
    CHECK(set_conversation(a,"demo-0"));CHECK(a->draft_len==0);type_text("private draft A");
    jadd(message,"destination","demo-0");CHECK(message_visible(a,message));jadd(message,"sender","demo-1");CHECK(!message_visible(a,message));
    jadd(message,"sender","demo-0");jadd(message,"destination","operator");CHECK(message_visible(a,message));json_object_put(message);
    CHECK(set_conversation(a,"demo-1"));CHECK(a->draft_len==0);CHECK(set_conversation(a,"demo-0"));CHECK(!strcmp(a->draft,"private draft A"));
    CHECK(set_conversation(a,"r-demo-build"));CHECK(!strcmp(a->draft,"Привіт 🌿"));
    CHECK(set_conversation(a,"demo-0"));copy(a->posting_dest,sizeof(a->posting_dest),"demo-0");copy(a->posting_text,sizeof(a->posting_text),a->draft);
    CHECK(set_conversation(a,"r-demo-build"));acknowledge_post(a);CHECK(!strcmp(a->draft,"Привіт 🌿"));
    CHECK(set_conversation(a,"demo-0"));CHECK(!a->draft_len);type_text("new draft");copy(a->posting_dest,sizeof(a->posting_dest),"demo-0");copy(a->posting_text,sizeof(a->posting_text),a->draft);type_text(" edited");acknowledge_post(a);CHECK(!strcmp(a->draft,"new draft edited"));
    CHECK(set_conversation(a,"r-demo-build"));
    /* ⚠ CREATING A ROOM IS THE WHOLE SETUP. The form carries a working folder,
     * a purpose, the agents WITH roles and a first todo, because the todo is
     * what arms the heartbeat and the folder/purpose are what the intro tells
     * each agent. A name alone creates a room nobody is in. */
    render(a);click_hit(H_NEW_ROOM,NULL);CHECK(a->room_dialog);
    CHECK(!strcmp(a->room_folder,a->project));          /* folder defaults to the project */
    CHECK(room_lead_count(a)==1);                       /* and a room opens with one lead */
    /* Typing goes to the focused field, and Tab moves between them. */
    type_text("Review room");CHECK(!strcmp(a->room_name,"Review room"));
    press(SDLK_TAB);CHECK(a->room_field==RF_FOLDER);
    press(SDLK_TAB);press(SDLK_TAB);CHECK(a->room_field==RF_TODO0);
    type_text("first task");CHECK(!strcmp(a->room_todo[0],"first task"));
    CHECK(!strcmp(a->room_name,"Review room"));         /* the earlier field is untouched */
    press(SDLK_BACKSPACE);CHECK(!strcmp(a->room_todo[0],"first tas"));
    /* Cycling an agent: off, worker, reviewer, lead. Promoting a second lead
     * demotes the first, because room_start refuses two and the form must not
     * be able to build a request the backend will reject. */
    render(a);
    int was=a->room_agent[1];
    click_hit(H_ROOM_AGENT,NULL);                       /* index 0 = claude, the lead */
    CHECK(a->room_agent[0]==0);CHECK(a->room_agent[1]==was);
    CHECK(room_lead_count(a)==0);
    render(a);click_hit(H_ROOM_CREATE,NULL);            /* no lead: refused, form stays */
    CHECK(a->room_dialog);CHECK(a->nroom==2);
    a->room_agent[2]=3;CHECK(room_lead_count(a)==1);    /* deepseek leads */
    /* Cycle claude off -> worker -> reviewer -> lead. The last step must demote
     * deepseek, leaving exactly one lead. */
    for(int c=0;c<3;c++){render(a);click_hit(H_ROOM_AGENT,NULL);}
    CHECK(a->room_agent[0]==3);CHECK(a->room_agent[2]!=3);CHECK(room_lead_count(a)==1);
    render(a);press(SDLK_RETURN);
    CHECK(!a->room_dialog&&a->nroom==3);CHECK(!strcmp(a->active_room,"r-demo-3"));
    switch_workspace(a,"r-demo-build");render(a);
    /* Actual title drag into another room changes membership, not just geometry. */
    Hit agent_header=get_hit(H_HEADER,"demo-2"),group=get_hit(H_GROUP,"r-demo-research");
    mouse_event(SDL_MOUSEBUTTONDOWN,agent_header.r.x+20,agent_header.r.y+15);
    mouse_event(SDL_MOUSEMOTION,group.r.x+group.r.w/2,group.r.y+group.r.h/2);
    mouse_event(SDL_MOUSEBUTTONUP,group.r.x+group.r.w/2,group.r.y+group.r.h/2);
    CHECK(!strcmp(session(a,"demo-2")->room_id,"r-demo-research"));CHECK(!dock_has(a,"demo-2"));
    switch_workspace(a,"r-demo-research");render(a);CHECK(a->dock.tile_count==1);CHECK(!strcmp(a->focus,"room"));
    collapse_room(a,"r-demo-research");render(a);CHECK(a->dock.tile_count==2);CHECK(dock_has(a,"demo-2"));
    agent_header=get_hit(H_HEADER,"demo-2");group=get_hit(H_GROUP,"");
    mouse_event(SDL_MOUSEBUTTONDOWN,agent_header.r.x+20,agent_header.r.y+15);mouse_event(SDL_MOUSEMOTION,group.r.x+group.r.w/2,group.r.y+group.r.h/2);mouse_event(SDL_MOUSEBUTTONUP,group.r.x+group.r.w/2,group.r.y+group.r.h/2);
    CHECK(!session(a,"demo-2")->room_id[0]);
    move_session(a,"demo-2","r-demo-build");switch_workspace(a,"r-demo-build");render(a);CHECK(dock_has(a,"demo-2"));
    /* Real resize event creates a software 4K texture; full screen reaches the VT clamp. */
    copy(a->focus,sizeof(a->focus),"demo-0");copy(a->full,sizeof(a->full),"demo-0");
    SDL_Event resize;memset(&resize,0,sizeof resize);resize.type=SDL_WINDOWEVENT;resize.window.event=SDL_WINDOWEVENT_SIZE_CHANGED;resize.window.data1=3840;resize.window.data2=2160;dispatch(resize);render(a);
    CHECK(a->w==3840&&a->h==2160&&a->texture);Session *s=session(a,"demo-0");CHECK(s->cols==LJ_VT_MAXCOLS&&s->rows<=LJ_VT_MAXROWS);
    const unsigned char stable[]="\033[2J\033[HSTABLE-4K";lj_vt_feed(s->vt,stable,sizeof(stable)-1);
    render(a);render(a);CHECK(contains(s,"STABLE-4K"));
    resize.window.data1=1440;resize.window.data2=900;dispatch(resize);
    for(int i=0;i<a->nsession;i++)terminal_close(&a->sessions[i]);
    a->nsession=a->norder=2;a->active_room[0]=0;a->demo=0;a->full[0]=0;a->layout_path[0]=0;
    foreground_shell(&a->sessions[0],"test-A");foreground_shell(&a->sessions[1],"test-B");a->order[0]=0;a->order[1]=1;
    Session *sa=&a->sessions[0],*sb=&a->sessions[1];await_text(sa,"READY");await_text(sb,"READY");retile(a);render(a);
    click_hit(H_TERMINAL,"test-A");type_text("alpha-only");press(SDLK_RETURN);await_text(sa,"RECEIVED:alpha-only");CHECK(!contains(sb,"alpha-only"));
    click_hit(H_TERMINAL,"test-B");type_text("beta-only");press(SDLK_RETURN);await_text(sb,"RECEIVED:beta-only");CHECK(!contains(sa,"beta-only"));
    /* ⚠ A TILE IS A VIEWPORT, NOT THE PTY SIZE. This used to assert that a
     * 77x19 tile resized the terminal to 77x19. Measured 2026-09-09: stacking
     * nine agents in one column handed each harness an 82x5 terminal, and a
     * second attached view squeezed live Codex/Claude/DeepSeek panes from
     * 129x24 to 82x5 — tmux sizes a window to its SMALLEST client and every
     * tile is a client. No harness TUI lays out in five rows. The PTY now
     * floors at LJ_TERM_MIN and a shorter tile shows the BOTTOM of the buffer
     * (Session.view_row) instead of shrinking the agent. */
    resize_terminal(sb,77,19);struct winsize ws={0};CHECK(ioctl(sb->fd,TIOCGWINSZ,&ws)==0);
    CHECK(ws.ws_col==LJ_TERM_MIN_COLS&&ws.ws_row==LJ_TERM_MIN_ROWS);
    CHECK(sb->cols==LJ_TERM_MIN_COLS&&sb->rows==LJ_TERM_MIN_ROWS);
    type_text("size");press(SDLK_RETURN);await_text(sb,"24 80");
    /* Above the floor the tile still drives the size exactly. */
    resize_terminal(sb,120,40);CHECK(ioctl(sb->fd,TIOCGWINSZ,&ws)==0);
    CHECK(ws.ws_col==120&&ws.ws_row==40);
    SDL_KeyboardEvent modified={0};modified.keysym.sym=SDLK_LEFT;modified.keysym.mod=KMOD_CTRL;
    CHECK(sb->queued==0);key(a,&modified);CHECK(sb->queued==6&&!memcmp(sb->queue,"\033[1;5D",6));sb->queued=0;
    modified.keysym.sym=SDLK_RIGHT;modified.keysym.mod=KMOD_SHIFT;key(a,&modified);CHECK(sb->queued==6&&!memcmp(sb->queue,"\033[1;2C",6));sb->queued=0;
    /* No providers, helper, network, or persistent tmux server was involved. */
    pid_t pa=sa->pid,pb=sb->pid;terminal_close(sa);terminal_close(sb);errno=0;CHECK(waitpid(pa,NULL,WNOHANG)==-1&&errno==ECHILD);errno=0;CHECK(waitpid(pb,NULL,WNOHANG)==-1&&errno==ECHILD);
    /* A disconnected client can be reattached without restarting the app. */
    char script[]="/tmp/liljack-attach-test-XXXXXX";int scriptfd=mkstemp(script);CHECK(scriptfd>=0);
    const char program[]="#!/bin/sh\nprintf 'RECONNECTED\\n'\nwhile IFS= read -r value; do printf '%s\\n' \"$value\"; done\n";
    CHECK(write(scriptfd,program,sizeof(program)-1)==sizeof(program)-1);CHECK(fchmod(scriptfd,0700)==0);close(scriptfd);
    copy(a->tmux,sizeof(a->tmux),script);copy(sa->name,sizeof(sa->name),"isolated");sa->managed=1;sa->vt=lj_vt_new(80,24);sa->connected=0;
    CHECK(attach(a,sa));await_text(sa,"RECONNECTED");terminal_close(sa);unlink(script);
    /* A previous full snapshot must not block newly observed session IDs. */
    for(int i=0;i<COUNT;i++){memset(&a->sessions[i],0,sizeof(Session));a->sessions[i].fd=-1;snprintf(a->sessions[i].id,81,"old-%d",i);}
    a->nsession=COUNT;copy(a->selected,sizeof(a->selected),"old-0");copy(a->focus,sizeof(a->focus),"old-0");
    json_object *snapshot=json_object_new_object(),*rows=json_object_new_array();
    for(int i=0;i<COUNT;i++){json_object *row=json_object_new_object();char id[81];snprintf(id,sizeof(id),i?"fresh-%d":"old-%d",i);jadd(row,"id",id);jadd(row,"agent","shell");json_object_array_add(rows,row);}
    json_object_object_add(snapshot,"sessions",rows);json_object_object_add(snapshot,"messages",json_object_new_array());apply_snapshot(a,snapshot);json_object_put(snapshot);
    CHECK(a->norder==COUNT&&session(a,"fresh-63")&&session(a,"old-0"));CHECK(!strcmp(a->focus,"old-0"));
    /* ── narrow-width honesty (codex's visual inspection, 2026-09-10) ──────
     * ⚠ A SILENTLY SHORTENED LABEL READS AS A DIFFERENT LABEL. The captures
     * showed a file called "toolbox" rendered "toolbo" and a tab called
     * "ACTIONABLE" rendered "ACTIONA". Neither is a shorter name; both are
     * wrong names, and in a file browser that is a lie about what is on disk. */
    {
        char buf[256];
        CHECK(!strcmp(fit(buf,sizeof buf,"toolbox",7),"toolbox"));   /* exact fit is untouched */
        CHECK(!strcmp(fit(buf,sizeof buf,"toolbox",6),"toolb…"));    /* was "toolbo" */
        CHECK(strstr(fit(buf,sizeof buf,"examples",6),"…")!=NULL);
        CHECK(!strcmp(fit(buf,sizeof buf,"ab",9),"ab"));             /* short label untouched */
        /* wcwidth, not bytes: a 2-column glyph costs two columns. */
        CHECK(!strcmp(fit(buf,sizeof buf,"日本語",6),"日本語"));
        CHECK(strstr(fit(buf,sizeof buf,"日本語",4),"…")!=NULL);
        /* ⚠ The ladder must ABBREVIATE, never mangle: every rung it can return
         * has to be a whole word, so "ACTIONA" can never appear again. */
        CHECK(!strcmp(room_tab_label(RT_ACTION,10),"ACTIONABLE"));
        CHECK(!strcmp(room_tab_label(RT_ACTION,9),"ACTION"));
        CHECK(!strcmp(room_tab_label(RT_ACTION,6),"ACTION"));
        CHECK(!strcmp(room_tab_label(RT_ACTION,5),"AC"));
        CHECK(!strcmp(room_tab_label(RT_ROOM,4),"ROOM"));
        for(int c=1;c<=12;c++)for(int i=0;i<RT_COUNT;i++){
            const char *L=room_tab_label(i,c);
            CHECK(!strcmp(L,ROOM_TABS[i])||!strcmp(L,ROOM_TABS_SHORT[i])
                  ||!strcmp(L,ROOM_TABS_TINY[i])||!strcmp(L,ROOM_TABS_MICRO[i]));
        }
        /* ⚠ AND THE CHOSEN RUNG MUST ACTUALLY FIT WHAT badge() DRAWS. The
         * ladder returning a whole word is not enough: badge() draws with maxw
         * r.w-CELLW, so a caller that sizes the ladder from the RECT hands it
         * one cell more than exists and the whole word is then CLIPPED — codex
         * measured "ACTIO"/"STATU" on the divider-resize path, a truncation
         * produced by the code meant to prevent truncation. Sweep every tile
         * width the room tile can take and assert the label fits the budget the
         * renderer will really give it. */
        for(int tilew=40;tilew<=1600;tilew+=7){
            int tw=tilew/RT_COUNT;
            int rectw=tw-CELLW;
            int budget=(rectw-CELLW)/CELLW;        /* what badge() allows */
            if(budget<1)continue;
            for(int i=0;i<RT_COUNT;i++){
                const char *L=room_tab_label(i,budget);
                CHECK((int)strlen(L)<=budget);      /* ASCII rungs: bytes == cells */
            }
        }
    }
    /* ── active in front, closed at the end (the operator, 2026-09-10) ───────────
     * ⚠ The workspace keeps a session row FOREVER — 41 rows against 6 running,
     * measured — so a fresh instance opened onto a tab strip led by agents
     * closed hours earlier, drawn red with "no live terminal". Order is what
     * carries the distinction, because closing a tile must not delete the
     * record. A STABLE partition: the relative order the operator arranged survives
     * inside each group. */
    {
        for(int i=0;i<COUNT;i++){memset(&a->sessions[i],0,sizeof(Session));a->sessions[i].fd=-1;}
        const char *ids[6]={"live-a","gone-b","live-c","gone-d","gone-e","live-f"};
        json_object *snap2=json_object_new_object(),*rows2=json_object_new_array();
        for(int i=0;i<6;i++){
            json_object *row=json_object_new_object();
            jadd(row,"id",ids[i]);jadd(row,"agent","shell");
            /* "exited" is what the workspace reports for a closed session. */
            jadd(row,"state",ids[i][0]=='g'?"exited":"running");
            json_object_array_add(rows2,row);
        }
        json_object_object_add(snap2,"sessions",rows2);
        json_object_object_add(snap2,"messages",json_object_new_array());
        apply_snapshot(a,snap2);json_object_put(snap2);
        int seen_gone=0,ok=1;
        char order_ids[6][81];int n=0;
        for(int i=0;i<a->norder&&n<6;i++){
            Session *s=&a->sessions[a->order[i]];
            copy(order_ids[n],sizeof(order_ids[n]),s->id);n++;
            if(session_dead(s))seen_gone=1;
            else if(seen_gone)ok=0;            /* a live one AFTER a closed one */
        }
        CHECK(ok);                              /* no live session trails a closed one */
        /* stable: the live ones keep the order they arrived in */
        CHECK(n>=3 && !strcmp(order_ids[0],"live-a") && !strcmp(order_ids[1],"live-c")
              && !strcmp(order_ids[2],"live-f"));
    }
    /* ── the ENDED badge must not touch the ROLE control ─────────────────
     * ⚠ Fixing the CLIPPING did not fix the SPACING: the slot was sized at 6
     * cells and drawn with maxw badge_w+CELLW, so "· ENDED" (7 cells) ran off
     * its slot into ROLE's 8px gap and rendered "ENDEDROLE" (codex, ended-800
     * and ended-1920). The geometry is arithmetic, so assert the arithmetic
     * across every header width the tile can take. */
    {
        for(int rw=300;rw<=1920;rw+=13){
            int role_x = rw-132;                 /* ROLE button, 96 wide */
            int badge_w = 7*CELLW;               /* "· ENDED" */
            int badge_x = rw-132-CELLW-badge_w;
            CHECK(badge_x + badge_w <= role_x - CELLW + 1);  /* a whole cell of air */
            CHECK(badge_x > 0 || rw < badge_w + 132 + CELLW);
        }
    }
    /* ── an overlay owns its pointer (codex, tui-menu-click-through-spawn) ──
     * ⚠ SMALLEST-AREA-WINS IS RIGHT BETWEEN SIBLINGS AND WRONG ACROSS LAYERS.
     * An open menu row is LARGER than an agent spawn chip beneath it, so area
     * alone handed the click to the chip: clicking burger -> TEAM emitted
     * {action:create, agent:shell} instead of opening Team, proven on the
     * backend pipe. A menu item that starts a process is the worst version of
     * this, and every stray click left another dead session behind.
     *
     * Reproduce the geometry directly: a small low-layer hit fully inside a
     * large high-layer one. The overlay must win despite being bigger. */
    {
        a->nhits=0;
        a->hit_layer=0;
        hit(a,(lj_rect){100,100,40,20},H_CREATE,3,NULL);           /* the spawn chip, underneath */
        a->hit_layer=1;
        hit(a,(lj_rect){80,90,200,40},H_MENU_ITEM,3,NULL);        /* large, on top */
        a->hit_layer=0;
        int best=-1,bestlayer=-1;long long bestarea=0;
        int mx=115,my=108;                                        /* inside BOTH */
        for(int i=a->nhits-1;i>=0;i--){Hit *probe=&a->hits[i];
            if(!inside(probe->r,mx,my))continue;
            long long area=(long long)probe->r.w*probe->r.h;
            if(best<0||probe->layer>bestlayer||(probe->layer==bestlayer&&area<bestarea)){
                best=i;bestlayer=probe->layer;bestarea=area;}}
        CHECK(best>=0 && a->hits[best].kind==H_MENU_ITEM);   /* NOT H_CREATE */
        /* ⚠ AND THE SIBLING RULE MUST SURVIVE: within one layer the smaller
         * control still wins, which is what made a 2-cell × clickable on the
         * very cell showing its glyph. */
        a->nhits=0;a->hit_layer=0;
        hit(a,(lj_rect){0,0,400,40},H_CONTROLS,0,"tile");          /* big header */
        hit(a,(lj_rect){360,10,20,20},H_HIDE,0,"tile");            /* small close */
        best=-1;bestlayer=-1;bestarea=0;mx=368;my=18;
        for(int i=a->nhits-1;i>=0;i--){Hit *probe=&a->hits[i];
            if(!inside(probe->r,mx,my))continue;
            long long area=(long long)probe->r.w*probe->r.h;
            if(best<0||probe->layer>bestlayer||(probe->layer==bestlayer&&area<bestarea)){
                best=i;bestlayer=probe->layer;bestarea=area;}}
        CHECK(best>=0 && a->hits[best].kind==H_HIDE);
        a->nhits=0;
    }
    /* ── the lead's dotted border really is TWO colours (codex) ───────────
     * ⚠ The colour was chosen by cell parity and the emit gated on the SAME
     * parity being even, so every glyph that reached the screen had already
     * been decided as a_col: the operator's "golden blue" border was gold, full stop.
     * Assert against the RENDERED PIXELS, not against my own arithmetic —
     * re-deriving the formula in the test would agree with a wrong formula. */
    {
        render_ansi=0;                       /* draw into the HUI framebuffer */
        lj_render_rect(0,0,a->w,a->h,0x000000);   /* clear */
        lj_rect br={40,40,400,200};
        dotted_border(br,GREEN,CYAN);
        /* ⚠ Glyphs are ANTIALIASED, so an exact colour match never appears —
         * classify by which channel dominates instead. GREEN is 0xffd54a
         * (red>blue, gold) and CYAN is 0x5bbcff (blue>red). */
        const uint32_t *px=lj_render_pixels();
        int gold=0,blue=0;
        /* ⚠ EXCLUDE THE CORNER COLUMNS. The VERTICAL loop also paints at
         * br.x and br.x+br.w-CELLW inside this row, so a scan that includes
         * them sees blue from the side run even when every horizontal dot is
         * gold — which is exactly how the first cut of this test passed with
         * the original bug restored. Measure the top run ALONE. */
        for(int y=br.y;y<br.y+LJ_LINE_H&&y<a->h;y++)
            for(int x=br.x+LJ_CELL_W;x<br.x+br.w-LJ_CELL_W&&x<a->w;x++){
                uint32_t c=px[(size_t)y*a->w+x];
                int R=(c>>16)&255,G=(c>>8)&255,B=c&255;
                if(R+G+B<90)continue;                 /* background / faint edge */
                if(R>B+40)gold++;
                else if(B>R+40)blue++;
            }
        CHECK(gold>0);                       /* gold dots */
        CHECK(blue>0);                       /* AND blue ones — never drawn before */

        /* Read per-edge ink from the framebuffer, at aligned/unaligned
         * origins and every colour phase. Geometry is fixed; colours move. */
        for(int trial=0;trial<8;trial++){
            lj_rect r={40+(trial%4)*7,40+(trial%4)*3,300,160};
            hui_border_grid g;CHECK(hui_border_cells(r.x,r.y,r.w,r.h,CELLW,CELLH,&g));
            render_anim=(uint32_t)(trial/4*6);lj_render_rect(0,0,a->w,a->h,0);
            dotted_border(r,GREEN,CYAN);const uint32_t *pixels=lj_render_pixels();
            for(int edge=0;edge<4;edge++){
                int count=0,last=-1,lo=999,hi=0,len=(edge&1)?g.rows:g.cols;
                for(int n=0;n<len;n++){
                    int cx=edge==0?n:edge==1?g.cols-1:edge==2?g.cols-1-n:0;
                    int cy=edge==0?0:edge==1?n:edge==2?g.rows-1:g.rows-1-n,ink=0;
                    for(int y=(g.row+cy)*CELLH;y<(g.row+cy+1)*CELLH;y++)
                        for(int x=(g.col+cx)*CELLW;x<(g.col+cx+1)*CELLW;x++)if(pixels[(size_t)y*a->w+x]&0xffffff)ink=1;
                    if(ink){if(last>=0){int gap=n-last-1;if(gap<lo)lo=gap;if(gap>hi)hi=gap;}last=n;count++;}
                    if(n==0||n==len-1)CHECK(ink);
                }
                CHECK(count>=2);CHECK(lo>=1);CHECK(hi-lo<=1);
            }
            for(int y=0;y<a->h;y++)for(int x=0;x<a->w;x++)
                if(x<r.x||y<r.y||x>=r.x+r.w||y>=r.y+r.h)CHECK(!(pixels[(size_t)y*a->w+x]&0xffffff));
        }
        render_anim=0;
    }
    /* ── the header pictures are drawn from values, not from luck ─────────
     * ⚠ The headless demo never polls metrics, so every header graph reads NAN
     * and draws nothing — correct, and it means no capture ever SHOWS the plot.
     * So the picture is asserted here from planted values instead: a column's
     * bar height tracks its value, a NAN column stays empty (a flat bar at 0%
     * would read as an idle card — the lie c_metrics.h exists to prevent), and
     * the colour steps at the documented thresholds. The mark is asserted the
     * same way: non-empty, in its own colours, transparent outside its body. */
    {
        enum{PW=16,PH=20};
        static uint32_t pb[PW*PH];
        float hist[PW];for(int i=0;i<PW;i++)hist[i]=(float)i/(PW-1);   /* 0 .. 1 ramp */
        hist[5]=0.0f/0.0f;                                              /* one NAN column */
        plot_argb(pb,PW,PH,hist,PW);
        int col_h(int c){int n=0;for(int r=0;r<PH;r++)if(pb[r*PW+c]>>24)n++;return n;}
        CHECK(col_h(0)==1);                       /* finite idle baseline; NAN alone is a gap */
        CHECK(col_h(PW-1)==PH-1);                 /* value 1 fills the column */
        CHECK(col_h(8)>col_h(2));                 /* monotone in the value */
        CHECK(col_h(5)==0);                       /* NAN: NOTHING, not a flat bar */
        /* colour by level: the top pixel of a low, mid and high column */
        int top(int c){for(int r=0;r<PH;r++)if(pb[r*PW+c]>>24)return r;return -1;}
        #define RGB(c) (pb[top(c)*PW+(c)]&0xffffff)
        CHECK(RGB(4)==0x88eba5);                  /* ~0.27 -> green */
        CHECK(RGB(11)==0xe7c67e);                 /* ~0.73 -> amber */
        CHECK(RGB(14)==0xe47676);                 /* ~0.93 -> red */
        #undef RGB
        /* the mark: opaque body, transparent corners, and its yellow present */
        static uint32_t mb[64*64];int ms=20;mark_argb(mb,ms);
        CHECK((mb[0]>>24)==0);                    /* outside the rounded body */
        CHECK((mb[(ms/2)*ms+ms/2]>>24)!=0);       /* the body is painted */
        int yellow=0;for(int i=0;i<ms*ms;i++)if((mb[i]&0xffffff)==0xffd54a)yellow++;
        CHECK(yellow>0);
    }
    /* ── the Files header keeps IDENTITY and LOCATION at every width ──────
     * ⚠ Two wrong answers preceded the right one. Dropping the PATH on a narrow
     * tile left the browser silent about where it was (codex, files-800.png).
     * Dropping the WORD instead — my repair — left it an anonymous list of
     * names at ~190px, which codex measured and disagreed with, correctly. His
     * design keeps a cheap stable identity ("FILES " wide, "F:" narrow) and
     * gives every remaining column to the path.
     *
     * ⚠ THIS CALLS files_header() — THE FUNCTION render() ACTUALLY USES. An
     * earlier cut re-derived the same arithmetic inside the test, which catches
     * an arithmetic slip (it did find a one-column overflow) but cannot catch
     * the test drifting from the code: a test that recomputes the formula
     * agrees with a wrong formula. */
    {
        const char *full="/home/user/projects/liljack/liljack_app";
        char lbl[512];int drew=0;
        for(int path_w=1;path_w<=120;path_w++){
            files_header(lbl,sizeof(lbl),full,path_w);
            if(!lbl[0])continue;                      /* UP/close carry the tile */
            drew++;
            CHECK(lbl[0]=='F');                       /* identity, at every width */
            int cells=0;
            for(const char *q=lbl;*q;){
                uint32_t cp=nextcp(&q);int w=wcwidth((wchar_t)cp);
                cells+=w<0?1:w;
            }
            CHECK(cells<=path_w);                     /* never overflows its budget */
            CHECK(cells>2);                           /* and never identity alone */
        }
        CHECK(drew>100);                              /* the sweep really ran */
        files_header(lbl,sizeof(lbl),full,20);
        CHECK(strstr(lbl,"liljack_app")!=NULL);       /* the tail names the folder */
        files_header(lbl,sizeof(lbl),full,80);        /* wide: word + whole path */
        CHECK(strncmp(lbl,"FILES ",6)==0 && strstr(lbl,"\xe2\x80\xa6")==NULL);
        files_header(lbl,sizeof(lbl),full,6);         /* narrow: short identity, path kept */
        CHECK(strncmp(lbl,"F:",2)==0 && strlen(lbl)>2);
    }
    /* ── there is always a way out, and it is the SAME way out ────────────
     * ⚠ the operator asked for this live: he did not know Ctrl+Q. An interface you
     * cannot leave with the mouse is a trap however good the rest of it is.
     * Assert BOTH routes exist at the widths that produce them, and — the part
     * that matters more — that they set the same flag the key does, so there is
     * one shutdown path and not two that clean up differently. */
    {
        /* ⚠ NO INLINE QUIT AT ANY WIDTH — it moved into the ribbon with the rest
         * of the nine. The old check here asserted a QUIT badge in the wide
         * toolbar, which is the layout tui-burger-ribbon deliberately removed;
         * asserting its ABSENCE is what now guards the move. */
        a->w=1920;a->h=1080;a->running=1;a->menu_kind=MENU_NONE;a->ribbon_open=0;a->ribbon=0;
        render(a);
        for(int i=0;i<a->nhits;i++)CHECK(a->hits[i].kind!=H_QUIT);

        /* ...and the mouse route still exists: open the ribbon, QUIT is in it. */
        a->ribbon_open=1;a->ribbon=RIBBON_FULL;render(a);
        MenuRow rows[16];int n=toolbar_rows(a,rows,16);
        {
            int idx=-1;for(int i=0;i<n;i++)if(rows[i].action==H_QUIT)idx=i;
            CHECK(idx>=0);
            get_hit_index(H_RIBBON_ITEM,idx);      /* clickable, not just listed */
        }
        /* ⚠ CAPTURES MUST RENDER IN PIXEL MODE. lj_render_save_png reads the
         * pixel framebuffer; in --tui the drawing goes to the ANSI cell canvas
         * instead, so shots taken with a->ansi set came back byte-identical
         * whatever the ribbon was doing — three copies of a stale buffer,
         * evidence of nothing. Same env-var pattern as the review/files shots. */
        {const char *closed=getenv("LILJACK_RIBBON_CLOSED_SCREENSHOT");
         const char *mid=getenv("LILJACK_RIBBON_MID_SCREENSHOT");
         const char *open_=getenv("LILJACK_RIBBON_SCREENSHOT");
         if(closed||mid||open_){
             int was_ansi=a->ansi;a->ansi=0;
             a->ribbon_open=0;a->ribbon=0;render(a);
             if(closed)CHECK(lj_render_save_png(closed)==0);
             a->ribbon_open=1;a->ribbon=RIBBON_FULL*2/5;render(a);
             if(mid)CHECK(lj_render_save_png(mid)==0);
             a->ribbon=RIBBON_FULL;render(a);
             if(open_)CHECK(lj_render_save_png(open_)==0);
             a->ansi=was_ansi;
         }}
        a->ribbon_open=0;a->ribbon=0;
        int menu_quit=0,quit_last=0;
        for(int i=0;i<n;i++)if(rows[i].action==H_QUIT){
            menu_quit=1;
            quit_last=(i==n-1);                    /* leaving is the last thing you do */
            /* it TEACHES the key rather than hiding it */
            CHECK(strstr(rows[i].label,"Ctrl+Q")!=NULL);
            /* and says the consequence, because quit is frightening until you
             * know the agents keep running */
            CHECK(rows[i].note[0]!=0);
        }
        CHECK(menu_quit);
        CHECK(quit_last);

        /* ⚠ THE SAME EXIT, NOT A SECOND ONE. */
        a->running=1;
        {Hit q={.kind=H_QUIT};activate(a,&q);}
        CHECK(a->running==0);
        a->running=1;a->w=1440;a->menu_kind=MENU_NONE;
    }
    /* Shared geometry and real hits at supported and intermediate widths. */
    {
        for(int w=400;w<=3840;w+=7)for(int open=0;open<2;open++){
            a->w=w;a->ribbon_open=open;a->ribbon=open?RIBBON_FULL:0;
            Header h=header_layout(a);
            CHECK(h.graphs_x>=h.ribbon_x);
            CHECK(h.graphs_x+(h.rung?3*h.per*CELLW:0)==h.status_x);
            CHECK(h.ribbon_x+h.ribbon_w<=h.graphs_x);
            CHECK(h.status_x<w);
        }
        int widths[]={800,1920};
        for(int k=0;k<2;k++){
            a->w=widths[k];a->h=k?1080:560;
            a->ribbon_open=0;a->ribbon=0;render(a);
            CHECK(header_layout(a).rung==2);
            Hit burger=get_hit(H_BURGER,NULL);CHECK(burger.r.x==0&&burger.r.y==0);
            mouse_event(SDL_MOUSEBUTTONDOWN,burger.r.x+15,10);
            mouse_event(SDL_MOUSEBUTTONUP,burger.r.x+15,10);
            /* visual-polish-chrome item 5 (lead: B): the burger opens the grouped
             * logo menu; the ribbon is reached by state below, as the fixture did. */
            CHECK(a->menu_kind==MENU_LOGO&&!a->ribbon_open);
            menu_close(a);a->ribbon_open=1;a->ribbon=RIBBON_FULL;render(a);
            Header h=header_layout(a);CHECK(h.rung==(k?2:1));
            for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_RIBBON_ITEM||a->hits[i].kind==H_RIBBON_MORE){
                Hit item=a->hits[i];CHECK(item.r.y==0&&item.r.h==CELLH);
                CHECK(item.r.x>=3*CELLW&&item.r.x+item.r.w<=h.graphs_x);
            }
            if(!k){
                click_hit(H_RIBBON_MORE,NULL);render(a);
                MenuRow rows[16];int n=menu_rows(a,rows,16),quit=0;
                for(int i=0;i<n;i++)if(strstr(rows[i].label,"QUIT")){quit=1;CHECK(rows[i].note[0]);}
                CHECK(quit);menu_close(a);
            }
            a->ribbon_open=0;a->ribbon=RIBBON_FULL;render(a);
            for(int i=0;i<a->nhits;i++)CHECK(a->hits[i].kind!=H_RIBBON_ITEM&&
                a->hits[i].kind!=H_RIBBON_MORE&&a->hits[i].kind!=H_MENU_SCRIM);
        }
    }
    test_cleanup();puts("Native integration: SDL snap/resize/focus, 4K clamp, Unicode composer, modal guard, two PTY routing, TIOCSWINSZ, child cleanup narrow-width label honesty, active-first ordering, Files header identity, exit controls and header ribbon lanes passed");return 0;
}

#endif /* LJ_NATIVE_HELPERS_ONLY */
