/* lilJack native workspace: SDL input/presentation, HUI drawing, owkterm PTYs.
 * Backend IPC carries workspace records only. No PTY bytes cross Python.
 * Owned attachment children are detached/reaped at exit; tmux sessions persist.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <SDL.h>
#include <json-c/json.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include "c_dock.h"
#include "hui_cell_row.h"
#include "hui_divider.h"
#include "hui_border.h"
#include "c_blocks.h"
#include "c_render.h"
#include "c_layout.h"
/* The legacy window offset shadows c_dock.h's enum edge. Terminal geometry
 * uses ANSI_DOCK_TOP; passing the old 152px offset rejects every top drop. */
#undef LJ_DOCK_TOP
#include "c_media.h"
#include "c_canvas.h"
#include <math.h>
#include "c_popup.h"
#include "c_metrics.h"
#include "c_ansi.h"
#include "c_review.h"
#include "c_dash.h"
#include "c_env.h"
#include "c_theme.h"
#include "owkterm_vt.h"

#define COUNT 64
#define INPUT_CAP 65536
#define RESPONSE_CAP (8u*1024u*1024u)
/* Palette. Structure is a dark dashboard's — a near-black ground, one slightly
 * lighter surface for bars, a single hairline rule, uppercase spaced labels and
 * small badge pills — carrying lilJack's blue and yellow rather than its
 * violet. the operator, 2026-09-09: "inspire by our dashboard looks". */
#define BG (lj_theme_rgb(LJ_THEME_BG))        /* ground                                        */
#define PANEL (lj_theme_rgb(LJ_THEME_PANEL))     /* bars and tile surfaces                        */
#define EDGE (lj_theme_rgb(LJ_THEME_EDGE))      /* hairline rule                                 */
#define SURF2 (lj_theme_rgb(LJ_THEME_SURF2))     /* raised: the active tab, a focused header      */
#define SURF3 (lj_theme_rgb(LJ_THEME_SURF3))     /* lifted: whatever the pointer is over          */
#define GREEN (lj_theme_rgb(LJ_THEME_GREEN))
#define TEXT (lj_theme_rgb(LJ_THEME_TEXT))
#define DIM (lj_theme_rgb(LJ_THEME_DIM))
#define CYAN (lj_theme_rgb(LJ_THEME_CYAN))
#define AMBER (lj_theme_rgb(LJ_THEME_AMBER))
#define RED (lj_theme_rgb(LJ_THEME_RED))
#define CELLW LJ_CELL_W
#define CELLH LJ_LINE_H

typedef struct {
    char id[81],agent[32],name[128],role[20],parent[81],state[40],task[256],room_id[81];
    char unavailable[120];   /* why the heartbeat gave up on it, empty if it has not */
    char daemon_id[64];      /* native backend: the supervisor's internal session id */
    void *vt;
    pid_t pid;
    int fd,cols,rows,seen,managed,connected,dock_pending;
    int native;              /* 1 = owkterm-native (sessiond) backend, no tmux */
    char *queue;size_t queued;
    unsigned char *rx;       /* native frame reassembly buffer */
    size_t rxlen,rxcap;
    int view_row;   /* first buffer row the tile shows; >0 when the tile is shorter than the PTY */
    lj_rect grid;
} Session;
typedef struct {lj_rect r;int kind,index,layer;char id[81];} Hit;
typedef struct {
    char id[81],name[161],state[16],phase[12];
    int collapsed,days_until_purge,questions,awaiting;
} Room;
typedef struct {char id[81];lj_dock dock;int initialized;} RoomView;
typedef struct {char destination[81],reply[128];char *text;} ChatDraft;
enum {H_SESSION=1,H_CREATE,H_HEADER,H_TERMINAL,H_RETILE,H_FULL,H_TEAM,H_HIDE,
      H_ROOM,H_POST,H_STAGE,H_DEST,H_ROLE,H_PARENT,H_TEAM_CLOSE,H_EFFECTS,H_QUIT,
      H_GROUP,H_COLLAPSE,H_NEW_ROOM,H_ROOM_CREATE,H_ROOM_CANCEL,
      H_CONTROLS,H_LEAD,H_AGENT_FOCUS,H_AGENT_DM,H_CONTROL,H_STOP_SESSION,H_OPEN_HERE,H_CANVAS,H_CANVAS_BODY,H_TEAM_PAGE,H_STATUS,H_REVIEW,H_REVIEW_BODY,H_DASH_BODY,H_GIT,H_GIT_BODY,
      H_FILES,H_FILES_BODY,H_FILE_ENTRY,H_FILE_UP,H_ROOM_FIELD,H_ROOM_AGENT,
      H_ABOUT,H_ABOUT_CLOSE,H_ABOUT_PLAY,H_BURGER,H_MENU_ITEM,H_RIBBON_ITEM,H_MENU_SCRIM,H_FILE_MENU,
      H_STATE_FILTER,H_ROOM_MENU,H_ROOM_ACTION,H_ROOM_STATE,H_ROOM_PURGE,H_LOAD,
      H_ROOM_TAB,H_ANSWER,H_ANSWER_DM,H_POPUP_BODY,H_POPUP_DRAG,H_POPUP_CLOSE,
      H_POPUP_TOGGLE,H_POPUP_BACK,H_POPUP_FORWARD,H_POPUP_MUTE,
      H_POPUP_VOL_DOWN,H_POPUP_VOL_UP,H_POPUP_SEEK,H_RIBBON_MORE,H_POPUP_RESOLUTION,H_POPUP_RESIZE,H_POPUP_VOLUME,H_SETTINGS,H_SET_ROW,H_SET_DEC,H_SET_INC,H_SET_RESET,H_SET_CLOSE,H_SET_SAVE,H_ROOM_MORE,
      H_ROOM_FOLDER_ENTRY,H_ROOM_FOLDER_UP,H_ROOM_FOLDER_USE,H_ROOM_FOLDER_BROWSE,
      H_FB_OPEN,H_FB_HOME,H_FB_PROJECT,H_FB_HIDDEN,H_FB_CRUMB,H_FB_PATH,H_FB_CANCEL,H_FB_CLOSE,H_FB_BODY};
/* Which dropdown is open, and one row of it. */
enum {MENU_NONE=0,MENU_ROLE,MENU_FILE,MENU_FILES,MENU_ROOM,MENU_STATE,MENU_TOOLBAR,MENU_OPEN_HERE,MENU_LOGO,MENU_ROOMS};
/* The room lifecycle, in the order the operator named it. `removed` is a 30-day
 * trash, not a delete, so it is listed last and never shown by default. */
static const char *ROOM_STATES[6]={"active","resolved","backlog","abandoned","archived","removed"};
typedef struct {
    char label[72];int action,index,checked,disabled,header;char note[96];
} MenuRow;
typedef struct {
    SDL_Window *window;SDL_Renderer *renderer;SDL_Texture *texture;
    int w,h,running,dirty,demo,effects,fullscreen,frames,rendered,minimised,ansi;
    int copy_pause,copy_pause_painted,gallery;
    uint64_t metrics_revision;
    uint32_t header_plot[3][8*LJ_CELL_W*40]; /* retained until queued sixel emission */
    char project[PATH_MAX],root[PATH_MAX],repo[PATH_MAX],layout_path[PATH_MAX];
    char tmux[PATH_MAX],socket[128],mood[64],toast[512],selected[81],focus[81],full[81];
    char sessiond_sock[PATH_MAX];
    Session sessions[COUNT];int nsession,order[COUNT],norder,sidebar_scroll;
    int hit_layer;                 /* stacking level currently being drawn */
    lj_rect lead_border[4];int nlead_border;   /* drawn after tile content */
    lj_dock dock;Hit hits[512];int nhits;
    char drag[81];int dragx,dragy,dragging,split,mousex,mousey;
    lj_dock_drag split_drag;
    int selecting,selr0,selc0,selr1,selc1;char selection_id[81];
    int sel_shown;                      /* the last selection stays highlighted after release until the next click or key */
    int sel_base;                       /* LJ_VT_SCROLLED at press: rows are stored relative to it so the highlight follows the text */
    int team,destination,room_scroll,team_page,status_open,status_scroll;
    int review_scroll,review_height,review_view_height;
    int dash_scroll,dash_height,dash_view_height;
    int git_scroll,git_height,room_log_before;
    json_object *files;char files_path[1024];int files_scroll,files_height;
    char stop_confirm[81];
    /* What the pointer is over, resolved from LAST frame's hit list with the
     * same smallest-target rule a click uses — so what lights up is exactly
     * what would activate. One frame of latency, which nobody can see. */
    int hover_kind,hover_index;char hover_id[81];
    uint32_t anim;              /* animation tick, advanced by the frame clock */
    Room rooms[COUNT];int nroom;
    RoomView room_views[COUNT+1];int nroom_view;
    char active_room[81],chat_dest[81];
    char room_filter[16];          /* which lifecycle state the tab strip shows */
    /* ⚠ The room tile is TABBED. the operator, 2026-09-09: room chat is everything the
     * agents say to each other, ACTIONABLE is the all-hands view with the todo
     * list beside it, STATUS and REVIEW moved IN here, and the private-DM tab
     * is gone from this tile entirely. */
    int room_tab;                  /* 0 ROOM · 1 ACTIONABLE · 2 STATUS · 3 REVIEW */
    int tab_name_cap;              /* the shared room-name cap draw_tabs chose this frame */
    json_object *room_states;      /* counts per state, straight from the backend */
    ChatDraft drafts[COUNT*2+1];int ndraft;
    char posting_dest[81],posting_text[16001];
    /* NEW ROOM form. the operator, 2026-09-09: a new room must offer the available
     * agents with role assignment, its own working folder, and a first TODO —
     * because the TODO is what arms the heartbeat and the intro is what tells
     * an agent where it is. A bare name creates a room nobody is in. */
    int room_dialog,room_field,room_agent[4];
    char room_name[161],room_folder[512],room_purpose[257],room_todo[3][161];
    char room_folder_listed[520],room_folder_base[512],room_folder_error[160];
    char room_folder_entries[500][256];
    int room_folder_count,room_folder_truncated;
    /* The folder browser popup (folder_browser_*): room_folder_browse is the
     * folder it shows; FOLDER itself only changes on USE FOLDER. */
    char room_folder_browse[512],fb_typed[512];
    int fb_open,fb_sel,fb_scroll,fb_rows,fb_follow,fb_hidden,fb_edit,fb_click;   /* fb_click: row a click selected, -1 none */
    int fb_placed,fb_dragging,fb_resizing,fb_mouse,fb_dx,fb_dy;
    lj_rect fb_rect;
    char draft[16001],reply[128];size_t draft_len;
    json_object *messages,*status_ribbon,*review_panel,*room_log,*room_questions,*room_dms,*dashboard;
    json_object *room_todos;       /* the ACTIVE room's todo list, from rooms[].todos */
    json_object *all_tasks;        /* every open row, for work that belongs to no room */
    int todo_scroll,todo_height;lj_rect todo_rect;   /* the todo column scrolls on its own */
    /* ⚠ The question the composer is ANSWERING, if any. Without it, clicking a
     * question and typing posted an ordinary message: the todo stayed blocked,
     * the asker got nothing, and the room looked answered when it was not. */
    char answer_qid[81];
    char answer_dm[81],answer_dm_room[81];
    char tasks_error[300];         /* why the registry could not be read, if it could not */
    pid_t backend_pid;int backend_in,backend_out,busy;
    Uint32 room_starting;          /* ticks when a room_start went out (or was queued); 0 = none outstanding */
    int start_inflight,start_pending;   /* the outstanding request / the queued one IS that room_start */
    char *pending;                 /* one deferred request, see request() */
    char *response;size_t response_len;
    uint32_t last_refresh,last_paint;
    lj_media media;int has_media;
    /* The shared room canvas (OPEN HERE → CANVAS). the operator drags in the tile;
     * agents append with `liljack room --draw`; the file is the only truth. */
    lj_canvas canvas;int has_canvas,canvas_drawing,canvas_npts;float canvas_pts[LJ_CANVAS_MAX_POINTS*2];lj_rect canvas_body;
    lj_popup popup;int about_open;   /* About + the rickroll deepseek wrote */
    int settings_open,settings_sel,settings_scroll,settings_chan;char settings_hex[8]; /* THEME → Theme editor… (rows: settings, colours, glyphs) */
    lj_rect popup_rect;
    int popup_placed,popup_dragging,popup_dx,popup_dy,popup_mouse;
    int popup_resizing;
    uint32_t *popup_pixels; /* owned until the terminal presents */
    size_t popup_pixel_cap;
    /* ⚠ A DROPDOWN, NOT A POPUP WINDOW (the operator, 2026-09-09: "it should not be a
     * popup window it should be a dropdown"). One at a time, hanging off the
     * control that opened it, closed by choosing or by clicking anywhere else. */
    int menu_kind;lj_rect menu_at;char menu_target[256];int menu_sel; /* keyboard row in the logo menu, -1 none */
    /* The burger ribbon: `ribbon_open` is the toggle, `ribbon` the slide
     * position 0..RIBBON_FULL. They are SEPARATE from menu_kind so a
     * sub-menu flyout can open without the ribbon reading itself as closed
     * and sliding shut under its own child. */
    int ribbon,ribbon_open;
} App;

static int render_ansi;
static uint32_t render_anim; /* same frame clock as App.anim */
static volatile sig_atomic_t quitting;
static void quit_signal(int sig){(void)sig;quitting=1;}
static SDL_Keymod modifiers(void){return lj_ansi_modifiers();}

static int clamp(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}
/* Floor division: a rect may start left of the origin, and C truncates toward
 * zero, which would snap a negative edge the wrong way. */
static int floordiv(int v,int unit){return v>=0?v/unit:-((-v+unit-1)/unit);}
static void copy(char *d,size_t n,const char *s){snprintf(d,n,"%s",s?s:"");}
static int inside(lj_rect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static const char *jstr(json_object *o,const char *key){json_object *v=NULL;return json_object_object_get_ex(o,key,&v)&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";}
static json_object *jget(json_object *o,const char *key){json_object *v=NULL;json_object_object_get_ex(o,key,&v);return v;}
static void jadd(json_object *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v));}
static void toast(App *a,const char *s){copy(a->toast,sizeof(a->toast),s);a->dirty=1;}
static Session *session(App *a,const char *id){for(int i=0;i<a->nsession;i++)if(!strcmp(a->sessions[i].id,id))return &a->sessions[i];return NULL;}
static Room *room_by_id(App *a,const char *id){for(int i=0;i<a->nroom;i++)if(!strcmp(a->rooms[i].id,id))return &a->rooms[i];return NULL;}
static const char *room_destination(App *a){return a->active_room[0]?a->active_room:"room";}
static const char *conversation(App *a){return a->chat_dest[0]?a->chat_dest:room_destination(a);}
static int in_workspace(App *a,Session *s){return !strcmp(s->room_id,a->active_room);}
static int is_dm(App *a){const char *dest=conversation(a);return strcmp(dest,"room")&&strncmp(dest,"r-",2);}
static int message_visible(App *a,json_object *m){
    const char *dest=conversation(a),*sender=jstr(m,"sender"),*target=jstr(m,"destination");
    if(!is_dm(a))return !strcmp(target,dest);
    return (!strcmp(sender,"operator")&&!strcmp(target,dest))||(!strcmp(sender,dest)&&!strcmp(target,"operator"));
}
static ChatDraft *draft_slot(App *a,const char *dest,int create){
    for(int i=0;i<a->ndraft;i++)if(!strcmp(a->drafts[i].destination,dest))return &a->drafts[i];
    if(!create||a->ndraft>=COUNT*2+1)return NULL;
    ChatDraft *slot=&a->drafts[a->ndraft++];copy(slot->destination,sizeof(slot->destination),dest);return slot;
}
static int set_conversation(App *a,const char *dest){
    if(!dest||!*dest)return 0;if(!strcmp(conversation(a),dest)){copy(a->chat_dest,sizeof(a->chat_dest),dest);return 1;}
    ChatDraft *old=draft_slot(a,conversation(a),1);if(!old){toast(a,"Conversation draft limit reached");return 0;}
    char *saved=strdup(a->draft);if(!saved){toast(a,"Could not preserve draft");return 0;}
    free(old->text);old->text=saved;copy(old->reply,sizeof(old->reply),a->reply);
    copy(a->chat_dest,sizeof(a->chat_dest),dest);ChatDraft *next=draft_slot(a,dest,0);
    copy(a->draft,sizeof(a->draft),next&&next->text?next->text:"");a->draft_len=strlen(a->draft);copy(a->reply,sizeof(a->reply),next?next->reply:"");
    a->destination=is_dm(a);a->room_scroll=0;a->dirty=1;return 1;
}
static void drafts_close(App *a){for(int i=0;i<a->ndraft;i++)free(a->drafts[i].text);a->ndraft=0;}
static void acknowledge_post(App *a){
    if(!a->posting_dest[0])return;
    if(!strcmp(conversation(a),a->posting_dest)&&!strcmp(a->draft,a->posting_text)){a->draft[0]=0;a->draft_len=0;a->reply[0]=0;}
    ChatDraft *slot=draft_slot(a,a->posting_dest,0);
    if(slot&&slot->text&&!strcmp(slot->text,a->posting_text)){free(slot->text);slot->text=NULL;slot->reply[0]=0;}
    a->posting_dest[0]=0;a->posting_text[0]=0;
}
static const char *icon(const char *agent){return !strcmp(agent,"claude")?lj_theme_glyph(LJ_THEME_ICON_CLAUDE):!strcmp(agent,"codex")?lj_theme_glyph(LJ_THEME_ICON_CODEX):!strcmp(agent,"deepseek")?lj_theme_glyph(LJ_THEME_ICON_DEEPSEEK):lj_theme_glyph(LJ_THEME_ICON_SHELL);}
/* ⚠ THE TUI IS A CELL GRID AND A CLICK ARRIVES AT A CELL CENTRE.
 * c_ansi maps a click on column N to pixel (N-1)*10+5, while the layout is
 * pixel-based at arbitrary offsets. A widget narrower than a cell therefore
 * PAINTS cells whose centre falls outside its own rect, and those cells are
 * dead. Measured on this layout: every 22px close button paints three cells
 * and one or two of them do not respond (which is why the × only closed when
 * clicked in the middle), and an 8px dock divider has no clickable centre at
 * all for 2 of every 10 pixel offsets — the dividers that refuse to drag.
 * Growing the HIT area to the whole cells the widget paints makes what you
 * see and what you can click the same thing. Drawing is untouched. */
static lj_rect cellsnap(lj_rect r){
    int x0=floordiv(r.x,CELLW)*CELLW,y0=floordiv(r.y,CELLH)*CELLH;
    int x1=floordiv(r.x+r.w+CELLW-1,CELLW)*CELLW,y1=floordiv(r.y+r.h+CELLH-1,CELLH)*CELLH;
    return (lj_rect){x0,y0,x1-x0,y1-y0};
}
static lj_rect hitrect(lj_rect r){return cellsnap(r);}
static int hovered(App *a,int kind,int index,const char *id){
    return a->hover_kind==kind&&a->hover_index==index&&!strcmp(a->hover_id,id?id:"");
}
/* Round every EDGE to the nearest cell boundary. Adjacent tiles share an edge
 * coordinate, and rounding is a function of that coordinate alone, so they
 * round identically and cannot overlap or leave a crack. This is what makes a
 * one-row header actually one row: at a sub-cell tile origin the header's
 * 20px still straddled two rows, so the first line of agent output shared a
 * cell with the close button. It also removes the ragged part of the gap
 * between tiles, which is the "large gaps between vertical elements". */
/* ⚠ TUI CHROME IS COUNTED IN ROWS, NOT PIXELS. The window constants land on
 * sub-cell offsets (a dock top of 112px is row 5.6), so every panel straddled
 * two rows and the layout spent five rows on chrome before any agent output.
 * The terminal gets whole rows instead:
 *   row 0  title · project · mood      row 1  menu
 *   row 2  room bar                    row 3+ sidebar and tiles
 *   last 2 rows  toast and status ribbon
 * the operator, 2026-09-09: "we still have large gaps between vertical elements". */
#define ANSI_MARGIN      0            /* a TUI uses the whole width */
#define ANSI_SIDEBAR_TOP (1*CELLH)   /* room tabs        */
#define ANSI_AGENT_TOP   (2*CELLH)   /* agent chips      */
#define ANSI_DOCK_TOP    (3*CELLH)   /* tiles, full width */
#define ANSI_FOOTER_H    (2*CELLH)
static lj_rect cellround(lj_rect r){
    int x0=(r.x+CELLW/2)/CELLW*CELLW,y0=(r.y+CELLH/2)/CELLH*CELLH;
    int x1=(r.x+r.w+CELLW/2)/CELLW*CELLW,y1=(r.y+r.h+CELLH/2)/CELLH*CELLH;
    return (lj_rect){x0,y0,x1-x0,y1-y0};
}
/* Large surfaces you click to FOCUS, as opposed to small controls you click to
 * ACT. A dock divider may share a cell with either once hit areas are snapped;
 * it must lose to a control and win over a surface. Without this the × on a
 * tile that touches a divider was swallowed by the resize grab — the same
 * symptom as the old off-by-a-cell miss, from the opposite direction. */
static int surface_kind(int k){
    return k==H_HEADER||k==H_TERMINAL||k==H_ROOM||k==H_GROUP||k==H_SESSION||
           k==H_REVIEW_BODY||k==H_GIT_BODY;
}
/* ⚠ WHAT IS DRAWN ON TOP MUST BE CLICKED ON TOP. Smallest-area-wins is the
 * right rule BETWEEN SIBLINGS — the more specific control is the one you meant
 * — and the wrong rule ACROSS LAYERS: an open menu row is larger than an agent
 * spawn chip beneath it, so area alone handed the click to the chip. codex
 * proved it on the backend pipe: clicking burger -> TEAM emitted
 * {action:create, agent:shell} instead of opening Team. A menu item that spawns
 * a process is the worst possible version of this bug, and every stray click
 * left another dead session in the workspace.
 *
 * `hit_layer` is the stacking level being drawn; overlays raise it. Resolution
 * takes the HIGHEST layer first, then smallest area within it, so both rules
 * keep the case they are right about. */
static void hit(App *a,lj_rect r,int kind,int index,const char *id){if(a->nhits<512){Hit *h=&a->hits[a->nhits++];*h=(Hit){.r=hitrect(r),.kind=kind,.index=index,.layer=a->hit_layer};copy(h->id,sizeof(h->id),id);}}
static void rect(lj_rect r,uint32_t color){if(r.w>0&&r.h>0){if(render_ansi)lj_ansi_rect(r.x,r.y,r.w,r.h,color);else lj_render_rect(r.x,r.y,r.w,r.h,color);}}
static void text(int x,int y,const char *s,uint32_t color,int maxw){if(maxw>0){if(render_ansi)lj_ansi_text(x,y,s,color,maxw);else lj_render_text(x,y,s,color,maxw);}}

#ifdef LJ_GIT_RENDERER
#include "c_git_dag.h"
#else
static int lj_git_dag_draw(json_object *snapshot,int x,int y,int w,int h,int scroll,int ansi){
    (void)ansi;int row=0;char label[1024];json_object *nodes=jget(snapshot,"rows");
    const char *reason=!snapshot?"Loading Git history…":jstr(snapshot,"error");
    if(*reason){if(!scroll)text(x,y,reason,DIM,w);return CELLH;}
    size_t n=nodes?json_object_array_length(nodes):0;
    if(!n){if(!scroll)text(x,y,"No commits available",DIM,w);return CELLH;}
    for(size_t i=0;i<n;i++){
        json_object *node=json_object_array_get_idx(nodes,i);int yy=y+row++*CELLH-scroll;
        snprintf(label,sizeof(label),"%.12s  %s  %s",jstr(node,"id"),jstr(node,"refs"),jstr(node,"subject"));
        if(yy>=y&&yy+CELLH<=y+h)text(x,yy,label,TEXT,w);
    }
    int yy=y+row++*CELLH-scroll;
    if(yy>=y&&yy+CELLH<=y+h)text(x,yy,json_object_get_boolean(jget(snapshot,"truncated"))?"More commits exist · DAG renderer pending":"Commit list · DAG renderer pending",DIM,w);
    return row*CELLH;
}
#endif
static void glyph(int x,int y,uint32_t cp,uint32_t color,int cells){if(render_ansi)lj_ansi_glyph(x,y,cp,color,cells);else lj_render_glyph(x,y,cp,color,cells);}
static void panel(lj_rect r,uint32_t color,int radius){if(render_ansi)rect(r,color);else lj_render_panel(r.x,r.y,r.w,r.h,radius,color);}
/* Rules share the physical-pixel separator strips in sixel terminals. */
static void hairline(lj_rect r,uint32_t c,int vertical){
    if(render_ansi&&lj_ansi_images_available()){
        lj_ansi_separator(r.x,r.y,r.w,r.h,c,vertical);return;
    }
    if(vertical)rect((lj_rect){r.x+r.w/2,r.y,1,r.h},c);
    else rect((lj_rect){r.x,r.y+r.h/2,r.w,1},c);
}
static uint32_t divider_colour(int hot){
    return lj_theme_rgb(hot?LJ_THEME_DIVIDER_HOT:LJ_THEME_DIVIDER);
}
/* Per-edge anchors share the presenter's contained-cell frame. Occupied
 * cells still win; normal lead chrome reserves its decoration ring. */
static void dotted_border(lj_rect r,uint32_t a_col,uint32_t b_col){
    hui_border_grid g;if(!hui_border_cells(r.x,r.y,r.w,r.h,CELLW,CELLH,&g))return;
    for(int edge=0;edge<4;edge++){
        int count=hui_border_edge_count((edge&1)?g.rows:g.cols);
        for(int i=0;i<count-1;i++){
            hui_border_point p;if(!hui_border_anchor(g.cols,g.rows,edge,i,&p))continue;
            uint32_t color=hui_border_color_index(p.ordinal,render_anim/4)?b_col:a_col;
            if(render_ansi){
                int drawn=lj_ansi_dot((g.col+p.col)*CELLW,(g.row+p.row)*CELLH,color);
                /* Short vertical edges need an interior anchor even when the
                 * preferred cell contains text. Only free cells are eligible. */
                if(!drawn&&(edge&1)&&count==3&&i==1){
                    for(int attempt=0;attempt<g.rows;attempt++){
                        int row=hui_border_interior_candidate(g.rows,attempt);
                        if(row>=0&&lj_ansi_dot((g.col+p.col)*CELLW,(g.row+row)*CELLH,color))break;
                    }
                }
            }
            else{
                int at=hui_border_position((edge&1)?g.rows*CELLH-1:g.cols*CELLW-1,count-1,i);
                int x=g.col*CELLW+(edge==0?at:edge==1?g.cols*CELLW-1:edge==2?g.cols*CELLW-1-at:0);
                int y=g.row*CELLH+(edge==0?0:edge==1?at:edge==2?g.rows*CELLH-1:g.rows*CELLH-1-at);
                rect((lj_rect){x,y,1,1},color);
            }
        }
    }
    /* The sixel strip is placed one cell OUTSIDE the rect it is given, so the
     * INSET rect makes it land on the tile's own gutter cells — the same cells
     * the placeholders above occupy — never on a neighbour's row or column. */
    if(render_ansi&&lj_ansi_images_available()&&r.w>2*CELLW&&r.h>2*CELLH)
        lj_ansi_border(r.x+CELLW,r.y+CELLH,r.w-2*CELLW,r.h-2*CELLH,a_col,b_col,SDL_GetTicks()/LJ_BORDER_TICK_MS);
}
static void border(lj_rect r,uint32_t c){rect((lj_rect){r.x,r.y,r.w,1},c);rect((lj_rect){r.x,r.y+r.h-1,r.w,1},c);rect((lj_rect){r.x,r.y,1,r.h},c);rect((lj_rect){r.x+r.w-1,r.y,1,r.h},c);}
static void button(App *a,lj_rect r,const char *s,int kind,int index,const char *id,int active){
    int pad=r.w<40?5:9,hot=hovered(a,kind,index,id);
    if(render_ansi){
        panel(r,active?GREEN:hot?SURF3:PANEL,0);
        text(r.x+pad,r.y,s,active?BG:hot?GREEN:TEXT,r.w-pad*2);
    } else {
        panel(r,active?GREEN:hot?GREEN:EDGE,7);
        panel((lj_rect){r.x+1,r.y+1,r.w-2,r.h-2},active?lj_theme_rgb(LJ_THEME_BUTTON_ACTIVE):hot?SURF3:lj_theme_rgb(LJ_THEME_BUTTON_IDLE),6);
        text(r.x+pad,r.y+(r.h-20)/2,s,active?GREEN:TEXT,r.w-pad*2);
    }
    hit(a,r,kind,index,id);
}
static uint32_t cellcolor(uint32_t c){
    return c&0x1000000?c&0xffffff:lj_theme_rgb((lj_theme_id)(LJ_THEME_ANSI_0+c%16));
}
static void child_stop(pid_t pid){
    if(pid<=0)return;
    if(waitpid(pid,NULL,WNOHANG)!=0)return;
    kill(pid,SIGHUP);struct timespec t={0,10000000};
    for(int i=0;i<50;i++){if(waitpid(pid,NULL,WNOHANG)!=0)return;nanosleep(&t,NULL);}
    kill(pid,SIGKILL);waitpid(pid,NULL,0);
}
static void terminal_close(Session *s){if(s->fd>=0)close(s->fd);s->fd=-1;child_stop(s->pid);s->pid=0;s->connected=0;free(s->queue);s->queue=NULL;s->queued=0;free(s->rx);s->rx=NULL;s->rxlen=s->rxcap=0;if(s->vt)lj_vt_free(s->vt);s->vt=NULL;}
/* ── owkterm-native (sessiond) transport ───────────────────────────────────
 * The supervisor owns every agent pty master; the UI is one more client over a
 * unix socket. Frames are 4-byte big-endian length + 1-byte tag + body:
 *   C control (JSON, UI -> supervisor)   O output (supervisor -> UI)
 *   I input (UI -> supervisor)           F SCM_RIGHTS fd (ignored; resize is op)
 * The master fd passed on attach is deliberately not consumed — the byte stream
 * is the only channel, so no second reader steals output from the VT parser. */
static int send_bytes(App *a,Session *s,const char *buf,size_t n);
static int sel_shift(App *a,Session *s);            /* selection anchor: rows scrolled since the press */
static int frame_write(int fd,unsigned char tag,const void *body,size_t n){
    unsigned char *frame=malloc(5+n);if(!frame)return -1;
    uint32_t len=1+(uint32_t)n;
    frame[0]=(unsigned char)(len>>24);frame[1]=(unsigned char)(len>>16);
    frame[2]=(unsigned char)(len>>8);frame[3]=(unsigned char)len;frame[4]=tag;
    if(n)memcpy(frame+5,body,n);
    size_t total=5+n,sent=0;
    while(sent<total){ssize_t w=write(fd,frame+sent,total-sent);
        if(w<0){if(errno==EINTR)continue;free(frame);return -1;}sent+=(size_t)w;}
    free(frame);return 0;
}
static int native_ctl(int fd,const char *op,const char *session,int cols,int rows,int sig){
    json_object *o=json_object_new_object();
    json_object_object_add(o,"op",json_object_new_string(op));
    json_object_object_add(o,"session",json_object_new_string(session));
    if(cols>0)json_object_object_add(o,"cols",json_object_new_int(cols));
    if(rows>0)json_object_object_add(o,"rows",json_object_new_int(rows));
    if(sig>=0)json_object_object_add(o,"signal",json_object_new_int(sig));
    const char *str=json_object_to_json_string(o);
    int r=frame_write(fd,'C',str,strlen(str));
    json_object_put(o);
    return r;
}
static void native_parse(App *a,Session *s){
    while(s->rxlen>=4){
        uint32_t len=((uint32_t)s->rx[0]<<24)|((uint32_t)s->rx[1]<<16)|((uint32_t)s->rx[2]<<8)|s->rx[3];
        if(len<1||len>0x100000){s->connected=0;return;}
        if(s->rxlen<4+len)break;
        unsigned char tag=s->rx[4];unsigned char *body=s->rx+5;size_t blen=len-1;
        if(tag=='O'){lj_vt_feed(s->vt,body,(int)blen);
            const char *reply=lj_vt_reply(s->vt);if(reply&&*reply)send_bytes(a,s,reply,strlen(reply));lj_vt_clear_reply(s->vt);}
        else if(tag=='C'&&blen<512){char tmp[512];memcpy(tmp,body,blen);tmp[blen]=0;
            json_object *o=json_tokener_parse(tmp);if(o){const char *op=jstr(o,"op");
                if(op&&!strcmp(op,"exited"))s->connected=0;json_object_put(o);}}
        size_t used=4+len;memmove(s->rx,s->rx+used,s->rxlen-used);s->rxlen-=used;
    }
}
static int native_attach(App *a,Session *s){
    if(!a->sessiond_sock[0]){toast(a,"No native session supervisor socket");return 0;}
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0){toast(a,"Could not open supervisor socket");return 0;}
    struct sockaddr_un addr;memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;
    if(strlen(a->sessiond_sock)>=sizeof(addr.sun_path)){close(fd);toast(a,"Native supervisor socket path too long");return 0;}
    strcpy(addr.sun_path,a->sessiond_sock);
    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0){close(fd);toast(a,"Native session supervisor is not running");return 0;}
    s->cols=80;s->rows=24;s->vt=lj_vt_new(s->cols,s->rows);s->queue=malloc(INPUT_CAP);
    if(!s->vt||!s->queue){close(fd);terminal_close(s);toast(a,"Terminal allocation failed");return 0;}
    const char *did=s->daemon_id[0]?s->daemon_id:s->name;
    s->fd=fd;
    /* Send attach on a still-blocking socket; then the socket goes non-blocking
     * for the frame pump. The SCM_RIGHTS master fd the supervisor sends back is
     * discarded by recv() — the byte stream is the only channel we render. */
    native_ctl(fd,"attach",did,0,0,-1);
    fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,O_NONBLOCK);s->connected=1;
    return 1;
}
static int attach(App *a,Session *s){
    if(s->vt&&!s->connected&&s->managed&&strcmp(s->state,"exited")&&!a->demo)terminal_close(s);
    if(s->vt||!s->managed)return s->vt!=NULL;
    if(s->native)return native_attach(a,s);
    if(!a->tmux[0])return s->vt!=NULL;
    s->cols=80;s->rows=24;s->vt=lj_vt_new(s->cols,s->rows);
    s->queue=malloc(INPUT_CAP);if(!s->vt||!s->queue){terminal_close(s);toast(a,"Terminal allocation failed");return 0;}
    struct winsize ws={.ws_row=24,.ws_col=80};
    s->pid=forkpty(&s->fd,NULL,NULL,&ws);
    if(s->pid==0){if(lj_child_env(LJ_ENV_ATTACH)<0)_exit(127);setenv("TERM","xterm-256color",1);
        execl(a->tmux,a->tmux,"-L",a->socket,"-f","/dev/null","attach-session","-t",s->name,(char*)0);_exit(127);}
    if(s->pid<0){terminal_close(s);toast(a,"Could not attach PTY");return 0;}
    fcntl(s->fd,F_SETFD,FD_CLOEXEC);fcntl(s->fd,F_SETFL,O_NONBLOCK);s->connected=1;return 1;
}
/* The classic terminal minimum, and what attach() already opens with. Below
 * this a harness TUI cannot lay itself out. */
#define LJ_TERM_MIN_COLS 80
#define LJ_TERM_MIN_ROWS 24
static void resize_terminal(Session *s,int cols,int rows){
    cols=clamp(cols,LJ_TERM_MIN_COLS,LJ_VT_MAXCOLS);rows=clamp(rows,LJ_TERM_MIN_ROWS,LJ_VT_MAXROWS);
    if(cols==s->cols&&rows==s->rows)return;
    s->cols=cols;s->rows=rows;lj_vt_resize(s->vt,cols,rows);
    if(s->native){if(s->fd>=0)native_ctl(s->fd,"resize",s->daemon_id[0]?s->daemon_id:s->name,cols,rows,-1);return;}
    if(s->fd>=0){struct winsize ws={.ws_row=(unsigned short)rows,.ws_col=(unsigned short)cols};ioctl(s->fd,TIOCSWINSZ,&ws);}
}
static int send_bytes(App *a,Session *s,const char *buf,size_t n){
    if(a->demo||!s||!s->connected||s->fd<0){toast(a,a->demo?"Demo replay: open the live app to interact with agents":"Terminal is disconnected");return 0;}
    if(s->native){
        /* Input is a small I frame; send it directly rather than queueing. */
        if(n>65536)n=65536;
        size_t cap=5+n;unsigned char *frame=malloc(cap);if(!frame){toast(a,"Out of memory");return 0;}
        uint32_t len=1+(uint32_t)n;
        frame[0]=(unsigned char)(len>>24);frame[1]=(unsigned char)(len>>16);
        frame[2]=(unsigned char)(len>>8);frame[3]=(unsigned char)len;frame[4]='I';
        memcpy(frame+5,buf,n);
        size_t sent=0;
        while(sent<cap){ssize_t w=write(s->fd,frame+sent,cap-sent);
            if(w<0){if(errno==EINTR)continue;break;}sent+=(size_t)w;}
        free(frame);
        lj_vt_scroll(s->vt,-100000);a->dirty=1;return 1;
    }
    if(n>INPUT_CAP-s->queued){toast(a,"Terminal input queue full; wait for the session");return 0;}
    memcpy(s->queue+s->queued,buf,n);s->queued+=n;lj_vt_scroll(s->vt,-100000);a->dirty=1;return 1;
}
static int paste(App *a,Session *s,const char *str,int require_brackets){
    if(!s||!s->vt)return 0;
    int brackets=lj_vt_state(s->vt,6);
    if(require_brackets&&!brackets){toast(a,"Agent has not enabled bracketed paste; type directly in its terminal");return 0;}
    size_t n=strlen(str);if(n+12>INPUT_CAP-(s?s->queued:0)){toast(a,"Paste exceeds terminal queue limit");return 0;}
    if(brackets)send_bytes(a,s,"\033[200~",6);
    /* Strip any embedded end-of-paste sequence, preserving UTF-8. */
    const char *p=str,*q;while((q=strstr(p,"\033[201~"))){send_bytes(a,s,p,(size_t)(q-p));p=q+6;}
    int ok=send_bytes(a,s,p,strlen(p));if(brackets)send_bytes(a,s,"\033[201~",6);return ok;
}
static int poll_terminal(App *a,Session *s){
    if(s->fd<0||!s->connected)return 0;
    if(s->native){
        int changed=0;unsigned char data[32768];
        for(int i=0;i<8;i++){ssize_t n=read(s->fd,data,sizeof(data));
            if(n<0&&(errno==EAGAIN||errno==EINTR))break;
            if(n<=0){s->connected=0;changed=1;break;}
            if(s->rxlen+(size_t)n>s->rxcap){
                size_t nc=s->rxcap?s->rxcap*2:16384;
                while(nc<s->rxlen+(size_t)n)nc*=2;
                unsigned char *p=realloc(s->rx,nc);if(!p){s->connected=0;break;}
                s->rx=p;s->rxcap=nc;
            }
            memcpy(s->rx+s->rxlen,data,(size_t)n);s->rxlen+=(size_t)n;changed=1;
        }
        if(s->rxlen)native_parse(a,s);
        return changed;
    }
    if(s->queued){ssize_t n=write(s->fd,s->queue,s->queued);if(n>0){s->queued-=(size_t)n;memmove(s->queue,s->queue+n,s->queued);}else if(n<0&&errno!=EAGAIN&&errno!=EINTR)s->connected=0;}
    int changed=0;unsigned char data[32768];
    for(int i=0;i<8;i++){ssize_t n=read(s->fd,data,sizeof(data));
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){s->connected=0;changed=1;break;}
        lj_vt_feed(s->vt,data,(int)n);const char *reply=lj_vt_reply(s->vt);
        if(reply&&*reply)send_bytes(a,s,reply,strlen(reply));lj_vt_clear_reply(s->vt);changed=1;
    }
    return changed;
}
static int backend_start(App *a){
    int in[2],out[2];if(pipe(in))return 0;if(pipe(out)){close(in[0]);close(in[1]);return 0;}
    for(int i=0;i<2;i++){fcntl(in[i],F_SETFD,FD_CLOEXEC);fcntl(out[i],F_SETFD,FD_CLOEXEC);}
    a->backend_pid=fork();
    if(!a->backend_pid){dup2(in[0],0);dup2(out[1],1);for(int i=0;i<2;i++){close(in[i]);close(out[i]);}if(lj_child_env(LJ_ENV_HELPER)<0)_exit(127);
        const char *toolbox_root=getenv("LILJACK_TOOLBOX_ROOT");char path[3*PATH_MAX+64];snprintf(path,sizeof(path),"%s:%s/toolbox%s%s/toolbox",a->repo,a->repo,toolbox_root?":":"",toolbox_root?toolbox_root:"");setenv("PYTHONPATH",path,1);
        execlp("python3","python3","-m","liljack_app.backend","--stdio",a->root,a->project,(char*)0);_exit(127);}
    close(in[0]);close(out[1]);
    if(a->backend_pid<0){close(in[1]);close(out[0]);return 0;}
    a->backend_in=in[1];a->backend_out=out[0];fcntl(a->backend_out,F_SETFL,O_NONBLOCK);
    a->response=malloc(RESPONSE_CAP+1);if(!a->response)return 0;return 1;
}
static int send_request(App *a,const char *s);
/* ⚠ A CLICK THAT ARRIVES DURING A REFRESH USED TO BE THROWN AWAY. The app polls
 * the helper every 3 seconds, and while that reply was outstanding every UI
 * action was refused with a toast — so buttons "sometimes did nothing", with
 * the reason a line of text at the bottom of the screen that nobody reads while
 * looking at the thing they just clicked. One deferred slot is enough: the
 * newest intent replaces an older unsent one, and it is sent the moment the
 * helper answers. */
static int request(App *a,json_object *o){
    if(a->demo){json_object_put(o);toast(a,"Demo workspace: actions are previews; no live agents launched");return 0;}
    if(a->backend_in<0){json_object_put(o);toast(a,"Workspace helper is not running");return 0;}
    /* ⚠ ONLY THE room_start's OWN REPLY RE-ARMS CREATE. Clearing the guard on any
     * error let an unrelated in-flight failure re-open CREATE while the start was
     * still queued behind it, so a second CREATE queued a duplicate (codex
     * review, room 2103). Track which request is the start. */
    int is_start=!strcmp(jstr(o,"action"),"room_start");
    if(a->busy&&a->start_pending&&!is_start){
        /* "newest intent replaces older" must never silently drop a room start */
        json_object_put(o);toast(a,"A room is starting · try that again in a moment");return 0;
    }
    if(a->busy){
        const char *text=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
        char *copy_of=strdup(text);json_object_put(o);
        if(!copy_of){toast(a,"Out of memory queueing that action");return 0;}
        free(a->pending);a->pending=copy_of;a->start_pending=is_start;return 1;
    }
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);size_t n=strlen(s),sent=0;int ok=1;
    /* Each small UI request has one owned reader, and is bounded by the composer. */
    while(sent<n){ssize_t r=write(a->backend_in,s+sent,n-sent);if(r<0){if(errno==EINTR)continue;ok=0;break;}sent+=(size_t)r;}
    if(ok&&write(a->backend_in,"\n",1)!=1)ok=0;json_object_put(o);
    if(!ok){toast(a,"Workspace helper disconnected; terminals remain attached");return 0;}
    a->busy=1;a->start_inflight=is_start;a->last_refresh=SDL_GetTicks();return 1;
}
/* Flush the deferred request, if any. Called once each reply is consumed. */
static int send_request(App *a,const char *s){
    size_t n=strlen(s),sent=0;int ok=1;
    while(sent<n){ssize_t r=write(a->backend_in,s+sent,n-sent);if(r<0){if(errno==EINTR)continue;ok=0;break;}sent+=(size_t)r;}
    if(ok&&write(a->backend_in,"\n",1)!=1)ok=0;
    if(!ok){toast(a,"Workspace helper disconnected; terminals remain attached");return 0;}
    a->busy=1;a->last_refresh=SDL_GetTicks();return 1;
}
static int action(App *a,const char *name){json_object *o=json_object_new_object();jadd(o,"action",name);return request(a,o);}
static int request_room_log(App *a,int before_seq){
    if(!a->active_room[0])return action(a,"refresh");
    json_object *o=json_object_new_object();jadd(o,"action","room_log");jadd(o,"room",a->active_room);
    json_object_object_add(o,"before_seq",json_object_new_int(before_seq));
    json_object_object_add(o,"limit",json_object_new_int(100));int ok=request(a,o);if(ok)a->room_log_before=before_seq;return ok;
}
static int request_files(App *a,const char *path){
    json_object *o=json_object_new_object();jadd(o,"action","files_list");
    jadd(o,"room",a->active_room);jadd(o,"path",path?path:"");
    return request(a,o);
}
static RoomView *room_view(App *a,const char *id){
    for(int i=0;i<a->nroom_view;i++)if(!strcmp(a->room_views[i].id,id))return &a->room_views[i];
    if(a->nroom_view>=COUNT+1)return NULL;RoomView *v=&a->room_views[a->nroom_view++];copy(v->id,sizeof(v->id),id);lj_dock_init(&v->dock);return v;
}
static void layout_file(App *a,char *path,size_t size){snprintf(path,size,"%s.%s",a->layout_path,a->active_room[0]?a->active_room:"standalone");}
static void save_layout(App *a){
    RoomView *v=room_view(a,a->active_room);if(v){v->dock=a->dock;v->initialized=1;}
    if(!a->demo&&a->layout_path[0]){char path[PATH_MAX+96];layout_file(a,path,sizeof(path));if(!lj_dock_save(&a->dock,path))toast(a,"Could not save room layout");}
}
/* Tiles that are VIEWS, not agent sessions. prune_workspace drops any leaf with
 * no live session behind it, so a view missing from this list is created and
 * then silently removed by the next snapshot — which is exactly what happened
 * to the Files tile. Add a view here when you add one. */
static int view_tile(const char *id){
    return !strcmp(id,"room")||!strcmp(id,"media")||!strcmp(id,"canvas")||!strcmp(id,"review")||
           !strcmp(id,"git")||!strcmp(id,"files")||!strcmp(id,"load");
}
static int dock_has(App *a,const char *id){for(int i=0;i<LJ_DOCK_MAX_NODES;i++)if(a->dock.nodes[i].used&&!a->dock.nodes[i].axis&&!strcmp(a->dock.nodes[i].id,id))return 1;return 0;}
static int terminal_tile(Session *s){return s&&(s->managed||s->vt);}
/* ⚠ AN ENDED SESSION MUST LOOK ENDED. After `/exit` the process is gone but the
 * tmux session lingers (remain-on-exit), so the tile kept drawing the last
 * frame and read as a live agent. The workspace reports the state; a managed
 * session whose PTY has closed is the same fact reached locally. */
static int session_dead(Session *s){
    if(!s)return 0;
    /* Demo fixtures have a VT and no PTY by construction; without this every
     * fixture read as a session that had just exited. */
    if(!strcmp(s->state,"demo"))return 0;
    if(!strcmp(s->state,"exited")||!strcmp(s->state,"stopped")||!strcmp(s->state,"ended"))return 1;
    return s->managed&&s->vt&&!s->connected;
}
static void prune_workspace(App *a){
    char remove_ids[COUNT][81];int n=0;
    for(int i=0;i<LJ_DOCK_MAX_NODES;i++)if(a->dock.nodes[i].used&&!a->dock.nodes[i].axis){const char *id=a->dock.nodes[i].id;if(view_tile(id))continue;Session *s=session(a,id);if(!s||!s->seen||!in_workspace(a,s)||!terminal_tile(s)){if(n<COUNT)copy(remove_ids[n++],81,id);}}
    for(int i=0;i<n;i++){lj_dock_remove(&a->dock,remove_ids[i]);if(!strcmp(a->full,remove_ids[i]))a->full[0]=0;if(!strcmp(a->focus,remove_ids[i]))copy(a->focus,sizeof(a->focus),"room");}
}
static void retile(App *a){
    const char *ids[COUNT];int n=0,review=dock_has(a,"review"),git=dock_has(a,"git"),files=dock_has(a,"files");
    for(int i=0;i<a->norder&&n<COUNT-1-review-git-files-a->has_media-a->has_canvas;i++){Session *s=&a->sessions[a->order[i]];if(in_workspace(a,s)&&terminal_tile(s))ids[n++]=s->id;}
    if(!lj_dock_retile(&a->dock,ids,n)){toast(a,"Could not retile terminals");return;}
    if(a->has_media)lj_dock_drop(&a->dock,"media","room",LJ_DOCK_TOP);
    if(a->has_canvas)lj_dock_drop(&a->dock,"canvas","room",LJ_DOCK_TOP);
    if(review)lj_dock_drop(&a->dock,"review","room",LJ_DOCK_RIGHT);
    if(git)lj_dock_drop(&a->dock,"git","room",LJ_DOCK_RIGHT);
    if(files)lj_dock_drop(&a->dock,"files","room",LJ_DOCK_RIGHT);
    a->full[0]=0;save_layout(a);a->dirty=1;
}
static void load_workspace(App *a){
    RoomView *v=room_view(a,a->active_room);if(!v)return;
    /* ⚠ A ROOM THAT HAS NEVER BEEN ARRANGED SHOULD SHOW THE LOAD TILE. the operator
     * asked for live CPU/MEM/GPU tiles and then twice reported not seeing any
     * graph — because the tile is only ever docked by an explicit burger->LOAD,
     * and a saved layout that never contained one never will. A FIRST-TIME room
     * gets it; an existing layout is left exactly as arranged, because
     * re-adding a tile somebody closed is worse than never showing it. */
    int fresh_room=0;
    if(!v->initialized){char path[PATH_MAX+96];layout_file(a,path,sizeof(path));
        struct stat lst;fresh_room=(a->demo||!a->layout_path[0]||stat(path,&lst)!=0);
        if(!a->demo&&a->layout_path[0])lj_dock_load(&v->dock,path);v->initialized=1;}
    a->dock=v->dock;prune_workspace(a);
    for(int i=0;i<a->norder;i++){Session *s=&a->sessions[a->order[i]];if(s->dock_pending&&in_workspace(a,s)&&terminal_tile(s)){
        if(dock_has(a,s->id)||lj_dock_drop(&a->dock,s->id,"room",LJ_DOCK_LEFT))s->dock_pending=0;
    }}
    int terminals=0;for(int i=0;i<LJ_DOCK_MAX_NODES;i++)if(a->dock.nodes[i].used&&!a->dock.nodes[i].axis&&strcmp(a->dock.nodes[i].id,"room"))terminals++;
    if(!terminals)retile(a);
    if(fresh_room&&!a->demo&&!dock_has(a,"load")){
        lj_dock_drop(&a->dock,"load","room",LJ_DOCK_RIGHT);save_layout(a);
    }
}
static void switch_workspace(App *a,const char *id){
    if(*id&&!room_by_id(a,id))return;
    char destination[81];copy(destination,sizeof(destination),*id?id:"room");
    if(!set_conversation(a,destination))return;
    if(strcmp(a->active_room,id)){save_layout(a);copy(a->active_room,sizeof(a->active_room),id);load_workspace(a);a->room_log_before=0;
        if(a->room_log){json_object_put(a->room_log);a->room_log=NULL;}
        /* Each room has its own folder, so the listing belongs to the room. */
        if(a->files){json_object_put(a->files);a->files=NULL;}a->files_path[0]=0;a->files_scroll=0;
        if(dock_has(a,"files"))request_files(a,"");}
    copy(a->focus,sizeof(a->focus),"room");a->full[0]=0;a->split=-1;a->dirty=1;
}
static void move_session(App *a,const char *id,const char *target){
    Session *s=session(a,id);if(!s||(*target&&!room_by_id(a,target)))return;
    if(!strcmp(s->room_id,target))return;
    if(a->demo){copy(s->room_id,sizeof(s->room_id),target);s->dock_pending=1;prune_workspace(a);if(in_workspace(a,s)&&terminal_tile(s)&&!dock_has(a,s->id))lj_dock_drop(&a->dock,s->id,"room",LJ_DOCK_LEFT);save_layout(a);toast(a,"Demo: session moved to room");}
    else {json_object *o=json_object_new_object();jadd(o,"action","room_move");jadd(o,"session",id);jadd(o,"room",target);request(a,o);}
    if(!strcmp(a->focus,id))copy(a->focus,sizeof(a->focus),"room");a->dirty=1;
}
static void collapse_room(App *a,const char *id){
    Room *r=room_by_id(a,id);if(!r)return;
    if(a->demo)r->collapsed=!r->collapsed;
    else {json_object *o=json_object_new_object();jadd(o,"action","room_update");jadd(o,"room",id);json_object_object_add(o,"collapsed",json_object_new_boolean(!r->collapsed));request(a,o);}
    if(!strcmp(a->active_room,id)){copy(a->focus,sizeof(a->focus),"room");a->full[0]=0;}a->dirty=1;
}
static void apply_snapshot(App *a,json_object *d){
    copy(a->tmux,sizeof(a->tmux),jstr(d,"tmux_binary"));copy(a->socket,sizeof(a->socket),jstr(d,"socket"));
    copy(a->sessiond_sock,sizeof(a->sessiond_sock),jstr(d,"sessiond_sock"));
    copy(a->mood,sizeof(a->mood),jstr(d,"mood"));
    if(a->status_ribbon)json_object_put(a->status_ribbon);a->status_ribbon=jget(d,"status_ribbon");if(a->status_ribbon)json_object_get(a->status_ribbon);
    if(jget(d,"room_log")&&!strcmp(jstr(jget(d,"room_log"),"room"),a->active_room)){if(a->room_log)json_object_put(a->room_log);a->room_log=json_object_get(jget(d,"room_log"));}
    if(a->review_panel)json_object_put(a->review_panel);a->review_panel=jget(d,"review_panel");if(a->review_panel)json_object_get(a->review_panel);
    if(a->dashboard)json_object_put(a->dashboard);a->dashboard=jget(d,"dashboard");if(a->dashboard)json_object_get(a->dashboard);
    if(jget(d,"files")){if(a->files)json_object_put(a->files);a->files=json_object_get(jget(d,"files"));
        copy(a->files_path,sizeof(a->files_path),jstr(a->files,"path"));a->files_scroll=0;}
    if(!a->layout_path[0]){copy(a->layout_path,sizeof(a->layout_path),jstr(d,"layout"));}
    json_object *rooms=jget(d,"rooms");a->nroom=0;
    size_t room_count=rooms?json_object_array_length(rooms):0;
    for(size_t i=0;i<room_count&&a->nroom<COUNT;i++){
        json_object *item=json_object_array_get_idx(rooms,i);const char *id=jstr(item,"id");int safe=!strncmp(id,"r-",2)&&strlen(id)<81;
        for(const char *p=id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))safe=0;
        if(!safe)continue;Room *r=&a->rooms[a->nroom++];copy(r->id,sizeof(r->id),id);copy(r->name,sizeof(r->name),jstr(item,"name"));r->collapsed=json_object_get_boolean(jget(item,"collapsed"));
        copy(r->state,sizeof(r->state),jstr(item,"state")[0]?jstr(item,"state"):"active");
        copy(r->phase,sizeof(r->phase),jstr(item,"phase")[0]?jstr(item,"phase"):"WORK");
        r->days_until_purge=json_object_get_int(jget(item,"days_until_purge"));
        {json_object *q=jget(item,"questions"),*w=jget(item,"awaiting_alignment");
         r->questions=q?(int)json_object_array_length(q):0;
         r->awaiting=w?(int)json_object_array_length(w):0;}
        /* ⚠ The todo list lives on the ROOM, not in the room log — reading it
         * from the wrong object is why ACTIONABLE said "not in this snapshot"
         * while the backend was sending the field all along. */
        if(!strcmp(id,a->active_room)){
            if(a->room_todos)json_object_put(a->room_todos);
            a->room_todos=jget(item,"todos");if(a->room_todos)json_object_get(a->room_todos);
            if(a->room_dms)json_object_put(a->room_dms);
            a->room_dms=jget(item,"dms");if(a->room_dms)json_object_get(a->room_dms);
        }
    }
    if(a->room_states)json_object_put(a->room_states);
    a->room_states=jget(d,"room_states");if(a->room_states)json_object_get(a->room_states);
    if(a->room_questions)json_object_put(a->room_questions);
    a->room_questions=jget(d,"room_questions");if(a->room_questions)json_object_get(a->room_questions);
    if(jget(d,"tasks")){if(a->all_tasks)json_object_put(a->all_tasks);
        a->all_tasks=json_object_get(jget(d,"tasks"));
        copy(a->tasks_error,sizeof(a->tasks_error),jstr(d,"tasks_error"));}
    json_object *rows=jget(d,"sessions");a->norder=0;
    for(int i=0;i<a->nsession;i++)a->sessions[i].seen=0;
    size_t count=rows?json_object_array_length(rows):0;
    /* Reserve all identities present in this snapshot before reusing stale slots. */
    for(size_t i=0;i<count;i++){Session *s=session(a,jstr(json_object_array_get_idx(rows,i),"id"));if(s)s->seen=1;}
    for(size_t i=0;i<count&&a->norder<COUNT;i++){
        json_object *row=json_object_array_get_idx(rows,i);const char *id=jstr(row,"id");if(!*id)continue;
        Session *s=session(a,id);
        if(!s){
            if(a->nsession<COUNT)s=&a->sessions[a->nsession++];
            else for(int k=0;k<a->nsession;k++){Session *candidate=&a->sessions[k];
                if(!candidate->seen&&!candidate->connected&&strcmp(candidate->id,a->selected)&&strcmp(candidate->id,a->focus)){s=candidate;terminal_close(s);break;}}
            if(!s){toast(a,"Session display is full; close unused sessions and reopen the workspace");continue;}
            memset(s,0,sizeof(*s));s->fd=-1;copy(s->id,sizeof(s->id),id);
        }
        s->seen=1;copy(s->agent,sizeof(s->agent),jstr(row,"agent"));copy(s->role,sizeof(s->role),jstr(row,"role"));
        copy(s->state,sizeof(s->state),jstr(row,"state"));copy(s->task,sizeof(s->task),jstr(row,"task"));
        copy(s->parent,sizeof(s->parent),jstr(row,"lead_id"));copy(s->name,sizeof(s->name),jstr(row,"tmux_name"));
        {const char *backend=jstr(row,"backend");
         s->native=backend[0]&&!strcmp(backend,"native");
         copy(s->daemon_id,sizeof(s->daemon_id),jstr(row,"daemon_id"));}
        /* ⚠ UNAVAILABLE IS NOT ENDED AND NOT IDLE. The heartbeat marks an agent
         * unavailable WITH A REASON — no live terminal, an API failure, no
         * credit — and stops retrying it. Drawn as merely idle it looks like
         * someone who might pick the work up, which is the opposite of true. */
        {json_object *u=jget(row,"unavailable");
         copy(s->unavailable,sizeof(s->unavailable),
              u?(json_object_is_type(u,json_type_string)?json_object_get_string(u):jstr(u,"reason")):"");}
        char previous_room[81];copy(previous_room,sizeof(previous_room),s->room_id);copy(s->room_id,sizeof(s->room_id),jstr(row,"room_id"));
        if(strcmp(previous_room,s->room_id))s->dock_pending=1;
        s->managed=s->name[0]!=0;
        if(!terminal_tile(s))s->dock_pending=0;
        if(in_workspace(a,s)&&terminal_tile(s)&&s->dock_pending&&(dock_has(a,s->id)||lj_dock_drop(&a->dock,s->id,"room",LJ_DOCK_LEFT)))s->dock_pending=0;a->order[a->norder++]=(int)(s-a->sessions);
    }
    /* ⚠ ACTIVE IN FRONT, CLOSED AT THE END (the operator, 2026-09-10). The order came
     * straight from the backend's row order, which is creation order, so every
     * agent ever closed stayed wherever it first appeared and pushed the live
     * ones down the list. The workspace keeps a session row FOREVER — measured
     * today: 41 rows, 6 running — so a fresh instance opened onto a tab strip
     * led by agents the operator had closed hours earlier, rendered red with "no live
     * terminal". Closing a tile hides it in THAT instance; it does not and
     * should not delete the record, so the ordering is what has to carry the
     * distinction.
     *
     * A STABLE PARTITION, not a sort: relative order inside each group is the
     * order the operator arranged, and re-sorting live agents among themselves would
     * shuffle a layout he set on purpose. */
    {
        int live[COUNT], gone[COUNT], nl = 0, ng = 0;
        for(int i=0;i<a->norder;i++){
            int idx=a->order[i];
            if(session_dead(&a->sessions[idx])) { if(ng<COUNT) gone[ng++]=idx; }
            else { if(nl<COUNT) live[nl++]=idx; }
        }
        for(int i=0;i<nl;i++) a->order[i]=live[i];
        for(int i=0;i<ng;i++) a->order[nl+i]=gone[i];
        a->norder=nl+ng;
    }
    if(a->messages)json_object_put(a->messages);a->messages=json_object_get(jget(d,"messages"));
    if(!a->nroom_view)load_workspace(a);
    if(a->active_room[0]&&!room_by_id(a,a->active_room))switch_workspace(a,"");
    prune_workspace(a);Session *focused=session(a,a->focus);if(focused&&!in_workspace(a,focused))copy(a->focus,sizeof(a->focus),"room");Room *active=room_by_id(a,a->active_room);if(active&&active->collapsed){copy(a->focus,sizeof(a->focus),"room");a->full[0]=0;}
    const char *created_room=jstr(d,"created_room");
    if(*created_room){a->room_starting=0;a->room_dialog=0;a->room_name[0]=0;switch_workspace(a,created_room);
        /* ⚠ A PARTIAL START IS NOT A SUCCESS. codex cannot roll back a spawned
         * process, so room_start reports what actually launched; saying nothing
         * would leave a half-built room looking finished. */
        const char *st=jstr(d,"start_status");
        if(*st&&strcmp(st,"complete")){
            json_object *errs=jget(d,"start_errors");
            char why[420];snprintf(why,sizeof(why),"Room %s · %s",st,
                errs&&json_object_array_length(errs)?jstr(json_object_array_get_idx(errs,0),"message"):"see Team for what launched");
            toast(a,why);
        } else toast(a,"Room created · agents launching with their roles and first todo");
    }
    const char *created=jstr(d,"created");
    if(*created){Session *s=session(a,created);if(terminal_tile(s)){switch_workspace(a,s->room_id);copy(a->selected,sizeof(a->selected),s->id);copy(a->focus,sizeof(a->focus),s->id);lj_dock_drop(&a->dock,s->id,"room",LJ_DOCK_LEFT);attach(a,s);save_layout(a);toast(a,"Session opened · drag its title to arrange it");}}
    /* Stop closes the tile only after the backend acknowledges success.
     * The historical session row remains available in Team. */
    if(!strcmp(jstr(d,"control_sent"),"stop")){
        Session *stopped=session(a,jstr(d,"target"));
        if(stopped){
            terminal_close(stopped);stopped->managed=0;stopped->dock_pending=0;
            lj_dock_remove(&a->dock,stopped->id);
            if(!strcmp(a->focus,stopped->id))copy(a->focus,sizeof(a->focus),"room");
            if(!strcmp(a->full,stopped->id))a->full[0]=0;
            save_layout(a);
        }
    }
    const char *stage=jstr(d,"stage");
    if(*stage){Session *s=session(a,jstr(d,"target"));if(s&&paste(a,s,stage,1)){copy(a->selected,sizeof(a->selected),s->id);copy(a->focus,sizeof(a->focus),s->id);char msg[180];snprintf(msg,sizeof(msg),"Staged in %s / %.8s · press Enter there to submit",s->agent,s->id+2);toast(a,msg);}}
    if(*jstr(d,"posted")){acknowledge_post(a);a->room_scroll=0;if(!*stage)toast(a,"Message saved in this conversation");}
    json_object *bridge=jget(d,"bridge"),*issues=bridge?jget(bridge,"issues"):NULL;
    if(issues&&json_object_array_length(issues)){json_object *issue=json_object_array_get_idx(issues,0);copy(a->toast,sizeof(a->toast),json_object_get_string(issue));}
    a->dirty=1;
}
static int backend_poll(App *a){
    if(a->backend_out<0)return 0;
    for(int k=0;k<32;k++){
        ssize_t n=read(a->backend_out,a->response+a->response_len,RESPONSE_CAP-a->response_len);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){close(a->backend_out);a->backend_out=-1;a->busy=0;a->start_inflight=a->start_pending=0;a->room_starting=0;toast(a,"Workspace helper exited · terminal sessions remain open");return 1;}
        a->response_len+=(size_t)n;
        char *end=memchr(a->response,'\n',a->response_len);
        if(end){*end=0;json_object *o=json_tokener_parse(a->response);a->busy=0;
            int was_start=a->start_inflight;a->start_inflight=0;
            if(!o){toast(a,"Invalid workspace response");if(was_start)a->room_starting=0;}
            else if(!json_object_get_boolean(jget(o,"ok"))){toast(a,jstr(o,"error"));if(was_start)a->room_starting=0;}   /* a failed START may be retried */
            else apply_snapshot(a,jget(o,"data"));
            if(o)json_object_put(o);size_t used=(size_t)(end-a->response)+1;a->response_len-=used;memmove(a->response,a->response+used,a->response_len);
            if(a->pending&&!a->busy&&a->backend_in>=0){char *next=a->pending;int start=a->start_pending;a->pending=NULL;a->start_pending=0;
                if(send_request(a,next))a->start_inflight=start;else if(start)a->room_starting=0;free(next);}
            return 1;
        }
        if(a->response_len==RESPONSE_CAP){toast(a,"Workspace response exceeded 8 MB limit");close(a->backend_out);a->backend_out=-1;a->busy=0;a->start_inflight=a->start_pending=0;a->room_starting=0;return 1;}
    }return 0;
}
/* Select the most specific visible separator at a snapped junction. */
static int divider_at(App *a,int x,int y){
    int best=-1;long long smallest=0;
    for(int i=0;i<a->dock.divider_count;i++){
        lj_rect r=hitrect(a->dock.dividers[i].rect);
        long long area=(long long)r.w*r.h;
        if(inside(r,x,y)&&(best<0||area<smallest)){best=i;smallest=area;}
    }
    return best;
}
static int divider_hot(App *a,int i){
    if(a->split>=0){
        for(int k=0;k<a->split_drag.count;k++)if(a->split_drag.node[k]==a->dock.dividers[i].node)return 1;
        return 0;
    }
    int hover=divider_at(a,a->mousex,a->mousey);
    return hover>=0&&a->dock.dividers[hover].group==a->dock.dividers[i].group;
}
static void dock_geometry(App *a,lj_dock *dock){
    /* Tabs replaced the sidebar, so the tiles start at the left margin. */
    int marg=ANSI_MARGIN;
    int dtop=ANSI_DOCK_TOP,foot=ANSI_FOOTER_H;
    /* UNIFORM WINDOW MARGIN (the operator, 2026-09-12 23:30 EDT: "I want a uniform margin
     * in lilJack"). The ANSI layout is inset one cell on every side, so a lead
     * tile's ring lives in that margin and in the divider cells, and no tile is
     * inset differently from its neighbours. The ring must never share a cell
     * with text (WezTerm erases sixel under rewritten text; owkTerm replaces text
     * under sixel — measured 2026-09-12). LILJACK_ANSI_MARGIN=0 restores the flush
     * layout for comparison. */
    if(!(getenv("LILJACK_ANSI_MARGIN")&&!strcmp(getenv("LILJACK_ANSI_MARGIN"),"0"))){marg+=CELLW;dtop+=CELLH;foot+=CELLH;}   /* both the TUI and the native window: one look */
    lj_dock_layout(dock,(lj_rect){marg,dtop,a->w-2*marg,a->h-dtop-foot});
    Room *r=room_by_id(a,a->active_room);
    if(r&&r->collapsed&&strcmp(a->full,"review")&&strcmp(a->full,"git")){dock->tile_count=1;copy(dock->tiles[0].id,sizeof(dock->tiles[0].id),"room");dock->tiles[0].rect=(lj_rect){marg,dtop,a->w-2*marg,a->h-dtop-foot};dock->divider_count=0;return;}
    /* ⚠ FULL SCREEN MEANS THE SCREEN. The old zoom only filled the dock area,
     * so a maximised terminal still gave three rows to tabs and two to the
     * footer. the operator asked for the tile to take the workspace "for a sec", so it
     * takes everything but one row of hint. */
    if(a->full[0]){dock->tile_count=1;copy(dock->tiles[0].id,sizeof(dock->tiles[0].id),a->full);
        dock->tiles[0].rect=(lj_rect){0,0,a->w,a->h-CELLH};
        dock->divider_count=0;}
    if(!a->ansi)return;
    for(int i=0;i<dock->tile_count;i++)dock->tiles[i].rect=cellround(dock->tiles[i].rect);
    /* ⚠ A SEPARATOR NEEDS A CELL OF ITS OWN. The dock's gap is at most 12px —
     * less than one cell — so 2 of every 10 pixel offsets left it with no
     * clickable cell centre at all (those are the dividers that refused to
     * drag), and widening its hit area instead made it share a cell with the
     * next tile's header, so grabbing the header resized the split. Reserving
     * one whole row/column and clipping the tiles out of it gives the drag a
     * target that belongs to nobody else. */
    for(int i=0;i<dock->divider_count;i++){
        lj_rect *b=&dock->dividers[i].rect;int horizontal=b->h<=b->w;
        if(horizontal){
            b->y=(b->y+b->h/2)/CELLH*CELLH;b->h=CELLH;
            int x0=(b->x+CELLW/2)/CELLW*CELLW;
            int x1=(b->x+b->w+CELLW/2)/CELLW*CELLW;
            if(x1<=x0)x1=x0+CELLW;
            b->x=x0;b->w=x1-x0;
        }
        else {b->x=(b->x+b->w/2)/CELLW*CELLW;b->w=CELLW;}
        for(int t=0;t<dock->tile_count;t++){
            lj_rect *r=&dock->tiles[t].rect;
            if(r->x>=b->x+b->w||b->x>=r->x+r->w||r->y>=b->y+b->h||b->y>=r->y+r->h)continue;
            if(horizontal){
                if(r->y<b->y)r->h=b->y-r->y;
                else {int shift=b->y+b->h-r->y;r->y+=shift;r->h-=shift;}
            } else {
                if(r->x<b->x)r->w=b->x-r->x;
                else {int shift=b->x+b->w-r->x;r->x+=shift;r->w-=shift;}
            }
        }
    }
}
static void geometry(App *a){ dock_geometry(a,&a->dock); }
/* Predict a drop without changing the live tree, focus, PTYs or persistence.
 * Both paths use the same dock layout and terminal-cell separator reservation. */
static int drop_preview(App *a,const char *target,lj_dock_edge edge,lj_rect *rect_out){
    lj_dock candidate=a->dock;
    if(!lj_dock_drop(&candidate,a->drag,target,edge))return 0;
    dock_geometry(a,&candidate);
    for(int i=0;i<candidate.tile_count;i++)if(!strcmp(candidate.tiles[i].id,a->drag)){
        *rect_out=candidate.tiles[i].rect;return 1;
    }
    return 0;
}
static void demo(App *a){
    const char *agents[]={"codex","claude","deepseek","shell"};
    const char *tasks[]={"Building your workspace","Model design & ecosystem","Critical review","Your terminal"};
    const char *seeds[]={
        "\033[1;32m🌿 lilJack\033[0m\r\n\r\nC · HUI · owkterm · tmux\r\n\r\n\033[32m✓\033[0m Your sessions, one workspace\r\n\033[32m✓\033[0m Drag, resize and retile\r\n\033[32m✓\033[0m Привіт, друже!\r\n\033[32m✓\033[0m こんにちは 🌿\r\n\r\n\033[36m→ Ready when you are.\033[0m\r\n\r\nDemo replay · no agents launched\r\n",
        "🧠 Claude · lead\r\n\r\nModel work lives in its own room.\r\n\r\nReviewing the HUI font fallback\r\nand the shared ecosystem.\r\n\r\n\033[32m✓\033[0m Boundaries before complexity\r\n",
        "🔎 DeepSeek · reviewer\r\n\r\nПитання: хто отримує ввід?\r\n\r\nChecking identity, reconnect,\r\nand hostile terminal sequences.\r\n\r\n\033[36m●\033[0m Review in progress\r\n",
        "🌿 Shell\r\n\r\nOpen the live app to run commands.\r\n"};
    for(int i=0;i<4;i++){Session *s=&a->sessions[i];s->fd=-1;s->seen=1;s->managed=1;s->cols=80;s->rows=24;
        snprintf(s->id,sizeof(s->id),"demo-%d",i);copy(s->agent,sizeof(s->agent),agents[i]);copy(s->task,sizeof(s->task),tasks[i]);copy(s->role,sizeof(s->role),i==1?"lead":i==2?"reviewer":"worker");copy(s->state,sizeof(s->state),"demo");s->vt=lj_vt_new(80,24);lj_vt_feed(s->vt,(const unsigned char*)seeds[i],(int)strlen(seeds[i]));a->order[i]=i;}
    a->nroom=2;copy(a->rooms[0].id,81,"r-demo-build");copy(a->rooms[0].name,161,"Build room");
    copy(a->rooms[1].id,81,"r-demo-research");copy(a->rooms[1].name,161,"Research");a->rooms[1].collapsed=1;
    for(int i=0;i<3;i++)copy(a->sessions[i].room_id,81,"r-demo-build");
    copy(a->active_room,81,"r-demo-build");copy(a->chat_dest,81,"r-demo-build");
    a->nsession=a->norder=4;copy(a->selected,sizeof(a->selected),"demo-0");copy(a->focus,sizeof(a->focus),"room");copy(a->mood,sizeof(a->mood),"curious");
    a->messages=json_object_new_array();const char *messages[]={"Let’s make this beautiful, useful and ours. ✨","Перевіряю сесії та маршрутизацію вводу.","こんにちは 🌿 Every voice keeps its language."};
    const char *senders[]={"operator","demo-2","demo-1"};const char *langs[]={"en","uk","ja"};
    for(int i=0;i<3;i++){json_object *m=json_object_new_object();jadd(m,"sender",senders[i]);jadd(m,"text",messages[i]);jadd(m,"language",langs[i]);jadd(m,"destination","r-demo-build");jadd(m,"created_at","2026-09-08T23:40:00");json_object_array_add(a->messages,m);}
    retile(a);toast(a,"✨ DEMO · visual fixtures only · ./liljack opens your live workspace");
}

/* UTF-8 walking is only for editor wrapping/copy; VT performs its own strict decode. */
static uint32_t nextcp(const char **p){
    const unsigned char *s=(const unsigned char*)*p;uint32_t c=*s++;int n=0;
    if(c>=0xc2&&c<=0xdf){c&=31;n=1;}else if(c>=0xe0&&c<=0xef){c&=15;n=2;}else if(c>=0xf0&&c<=0xf4){c&=7;n=3;}else if(c>=128)c=0xfffd;
    for(int i=0;i<n;i++){if((*s&0xc0)!=0x80){c=0xfffd;break;}c=(c<<6)|(*s++&63);}*p=(const char*)s;return c;
}
static int wrapped_lines(const char *s,int cols){int lines=1,n=0;while(*s){uint32_t c=nextcp(&s);int w=wcwidth((wchar_t)c);if(w<0)w=1;if(c=='\n'){lines++;n=0;}else {if(n+w>cols&&n){lines++;n=0;}n+=w;}}return lines;}
/* ⚠ A CLIPPED LABEL MUST SAY IT IS CLIPPED. text() clips at maxw, so a name
 * too long for its column simply lost its tail — "toolbox" rendered "toolbo"
 * and "ACTIONABLE" rendered "ACTIONA" (codex's files-800.png / ended-800.png,
 * 2026-09-10). A silently shortened name is not a cosmetic problem: it reads
 * as a DIFFERENT name, and in a file browser that is a lie about what is on
 * disk. Copy at most `cols` display columns and mark the cut with '…'.
 * Width is wcwidth, so a CJK name spends two columns per glyph as it must. */
/* The Files header label: identity + as much of the path as fits.
 *
 * ⚠ EXTRACTED SO A TEST CAN CALL THE REAL THING. It was inline in render(),
 * which left the only available test one that RE-DERIVED the same arithmetic —
 * and a test that recomputes the formula agrees with a wrong formula. That is
 * exactly how the border test passed against the bug it was written for. */
/* Header history shows eight completed display buckets. */
#define LJ_RIBBON_SPARK 8
/* ── the mark ─────────────────────────────────────────────────────────────
 * The header's logo is the app's OWN desktop icon (desktop/liljack.svg), drawn
 * here from its geometry rather than from a raster, so it is the same mark at
 * 24px in a terminal and 48px in a window and needs no asset on disk at run
 * time. Circles become squares below ~12px; that is the honest rendering at
 * that size, not a different picture.
 *
 * ⚠ ONE BUFFER, THREE PRESENTERS — the thin-HUI rule (§6) made structural:
 * the mark and the graphs are each rendered ONCE into ARGB, and then either
 * blitted (window), handed to lj_ansi_image (sixel), or replaced by glyphs
 * (plain ANSI). The paths cannot drift because there is one picture. */
static void argb_fill(uint32_t *buf,int bw,int bh,int x,int y,int w,int h,uint32_t rgb){
    if(w<1)w=1;if(h<1)h=1;
    for(int r=y;r<y+h&&r<bh;r++)for(int c=x;c<x+w&&c<bw;c++)if(r>=0&&c>=0)buf[r*bw+c]=0xff000000u|rgb;
}
static void mark_argb(uint32_t *buf,int s){
    for(int i=0;i<s*s;i++)buf[i]=0;                     /* transparent outside the body */
    /* ⚠ AT 20px THE SVG'S 6-UNIT INSET IS 6*20/128 = 0. Integer scaling ate
     * it, the body started at the corner, and the "transparent outside the
     * rounded body" promise below was false — the planted test caught it. The
     * inset and the corner radius are floored at one pixel, and the corners
     * are genuinely rounded (rx=18 in the icon) by clearing outside the arc. */
    #define MX(v) ((v)*s/128)
    int in=MX(6)>0?MX(6):1, rad=MX(18)>1?MX(18):2, body=s-2*in;
    argb_fill(buf,s,s,in,in,body,body,lj_theme_rgb(LJ_THEME_BG));                            /* body */
    for(int cy=0;cy<2;cy++)for(int cx=0;cx<2;cx++){                        /* round the four corners */
        int ox=cx?in+body-rad:in, oy=cy?in+body-rad:in;                    /* corner square origin */
        int ccx=cx?ox:ox+rad-1, ccy=cy?oy:oy+rad-1;                        /* arc centre */
        for(int y=oy;y<oy+rad;y++)for(int x=ox;x<ox+rad;x++){
            int dx=x-ccx,dy=y-ccy;
            if(dx*dx+dy*dy>rad*rad&&y>=0&&y<s&&x>=0&&x<s)buf[y*s+x]=0;
        }
    }
    argb_fill(buf,s,s,in+rad,in,body-2*rad,1,lj_theme_rgb(LJ_THEME_EDGE));                    /* border, between the arcs */
    argb_fill(buf,s,s,in+rad,in+body-1,body-2*rad,1,lj_theme_rgb(LJ_THEME_EDGE));
    argb_fill(buf,s,s,in,in+rad,1,body-2*rad,lj_theme_rgb(LJ_THEME_EDGE));
    argb_fill(buf,s,s,in+body-1,in+rad,1,body-2*rad,lj_theme_rgb(LJ_THEME_EDGE));
    argb_fill(buf,s,s,MX(23),MX(23),MX(10),MX(10),lj_theme_rgb(LJ_THEME_RED));               /* window dots */
    argb_fill(buf,s,s,MX(41),MX(23),MX(10),MX(10),lj_theme_rgb(LJ_THEME_AMBER));
    argb_fill(buf,s,s,MX(59),MX(23),MX(10),MX(10),lj_theme_rgb(LJ_THEME_GRAPH_LOW));
    argb_fill(buf,s,s,MX(20),MX(44),MX(88),MX(3),lj_theme_rgb(LJ_THEME_AMBER));                /* hairline rule */
    for(int r=0;r<18;r++){int half=r<9?r:17-r;                            /* prompt triangle */
        argb_fill(buf,s,s,MX(24),MX(58+r),MX(half*16/9),MX(1),lj_theme_rgb(LJ_THEME_AMBER));}
    argb_fill(buf,s,s,MX(52),MX(62),MX(52),MX(9),lj_theme_rgb(LJ_THEME_TEXT));                /* text bars */
    argb_fill(buf,s,s,MX(52),MX(80),MX(32),MX(9),lj_theme_rgb(LJ_THEME_CYAN));
    argb_fill(buf,s,s,MX(92),MX(80),MX(9),MX(17),lj_theme_rgb(LJ_THEME_AMBER));                /* block cursor */
    #undef MX
}
/* Window path: put an ARGB picture straight into the framebuffer. */
static void blit_argb(App *a,int x,int y,const uint32_t *buf,int bw,int bh){
    uint32_t *p=lj_render_pixels();if(!p)return;
    for(int r=0;r<bh;r++){int py=y+r;if(py<0||py>=a->h)continue;
        for(int c=0;c<bw;c++){int px=x+c;if(px<0||px>=a->w)continue;
            uint32_t v=buf[r*bw+c];if(v>>24)p[(size_t)py*a->w+px]=v;}}
}
/* A load graph: newest sample at the right, one bar per column, height by
 * value, colour by level. NAN draws NOTHING — a flat bar at 0% would read as an
 * idle machine, the exact lie c_metrics.h exists to prevent. */
static void plot_argb(uint32_t *buf,int w,int h,const float *hist,int n){
    for(int i=0;i<w*h;i++)buf[i]=0;
    for(int c=0;c<w;c++){
        int idx=n-w+c;if(idx<0||!hist)continue;
        float v=hist[idx];if(!isfinite(v))continue;
        if(v<0)v=0;if(v>1)v=1;
        int bh=(int)(v*(h-1)+0.5f);
        /* A finite idle sample has a baseline; only missing data is a gap.
         * Match the cell fallback's lowest block instead of rounding low
         * loads (below 2.63% at 20px) into invisible columns. */
        if(bh<1)bh=1;
        argb_fill(buf,w,h,c,h-bh,1,bh,v>0.85f?lj_theme_rgb(LJ_THEME_GRAPH_HIGH):v>0.6f?lj_theme_rgb(LJ_THEME_GRAPH_MID):lj_theme_rgb(LJ_THEME_GRAPH_LOW));
    }
}
/* hue 0..360, s/v 0..1 -> 0xRRGGBB. Used for the per-core wheel and the fixed
 * MEM/GPU part hues of the stacked header plots. */
static uint32_t hsv_rgb(float hue,float s,float v){
    while(hue<0)hue+=360;while(hue>=360)hue-=360;
    float c=v*s,x=c*(1-fabsf(fmodf(hue/60.f,2.f)-1)),m=v-c,r,g,b;
    if(hue<60){r=c;g=x;b=0;}else if(hue<120){r=x;g=c;b=0;}else if(hue<180){r=0;g=c;b=x;}
    else if(hue<240){r=0;g=x;b=c;}else if(hue<300){r=x;g=0;b=c;}else{r=c;g=0;b=x;}
    return (uint32_t)((r+m)*255+.5f)<<16|(uint32_t)((g+m)*255+.5f)<<8|(uint32_t)((b+m)*255+.5f);
}
/* A stable colour per CPU core: the HSV wheel over the core count, core 0 at
 * the bottom of the stack. */
static uint32_t core_rgb(int core,int cores){return hsv_rgb(cores>0?360.f*(float)core/(float)cores:0,.65f,.9f);}
/* Stacked load graph (the operator 2026-09-13: "height of a column is load, colour is
 * a core — same for GPU and memory: swap, RAM"). One column per bucket, newest
 * at the right. `hist` is the aggregate ring exactly as plot_argb takes it and
 * it stays THE authority for gap and height: NaN draws nothing, a finite value
 * is the base height (1px baseline when idle) — the NaN/outage semantics are
 * untouched. The parts only SPLIT that base: parts[0..nbase) share it in
 * proportion to their values (CPU: each core's busy/core_count, so a core's
 * segment is its busy share of the machine load; MEM: used and cached, which
 * add up to the MEM value), part 0 lowest. Parts from nbase on stack ON TOP
 * with their own height (MEM: swap; GPU: memory), total clamped to 1.
 * ⚠ Proportion, not the raw part value: the parts and the aggregate travel
 * through the same buckets but are smoothed separately, so drawing the parts
 * raw could put the stack's top a pixel off the number in the label — split
 * the aggregate instead and the sum is the load by construction. Parts all
 * NaN or zero (a source that has no split) leave one segment in colours[0].
 * Level colours (green/amber/red) are gone here on purpose: colour now says
 * WHICH core or WHICH kind of memory, not how hot. L1/L2 cache activity is
 * not a part: it is not observable without perf counters. */
static void plot_stacked_argb(uint32_t *buf,int w,int h,const float *hist,int n,
                              const float *const *parts,int nparts,int nbase,const uint32_t *colours){
    for(int i=0;i<w*h;i++)buf[i]=0;
    if(nbase>nparts)nbase=nparts;
    for(int c=0;c<w;c++){
        int idx=n-w+c;if(idx<0||!hist)continue;
        float v=hist[idx];if(!isfinite(v))continue;         /* a gap, never a zero */
        if(v<0)v=0;if(v>1)v=1;
        float share=0;for(int p=0;p<nbase;p++){float s=parts&&parts[p]?parts[p][idx]:NAN;if(isfinite(s)&&s>0)share+=s;}
        float total=v;for(int p=nbase;p<nparts;p++){float s=parts&&parts[p]?parts[p][idx]:NAN;if(isfinite(s)&&s>0)total+=s;}
        float scale=total>1?1/total:1;                       /* clamp the stack to 1 keeping the proportions */
        float cum=0;int y0=0;
        for(int p=0;p<nparts;p++){
            float s=parts&&parts[p]?parts[p][idx]:NAN;
            float seg=p<nbase?(share>0?v*(isfinite(s)&&s>0?s/share:0):(p==0?v:0)):(isfinite(s)&&s>0?s:0);
            if(seg<=0)continue;
            cum+=seg*scale;if(cum>1)cum=1;
            int y1=(int)(cum*(h-1)+0.5f);
            if(y1>y0){argb_fill(buf,w,h,c,h-y1,1,y1-y0,colours[p]);y0=y1;}
        }
        /* A finite idle sample has a baseline; match plot_argb (and the cell
         * fallback's lowest block) so low loads are never invisible columns. */
        if(y0<1)argb_fill(buf,w,h,c,h-1,1,1,colours[0]);
    }
}
/* All row-0 positions are cell aligned, shared by paint and hit layout. */
static int label_cells(const char *s);
typedef struct { int status_x, graphs_x, per, rung, ribbon_x, ribbon_w; } Header;
static Header header_layout(App *a){
    int cells=a->w/CELLW;
    int status=label_cells(a->mood[0]?a->mood:"ready")+(a->demo?7:0);
    if(status<16)status=16;
    if(status>cells-3)status=cells-3;
    if(status<0)status=0;
    Header h={0};h.status_x=(cells-status-1)*CELLW;
    h.ribbon_x=3*CELLW;
    int avail=h.status_x/CELLW-3;
    /* Opening reserves enough room for three actions and MORE at 80 cells. */
    int reserve=(a->ribbon_open||a->ribbon>0)?26:0;
    h.rung=avail>=54+reserve?2:avail>=30?1:0;
    h.per=h.rung==2?18:10;
    h.graphs_x=h.status_x-(h.rung?3*h.per*CELLW:0);
    h.ribbon_w=h.graphs_x-h.ribbon_x;
    if(h.ribbon_w<0)h.ribbon_w=0;
    return h;
}
static const char *files_header(char *dst,size_t cap,const char *full,int path_w){
    const char *ident=path_w>8?"FILES ":"F:";
    int idw=(int)strlen(ident);                        /* ASCII: bytes == cells */
    if(path_w<=idw){if(cap)dst[0]=0;return dst;}       /* UP/close carry the tile */
    int room=path_w-idw;
    const char *shown=full;int elided=0;
    /* ⚠ One column left is still a case: guarding on room>1 emitted the WHOLE
     * path unelided and ran it through UP and the close button. A bare … is the
     * honest rendering of "there is a path and none of it fits". */
    if(room>=1&&(int)strlen(full)>room){
        shown=full+strlen(full)-(room-1);              /* one column for the … */
        elided=1;
    }
    snprintf(dst,cap,"%s%s%s",ident,elided?lj_theme_glyph(LJ_THEME_ELLIPSIS):"",shown);
    return dst;
}
static const char *fit(char *dst,size_t cap,const char *src,int cols){
    if(cols<1){if(cap)dst[0]=0;return dst;}
    /* Width from the renderer's table (lj_render_cells_measure: the icon set is
     * two cells, 🛠 included), the same measure as label_cells — fit() used glibc
     * wcwidth, so "🛠 codex" measured 7 here and 8 there and a 7-column field
     * lost its last letter instead of eliding (status-elide, 2026-09-13). */
    int total=0;for(const char *q=src;*q;){uint32_t c=nextcp(&q);int w=lj_render_cells_measure(c);if(w<0)w=1;total+=w;}
    if(total<=cols){snprintf(dst,cap,"%s",src);return dst;}
    int budget=cols-1;                       /* one column for the ellipsis */
    size_t len=0;int used=0;const char *q=src;
    while(*q){const char *start=q;uint32_t c=nextcp(&q);int w=lj_render_cells_measure(c);if(w<0)w=1;
        if(used+w>budget)break;
        size_t z=(size_t)(q-start);if(len+z+4>=cap)break;
        memcpy(dst+len,start,z);len+=z;used+=w;}
    snprintf(dst+len,cap-len,lj_theme_glyph(LJ_THEME_ELLIPSIS));
    return dst;
}
static void wrapped(int x,int y,int width,int top,int bottom,const char *s,uint32_t color){
    int cols=width/CELLW,n=0;char line[4096];size_t len=0;if(cols<1)return;
    while(*s){const char *start=s;uint32_t c=nextcp(&s);int w=wcwidth((wchar_t)c);if(w<0)w=1;
        if(c=='\n'||(n+w>cols&&n)||len+(size_t)(s-start)>=sizeof(line)){
            line[len]=0;if(y>=top&&y+CELLH<=bottom)text(x,y,line,color,width);y+=CELLH;len=0;n=0;if(c=='\n')continue;
        }size_t z=(size_t)(s-start);if(len+z<sizeof(line)){memcpy(line+len,start,z);len+=z;}n+=w;
    }line[len]=0;if(y>=top&&y+CELLH<=bottom)text(x,y,line,color,width);
}
/* Fields in tab order. AGENTS is a row of four cycling badges, not text. */
enum {RF_NAME=0,RF_FOLDER,RF_PURPOSE,RF_TODO0,RF_TODO1,RF_TODO2,RF_COUNT};
static const char *ROLE_NAME[4]={"","worker","reviewer","lead"};
static char *room_field_buf(App *a,int field,size_t *cap){
    switch(field){
        case RF_NAME:    if(cap)*cap=sizeof(a->room_name);    return a->room_name;
        case RF_FOLDER:  if(cap)*cap=sizeof(a->room_folder);  return a->room_folder;
        case RF_PURPOSE: if(cap)*cap=sizeof(a->room_purpose); return a->room_purpose;
        default:         if(cap)*cap=sizeof(a->room_todo[0]); return a->room_todo[clamp(field-RF_TODO0,0,2)];
    }
}
static int room_lead_count(App *a){int n=0;for(int i=0;i<4;i++)if(a->room_agent[i]==3)n++;return n;}
/* Folders sort the way a person scans them: case-folded, then byte order. */
static int room_folder_compare(const void *x,const void *y){
    const unsigned char *p=x,*q=y;
    for(;*p&&*q;p++,q++){int d=tolower(*p)-tolower(*q);if(d)return d;}
    if(*p||*q)return *p?1:-1;
    return strcmp(x,y);
}
static void room_folder_read(App *a){
    char key[520];snprintf(key,sizeof key,"%d%s",a->fb_hidden?1:0,a->room_folder_browse);
    if(!strcmp(a->room_folder_listed,key))return;
    copy(a->room_folder_listed,sizeof(a->room_folder_listed),key);
    a->room_folder_count=a->room_folder_truncated=0;
    a->room_folder_error[0]=a->room_folder_base[0]=0;
    char resolved[PATH_MAX];
    if(!realpath(a->room_folder_browse,resolved)){
        snprintf(a->room_folder_error,sizeof(a->room_folder_error),"Cannot browse: %s",strerror(errno));return;
    }
    if(strlen(resolved)>=sizeof(a->room_folder_base)){
        copy(a->room_folder_error,sizeof(a->room_folder_error),"Folder path exceeds 511 bytes");return;
    }
    DIR *dir=opendir(resolved);
    if(!dir){snprintf(a->room_folder_error,sizeof(a->room_folder_error),"Cannot browse: %s",strerror(errno));return;}
    copy(a->room_folder_base,sizeof(a->room_folder_base),resolved);
    struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(!a->fb_hidden&&entry->d_name[0]=='.')continue;
        char path[PATH_MAX];struct stat st;
        int n=snprintf(path,sizeof(path),"%s/%s",resolved,entry->d_name);
        if(n<0||(size_t)n>=sizeof(path)||stat(path,&st)||!S_ISDIR(st.st_mode))continue;
        if(a->room_folder_count==500){a->room_folder_truncated=1;break;}
        copy(a->room_folder_entries[a->room_folder_count++],256,entry->d_name);
    }
    closedir(dir);
    qsort(a->room_folder_entries,a->room_folder_count,sizeof(a->room_folder_entries[0]),room_folder_compare);
    a->fb_sel=clamp(a->fb_sel,0,a->room_folder_count?a->room_folder_count-1:0);
}
/* Go to `path`: absolute, "~" or "~/…", or relative to the folder on show.
 * Going UP re-selects the folder you came out of, so Left, Left, Right is a
 * round trip. Returns 0 (with a toast, nothing changed) for a non-folder. */
static int room_folder_go_path(App *a,const char *path){
    char expanded[PATH_MAX],resolved[PATH_MAX];struct stat st;
    if(path[0]=='~'&&(!path[1]||path[1]=='/')){const char *home=getenv("HOME");snprintf(expanded,sizeof expanded,"%s%s",home?home:"",path+1);}
    else if(path[0]=='/'||!a->room_folder_base[0])snprintf(expanded,sizeof expanded,"%s",path);
    else snprintf(expanded,sizeof expanded,"%s/%s",a->room_folder_base,path);
    if(!realpath(expanded,resolved)||stat(resolved,&st)||!S_ISDIR(st.st_mode)){
        toast(a,"Not a folder you can open");a->room_folder_listed[0]=0;return 0;
    }
    if(strlen(resolved)>=sizeof(a->room_folder_browse)){toast(a,"Folder path exceeds 511 bytes");return 0;}
    char came[256]="";const char *old=a->room_folder_base;size_t rl=strlen(resolved);
    if(strlen(old)>rl&&!strncmp(old,resolved,rl)&&(rl==1||old[rl]=='/')){
        const char *rest=old+(rl==1?1:rl+1);size_t n=strcspn(rest,"/");
        if(n<sizeof came){memcpy(came,rest,n);came[n]=0;}
    }
    copy(a->room_folder_browse,sizeof(a->room_folder_browse),resolved);
    a->room_folder_listed[0]=0;a->fb_sel=a->fb_scroll=0;a->fb_click=-1;a->fb_follow=1;room_folder_read(a);
    if(came[0])for(int i=0;i<a->room_folder_count;i++)if(!strcmp(a->room_folder_entries[i],came)){a->fb_sel=i;break;}
    a->dirty=1;return 1;
}
static void room_folder_go(App *a,const char *name){char copy_of[256];copy(copy_of,sizeof copy_of,name);room_folder_go_path(a,copy_of);}
static int folder_browser_text(App *a,const char *value);
static void room_dialog_open(App *a){
    a->room_dialog=1;a->room_field=RF_NAME;a->team=a->status_open=0;
    a->room_name[0]=a->room_purpose[0]=0;
    for(int i=0;i<3;i++)a->room_todo[i][0]=0;
    /* Default the folder to the project: the common case, and a real directory
     * so a hurried Create cannot strand half a room on a typo. */
    copy(a->room_folder,sizeof(a->room_folder),a->project);
    a->room_folder_listed[0]=0;a->fb_open=a->fb_edit=0;
    /* One lead, because a room with none has nobody who owns all the todos. */
    a->room_agent[0]=3;a->room_agent[1]=1;a->room_agent[2]=2;a->room_agent[3]=0;
}
static void room_name_input(App *a,const char *value){
    if(folder_browser_text(a,value))return;
    for(const unsigned char *p=(const unsigned char*)value;*p;p++)if(*p<32||*p==127){toast(a,"This field stays on one line");return;}
    size_t cap=0;char *buf=room_field_buf(a,a->room_field,&cap);
    size_t have=strlen(buf),n=strlen(value);
    if(have+n>=cap){toast(a,"That field is full");return;}
    memcpy(buf+have,value,n+1);a->dirty=1;
}
static void room_field_backspace(App *a){
    char *buf=room_field_buf(a,a->room_field,NULL);size_t n=strlen(buf);
    if(!n)return;
    do{n--;}while(n&&((unsigned char)buf[n]&0xc0)==0x80);
    buf[n]=0;a->dirty=1;
}
static void create_room(App *a){
    const char *name=a->room_name;while(*name==' ')name++;
    if(!*name){toast(a,"Give the room a name");return;}
    if(!a->room_folder[0]){toast(a,"Give the room a working folder");return;}
    if(room_lead_count(a)!=1){toast(a,"Pick exactly one lead — the lead owns every todo in the room");return;}
    if(a->demo){if(a->nroom>=COUNT){toast(a,"Room limit reached");return;}Room *r=&a->rooms[a->nroom++];memset(r,0,sizeof(*r));snprintf(r->id,sizeof(r->id),"r-demo-%d",a->nroom);copy(r->name,sizeof(r->name),name);a->room_dialog=0;switch_workspace(a,r->id);toast(a,"Demo room created · drag agents onto its title");}
    else {
        /* ⚠ ONE ROOM PER CREATE. the operator, 2026-09-16: "chat room creates 2 times".
         * A second CREATE (or Enter + a click) before the reply was queued by
         * request() and sent after it, so the DB holds removed twins of "lilJack's
         * debug" and "ring dashboard rewiew" with their own sessions and todo
         * rows. Hold further starts until the reply, an error, or 2 minutes. */
        Uint32 now=SDL_GetTicks();   /* |1 marks "set" even at tick 0, so now may trail it by one */
        if(a->room_starting&&(now<a->room_starting||now-a->room_starting<120000u)){toast(a,"Creating the room · one moment");return;}
        a->room_starting=0;
        /* room_start is codex's action: it creates the room, launches every
         * agent with its role in one go, seeds the todo list and sends each
         * agent its intro. room_create (name only) still exists for an empty
         * room; this path deliberately does not use it. */
        json_object *o=json_object_new_object();
        jadd(o,"action","room_start");jadd(o,"name",name);
        jadd(o,"folder",a->room_folder);jadd(o,"purpose",a->room_purpose);
        json_object *agents=json_object_new_array();
        const char *kinds[4]={"claude","codex","deepseek","shell"};
        /* Lead first: codex launches in array order and a worker needs its lead
         * to exist before it can be parented. */
        for(int pass=3;pass>=1;pass--)for(int i=0;i<4;i++){
            if(a->room_agent[i]!=pass)continue;
            json_object *one=json_object_new_object();
            jadd(one,"agent",kinds[i]);jadd(one,"role",ROLE_NAME[pass]);
            json_object_array_add(agents,one);
        }
        json_object_object_add(o,"agents",agents);
        json_object *todo=json_object_new_array();
        for(int i=0;i<3;i++){const char *t=a->room_todo[i];while(*t==' ')t++;
            if(!*t)continue;json_object *item=json_object_new_object();
            jadd(item,"title",t);json_object_array_add(todo,item);}
        json_object_object_add(o,"todo",todo);
        char rid[64];snprintf(rid,sizeof(rid),"%08x%08x",(unsigned)time(NULL),(unsigned)a->rendered);
        jadd(o,"request_id",rid);
        if(request(a,o))a->room_starting=SDL_GetTicks()|1;
    }
}
static int group_count(App *a,const char *id){int n=0;for(int i=0;i<a->norder;i++)if(!strcmp(a->sessions[a->order[i]].room_id,id))n++;return n;}
/* ⚠ ROOMS ARE TABS, NOT A SIDE PANEL. the operator, 2026-09-09: "room folder selector
 * and file explorer, rooms are browser tabs not side panel so we have more
 * space for terminals". The old list cost a 19-to-24 column column down the
 * whole height — on a 170x45 terminal that is a thousand cells of chrome that
 * the terminals wanted. Tabs cost two rows and the tiles get the full width.
 * The hit KINDS are unchanged (H_GROUP for a room, H_SESSION for an agent,
 * H_CREATE to spawn), so drag-into-room and click-to-tile behave as before and
 * the native tests still address the same targets. */
static int label_cells(const char *sess){int n=0;for(const char *q=sess;*q;)n+=lj_render_cells_measure(nextcp(&q));return n;}
static void badge(App *a,lj_rect r,const char *sess,uint32_t fg,uint32_t bg,int kind,int index,const char *id){
    /* ⚠ ONE HOVER RULE FOR THE WHOLE UI (the operator: "hover over tabs that persist
     * in all UI"). Every clickable thing goes through badge() or button(), so
     * putting the reaction here means a new control cannot forget to have one.
     * The lift is a surface step, never a colour change — a control must not
     * look SELECTED just because the pointer crossed it. */
    if(kind&&hovered(a,kind,index,id)&&bg!=GREEN)bg=SURF3;
    panel(r,bg,0);text(r.x+CELLW/2,r.y,sess,fg,r.w-CELLW);
    if(kind)hit(a,r,kind,index,id);
}
/* Reserve by identity and field bounds, never by the current status. The
 * measured templates cover every form before a snapshot can change it.
 * ⚠ TABS ARE THEIR LABEL, NOT A RESERVATION. the operator, 2026-09-16: "this one liner
 * is too large and takes sooo much space in its tab". The slot used to reserve
 * " ·" plus every byte of Room.phase (13 cells) against " ?2147483647", so
 * "Build room 3" was a 29-cell tab and Research fell behind "+1" at 800 px.
 * The status is ONE mark in a 2-cell slot, right-aligned: an amber "?" when the
 * lead has open questions, else the phase initial (A ALIGN, P PLAN, R REVIEW,
 * ✓ RESOLVED; WORK shows nothing). the operator, 2026-09-17, "fix gap": the earlier
 * 6-cell slot (" ·ALGN", " ?99+") left ~7 blank cells after every quiet tab.
 * The question count and the phase word live in the room header and the rooms
 * menu. Width still depends on identity alone. */
#define TAB_STATUS_CELLS 2      /* one gap cell + one mark */
#define TAB_NAME_MIN_CELLS 8    /* a shrunk name keeps 7 cells and the ellipsis    */
static void room_tab_status(const Room *r,char *out,size_t cap){
    out[0]=0;if(!r)return;
    if(r->questions>0){snprintf(out,cap,"?");return;}
    if(!r->phase[0]||!strcmp(r->phase,"WORK"))return;
    if(!strcmp(r->phase,"RESOLVED")){snprintf(out,cap,"%s",lj_theme_glyph(LJ_THEME_CHECK));return;}
    snprintf(out,cap,"%c",toupper((unsigned char)r->phase[0]));
}
static int room_tab_name_cells(const Room *r){return label_cells(r?r->name:"STANDALONE");}
/* The whole tab in cells, both pads included, with the name held to `cap` cells
 * (cap<0: the full name). */
static int room_tab_cells(const Room *r,int cap){
    int name=room_tab_name_cells(r);if(cap>=0&&name>cap)name=cap;
    char count[16];snprintf(count,sizeof count," %d",COUNT);
    return 1+label_cells(r?lj_theme_glyph(LJ_THEME_ROOM):lj_theme_glyph(LJ_THEME_STANDALONE))+1+name+label_cells(count)+(r?TAB_STATUS_CELLS:0)+1;
}
static int room_tab_slot_cells(const Room *r){return room_tab_cells(r,-1)-2;}
static int agent_chip_slot_cells(const Session *s){
    char widest[256];
    /* %.14s below is at most fourteen bytes/cells; ASCII fills that bound. */
    snprintf(widest,sizeof widest,"%s %s %s12345678901234",icon(s->agent),s->agent,lj_theme_glyph(LJ_THEME_WARNING));
    return label_cells(widest);
}
/* Flat strip primitives also paint the native backing pixels used by sixel.
 * Cell text remains the fallback; thin rules never replace a text glyph. */
static void tab_fill(lj_rect r,uint32_t color){rect(r,color);if(render_ansi)lj_render_rect(r.x,r.y,r.w,r.h,color);}
static void tab_text(int x,int y,const char *label,uint32_t color,int width){
    text(x,y,label,color,width);
    if(render_ansi){render_ansi=0;text(x,y,label,color,width);render_ansi=1;}
}
static void tab_rule(lj_rect r,uint32_t color){lj_render_rect(r.x,r.y,r.w,r.h,color);}
static void tab_slot(App *a,lj_rect r,const char *label,int active,uint32_t color,int kind,const char *id,const char *status,uint32_t scolor){
    /* WEIGHT, NOT A LINE (visual-polish-chrome item 1): a strip cannot share a
     * cell with the label, so on the terminal the active tab is the raised
     * surface + bright text; the window keeps its 2 px accent on top of that. */
    uint32_t bg=hovered(a,kind,0,id)?SURF3:PANEL;
    tab_fill(r,bg);
    /* terminal only: the active tab's CELLS get the raised surface; the pixel
     * mirror keeps the window design (accent on top, PANEL below, joined to
     * the content), which is what tests/test_liljack_tabs.c asserts. */
    if(active&&render_ansi&&!hovered(a,kind,0,id))rect(r,SURF2);
    if(active)tab_rule((lj_rect){r.x,r.y,r.w,2},CYAN);
    else tab_rule((lj_rect){r.x,r.y+r.h-1,r.w,1},EDGE);
    int scells=status&&*status?label_cells(status):0;
    tab_text(r.x+CELLW,r.y,label,color,r.w-2*CELLW-(scells?scells+1:0)*CELLW);
    if(scells)tab_text(r.x+r.w-(scells+1)*CELLW,r.y,status,scolor,scells*CELLW);
    hit(a,r,kind,0,id);
}
static void tab_action(App *a,lj_rect r,const char *label,int kind){
    tab_fill(r,hovered(a,kind,0,"")?SURF3:SURF2);
    tab_text(r.x+CELLW/2,r.y,label,CYAN,r.w-CELLW);hit(a,r,kind,0,"");
}
/* ⚠ THERE IS NO tab_present ANY MORE. It copied lilJack's own 10x20-cell pixel
 * canvas of the strip and shipped it as a sixel image over the cells, so on a
 * host with 17x39 cells the labels were a stretched, sixel-quantised raster —
 * the operator: "tile and menu tab fonts are scaled and blurry"; and with a menu open
 * (when it skipped itself) the text was suddenly crisp. Tabs, the agent bar
 * and OPEN HERE are CELLS rendered by the host font. Sixel is for pictures. */
/* Row of room tabs on the left, the view menu on the right. */
static void draw_tabs(App *a,lj_rect bar){
    char label[256];
    tab_fill(bar,PANEL);tab_rule((lj_rect){bar.x,bar.y+bar.h-1,bar.w,1},EDGE);
    /* ⚠ THIS ROW IS THE TABS' AT EVERY WIDTH. It used to be shared: the toolbar
     * drew eight badges plus QUIT inline while it fitted and collapsed to a
     * burger only below ~28 cells of slack, so the two competed for the row at
     * every size — at 800x560 the strip had truncated to a single tab
     * ("Build r") with Research and + ROOM gone entirely. A room you cannot see
     * is a room you cannot open. The nine moved into the ribbon (thin-HUI §2),
     * which handed this row the ~64 cells the toolbar was taking, and there is
     * no width at which anything but the burger shares it.
     *
     * The burger used to keep four cells at the far right; thin-HUI §1 moved
     * it up into the header beside the mark and the graphs, so the row is now
     * the tabs' alone. */
    /* The burger moved up into the header (thin-HUI §1); nothing but tabs
     * shares this row any more, so its right edge is the row's right edge. */
    /* Fixed status slots must not consume the room-creation control. */
    int tabs_right=bar.x+bar.w-8*CELLW;
    /* ⚠ THE LIFECYCLE IS A FILTER, NOT SIX MORE TABS. the operator named six states
     * and one of them is a 30-day trash; showing them all at once would bury
     * the rooms actually being worked. So the strip shows ONE state at a time
     * and this badge says which, with its count, and opens the rest. */
    const char *filt=a->room_filter[0]?a->room_filter:"active";
    /* ⚠ NO COUNT IS NOT A COUNT OF ZERO. Without the backend's tally (demo, or
     * a helper that has not answered yet) the badge shows no number at all
     * rather than a 0 sitting next to visible tabs. */
    json_object *fv=a->room_states?jget(a->room_states,filt):NULL;
    if(fv)snprintf(label,sizeof(label),"%s %d %s",filt,json_object_get_int(fv),lj_theme_glyph(LJ_THEME_DOWN));
    else snprintf(label,sizeof(label),"%s %s",filt,lj_theme_glyph(LJ_THEME_DOWN));
    for(char *q=label;*q&&*q!=' ';q++)*q=(char)toupper((unsigned char)*q);
    int fw=(label_cells(label)+2)*CELLW;
    /* Draw the action in both the cell and pixel surfaces. */
    tab_action(a,(lj_rect){bar.x,bar.y,fw,CELLH},label,H_STATE_FILTER);
    int x=bar.x+fw;
    /* ⚠ SHRINK BEFORE HIDE, THEN WHOLE OR NOT AT ALL (item 1 + the operator
     * 2026-09-16 "autoresize"). Pass 1 finds the widest name cap every
     * admitted tab can share and still fit; names longer than the cap end in
     * the ellipsis, never a chop mid-word ("Rese"). Only when even
     * TAB_NAME_MIN_CELLS does not fit do rooms go behind the "+N ▾" badge
     * (MENU_ROOMS, which lists full names). The cap is a function of identity
     * and bar width only, so a phase or question flip never moves a tab. */
    int ncell[COUNT+1],fixed[COUNT+1],maxname=0;
    for(int g=-1;g<a->nroom;g++){Room *r=g<0?NULL:&a->rooms[g];
        if(r&&strcmp(r->state[0]?r->state:"active",filt))continue;
        ncell[g+1]=room_tab_name_cells(r);fixed[g+1]=room_tab_cells(r,0);if(ncell[g+1]>maxname)maxname=ncell[g+1];}
    int cap=maxname,need=0;
    for(;;){need=0;
        for(int g=-1;g<a->nroom;g++){Room *r=g<0?NULL:&a->rooms[g];if(r&&strcmp(r->state[0]?r->state:"active",filt))continue;
            need+=(fixed[g+1]+(ncell[g+1]<cap?ncell[g+1]:cap))*CELLW;}
        if(x+need<=tabs_right||cap<=TAB_NAME_MIN_CELLS)break;
        cap--;}
    int badge_cells=lj_theme_int(LJ_THEME_TAB_BADGE_MIN_CELLS);if(badge_cells<4)badge_cells=6;   /* theme: tab.badge_min_cells */
    int overflow=x+need>tabs_right,limit=overflow?tabs_right-badge_cells*CELLW:tabs_right,hidden=0;
    a->tab_name_cap=cap;
    for(int g=-1;g<a->nroom;g++){
        Room *r=g<0?NULL:&a->rooms[g];const char *id=r?r->id:"";
        /* Standalone is not a room and has no state, so it is always present. */
        if(r&&strcmp(r->state[0]?r->state:"active",filt))continue;
        int active=!strcmp(id,a->active_room),count=group_count(a,id);
        /* ⚠ THE PHASE IS THE RULE MADE VISIBLE. A room in ALIGN is deliberately
         * NOT working — agents acknowledge and wait — and a room with an open
         * question is stalled on its lead, not on its workers. Neither is
         * distinguishable from "quiet" unless the tab says so: the status slot
         * at the tab's right edge, amber for a question. */
        char name[256],status[32];fit(name,sizeof name,r?r->name:"STANDALONE",cap);room_tab_status(r,status,sizeof status);
        snprintf(label,sizeof(label),"%s %s %d",r?lj_theme_glyph(LJ_THEME_ROOM):lj_theme_glyph(LJ_THEME_STANDALONE),name,count);
        int w=room_tab_cells(r,cap)*CELLW;
        if(hidden||x+w>limit){hidden++;continue;}          /* order kept: once one is hidden, the rest are */
        lj_rect tab={x,bar.y,w,CELLH};
        tab_slot(a,tab,label,active,active?TEXT:DIM,H_GROUP,id,status,r&&r->questions>0?AMBER:CYAN);
        if(a->dragging&&session(a,a->drag)&&inside(tab,a->mousex,a->mousey))border(tab,CYAN);
        x+=w;
    }
    if(hidden){
        snprintf(label,sizeof(label),"+%d %s",hidden,lj_theme_glyph(LJ_THEME_DOWN));
        tab_action(a,(lj_rect){limit,bar.y,badge_cells*CELLW,CELLH},label,H_ROOM_MORE);
    }
    /* ⚠ + ROOM STAYS IN THE ROW, not in the burger. It is how you make the
     * thing this row is a list of, so it belongs beside the list; and with the
     * toolbar gone there is room for it at every supported width, which is why
     * it did not need to move into the ribbon to survive.
     * TODO(menu): a top-level ribbon item that needs a place in THIS row instead
     * of the ribbon attaches here — the rule is whether it acts on the tab list. */
    if(bar.w>=8*CELLW)tab_action(a,(lj_rect){tabs_right+CELLW,bar.y,7*CELLW,CELLH},"+ ROOM",H_NEW_ROOM);
}
/* Row of agent chips for the open room, with the spawn buttons on the right. */
static void draw_agentbar(App *a,lj_rect bar){
    char label[256];
    tab_fill(bar,PANEL);tab_rule((lj_rect){bar.x,bar.y+bar.h-1,bar.w,1},EDGE);
    int spawnx=bar.x+bar.w-12*CELLW,chipmax=spawnx-4*CELLW;
    tab_action(a,(lj_rect){spawnx,bar.y,12*CELLW,CELLH},"OPEN HERE ▾",H_OPEN_HERE);
    int members=group_count(a,a->active_room);
    int share=members?((chipmax-bar.x)/CELLW)/members:0;
    int x=bar.x,hidden=0;
    for(int i=0;i<a->norder;i++){
        Session *agent=&a->sessions[a->order[i]];
        if(strcmp(agent->room_id,a->active_room))continue;
        int selected=!strcmp(a->selected,agent->id);
        /* live vs idle is the distinction that matters: an observed session is
         * one the app cannot type into and must not look interactive. */
        
        const char *role=agent->role[0]?agent->role:"worker";
        int dead=session_dead(agent);
        /* Ended agents keep the compact grey glyph form inside the same slot;
         * a snapshot must not move every following chip when one agent exits. */
        if(agent->unavailable[0])
            snprintf(label,sizeof(label),"%s %s %s%.14s",icon(agent->agent),agent->agent,lj_theme_glyph(LJ_THEME_WARNING),agent->unavailable);
        else if(dead)snprintf(label,sizeof(label),"%s ·",icon(agent->agent));
        else snprintf(label,sizeof(label),"%s %s·%s",icon(agent->agent),agent->agent,
                 !strcmp(role,"lead")?"LEAD":!strcmp(role,"reviewer")?"REV":"WRK");
        int w=(agent_chip_slot_cells(agent)+2)*CELLW;
        /* Bound the natural widest form by this roster's viewport budget.
         * Keep enough for the identity/longest role; only status detail is
         * elided in crowded strips. Neither budget depends on live state. */
        char normal[128];snprintf(normal,sizeof normal,"%s %s·LEAD",icon(agent->agent),agent->agent);
        int minimum=label_cells(normal)+2,limit=share>minimum?share:minimum;
        if(w>limit*CELLW)w=limit*CELLW;
        if(x+w>chipmax){hidden++;continue;}   /* ⚠ never draw under OPEN HERE */
        char shown[256];fit(shown,sizeof shown,label,w/CELLW-2);
        tab_slot(a,(lj_rect){x,bar.y,w,CELLH},shown,selected,
                 agent->unavailable[0]?RED:dead?lj_theme_rgb(LJ_THEME_ENDED):selected?TEXT:DIM,H_SESSION,agent->id,NULL,0);
        x+=w;
    }
    if(!members)tab_text(bar.x,bar.y,"no agents · OPEN HERE to start",DIM,chipmax-bar.x);
    if(hidden){snprintf(label,sizeof(label),"+%d",hidden);tab_action(a,(lj_rect){chipmax,bar.y,4*CELLW,CELLH},label,H_TEAM);}
}
static void menu_open(App *a,int kind,lj_rect at,const char *target){
    a->menu_kind=kind;a->menu_at=at;copy(a->menu_target,sizeof(a->menu_target),target);a->menu_sel=-1;
    /* ⚠ THE TWO OVERLAYS ARE EXCLUSIVE. A dropdown opened while the ribbon was
     * out (state filter, ROLE, a room's right-click menu) left the ribbon lying
     * across the tiles under the flyout — the reverse direction was handled and
     * this one was not (deepseek review, tui-visual-tabs-menus F3). The one
     * dropdowns that belong to the ribbon (FILES and MORE) re-assert ribbon_open
     * right after this call, so the parent stays out under its own child. */
    a->ribbon_open=0;
    a->team=a->status_open=0;a->dirty=1;
}
static void menu_close(App *a){a->menu_kind=MENU_NONE;a->menu_target[0]=0;a->dirty=1;}
/* ⚠ THE NINE, IN ONE PLACE. The ribbon draws this list and activate() dispatches
 * against it, so an item cannot be drawn in an order the click handler does not
 * share — which is exactly how a menu ends up firing the row above the one you
 * pressed. Order is the spec's (thin-HUI §2): Team, Retile, Full, Load, Status,
 * Review, Files, About, Quit.
 *
 * ⚠ "+ ROOM" IS NOT ONE OF THEM and was dropped from here deliberately: it lives
 * in the tab row, and with the inline toolbar gone that row has ~64 more cells
 * than it used to, so the badge draws at every supported width instead of being
 * the first thing squeezed out. A second copy in the burger would be two places
 * to keep in step for an action that now always has a home. */
static int toolbar_rows(App *a,MenuRow *out,int cap){
    int n=0;
    #define TROW(L,A) (n<cap?(copy(out[n].label,sizeof(out[n].label),L),out[n].action=(A),out[n].index=0,out[n].checked=0,out[n].disabled=0,out[n].note[0]=0,&out[n++]):NULL)
    TROW("TEAM",H_TEAM);TROW("RETILE",H_RETILE);TROW("FULL",H_FULL);TROW("LOAD",H_LOAD);
    TROW("STATUS",H_STATUS);TROW("REVIEW",H_REVIEW);TROW("FILES",H_FILES);TROW("ABOUT",H_ABOUT);
    /* TODO(menu): ATTACHMENT POINT 1 of 4 — a new top-level ribbon item is one
     * more TROW here, BEFORE the QUIT line (quit stays last, see below). It also
     * wants an H_* hit kind in the enum, an ON rule in toolbar_on() if it
     * toggles something, and — only if it has children — a flyout in the
     * H_RIBBON_ITEM handler. Nothing else: the ribbon measures and lays itself
     * out from this list, so an item added here simply appears. */
    /* ⚠ THE WAY OUT MUST BE VISIBLE. the operator asked for this live: he did not know
     * Ctrl+Q, and an interface you cannot leave with the mouse is a trap however
     * good the rest of it is. Last item, because leaving is the last thing you
     * do — and it carries its accelerator, the convention every terminal UI
     * toolkit uses to TEACH the key rather than hide it. */
    {MenuRow *q=TROW("QUIT  Ctrl+Q",H_QUIT);
     if(q)copy(q->note,sizeof(q->note),"agents keep running");}
    #undef TROW
    (void)a;
    return n;
}
/* Which of the nine are currently ON, so the ribbon keeps the state the inline
 * badges used to show. Losing it would make the ribbon prettier and stupider. */
static int toolbar_on(App *a,int action){
    if(action==H_TEAM)return a->team;
    if(action==H_FULL)return a->full[0]!=0;
    if(action==H_LOAD)return dock_has(a,"load");
    if(action==H_STATUS)return a->status_open;
    if(action==H_REVIEW)return !strcmp(a->focus,"review");
    if(action==H_FILES)return !strcmp(a->focus,"files");
    if(action==H_ABOUT)return a->about_open;
    /* TODO(menu): ATTACHMENT POINT 2 of 4 — a new item that TOGGLES something
     * returns its live flag here, so the ribbon shows state rather than only
     * offering an action. An item that merely does a thing needs no entry. */
    return 0;
}
/* Build the rows for whatever is open. Rebuilt every frame so a checkmark and a
 * note always describe the CURRENT state, never the state when it opened. */
static int menu_rows(App *a,MenuRow *out,int cap){
    int n=0;
    #define ROW(L,A,I) (n<cap?(copy(out[n].label,sizeof(out[n].label),L),out[n].action=(A),out[n].index=(I),out[n].checked=0,out[n].disabled=0,out[n].header=0,out[n].note[0]=0,&out[n++]):NULL)
    if(a->menu_kind==MENU_LOGO){
        /* ── the logo menu (visual-polish-chrome item 5, the operator: "organise the
         * logo menu and submenu"; lead chose B, room seq 1462): one grouped
         * dropdown under the mark. A header row has no action, so it is inert
         * and draws in the accent colour; the items are the same H_ kinds the
         * ribbon dispatched, so activate() needs no new arm. Files nests the
         * existing MENU_FILES flyout; Quit stays last with its accelerator. */
        #define HDR(L) do{MenuRow *hd=ROW(L,0,0);if(hd)hd->header=1;}while(0)
        #define NOTE(R,S) do{if(R)copy((R)->note,sizeof((R)->note),S);}while(0)
        MenuRow *r;
        HDR("ROOMS");ROW("New room",H_NEW_ROOM,0);ROW("Retile",H_RETILE,0);
        r=ROW("Full screen",H_FULL,0);if(r)r->checked=a->full[0]!=0;NOTE(r,"F11");
        HDR("SESSIONS");r=ROW("Team",H_TEAM,0);if(r)r->checked=a->team;NOTE(r,"F7");
        r=ROW("Status",H_STATUS,0);if(r)r->checked=a->status_open;
        r=ROW("Load graph",H_LOAD,0);if(r)r->checked=dock_has(a,"load");
        HDR("MEDIA");r=ROW("Files",H_FILES,0);NOTE(r,"\u25b8");
        r=ROW("Review",H_REVIEW,0);if(r)r->checked=!strcmp(a->focus,"review");
        r=ROW("Play video\u2026",H_ABOUT_PLAY,0);if(r)r->checked=lj_popup_is_open(&a->popup);
        HDR("THEME");r=ROW("Theme editor\u2026",H_SETTINGS,0);if(r)r->checked=a->settings_open;
        r=ROW("Scanline effects",H_EFFECTS,0);if(r)r->checked=a->effects;
        HDR("HELP");r=ROW("About lilJack",H_ABOUT,0);
        r=ROW("Quit",H_QUIT,0);NOTE(r,"Ctrl+Q");
        #undef HDR
        #undef NOTE
    } else if(a->menu_kind==MENU_ROOMS){
        /* the tab strip's overflow list: every room the filter admits, the
         * open one checked; STANDALONE first as in the strip */
        const char *filt=a->room_filter[0]?a->room_filter:"active";
        {MenuRow *r0=ROW("STANDALONE",H_GROUP,-1);if(r0)r0->checked=!a->active_room[0];}
        for(int g=0;g<a->nroom&&n<cap;g++){Room *rm=&a->rooms[g];if(strcmp(rm->state[0]?rm->state:"active",filt))continue;
            char lbl[80];snprintf(lbl,sizeof lbl,"%s %s",lj_theme_glyph(LJ_THEME_ROOM),rm->name);
            MenuRow *r1=ROW(lbl,H_GROUP,g);if(r1){r1->checked=!strcmp(rm->id,a->active_room);
                char cnt[16];snprintf(cnt,sizeof cnt,"%d",group_count(a,rm->id));copy(r1->note,sizeof(r1->note),cnt);}}
    } else if(a->menu_kind==MENU_OPEN_HERE){
        const char *names[]={"CLAUDE","CODEX","DEEPSEEK","SHELL"};
        for(int i=0;i<4;i++)ROW(names[i],H_CREATE,i);
        {MenuRow *o=ROW("CANVAS",H_CANVAS,0);if(o)copy(o->note,sizeof(o->note),"draw together in this room");}
    } else if(a->menu_kind==MENU_TOOLBAR){
        MenuRow all[16];int count=toolbar_rows(a,all,16);
        int start=atoi(a->menu_target);
        for(int i=start;i<count&&n<cap;i++){out[n]=all[i];out[n].action=H_RIBBON_ITEM;out[n].index=i;n++;}
    } else if(a->menu_kind==MENU_FILES){
        /* The FILES flyout. Both rows are backed by handlers that already exist;
         * the target carries the browsed path with the 'd' type prefix the
         * MENU_FILE branch of activate() expects, so one dispatch serves both.
         * TODO(menu): ATTACHMENT POINT 4 of 4 — a flyout's ROWS are declared in a
         * branch like this one. Still wanted here: recent folders, and "open a
         * room in any folder" (the operator, thin-HUI spec). Neither exists yet —
         * nothing in the app records a folder history and Room carries no folder
         * field, so listing them would mean inventing the data. Left undone
         * rather than faked. */
        ROW("FILES PANEL",H_FILES,0);
        {MenuRow *o=ROW("OPEN A NEW ROOM HERE",H_NEW_ROOM,1);
         if(o&&!a->files_path[0]){o->disabled=1;copy(o->note,sizeof(o->note),"browse to a folder first");}
         else if(o)copy(o->note,sizeof(o->note),"agents start in this folder");}
    } else if(a->menu_kind==MENU_ROLE){
        Session *s=session(a,a->menu_target);
        const char *roles[3]={"worker","reviewer","lead"};
        Session *lead=NULL;
        for(int i=0;i<a->norder;i++){Session *p2=&a->sessions[a->order[i]];
            if(!strcmp(p2->role,"lead")&&!strcmp(p2->room_id,s?s->room_id:""))lead=p2;}
        for(int r=0;r<3;r++){
            MenuRow *row=ROW(roles[r],H_ROLE,r);if(!row)break;
            row->checked=s&&!strcmp(s->role[0]?s->role:"worker",roles[r]);
            /* ⚠ Choosing lead when the room already has one is a HANDOVER, and
             * the menu says so before the click, not after. */
            if(r==2&&lead&&s&&lead!=s)snprintf(row->note,sizeof(row->note),"takes over from %s",lead->agent);
        }
        ROW("— control —",0,0);
        ROW("focus",H_AGENT_FOCUS,0);ROW("direct message",H_AGENT_DM,0);
        ROW("submit (Enter)",H_CONTROL,0);ROW("interrupt (Ctrl+C)",H_CONTROL,1);
        MenuRow *stop=ROW("stop this session",H_STOP_SESSION,2);
        if(stop)copy(stop->note,sizeof(stop->note),"ends its tmux session");
    } else if(a->menu_kind==MENU_STATE){
        for(int i=0;i<6;i++){
            /* ⚠ SAME WORD, SAME CASE. The badge uppercases "ACTIVE 3 ▾" while
             * these rows said "active" — one concept, two spellings, across a
             * click (deepseek review F1). The row's chrome is uppercase
             * everywhere (TEAM, + ROOM, STANDALONE), so the dropdown follows the
             * badge. Safe because the click is INDEX-keyed — activate() reads
             * ROOM_STATES[clamp(index,0,5)], never the label. */
            char up[40];copy(up,sizeof(up),ROOM_STATES[i]);
            for(char *q=up;*q;q++)*q=(char)toupper((unsigned char)*q);
            MenuRow *row=ROW(up,H_STATE_FILTER,i);if(!row)break;
            row->checked=!strcmp(a->room_filter[0]?a->room_filter:"active",ROOM_STATES[i]);
            json_object *v=a->room_states?jget(a->room_states,ROOM_STATES[i]):NULL;
            if(v)snprintf(row->note,sizeof(row->note),"%d",json_object_get_int(v));
            else copy(row->note,sizeof(row->note),"—");
        }
    } else if(a->menu_kind==MENU_ROOM){
        Room *r=room_by_id(a,a->menu_target);
        ROW("live view",H_ROOM_ACTION,0);ROW("status",H_ROOM_ACTION,1);
        ROW("review",H_ROOM_ACTION,2);ROW("files",H_ROOM_ACTION,3);
        ROW("tools",H_ROOM_ACTION,4);
        MenuRow *mv=ROW("move all team here",H_ROOM_ACTION,5);
        if(mv)copy(mv->note,sizeof(mv->note),"one transaction, one lead");
        ROW("— state —",0,0);
        for(int i=0;i<6;i++){
            MenuRow *row=ROW(ROOM_STATES[i],H_ROOM_STATE,i);if(!row)break;
            row->checked=r&&!strcmp(r->state[0]?r->state:"active",ROOM_STATES[i]);
            if(i==5)copy(row->note,sizeof(row->note),"trash, 30 days");
        }
        if(r&&!strcmp(r->state,"removed")){
            MenuRow *pg=ROW("purge for good",H_ROOM_PURGE,0);
            if(pg){
                /* ⚠ The gate is the backend's; this only reports it, so the menu
                 * cannot promise a purge the store will refuse. */
                if(r->days_until_purge>0){pg->disabled=1;
                    snprintf(pg->note,sizeof(pg->note),"in %d day(s)",r->days_until_purge);}
                else copy(pg->note,sizeof(pg->note),"cannot be undone");
            }
        }
    } else if(a->menu_kind==MENU_FILE){
        int dir=a->menu_target[0]=='d';
        MenuRow *o=ROW("open a NEW ROOM here",H_NEW_ROOM,1);
        if(o&&!dir){o->disabled=1;copy(o->note,sizeof(o->note),"pick a folder, not a file");}
        else if(o)copy(o->note,sizeof(o->note),"agents start in this folder");
        ROW("browse into it",H_FILE_ENTRY,dir);
    }
    #undef ROW
    return n;
}
/* ⚠ A MUTED HEARTBEAT MUST BE LOUD. the operator, 2026-09-09: "heartbeat is a must".
 * One empty file mutes every delivery, and one sat in the workspace for TWENTY
 * HOURS while eight tasks were open and nobody could tell from the screen. It
 * costs two stat() calls to never let that happen quietly again. */
static const char *heartbeat_mute(App *a){
    static char note[320];
    const char *home=getenv("HOME");if(!home)return NULL;
    const char *paths[2];char a1[PATH_MAX],a2[PATH_MAX];
    snprintf(a1,sizeof(a1),"%s/room-delivery.stop",a->root[0]?a->root:"");
    snprintf(a2,sizeof(a2),"%s/.cache/liljack/heartbeat.stop",home);
    paths[0]=a->root[0]?a1:NULL;paths[1]=a2;
    for(int i=0;i<2;i++){
        struct stat st;
        if(!paths[i]||stat(paths[i],&st))continue;
        snprintf(note,sizeof(note),"⚠ HEARTBEAT MUTED — delete %s",paths[i]);
        return note;
    }
    return NULL;
}
static void draw_menu(App *a){
    if(!a->menu_kind)return;
    MenuRow rows[24];int n=menu_rows(a,rows,24);
    if(!n){menu_close(a);return;}
    /* An open menu is an OVERLAY: everything it registers sits above the
     * workspace, so a click inside it can never fall through to a control it
     * is covering. Restored at the end of this function, not left raised. */
    int prior_layer=a->hit_layer;a->hit_layer=prior_layer+1;
    int w=14*CELLW;
    for(int i=0;i<n;i++){
        int need=(int)strlen(rows[i].label)+(rows[i].note[0]?(int)strlen(rows[i].note)+3:0)+4;
        if(need*CELLW>w)w=need*CELLW;
    }
    if(w>a->w-2*CELLW)w=a->w-2*CELLW;
    int x=a->menu_at.x,y=a->menu_at.y+a->menu_at.h;
    if(x+w>a->w)x=a->w-w;if(x<0)x=0;
    if(y+n*CELLH>a->h)y=a->menu_at.y-n*CELLH;if(y<0)y=0;
    /* A scrim behind it, so a click anywhere else dismisses rather than acting. */
    hit(a,(lj_rect){0,0,a->w,a->h},H_MENU_SCRIM,0,NULL);
    panel((lj_rect){x,y,w,n*CELLH},SURF2,0);
    for(int i=0;i<n;i++){
        lj_rect row={x,y+i*CELLH,w,CELLH};
        int sep=!rows[i].action;
        if(sep){text(row.x+CELLW/2,row.y,rows[i].label,rows[i].header?CYAN:DIM,w-CELLW);continue;}
        int hot=hovered(a,H_MENU_ITEM,i,a->menu_target)||(a->menu_kind==MENU_LOGO&&a->menu_sel==i);
        if(rows[i].disabled){text(row.x+CELLW/2+(a->menu_kind==MENU_LOGO?2*CELLW:0),row.y,rows[i].label,lj_theme_rgb(LJ_THEME_DISABLED),w-CELLW);
            if(rows[i].note[0])text(row.x+w-((int)strlen(rows[i].note)+1)*CELLW,row.y,rows[i].note,lj_theme_rgb(LJ_THEME_DISABLED),w);
            hit(a,row,H_MENU_ITEM,i,a->menu_target);continue;}
        if(hot)panel(row,SURF3,0);
        char line[180];snprintf(line,sizeof(line),"%s %s",rows[i].checked?lj_theme_glyph(LJ_THEME_CHECK):" ",rows[i].label);
        text(row.x+CELLW/2,row.y,line,rows[i].checked?GREEN:hot?GREEN:TEXT,w-CELLW);
        if(rows[i].note[0])text(row.x+w-((int)strlen(rows[i].note)+1)*CELLW,row.y,rows[i].note,AMBER,w);
        hit(a,row,H_MENU_ITEM,i,a->menu_target);
    }
    /* ⚠ THE FRAME GOES OUTSIDE THE CONTENT. border() on a cell grid paints the
     * whole first and last ROW with ─, which ate "worker" off the top of this
     * menu and "stop this session" off the bottom. A raised surface is already
     * the boundary; the rule below and to the right just lifts it off whatever
     * it covers, and costs no row the menu was using. */
    hairline((lj_rect){x,y+n*CELLH,w,1},EDGE,0);
    hairline((lj_rect){x+w,y,1,n*CELLH},EDGE,1);
    a->hit_layer=prior_layer;
}
/* ── the burger ribbon ────────────────────────────────────────────────────
 * The ribbon unfolds right from the burger in row 0. Its overflow uses the
 * existing flyout; the ribbon itself never paints over the dock or tabs.
 *
 * ⚠ IT IS DRAWN FROM ITS OWN STATE, NOT FROM menu_kind. The slide-BACK has to
 * keep painting after the menu is logically closed, and a sub-menu flyout has
 * to be able to open without its parent reading itself as shut. `ribbon_open`
 * is the toggle and `ribbon` is where the slide has got to; the ribbon is drawn
 * whenever `ribbon > 0`, which is true for the whole of the closing animation.
 *
 * ⚠ The items are revealed by CLIPPING, not by scaling: an item is drawn only
 * once the panel has grown wide enough to hold it whole. Squeezing glyphs into
 * a part-open panel is the truncation this codebase refuses everywhere else. */
#define RIBBON_FULL 100
static int draw_ribbon(App *a,int y){
    int target=a->ribbon_open?RIBBON_FULL:0;
    int moving=a->ribbon!=target;
    if(moving){
        int step=RIBBON_FULL/8;                 /* ~8 frames ≈ 130ms at 60fps */
        a->ribbon+=a->ribbon<target?step:-step;
        if(a->ribbon>RIBBON_FULL)a->ribbon=RIBBON_FULL;
        if(a->ribbon<0)a->ribbon=0;
    }
    /* ⚠ The caller feeds this back into a->dirty, which render() clears on its
     * last line — setting dirty here would be undone and the slide would stop
     * dead after one frame. */
    if(a->ribbon<=0)return moving;
    MenuRow rows[16];int n=toolbar_rows(a,rows,16);
    if(n<=0)return moving;
    int wcell[16],full=0;
    for(int i=0;i<n;i++){
        /* A note (QUIT's "agents keep running") rides in the item's own cells,
         * dimmed, so the reassurance the note exists to give is not lost in the
         * move from the old dropdown to the ribbon (deepseek review F4). */
        wcell[i]=label_cells(rows[i].label)+2+(rows[i].note[0]?label_cells(rows[i].note)+1:0);
        full+=wcell[i];
    }
    Header hdr=header_layout(a);
    int avail=hdr.ribbon_w,fullpx=full*CELLW;
    if(fullpx>avail)fullpx=avail;
    int w=fullpx*a->ribbon/RIBBON_FULL,x0=hdr.ribbon_x;
    if(w<=0)return moving;
    /* ⚠ A CLOSING RIBBON IS A PICTURE, NOT A CONTROL. It keeps painting while it
     * slides back, but the moment it is logically shut it must stop catching
     * clicks — scrim included. Leaving the scrim registered for the ~8 frames of
     * the slide swallowed the very next click anywhere on screen, because a
     * full-window hit at a raised layer beats every real control under it. Found
     * by the native suite: a click on the review panel's Git DAG button landed
     * on the dismissed ribbon's scrim and did nothing.
     *
     * badge() with kind 0 draws and registers nothing, so the visual is
     * unchanged and only the hits go away. */
    int live=a->ribbon_open;
    int prior=a->hit_layer;a->hit_layer=prior+1;
    if(live)hit(a,(lj_rect){0,0,a->w,a->h},H_MENU_SCRIM,0,NULL);
    panel((lj_rect){x0,y,w,CELLH},SURF2,0);
    int x=x0,shown=0,overflow=full*CELLW>avail;
    int items_end=x0+w-(overflow?6*CELLW:0);
    for(int i=0;i<n;i++){
        int iw=wcell[i]*CELLW;
        if(x+iw>items_end)break;                     /* not emerged yet */
        int on=toolbar_on(a,rows[i].action),hot=live&&hovered(a,H_RIBBON_ITEM,i,"");
        uint32_t fg=rows[i].action==H_QUIT?AMBER:on?BG:hot?GREEN:TEXT;
        badge(a,(lj_rect){x,y,iw-CELLW,CELLH},rows[i].label,fg,on?GREEN:hot?SURF3:PANEL,
              live?H_RIBBON_ITEM:0,i,"");
        if(rows[i].note[0])text(x+CELLW/2+(label_cells(rows[i].label)+1)*CELLW,y,rows[i].note,DIM,
                                (label_cells(rows[i].note)+1)*CELLW);
        x+=iw;shown++;
    }
    if(overflow&&w>=6*CELLW){
        badge(a,(lj_rect){x0+w-6*CELLW,y,6*CELLW,CELLH},"MORE",CYAN,SURF2,
              live?H_RIBBON_MORE:0,shown,NULL);
    }
    a->hit_layer=prior;
    return moving;
}
#ifndef LJ_RICKROLL
#define LJ_RICKROLL "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
#endif
static void open_media(App *a,const char *path);   /* defined with the input helpers */
static void open_canvas(App *a);
enum {RT_ROOM=0,RT_ACTION,RT_STATUS,RT_REVIEW,RT_COUNT};
static const char *ROOM_TABS[RT_COUNT]={"ROOM","ACTIONABLE","STATUS","REVIEW"};
/* ⚠ AN ABBREVIATION IS DESIGNED; A TRUNCATION IS AN ACCIDENT, AND THEY LOOK
 * DIFFERENT TO A READER. Four equal tabs across a narrow tile left no room for
 * "ACTIONABLE", and badge() simply clipped it: codex's ended-800.png shows the
 * tab reading "ACTIONA", and at the width in files-800.png the row degrades to
 * "RO AC ST RE". "ACTIONA" is not a shorter word, it is a broken one. So each
 * tab carries its own short and tiny forms and the row picks the widest ladder
 * rung that FITS — a deliberate "ACTION" instead of a mangled "ACTIONA". */
static const char *ROOM_TABS_SHORT[RT_COUNT]={"ROOM","ACTION","STATUS","REVIEW"};
static const char *ROOM_TABS_TINY[RT_COUNT] ={"RM","AC","ST","RV"};
/* ⚠ THE LADDER NEEDS A BOTTOM RUNG OR IT FALLS OFF. "AC" is two cells, so at a
 * ONE-cell budget even the tiny form was clipped — the ladder handing back a
 * whole word that the renderer then cut in half, which is the exact failure it
 * exists to prevent. A single distinct letter is still a DESIGNED abbreviation;
 * "ACTIO" is not. V for reVIEW, because R is already ROOM. */
static const char *ROOM_TABS_MICRO[RT_COUNT]={"R","A","S","V"};
static const char *room_tab_label(int i,int cells){
    if(cells>=(int)strlen(ROOM_TABS[i]))return ROOM_TABS[i];
    if(cells>=(int)strlen(ROOM_TABS_SHORT[i]))return ROOM_TABS_SHORT[i];
    if(cells>=(int)strlen(ROOM_TABS_TINY[i]))return ROOM_TABS_TINY[i];
    return ROOM_TABS_MICRO[i];
}

/* The message feed, used by ROOM (everything) and ACTIONABLE (the all-hands
 * view). Returns the content height so the caller can scroll it. */
/* Each viewport owns a fade history. A new/changed layout or new content only
 * establishes a baseline; it never flashes a scrollbar without scrolling. */
typedef struct {lj_rect r;int scroll,content,seen,shown;uint32_t start,last;} ScrollFade;
static ScrollFade scroll_fades[16];
static int scrollbar_frame;
static int scrollbar_opacity(ScrollFade *s,uint32_t now){
    if(!s->shown)return 0;
    uint32_t age=now-s->last,rise=now-s->start;
    if(age>=1600)return 0;
    int alpha=rise<160?(int)(rise*255/160):255;
    if(age>1200){int out=(int)((1600-age)*255/400);if(out<alpha)alpha=out;}
    return alpha;
}
static void draw_scrollbar(lj_rect r,int scroll,int content_h,int view_h,int active){
    if(r.w<CELLW||view_h<CELLH)return;
    ScrollFade *s=NULL,*old=&scroll_fades[0];
    for(int i=0;i<16;i++){
        ScrollFade *p=&scroll_fades[i];
        if(p->seen&&p->r.x==r.x&&p->r.y==r.y&&p->r.w==r.w&&p->r.h==r.h){s=p;break;}
        if(p->seen<old->seen)old=p;
    }
    int maxscroll=content_h>view_h?content_h-view_h:0;
    scroll=clamp(scroll,0,maxscroll);uint32_t now=SDL_GetTicks();
    if(!s){s=old;*s=(ScrollFade){.r=r,.scroll=scroll,.content=content_h};}
    if(!active||!maxscroll||s->seen+1<scrollbar_frame||s->content!=content_h)s->shown=0;
    else if(s->seen&&s->scroll!=scroll){
        if(!scrollbar_opacity(s,now))s->start=now;
        s->last=now;s->shown=1;
    }
    s->scroll=scroll;s->content=content_h;s->seen=scrollbar_frame;
    int alpha=scrollbar_opacity(s,now);if(!alpha)return;
    int thumb=clamp((int)((long long)view_h*view_h/content_h),4,view_h);
    int offset=(int)((long long)(view_h-thumb)*scroll/maxscroll);
    int x=(r.x+r.w-CELLW)/CELLW*CELLW;
    if(render_ansi)lj_ansi_scrollbar(x,r.y,view_h,offset,thumb,alpha);
    else {
        uint32_t color=0;for(int shift=0;shift<=16;shift+=8)
            color|=(((((PANEL>>shift)&255)*(255-alpha))+(((DIM>>shift)&255)*alpha))/255)<<shift;
        rect((lj_rect){x+CELLW-3,r.y,1,view_h},color);
        rect((lj_rect){x+CELLW-4,r.y+offset,3,thumb},color);
    }
}
static int scrollbar_active(App *a,const char *id,lj_rect r){
    return !a->team&&!a->status_open&&!a->about_open&&!a->settings_open&&!a->room_dialog&&
        (!strcmp(a->focus,id)||inside(r,a->mousex,a->mousey));
}
static lj_rect scrollbar_body(lj_rect r){
    r.w=(r.x+r.w-CELLW)/CELLW*CELLW-r.x;if(r.w<0)r.w=0;return r;
}
static int draw_feed(App *a,lj_rect b,int scroll){
    int cols=b.w/CELLW,total=0;
    size_t count=a->messages?json_object_array_length(a->messages):0;
    for(size_t i=0;i<count;i++){
        json_object *m=json_object_array_get_idx(a->messages,i);
        if(!message_visible(a,m))continue;
        total+=CELLH+wrapped_lines(jstr(m,"text"),cols)*CELLH;
        if(jget(m,"delivery"))total+=CELLH;
    }
    int y=b.y-scroll;
    if(total<b.h)y=b.y; else y=b.y+b.h-total+scroll;
    for(size_t i=0;i<count;i++){
        json_object *m=json_object_array_get_idx(a->messages,i);
        if(!message_visible(a,m))continue;
        const char *sender=jstr(m,"sender");Session *s=session(a,sender);
        char who[96],title[220];
        if(s)snprintf(who,sizeof(who),"%s / %.8s",s->agent,s->id+2);else copy(who,sizeof(who),sender);
        snprintf(title,sizeof(title),"%s %s · %s",s?icon(s->agent):lj_theme_glyph(LJ_THEME_ICON_USER),who,jstr(m,"language"));
        if(y>=b.y&&y+CELLH<=b.y+b.h)text(b.x,y,title,s?CYAN:GREEN,b.w);
        y+=CELLH;
        wrapped(b.x,y,b.w,b.y,b.y+b.h,jstr(m,"text"),TEXT);
        y+=wrapped_lines(jstr(m,"text"),cols)*CELLH;
        json_object *delivery=jget(m,"delivery");
        if(delivery){char st[300];snprintf(st,sizeof st,"%s · %s",jstr(delivery,"state"),jstr(delivery,"reason"));
            if(y>=b.y&&y+CELLH<=b.y+b.h)text(b.x,y,st,DIM,b.w);y+=CELLH;}
    }
    return total;
}

/* The room's todo list, down the right of ACTIONABLE. A question is the room
 * waiting on its LEAD, so those rows come first and carry the answer key. */
/* State colour, shared by every todo surface so one glance means one thing
 * everywhere. Blocked is amber because it is WAITING, not failing; done is dim
 * because it is history; a question is the brightest thing on screen because it
 * is the room stopped on its lead. */
static uint32_t todo_colour(const char *state,int question){
    if(question)return GREEN;
    if(!strcmp(state,"active"))return lj_theme_rgb(LJ_THEME_TASK_ACTIVE);
    if(!strcmp(state,"blocked"))return AMBER;
    if(!strcmp(state,"done"))return lj_theme_rgb(LJ_THEME_ENDED);
    if(!strcmp(state,"proposed"))return CYAN;
    return TEXT;                                   /* assigned, not started */
}
static const char *todo_mark(const char *state,int question){
    if(question)return "?";
    if(!strcmp(state,"active"))return lj_theme_glyph(LJ_THEME_ACTIVE);
    if(!strcmp(state,"blocked"))return lj_theme_glyph(LJ_THEME_BLOCKED);
    if(!strcmp(state,"done"))return lj_theme_glyph(LJ_THEME_CHECK);
    if(!strcmp(state,"proposed"))return lj_theme_glyph(LJ_THEME_STANDALONE);
    return lj_theme_glyph(LJ_THEME_EMPTY);
}
/* ⚠ A ROW IS ALWAYS TWO LINES TALL, IN VIEW OR NOT. It used to count its meta
 * line only while visible, so todo_height grew as you scrolled (800 -> 1000 for
 * the same 30 items) and the scroll clamp chased a moving end — codex review,
 * room 2103. Measure unconditionally; draw only what is inside `b`. */
static int todo_visible(lj_rect b,int y){return y>=b.y&&y+CELLH<=b.y+b.h;}
static int todo_row(App *a,lj_rect b,int y,json_object *t,const char *stem,int depth){
    const char *st=jstr(t,"state"),*qid=jstr(t,"question_id");
    const char *owner=jstr(t,"owner"),*prog=jstr(t,"progress");
    char line[320];
    snprintf(line,sizeof(line),"%s%s %s",stem,todo_mark(st,qid[0]!=0),jstr(t,"title"));
    if(todo_visible(b,y))text(b.x,y,line,todo_colour(st,qid[0]!=0),b.w);
    if(qid[0]&&todo_visible(b,y))hit(a,(lj_rect){b.x,y,b.w,CELLH},H_ANSWER,0,qid);
    y+=CELLH;
    if(todo_visible(b,y)){
        snprintf(line,sizeof(line),"%*s%s · %s%s%s",depth*2+2,"",
                 owner[0]?owner:"unassigned",st,prog[0]?" · ":"",prog[0]?prog:"");
        text(b.x,y,line,lj_theme_rgb(LJ_THEME_TASK_META),b.w);
    }
    return y+CELLH;
}
static int draw_dms(App *a,lj_rect b){
    size_t n=a->room_dms?json_object_array_length(a->room_dms):0;
    if(!n)return b.y;
    /* ⚠ DMS SHARE THE COLUMN, THEY DO NOT OWN IT. Twenty DMs took every row and
     * handed draw_todos a NEGATIVE height (todo_rect.h=-40 at 1280x720), so the
     * wheel could never land on the todo list — the operator's "I can't scroll
     * actionable todos", reproduced by codex (room 2103). The block gets at most
     * half the column and names how many DMs it could not show. */
    b.h=(b.h/2)/CELLH*CELLH;
    int y=b.y;
    text(b.x,y,"DMS · answer with a",CYAN,b.w);y+=CELLH;
    hairline((lj_rect){b.x,y,b.w,1},EDGE,0);y+=CELLH;
    for(size_t i=0;i<n;i++){
        /* keep the last row for the "+N more" line when anything is left over */
        if(y+CELLH>b.y+b.h-(i+1<n?CELLH:0)){
            char more[64];snprintf(more,sizeof more,"+%d more DM%s",(int)(n-i),n-i==1?"":"s");
            if(y+CELLH<=b.y+b.h){text(b.x,y,more,DIM,b.w);y+=CELLH;}
            break;
        }
        json_object *d=json_object_array_get_idx(a->room_dms,i);
        if(!d)continue;
        int unread=!json_object_get_boolean(jget(d,"read"));
        const char *agent=jstr(d,"agent"),*subj=jstr(d,"subject"),*id=jstr(d,"id");
        char line[400];
        snprintf(line,sizeof(line),"%s %s%s%s%s",unread?lj_theme_glyph(LJ_THEME_ACTIVE):lj_theme_glyph(LJ_THEME_EMPTY),agent[0]?agent:"(unknown)",
                 subj[0]?" — ":"",subj[0]?subj:"",unread?"":"  (answered)");
        text(b.x,y,line,unread?GREEN:DIM,b.w);
        hit(a,(lj_rect){b.x,y,b.w,CELLH},H_ANSWER_DM,0,id);
        y+=CELLH;
        const char *txt=jstr(d,"text");
        if(txt[0]&&y+CELLH<=b.y+b.h-(i+1<n?CELLH:0)){
            char t[360];snprintf(t,sizeof(t),"   %.340s",txt);
            text(b.x,y,t,DIM,b.w);y+=CELLH;
        }
    }
    return y;
}
static void draw_todos(App *a,lj_rect b){
    Room *room=room_by_id(a,a->active_room);
    json_object *todos=a->room_todos;
    int scoped=todos&&json_object_array_length(todos);
    if(!scoped)todos=a->all_tasks;
    size_t n=todos?json_object_array_length(todos):0;
    char head[160];
    snprintf(head,sizeof(head),"TODO · %s · %d",scoped?"this room":"all open work",(int)n);
    /* ⚠ A HAIRLINE NEEDS ITS OWN ROW — my own rule, broken here. Drawn at
     * b.y+CELLH-1 it lands INSIDE the header row and overwrote the header text
     * with ─, so the panel had no title at all. It gets a row; the body starts
     * below it. */
    text(b.x,b.y,head,CYAN,b.w);
    hairline((lj_rect){b.x,b.y+CELLH,b.w,1},EDGE,0);
    lj_rect body={b.x,b.y+2*CELLH,b.w,b.h-2*CELLH};
    a->todo_rect=body;
    lj_rect viewport=body;body=scrollbar_body(body);
    if(a->tasks_error[0]){wrapped(body.x,body.y,body.w,body.y,body.y+body.h,a->tasks_error,RED);return;}
    if(!todos){text(body.x,body.y,"todo list not in this snapshot",AMBER,body.w);return;}
    if(!n){
        text(body.x,body.y,"no open work",DIM,body.w);
        if(room&&room->questions){char q[120];snprintf(q,sizeof(q),"%d open question(s)",room->questions);
            text(body.x,body.y+CELLH,q,GREEN,body.w);}
        return;
    }
    /* ⚠ THE TREE IS THE DEV PROCESS MADE VISIBLE. A flat list hides the one
     * thing that decides what anyone can do next: what is waiting on what. A
     * task nests under the task it names in blocked_on, so a blocker and
     * everything it holds up read as one branch. A dependency that is NOT in
     * this list (another room, already purged) is drawn at the root with its id
     * named, never silently flattened as though it had none. */
    a->todo_scroll=clamp(a->todo_scroll,0,a->todo_height>body.h?a->todo_height-body.h:0);
    int y=body.y-a->todo_scroll;
    char kids[64][81];int nk;
    for(size_t i=0;i<n;i++){
        json_object *t=json_object_array_get_idx(todos,i);
        const char *dep=jstr(t,"blocked_on");
        int parent_here=0;
        if(dep[0])for(size_t j=0;j<n;j++)
            if(!strcmp(jstr(json_object_array_get_idx(todos,j),"id"),dep)){parent_here=1;break;}
        if(parent_here)continue;
        y=todo_row(a,body,y,t,"",0);
        if(dep[0]){
            char miss[220];snprintf(miss,sizeof(miss),"   %s waits on %s (not in this list)",lj_theme_glyph(LJ_THEME_TREE_LAST),dep);
            if(todo_visible(body,y))text(body.x,y,miss,lj_theme_rgb(LJ_THEME_DEPENDENCY),body.w);
            y+=CELLH;
        }
        nk=0;
        const char *me=jstr(t,"id");
        for(size_t j=0;j<n&&nk<64;j++){
            json_object *c=json_object_array_get_idx(todos,j);
            if(!strcmp(jstr(c,"blocked_on"),me))copy(kids[nk++],81,jstr(c,"id"));
        }
        for(int k=0;k<nk;k++)for(size_t j=0;j<n;j++){
            json_object *c=json_object_array_get_idx(todos,j);
            if(strcmp(jstr(c,"id"),kids[k]))continue;
            char prefix[40];snprintf(prefix,sizeof prefix,"  %s ",lj_theme_glyph(k==nk-1?LJ_THEME_TREE_LAST:LJ_THEME_TREE_MID));
            y=todo_row(a,body,y,c,prefix,1);
            const char *why=jstr(c,"blocked_reason");
            if(why[0]){
                char w2[260];snprintf(w2,sizeof(w2),"      %.190s",why);
                if(todo_visible(body,y))text(body.x,y,w2,lj_theme_rgb(LJ_THEME_DEPENDENCY),body.w);
                y+=CELLH;
            }
            break;
        }
    }
    a->todo_height=y-(body.y-a->todo_scroll);
    draw_scrollbar(viewport,a->todo_scroll,a->todo_height,body.h,scrollbar_active(a,"room",viewport));
}

/* Status headings, details and fallback metrics share the same viewport.
 * Whole glyph rows are omitted at the edges, just like wrapped field rows. */
static void status_text(lj_rect viewport,int x,int y,const char *value,uint32_t color,int width){
    if(y<viewport.y||y+CELLH>viewport.y+viewport.h||x<viewport.x)return;
    int available=viewport.x+viewport.w-x;
    if(width>available)width=available;
    if(width>=CELLW){char shown[4096];text(x,y,fit(shown,sizeof shown,value,width/CELLW),color,width);}
}
/* Room prose ends at a complete word; table fields use ordinary glyph-safe fit. */
static const char *status_words(char *out,size_t cap,const char *value,int cols){
    fit(out,cap,value,cols);
    if(label_cells(value)<=cols)return out;
    size_t n=strlen(out);
    if(n<3)return out;
    n-=3; /* fit's trailing ellipsis */
    while(n&&value[n]!=' ')n--;
    while(n&&value[n-1]==' ')n--;
    if(cap>n+3)memcpy(out+n,lj_theme_glyph(LJ_THEME_ELLIPSIS),4);
    return out;
}
/* Four semantic columns share the actual remaining cell budget, not printf's
 * minimum byte widths. Task/reason receives every cell left after metadata. */
static void status_columns(int cols,int widths[4]){
    int usable=cols>3?cols-3:0;
    widths[0]=usable/4;if(widths[0]>11)widths[0]=11;
    widths[1]=usable/5;if(widths[1]>8)widths[1]=8;
    widths[2]=usable/4;if(widths[2]>11)widths[2]=11;
    widths[3]=usable-widths[0]-widths[1]-widths[2];
}
static void status_agent(lj_rect body,int y,Session *ag,const char *what,uint32_t color){
    char identity[128];snprintf(identity,sizeof identity,"%s %s",icon(ag->agent),ag->agent);
    const char *values[]={identity,ag->role[0]?ag->role:"worker",what,
        ag->unavailable[0]?ag->unavailable:ag->task};
    int widths[4],x=body.x+CELLW;status_columns((body.w-CELLW)/CELLW,widths);
    for(int col=0;col<4;col++){
        status_text(body,x,y,values[col],color,widths[col]*CELLW);
        x+=(widths[col]+1)*CELLW;
    }
}
/* Preview only: trailing blank lines remain in the draft but cannot hide its
 * last text row. A return cue makes the preserved trailing newlines visible. */
static void draft_preview(App *a,lj_rect input,int x,int y,int width){
    size_t end=(size_t)a->draft_len;
    while(end&&a->draft[end-1]=='\n')end--;
    int trailing=end<(size_t)a->draft_len;
    char shown[sizeof a->draft];const char *value=a->draft;
    if(trailing){memcpy(shown,a->draft,end);shown[end]=0;value=shown;width-=CELLW;}
    if(width>=CELLW){
        int n=wrapped_lines(value,width/CELLW);
        wrapped(x,y-(n>1?(n-1)*CELLH:0),width,input.y+CELLH,
                input.y+input.h-CELLH,value,TEXT);
    }
    if(trailing)glyph(x+width,y,lj_theme_codepoint(LJ_THEME_RETURN_MARK),DIM,1); /* display only, never inserted */
}
static void draw_room(App *a,lj_rect r){
    if(r.w<150||r.h<170)return;
    int head=CELLH;
    /* Sub-tabs across the top of the tile. Private DM is deliberately absent:
     * the operator asked for it out of this tile entirely. DMs still exist as data. */
    int tw=r.w/RT_COUNT,ty0=r.y+head;
    for(int i=0;i<RT_COUNT;i++){
        int on=a->room_tab==i;
        /* ⚠ ASK FOR THE WIDTH badge() WILL ACTUALLY GIVE THE TEXT, NOT THE
         * WIDTH OF THE RECT. badge() draws at r.x+CELLW/2 with maxw r.w-CELLW,
         * so a rect of (tw-CELLW) leaves (tw-2*CELLW) for the label. Computing
         * the ladder from (tw-CELLW) claimed one cell more than exists, so at
         * the width where "ACTION" exactly fits the ladder's arithmetic badge
         * clipped it to "ACTIO" — a truncation again, produced by the very code
         * meant to prevent one. Found by codex on the divider-resize path
         * (ACTIO/STATU), which is simply where that width lands in practice. */
        lj_rect tr={r.x+i*tw,ty0,tw-CELLW,CELLH};
        badge(a,tr,room_tab_label(i,(tr.w-CELLW)/CELLW),
              on?BG:DIM,on?GREEN:SURF2,H_ROOM_TAB,i,NULL);
    }
    int top=ty0+CELLH,bottom=r.y+r.h-CELLH;
    int composer=(a->room_tab==RT_ROOM||a->room_tab==RT_ACTION);
    if(composer)bottom=r.y+r.h-5*CELLH;
    lj_rect body={r.x+CELLW,top,r.w-2*CELLW,bottom-top};
    if(body.h<CELLH)return;

    if(a->room_tab==RT_STATUS){
        lj_rect viewport=body;body=scrollbar_body(body);
        /* ⚠ STATUS IS ABOUT THE ROOM AND ITS PEOPLE FIRST, the machine second.
         * the operator: "status should have status of all 3 agents and more detailed
         * room mood". The ribbon is a machine readout and was the whole tab;
         * who is here, what they hold, and whether the room can move are the
         * questions actually being asked. */
        int y=body.y-a->status_scroll,total=0;
        Room *rm=room_by_id(a,a->active_room);
        char line[300];
        /* ── the room ─────────────────────────────────────────────── */
        status_text(body,body.x,y,"ROOM",CYAN,body.w);y+=CELLH;total+=CELLH;
        const char *ph=rm&&rm->phase[0]?rm->phase:"WORK";
        const char *meaning=!strcmp(ph,"ALIGN") ?"agents acknowledge and WAIT — no work yet"
                          :!strcmp(ph,"PLAN")   ?"the lead is writing the todo list"
                          :!strcmp(ph,"REVIEW") ?"all todos done — waiting on the operator"
                          :!strcmp(ph,"RESOLVED")?"finished; a new todo returns it to WORK"
                                                 :"agents are working their todos";
        snprintf(line,sizeof(line),"%s · %s · %s",rm?rm->name:"Standalone",ph,meaning);
        char summary[300];status_words(summary,sizeof summary,line,(body.w-CELLW)/CELLW);
        status_text(body,body.x+CELLW,y,summary,GREEN,body.w-CELLW);y+=CELLH;total+=CELLH;
        if(rm){
            snprintf(line,sizeof(line),"lifecycle %s%s",rm->state[0]?rm->state:"active",
                     rm->days_until_purge>0?" · purgeable later":"");
            status_text(body,body.x+CELLW,y,line,DIM,body.w-CELLW);y+=CELLH;total+=CELLH;
            if(rm->awaiting){snprintf(line,sizeof(line),"%d agent(s) have not aligned yet",rm->awaiting);
                status_text(body,body.x+CELLW,y,line,AMBER,body.w-CELLW);y+=CELLH;total+=CELLH;}
            if(rm->questions){snprintf(line,sizeof(line),"%d open question(s) — the room waits on its LEAD",rm->questions);
                status_text(body,body.x+CELLW,y,line,GREEN,body.w-CELLW);y+=CELLH;total+=CELLH;}
        }
        y+=CELLH;total+=CELLH;
        /* ── every agent in the room ──────────────────────────────── */
        status_text(body,body.x,y,"AGENTS",CYAN,body.w);y+=CELLH;total+=CELLH;
        int shown=0;
        for(int i=0;i<a->norder;i++){
            Session *ag=&a->sessions[a->order[i]];
            if(strcmp(ag->room_id,a->active_room))continue;
            shown++;
            int dead=session_dead(ag);
            const char *what=ag->unavailable[0]?"UNAVAILABLE":dead?"ENDED":a->demo?"replay":
                             ag->connected?"attached":ag->managed?"idle":"observed";
            status_agent(body,y,ag,what,ag->unavailable[0]?RED:dead?lj_theme_rgb(LJ_THEME_ENDED):
                         ag->connected||a->demo?TEXT:DIM);
            y+=CELLH;total+=CELLH;
        }
        if(!shown){status_text(body,body.x+CELLW,y,"no agents in this room",DIM,body.w-CELLW);y+=CELLH;total+=CELLH;}
        y+=CELLH;total+=CELLH;
        /* ── the machine ──────────────────────────────────────────── */
        status_text(body,body.x,y,"MACHINE",CYAN,body.w);y+=CELLH;total+=CELLH;
        json_object *fields=jget(a->status_ribbon,"fields");
        if(!fields||!json_object_array_length(fields)){
            /* ⚠ "status starting…" AND NOTHING ELSE IS A DEAD PANEL. The
             * backend ribbon may be slow, absent, or never arrive — but the
             * machine's own numbers are in-process and cost 178us to sample,
             * so there is no reason for MACHINE to be blank while CPU, MEM and
             * GPU are known. The ribbon stays authoritative when it lands; this
             * is what the panel shows until then, and it is real data, never a
             * placeholder zero (an unavailable source reads as its reason). */
            lj_sample now; lj_metrics_current(&now);
            int drew=0;
            for(int m=0;m<LJ_METRIC_COUNT;m++){
                const char *lab=lj_metrics_label(m);
                if(!lab||!*lab)continue;
                status_text(body,body.x+CELLW,y,lab,m==LJ_METRIC_GPU||m==LJ_METRIC_GPU_MEM?CYAN:TEXT,body.w-CELLW);
                y+=CELLH;total+=CELLH;drew=1;
            }
            if(!drew){status_text(body,body.x+CELLW,y,"status starting…",DIM,body.w-CELLW);y+=CELLH;total+=CELLH;}
            else {status_text(body,body.x+CELLW,y,"live sample · backend ribbon not yet reporting",DIM,body.w-CELLW);
                  y+=CELLH;total+=CELLH;}
        } else for(size_t i=0;i<json_object_array_length(fields);i++){
            json_object *f=json_object_array_get_idx(fields,i);
            const char *v=jstr(f,"text");
            int lines=wrapped_lines(v,(body.w-CELLW)/CELLW);
            if(y>=body.y&&y+CELLH<=body.y+body.h)
                status_text(body,body.x+CELLW,y,jstr(f,"kind"),json_object_get_boolean(jget(f,"stale"))?AMBER:DIM,body.w-CELLW);
            y+=CELLH;wrapped(body.x+CELLW,y,body.w-CELLW,body.y,body.y+body.h,v,TEXT);
            y+=lines*CELLH;total+=(lines+1)*CELLH;
        }
        a->status_scroll=clamp(a->status_scroll,0,total>body.h?total-body.h:0);
        draw_scrollbar(viewport,a->status_scroll,total,body.h,scrollbar_active(a,"room",viewport));
        hit(a,body,H_ROOM,0,NULL);return;
    }
    if(a->room_tab==RT_REVIEW){
        /* ⚠ REVIEW WITHOUT THE WORK IT REVIEWS IS HALF A PICTURE. The same todo
         * tree sits beside the review so completion figures and the tasks they
         * describe are read together, and one glyph means the same thing on
         * both tabs. Below the width where both fit, review keeps the tab —
         * the tree is one tab away on ACTIONABLE, the review is not. */
        lj_rect rv=body;
        if(body.w>72*CELLW){
            int todow=clamp(body.w/3,26*CELLW,42*CELLW);
            rv.w=body.w-todow-CELLW;
            draw_todos(a,(lj_rect){body.x+rv.w+CELLW,body.y,todow,body.h});
            hairline((lj_rect){body.x+rv.w,body.y,1,body.h},EDGE,1);
        }
        /* The Git DAG button lived in the old review TILE's header; review is a
         * tab now, so its entry point comes with it rather than disappearing. */
        badge(a,(lj_rect){rv.x+rv.w-10*CELLW,rv.y,9*CELLW,CELLH},"GIT DAG",
              dock_has(a,"git")?BG:CYAN,dock_has(a,"git")?GREEN:SURF2,H_GIT,0,NULL);
        lj_rect rb={rv.x,rv.y+CELLH,rv.w,rv.h-CELLH};
        a->review_scroll=clamp(a->review_scroll,0,a->review_height>rb.h?a->review_height-rb.h:0);
        a->review_height=lj_review_draw(a->review_panel,rb.x,rb.y,scrollbar_body(rb).w,rb.h,a->review_scroll,a->ansi);
        draw_scrollbar(rb,a->review_scroll,a->review_height,rb.h,scrollbar_active(a,"room",rb));
        hit(a,rb,H_REVIEW_BODY,0,NULL);return;
    }

    lj_rect feed=body;
    if(a->room_tab==RT_ACTION){
        /* ⚠ ON A NARROW TILE THE TODO LIST WINS THIS TAB. Side by side needs
         * ~60 columns; below that the split silently dropped the todo column
         * and ACTIONABLE looked identical to ROOM — which is how "the todo list
         * is missing" was reported twice. The chat is always one tab away, the
         * todo list is not, so when only one fits it is the list. */
        if(body.w>60*CELLW){
            int todow=clamp(body.w/3,24*CELLW,44*CELLW);
            feed.w=body.w-todow-CELLW;
            lj_rect tcol={body.x+feed.w+CELLW,body.y,todow,body.h};
            int dmh=draw_dms(a,tcol);
            draw_todos(a,(lj_rect){tcol.x,dmh,tcol.w,tcol.h-(dmh-tcol.y)});
            hairline((lj_rect){body.x+feed.w,body.y,1,body.h},EDGE,1);
        } else {
            int dmh=draw_dms(a,body);
            draw_todos(a,(lj_rect){body.x,dmh,body.w,body.h-(dmh-body.y)});
            feed.w=0;
        }
    }
    if(feed.w<=0)feed.h=0;
    int total=draw_feed(a,scrollbar_body(feed),a->room_scroll);
    a->room_scroll=clamp(a->room_scroll,0,total>feed.h?total-feed.h:0);
    /* Feed offset counts back from newest; thumb position counts from top. */
    draw_scrollbar(feed,total-feed.h-a->room_scroll,total,feed.h,scrollbar_active(a,"room",feed));

    lj_rect input={r.x,r.y+r.h-5*CELLH,r.w,3*CELLH};
    rect(input,BG);
    if(render_ansi){
        uint32_t c=!strcmp(a->focus,"room")?GREEN:EDGE;
        hairline((lj_rect){input.x,input.y,input.w,1},c,0);
        hairline((lj_rect){input.x,input.y+input.h-1,input.w,1},c,0);
    } else border(input,!strcmp(a->focus,"room")?GREEN:EDGE);
    hit(a,input,H_ROOM,0,NULL);
    int tx=input.x+(CELLW),tyy=input.y+(CELLH),twid=input.w-2*(CELLW);
    if(a->answer_qid[0]){
        char ph[200];snprintf(ph,sizeof(ph),"ANSWERING %.20s — Send unblocks the asker · Esc cancels",a->answer_qid);
        if(!a->draft_len)text(tx,tyy,ph,GREEN,twid);
        else draft_preview(a,input,tx,tyy,twid);
    }
    else if(a->answer_dm[0]){
        char ph[200];snprintf(ph,sizeof(ph),"REPLYING to DM %.20s — Send replies · Esc cancels",a->answer_dm);
        if(!a->draft_len)text(tx,tyy,ph,GREEN,twid);
        else draft_preview(a,input,tx,tyy,twid);
    }
    else if(!a->draft_len)text(tx,tyy,a->room_tab==RT_ACTION?"Message all hands…":"Message this room…",DIM,twid);
    else draft_preview(a,input,tx,tyy,twid);
    int sy=r.y+r.h-2*CELLH,sh=CELLH;
    button(a,(lj_rect){r.x+12,sy,80,sh},"Send",H_POST,0,NULL,1);
}

/* ── THEME → Theme editor… (theme-editor-graph-settings / theme-editor-full) ──
 * One scrolling dialog in three sections: SETTINGS (numeric theme settings —
 * codex's lj_theme_int / lj_theme_set_int / lj_theme_setting_get, each with the
 * consumer that must follow it live), COLOURS (every colour token: swatch, hex,
 * per-channel +/- and typed hex) and GLYPHS (the glyph tokens, read-only).
 * SAVE = lj_theme_save_user; RESET = compiled defaults (lj_theme_reset). The
 * THEME owns every value; bounds/step/default come from it, nothing here is
 * duplicated. Colour writes go through lj_theme_set_rgb (codex's setter,
 * theme-editor-full); until it is linked the colour rows are read-only and say so. */
typedef struct{lj_theme_setting_id id;const char *label;int (*apply)(int);}Setting;
static const Setting SETTINGS[]={
    {LJ_THEME_GRAPH_BUCKET_MS,"Plot bucket",lj_metrics_set_display_ms},
    {LJ_THEME_GRAPH_REPAINT_MS,"Plot repaint",lj_metrics_set_repaint_ms},
    {LJ_THEME_METRICS_POLL_MS,"Metrics poll",lj_metrics_set_interval_ms},
    {LJ_THEME_METRICS_LABEL_HOLD_MS,"Label hold",lj_metrics_set_label_hold_ms},
    {LJ_THEME_DIVIDER_IDLE_PX,"Divider idle px",NULL},       /* read live by c_ansi separators (codex) */
    {LJ_THEME_DIVIDER_HOT_PX,"Divider hot px",NULL},         /* read live by c_ansi separators (codex) */
    {LJ_THEME_TAB_BADGE_MIN_CELLS,"Tab badge cells",NULL},   /* read live by draw_tabs */
    {LJ_THEME_EFFECTS_SCANLINES,"Scanlines",NULL},           /* pushed to a->effects by theme_apply_all */
};
#define SETTING_COUNT ((int)(sizeof SETTINGS/sizeof *SETTINGS))
/* An env override is "valid" by the METRICS HEADER's contract for that key (lead
 * 1576): DISPLAY_MS 25..1000, DISPLAY_REPAINT_MS >= bucket, INTERVAL_MS any
 * non-negative integer (0 = never limit). A valid env keeps its documented
 * meaning at init; the theme applies otherwise, and every dialog edit applies
 * live regardless of env. */
static int env_int_valid(const char *name,int lo,int hi){const char *v=getenv(name);if(!v||!*v)return 0;char *end=NULL;long n=strtol(v,&end,10);if(end==v||*end)return 0;return n>=lo&&n<=hi;}
/* A valid env override wins at INIT (c_metrics read it already); anything else
 * follows the theme. Called after the theme loads and after every edit. */
static void theme_apply_metrics(int startup){
    int bucket=lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS),repaint=lj_theme_int(LJ_THEME_GRAPH_REPAINT_MS);
    if(!(startup&&env_int_valid("LJ_METRICS_DISPLAY_MS",25,1000)))lj_metrics_set_display_ms(bucket);
    if(!(startup&&env_int_valid("LJ_METRICS_DISPLAY_REPAINT_MS",lj_metrics_display_ms(),INT_MAX)))lj_metrics_set_repaint_ms(repaint);
    if(!(startup&&env_int_valid("LJ_METRICS_INTERVAL_MS",0,INT_MAX)))lj_metrics_set_interval_ms(lj_theme_int(LJ_THEME_METRICS_POLL_MS));   /* 0 = never limit is a valid override */
    lj_metrics_set_label_hold_ms(lj_theme_int(LJ_THEME_METRICS_LABEL_HOLD_MS));
}
enum{TR_HEADER,TR_SETTING,TR_COLOUR,TR_GLYPH};
typedef struct{int kind,index;const char *label;}ThemeRow;
#define THEME_ROWS_MAX 256
static int theme_rows(ThemeRow *out,int cap){
    int n=0;
    #define TR(K,I,L) do{if(n<cap){out[n].kind=(K);out[n].index=(I);out[n].label=(L);n++;}}while(0)
    TR(TR_HEADER,0,"SETTINGS");for(int i=0;i<SETTING_COUNT;i++)TR(TR_SETTING,i,SETTINGS[i].label);
    TR(TR_HEADER,1,"COLOURS");for(int i=0;i<LJ_THEME_COUNT;i++){const lj_theme_token *t=lj_theme_get((lj_theme_id)i);if(t&&!t->glyph[0])TR(TR_COLOUR,i,t->name);}
    TR(TR_HEADER,2,"GLYPHS");for(int i=0;i<LJ_THEME_COUNT;i++){const lj_theme_token *t=lj_theme_get((lj_theme_id)i);if(t&&t->glyph[0])TR(TR_GLYPH,i,t->name);}
    #undef TR
    return n;
}
static int theme_row_editable(const ThemeRow *r){return r->kind==TR_SETTING||r->kind==TR_COLOUR;}
/* Everything the theme drives that is not read live at the point of use. */
static void theme_apply_all(App *a,int startup){
    theme_apply_metrics(startup);
    int scan=lj_theme_int(LJ_THEME_EFFECTS_SCANLINES)!=0;
    if(!startup||!a->effects){a->effects=scan;if(a->ansi)lj_ansi_effects(scan);}   /* --effects on the command line wins at startup */
}
static void setting_set(App *a,int i,int v){
    if(i<0||i>=SETTING_COUNT)return;
    lj_theme_set_int(SETTINGS[i].id,v);           /* the theme clamps; repaint never below bucket */
    theme_apply_all(a,0);a->dirty=1;
}
static void colour_set(App *a,int id,uint32_t rgb){lj_theme_set_rgb((lj_theme_id)id,rgb&0xffffff);a->dirty=1;}
/* -/+ on the selected row: a setting steps by its theme step; a colour steps the
 * selected channel (settings_chan: 0 R, 1 G, 2 B; Tab cycles) by 8. */
static void theme_row_step(App *a,int row,int dir){
    ThemeRow rows[THEME_ROWS_MAX];int n=theme_rows(rows,THEME_ROWS_MAX);if(row<0||row>=n)return;
    ThemeRow *r=&rows[row];
    if(r->kind==TR_SETTING){const lj_theme_setting *d=lj_theme_setting_get(SETTINGS[r->index].id);if(d)setting_set(a,r->index,d->value+dir*d->step);}
    else if(r->kind==TR_COLOUR){
        uint32_t rgb=lj_theme_rgb((lj_theme_id)r->index);int sh=16-8*clamp(a->settings_chan,0,2);
        int c=(int)((rgb>>sh)&255)+dir*8;c=clamp(c,0,255);
        colour_set(a,r->index,(rgb&~(0xffu<<sh))|((uint32_t)c<<sh));
    }
}
/* Typed text while the editor is open: hex digits accumulate for the selected
 * colour row (six, then Enter applies); everything else is ignored. */
static void settings_text(App *a,const char *t){
    ThemeRow rows[THEME_ROWS_MAX];int n=theme_rows(rows,THEME_ROWS_MAX);
    if(a->settings_sel<0||a->settings_sel>=n||rows[a->settings_sel].kind!=TR_COLOUR)return;
    for(;*t;t++){char c=(char)tolower((unsigned char)*t);if(!isxdigit((unsigned char)c))continue;size_t l=strlen(a->settings_hex);if(l<6){a->settings_hex[l]=c;a->settings_hex[l+1]=0;}}
    a->dirty=1;
}
static void theme_reset_all(App *a){lj_theme_reset();theme_apply_all(a,0);a->dirty=1;}
static void draw_settings(App *a){
    a->nhits=0;
    ThemeRow rows[THEME_ROWS_MAX];int n=theme_rows(rows,THEME_ROWS_MAX);
    int rowh=CELLH,w=clamp(a->w-8*CELLW,44*CELLW,72*CELLW);
    int maxvis=clamp(a->h/CELLH-10,4,n),vis=n<maxvis?n:maxvis,h=(6+vis)*rowh;   /* +1: the bottom border row stays empty (a rule through the button row is ─ glyphs on the cell path) */
    lj_rect m={((a->w-w)/2)/CELLW*CELLW,((a->h-h)/2)/CELLH*CELLH,w,h};
    a->settings_sel=clamp(a->settings_sel,0,n-1);
    if(a->settings_sel<a->settings_scroll)a->settings_scroll=a->settings_sel;
    if(a->settings_sel>=a->settings_scroll+vis)a->settings_scroll=a->settings_sel-vis+1;
    a->settings_scroll=clamp(a->settings_scroll,0,n-vis);
    panel(m,PANEL,0);border(m,EDGE);
    int lx=m.x+CELLW,y=m.y;char label[160];
    {int nc=0,ng=0;for(int i=0;i<n;i++){nc+=rows[i].kind==TR_COLOUR;ng+=rows[i].kind==TR_GLYPH;}snprintf(label,sizeof label,"THEME EDITOR · %d settings · %d colours · %d glyphs",SETTING_COUNT,nc,ng);}
    text(lx,y,label,GREEN,w-12*CELLW);
    if(a->settings_scroll>0)text(m.x+w-10*CELLW,y,"↑ more",DIM,9*CELLW);
    y+=rowh;hairline((lj_rect){m.x,y,w,1},EDGE,0);y+=rowh;
    for(int v=0;v<vis;v++){
        int i=a->settings_scroll+v;ThemeRow *r=&rows[i];int sel=a->settings_sel==i;
        lj_rect row={m.x,y,w,rowh};
        if(r->kind==TR_HEADER){text(lx,y,r->label,CYAN,w-2*CELLW);y+=rowh;continue;}
        if(sel)panel(row,SURF2,0);hit(a,row,H_SET_ROW,i,NULL);
        if(r->kind==TR_SETTING){
            const lj_theme_setting *d=lj_theme_setting_get(SETTINGS[r->index].id);if(!d){y+=rowh;continue;}
            snprintf(label,sizeof label,"%-15s %s",r->label,d->name);text(lx+CELLW,y,label,sel?GREEN:TEXT,w-32*CELLW);
            snprintf(label,sizeof label,"%5d %s",d->value,d->unit);text(m.x+w-22*CELLW,y,label,TEXT,12*CELLW);   /* room for "cells" / "bool" */
        } else if(r->kind==TR_COLOUR){
            uint32_t rgb=lj_theme_rgb((lj_theme_id)r->index);
            text(lx+CELLW,y,r->label,sel?GREEN:TEXT,22*CELLW);
            panel((lj_rect){lx+24*CELLW,y+2,2*CELLW,rowh-4},rgb,2);          /* the swatch */
            if(sel&&a->settings_hex[0])snprintf(label,sizeof label,"#%s_",a->settings_hex);else snprintf(label,sizeof label,"#%06x",rgb&0xffffff);
            text(lx+27*CELLW,y,label,sel&&a->settings_hex[0]?AMBER:TEXT,8*CELLW);
            int ch[3]={(int)(rgb>>16&255),(int)(rgb>>8&255),(int)(rgb&255)};const char *nm[3]={"R","G","B"};
            for(int c=0;c<3;c++){snprintf(label,sizeof label,"%s%3d",nm[c],ch[c]);text(m.x+w-24*CELLW+c*5*CELLW,y,label,sel&&a->settings_chan==c?GREEN:DIM,4*CELLW);}
        } else {
            const lj_theme_token *t=lj_theme_get((lj_theme_id)r->index);
            text(lx+CELLW,y,r->label,sel?GREEN:TEXT,22*CELLW);
            if(t){text(lx+24*CELLW,y,t->glyph,lj_theme_rgb((lj_theme_id)r->index),6*CELLW);snprintf(label,sizeof label,"U+%04X",lj_theme_codepoint((lj_theme_id)r->index));text(lx+31*CELLW,y,label,DIM,8*CELLW);}
        }
        if(theme_row_editable(r)){
            button(a,(lj_rect){m.x+w-9*CELLW,y,3*CELLW,rowh},"-",H_SET_DEC,i,NULL,0);
            button(a,(lj_rect){m.x+w-5*CELLW,y,3*CELLW,rowh},"+",H_SET_INC,i,NULL,0);
        }
        y+=rowh;
    }
    y+=rowh/2;
    text(lx,y,"Applies live · Up/Down/PgUp/PgDn · Left/Right or -/+ · Tab channel · hex + Enter · Esc",DIM,w-12*CELLW);
    if(a->settings_scroll+vis<n)text(m.x+w-10*CELLW,y,"↓ more",DIM,9*CELLW);y+=rowh;
    button(a,(lj_rect){m.x+w-34*CELLW,y,10*CELLW,rowh},"SAVE",H_SET_SAVE,0,NULL,0);
    button(a,(lj_rect){m.x+w-23*CELLW,y,11*CELLW,rowh},"RESET",H_SET_RESET,0,NULL,0);
    button(a,(lj_rect){m.x+w-11*CELLW,y,10*CELLW,rowh},"CLOSE",H_SET_CLOSE,0,NULL,0);
}
/* ── the media popup ──────────────────────────────────────────────────────
 * EVERYTHING ON THE CELL GRID (visual-polish-chrome item 4, 2026-09-13). Text
 * is cell-bound in both paths and a sixel strip cannot share a cell with text,
 * so every 1 px line owns its own cell: the outer ring of cells is the FRAME
 * (hairline centred → 4-5 px of padding to the content, the divider rule), a
 * RULE is one row. Rows: 0 frame · 1 title · 2 rule · 3..h-5 video · h-4 rule ·
 * h-3 timeline · h-2 controls · h-1 frame. Columns 0 and w-1 are the frame.
 * On a terminal without sixel the same cells draw c_ansi's box glyphs; no
 * cell-glyph decoration is emitted where a strip can be. */
#define POPUP_MIN_COLS 20
#define POPUP_MIN_ROWS 10
static void popup_geometry(App *a){
    lj_rect *r=&a->popup_rect;
    if(!a->popup_placed){
        r->w=clamp(a->w/CELLW,1,66)*CELLW;
        r->h=clamp(a->h/CELLH,1,24)*CELLH;
        r->x=(a->w-r->w)/2;r->y=(a->h-r->h)/2;a->popup_placed=1;
    }
    int cols=clamp(a->w/CELLW,1,INT_MAX),rows=clamp(a->h/CELLH,1,INT_MAX);
    r->w=clamp(r->w/CELLW,cols<POPUP_MIN_COLS?cols:POPUP_MIN_COLS,cols)*CELLW;
    r->h=clamp(r->h/CELLH,rows<POPUP_MIN_ROWS?rows:POPUP_MIN_ROWS,rows)*CELLH;
    r->x=clamp(r->x,0,a->w>r->w?a->w-r->w:0)/CELLW*CELLW;
    r->y=clamp(r->y,0,a->h>r->h?a->h-r->h:0)/CELLH*CELLH;
}
/* The grip is the bottom-right FRAME cell; the close is the last inner cell of
 * the title row; the whole title row (frame row included) drags. */
static lj_rect popup_resize_rect(App *a){
    lj_rect r=a->popup_rect;return (lj_rect){r.x+r.w-CELLW,r.y+r.h-CELLH,CELLW,CELLH};
}
static lj_rect popup_close_rect(App *a){
    lj_rect r=a->popup_rect;return (lj_rect){r.x+r.w-2*CELLW,r.y+CELLH,CELLW,CELLH};
}
static lj_rect popup_title_rect(App *a){
    lj_rect r=a->popup_rect;return (lj_rect){r.x,r.y,r.w-2*CELLW,2*CELLH};
}
static void popup_poll(App *a){
    if(lj_popup_close_requested(&a->popup)){
        lj_popup_close(&a->popup);a->popup_dragging=a->popup_resizing=a->popup_mouse=0;a->dirty=1;
        free(a->popup_pixels);a->popup_pixels=NULL;a->popup_pixel_cap=0;
    }else if(lj_popup_is_open(&a->popup))a->dirty|=lj_popup_poll(&a->popup);
}
/* A 1 px line that owns its cell(s): a sixel strip in the terminal (block
 * fallback inside c_ansi), a centred 1 px rect in the window. */
static void popup_rule(App *a,lj_rect cells,uint32_t c,int vertical){
    if(a->ansi)lj_ansi_separator(cells.x,cells.y,cells.w,cells.h,c,vertical);
    else hairline(cells,c,vertical);
}
/* A horizontal track: `fill` px of `on` then `off` to the end, 2 px tall in
 * the window, one strip per colour in the terminal. */
static void popup_track(App *a,int x,int y,int w,int fill,uint32_t on,uint32_t off){
    if(w<=0)return;fill=clamp(fill,0,w);
    if(a->ansi){
        int fc=fill/CELLW*CELLW;                       /* strips are cell-granular */
        if(fc>0)lj_ansi_separator(x,y,fc,CELLH,on,0);
        if(w-fc>0)lj_ansi_separator(x+fc,y,w-fc,CELLH,off,0);
    } else {
        rect((lj_rect){x,y+CELLH/2-1,w,2},off);
        if(fill>0)rect((lj_rect){x,y+CELLH/2-1,fill,2},on);
    }
}
static void popup_controls(App *a){
    lj_rect r=a->popup_rect;lj_media *m=&a->popup.media;
    int wc=r.w/CELLW,hc=r.h/CELLH;if(hc<POPUP_MIN_ROWS||wc<POPUP_MIN_COLS)return;
    int ix=r.x+CELLW,iw=wc-2,right=r.x+r.w-CELLW;   /* inner columns */
    /* timeline row: times in cells, the track between them */
    int ay=r.y+(hc-3)*CELLH;char label[32];
    double position=lj_media_position(m);int current=(int)position,total=m->duration>0?(int)m->duration:0;
    snprintf(label,sizeof(label),"%02d:%02d",current/60,current%60);text(ix,ay,label,DIM,5*CELLW);
    if(total>0)snprintf(label,sizeof(label),"%02d:%02d",total/60,total%60);else snprintf(label,sizeof(label),"--:--");
    text(right-5*CELLW,ay,label,DIM,5*CELLW);
    int tx=ix+6*CELLW,tw=(iw-12)*CELLW;
    if(tw>=CELLW){
        int fill=m->duration>0?(int)(tw*position/m->duration):0;
        popup_track(a,tx,ay,tw,fill,CYAN,EDGE);
        if(m->duration>0){
            if(a->ansi)lj_ansi_dot(tx+clamp(fill,0,tw-1)/CELLW*CELLW,ay,a->popup_dragging?TEXT:CYAN);
            else panel((lj_rect){tx+clamp(fill,3,tw-3)-3,ay+CELLH/2-3,6,6},a->popup_dragging?TEXT:CYAN,3);
            hit(a,(lj_rect){tx,ay,tw,CELLH},H_POPUP_SEEK,0,NULL);
        }
    }
    /* controls row: glyph buttons (badge hover rule), a volume track, the way out */
    int by=ay+CELLH,x=ix,compact=iw<34;
    static const uint32_t PLAY=0x25B6,PAUSE=0x2016,BACK=0x25C0,LOUD=0x1F50A,MUTE=0x1F507;
    struct{uint32_t cp;const char *note;int kind;int cells;}c[]={
        {BACK,compact?"":"10s",H_POPUP_BACK,compact?3:5},
        {m->paused||m->ended?PLAY:PAUSE,"",H_POPUP_TOGGLE,3},
        {PLAY,compact?"":"10s",H_POPUP_FORWARD,compact?3:5},
        {m->muted?MUTE:LOUD,"",H_POPUP_MUTE,4}};
    for(int i=0;i<4;i++){
        lj_rect b={x,by,c[i].cells*CELLW,CELLH};
        int hot=hovered(a,c[i].kind,0,NULL);
        if(hot)panel(b,SURF3,4);
        glyph(b.x+CELLW/2,by,c[i].cp,hot?GREEN:TEXT,i==3?2:1);
        if(*c[i].note)text(b.x+2*CELLW,by,c[i].note,DIM,3*CELLW);
        hit(a,b,c[i].kind,0,NULL);x+=b.w;
    }
    int vw=(compact?4:8)*CELLW;
    if(x+vw+4*CELLW<=right){
        popup_track(a,x,by,vw,vw*m->volume/100,m->muted?DIM:GREEN,EDGE);
        hit(a,(lj_rect){x,by,vw,CELLH},H_POPUP_VOLUME,0,NULL);
        snprintf(label,sizeof(label),"%d%%",m->volume);text(x+vw+CELLW/2,by,label,DIM,4*CELLW);x+=vw+4*CELLW;
    }
    /* the right end: an error keeps its amber line; otherwise the way out */
    const char *st=lj_popup_status(&a->popup);int bad=strstr(st,"unavailable")||strstr(st,"failed")||strstr(st,"could not")||strstr(st,"exceed");
    const char *tail=bad?st:"Esc closes";int tl=label_cells(tail)*CELLW;
    if(right-CELLW-tl>=x+CELLW)text(right-CELLW-tl,by,tail,bad?AMBER:DIM,tl);
}
static void popup_control(App *a,Hit *h,int x){
    lj_popup *p=&a->popup;
    switch(h->kind){
    case H_POPUP_TOGGLE:lj_popup_toggle(p);break;
    case H_POPUP_BACK:lj_popup_seek(p,lj_media_position(&p->media)-10);break;
    case H_POPUP_FORWARD:lj_popup_seek(p,lj_media_position(&p->media)+10);break;
    case H_POPUP_MUTE:lj_popup_mute(p);break;
    case H_POPUP_VOL_DOWN:lj_popup_volume(p,p->media.volume-10);break;
    case H_POPUP_VOL_UP:lj_popup_volume(p,p->media.volume+10);break;
    case H_POPUP_VOLUME:if(h->r.w>1)lj_popup_volume(p,(int)(100.0*(x-h->r.x)/(h->r.w-1)+.5));break;
    case H_POPUP_RESOLUTION:{
        int height=p->media.decode_h==360?480:p->media.decode_h==480?720:360;
        if(!lj_media_resolution(&p->media,height))toast(a,"Could not change video resolution");
        break;
    }
    case H_POPUP_SEEK:
        if(h->r.w>1&&p->media.duration>0)lj_popup_seek(p,(double)(x-h->r.x)/(h->r.w-1)*p->media.duration);
        break;
    }
}
static void popup_render(App *a){
    if(!lj_popup_is_open(&a->popup))return;
    popup_geometry(a);lj_rect r=a->popup_rect;lj_media *m=&a->popup.media;
    if(a->ansi)lj_ansi_cover(r.x,r.y,r.w,r.h);
    int layer=a->hit_layer;a->hit_layer=layer+2;
    int wc=r.w/CELLW,hc=r.h/CELLH,ix=r.x+CELLW,iw=wc-2,right=r.x+r.w-CELLW;
    panel(r,PANEL,0);hit(a,r,H_POPUP_BODY,0,NULL);
    /* frame: hot while dragging/resizing or with the pointer on the handle */
    int hot=a->popup_dragging||a->popup_resizing||hovered(a,H_POPUP_DRAG,0,NULL)||hovered(a,H_POPUP_RESIZE,0,NULL);
    uint32_t line=hot?CYAN:EDGE;
    popup_rule(a,(lj_rect){r.x,r.y,r.w,CELLH},line,0);
    popup_rule(a,(lj_rect){r.x,r.y+r.h-CELLH,r.w,CELLH},line,0);
    popup_rule(a,(lj_rect){r.x,r.y+CELLH,CELLW,r.h-2*CELLH},line,1);
    popup_rule(a,(lj_rect){right,r.y+CELLH,CELLW,r.h-2*CELLH},line,1);
    /* title row: state glyph · title · dim state · chip · close */
    int ty=r.y+CELLH;
    hit(a,popup_title_rect(a),H_POPUP_DRAG,0,NULL);
    uint32_t stcp=m->ended?0x25A0:m->paused?0x2016:m->pixels?0x25B6:0x25CC;
    glyph(ix,ty,stcp,AMBER,1);
    const char *title=m->title[0]?m->title:"Video";
    int chip=iw>=40,close_x=r.x+r.w-2*CELLW,chip_x=close_x-CELLW-5*CELLW;
    int title_w=(chip?chip_x:close_x)-CELLW-(ix+2*CELLW);
    char line_buf[256];const char *st=lj_popup_status(&a->popup);
    if(m->paused||!m->pixels||m->ended)snprintf(line_buf,sizeof(line_buf),"%s · %s",title,st);else snprintf(line_buf,sizeof(line_buf),"%s",title);
    if(title_w>0)text(ix+2*CELLW,ty,line_buf,TEXT,title_w);
    if(chip){
        char res[16];snprintf(res,sizeof(res),"%dp",m->decode_h);
        lj_rect cr={chip_x,ty,5*CELLW,CELLH};panel(cr,hovered(a,H_POPUP_RESOLUTION,0,NULL)?SURF3:SURF2,4);
        text(cr.x+CELLW/2,ty,res,DIM,4*CELLW);hit(a,cr,H_POPUP_RESOLUTION,0,NULL);
    }
    {lj_rect cr=popup_close_rect(a);int h2=hovered(a,H_POPUP_CLOSE,0,NULL);if(h2)panel(cr,SURF3,4);
     glyph(cr.x,cr.y,lj_theme_codepoint(LJ_THEME_CLOSE),h2?GREEN:DIM,1);hit(a,cr,H_POPUP_CLOSE,0,NULL);}
    popup_rule(a,(lj_rect){ix,ty+CELLH,iw*CELLW,CELLH},EDGE,0);
    /* video body between the two rules */
    lj_rect body={ix,r.y+3*CELLH,iw*CELLW,(hc-7)*CELLH};
    if(body.w>0&&body.h>0){
        rect(body,BG);
        if(m->pixels){
            if(a->ansi){
                /* The wrapper owns decoding/retry; present its draw result in
                 * the terminal, with storage valid until present() completes. */
                size_t count=(size_t)m->w*m->h;
                if(count>a->popup_pixel_cap){
                    uint32_t *pixels=realloc(a->popup_pixels,count*sizeof(*pixels));
                    if(pixels){a->popup_pixels=pixels;a->popup_pixel_cap=count;}
                }
                if(count<=a->popup_pixel_cap){
                    memcpy(a->popup_pixels,m->pixels,count*sizeof(*m->pixels));
                    /* Fit before the cell presenter scales: resizing must not stretch video. */
                    double scale=(double)body.w/m->w;if((double)body.h/m->h<scale)scale=(double)body.h/m->h;
                    int w=clamp((int)(m->w*scale)/CELLW,1,body.w/CELLW)*CELLW;
                    int h=clamp((int)(m->h*scale)/CELLH,1,body.h/CELLH)*CELLH;
                    int x=body.x+((body.w-w)/2/CELLW)*CELLW,y=body.y+((body.h-h)/2/CELLH)*CELLH;
                    lj_ansi_image(x,y,w,h,a->popup_pixels,m->w,m->h);
                }
            }else lj_popup_draw(&a->popup,lj_render_pixels(),a->w,a->h,body.x,body.y,body.w,body.h);
        }else text(body.x+CELLW,body.y+CELLH,st[0]?st:"Loading video…",DIM,body.w-2*CELLW);
    }
    popup_rule(a,(lj_rect){ix,r.y+(hc-4)*CELLH,iw*CELLW,CELLH},EDGE,0);
    popup_controls(a);
    /* the grip: the corner frame cell */
    {lj_rect g=popup_resize_rect(a);
     if(a->ansi)glyph(g.x,g.y,lj_theme_codepoint(LJ_THEME_RESIZE),hot?CYAN:DIM,1);
     else for(int i=0;i<7;i++)rect((lj_rect){g.x+g.w-CELLW/2-i,g.y+g.h-CELLH/2-(6-i),1,7-i},hot?CYAN:DIM);
     hit(a,g,H_POPUP_RESIZE,0,NULL);}
    a->hit_layer=layer;
}
/* Intercept covered pointer events before dock dividers and terminal mouse
 * forwarding. The rest of the workspace stays usable outside the overlay. */
static int popup_event(App *a,SDL_Event *e){
    if(e->type==SDL_MOUSEBUTTONUP&&a->popup_mouse){
        a->popup_mouse=a->popup_dragging=a->popup_resizing=0;a->dirty=1;return 1;
    }
    if(!lj_popup_is_open(&a->popup))return 0;
    popup_geometry(a);lj_rect r=a->popup_rect;
    if(e->type==SDL_MOUSEMOTION){
        a->mousex=e->motion.x;a->mousey=e->motion.y;
        if(a->popup_resizing){
            a->popup_rect.w=clamp(a->mousex+a->popup_dx-r.x,POPUP_MIN_COLS*CELLW,a->w-r.x);
            a->popup_rect.h=clamp(a->mousey+a->popup_dy-r.y,POPUP_MIN_ROWS*CELLH,a->h-r.y);
            popup_geometry(a);a->dirty=1;return 1;
        }
        if(a->popup_dragging){
            a->popup_rect.x=a->mousex-a->popup_dx;a->popup_rect.y=a->mousey-a->popup_dy;
            popup_geometry(a);a->dirty=1;return 1;
        }
        if(inside(r,a->mousex,a->mousey)){a->dirty=1;return 1;}
    }
    if(e->type==SDL_MOUSEBUTTONDOWN&&inside(r,e->button.x,e->button.y)){
        a->mousex=e->button.x;a->mousey=e->button.y;a->popup_mouse=1;
        if(e->button.button==SDL_BUTTON_LEFT){
            if(inside(popup_close_rect(a),a->mousex,a->mousey))lj_popup_request_close(&a->popup);
            else if(inside(popup_resize_rect(a),a->mousex,a->mousey)){
                a->popup_resizing=1;a->popup_dx=r.x+r.w-a->mousex;a->popup_dy=r.y+r.h-a->mousey;
            }
            else if(a->mousey<r.y+2*CELLH){a->popup_dragging=1;a->popup_dx=a->mousex-r.x;a->popup_dy=a->mousey-r.y;}
            else for(int i=a->nhits-1;i>=0;i--){
                Hit *h=&a->hits[i];
                if(((h->kind>=H_POPUP_TOGGLE&&h->kind<=H_POPUP_SEEK)||h->kind==H_POPUP_RESOLUTION||h->kind==H_POPUP_VOLUME)&&inside(h->r,a->mousex,a->mousey)){
                    popup_control(a,h,a->mousex);break;
                }
            }
        }
        a->dirty=1;return 1;
    }
    return e->type==SDL_MOUSEWHEEL&&inside(r,a->mousex,a->mousey);
}
/* ── the folder browser popup ─────────────────────────────────────────────
 * ⚠ A FOLDER PICKER IS A WINDOW YOU CAN BROWSE IN. the operator, 2026-09-16: "improve
 * file browser popup window with proper folder browser controls and folder
 * icons, I can't really browse in a room folder selector. Make it a proper
 * popup floating window." The old picker was four rows inside the New Room form
 * with PREV/NEXT paging, only while FOLDER had focus, where Up/Down moved the
 * FORM field and a single click jumped into a folder. This is a floating,
 * draggable, resizable window over the dialog, modal while open, laid out on
 * the media popup's cell grid (frame lines are popup_rule, so a sixel host
 * draws them as strips). Rows: 0 frame · 1 title · 2 toolbar · 3 breadcrumb ·
 * 4 rule · 5.. list · rule · PATH · footer · frame. The folder on show is
 * room_folder_browse; FOLDER changes only on USE FOLDER (or Ctrl+Enter). */
#define FB_MIN_COLS 40
#define FB_MIN_ROWS 12
#define FB_FOLDER_ICON "\U0001F4C1"
static void folder_browser_geometry(App *a){
    lj_rect *r=&a->fb_rect;
    int cols=clamp(a->w/CELLW,1,INT_MAX),rows=clamp(a->h/CELLH,1,INT_MAX);
    if(!a->fb_placed){
        {int want=cols*3/5;if(want<64)want=cols-4<64?cols-4:64;r->w=clamp(want,FB_MIN_COLS,96)*CELLW;}r->h=clamp(rows*7/10,FB_MIN_ROWS,36)*CELLH;
        r->x=(a->w-r->w)/2;r->y=(a->h-r->h)/2;a->fb_placed=1;
    }
    r->w=clamp(r->w/CELLW,cols<FB_MIN_COLS?cols:FB_MIN_COLS,cols)*CELLW;
    r->h=clamp(r->h/CELLH,rows<FB_MIN_ROWS?rows:FB_MIN_ROWS,rows)*CELLH;
    r->x=clamp(r->x,0,a->w>r->w?a->w-r->w:0)/CELLW*CELLW;
    r->y=clamp(r->y,0,a->h>r->h?a->h-r->h:0)/CELLH*CELLH;
}
static lj_rect folder_browser_title_rect(App *a){lj_rect r=a->fb_rect;return (lj_rect){r.x,r.y,r.w-2*CELLW,2*CELLH};}
static lj_rect folder_browser_close_rect(App *a){lj_rect r=a->fb_rect;return (lj_rect){r.x+r.w-2*CELLW,r.y+CELLH,CELLW,CELLH};}
static lj_rect folder_browser_grip_rect(App *a){lj_rect r=a->fb_rect;return (lj_rect){r.x+r.w-CELLW,r.y+r.h-CELLH,CELLW,CELLH};}
static void folder_browser_open(App *a){
    /* Start where FOLDER points when that is a folder, else the project, else HOME. */
    char resolved[PATH_MAX];struct stat st;const char *start=a->room_folder;
    if(!start[0]||!realpath(start,resolved)||stat(resolved,&st)||!S_ISDIR(st.st_mode)){
        start=a->project[0]?a->project:getenv("HOME");if(!start)start="/";
    }else start=resolved;
    copy(a->room_folder_browse,sizeof a->room_folder_browse,start);
    a->room_folder_listed[0]=0;a->fb_open=a->fb_follow=1;a->fb_sel=a->fb_scroll=a->fb_edit=0;a->fb_click=-1;
    a->fb_placed=a->fb_dragging=a->fb_resizing=a->fb_mouse=0;
    room_folder_read(a);a->dirty=1;
}
static void folder_browser_close(App *a){a->fb_open=a->fb_edit=a->fb_dragging=a->fb_resizing=a->fb_mouse=0;a->dirty=1;}
static void folder_browser_use(App *a){
    room_folder_read(a);
    if(!a->room_folder_base[0]){toast(a,"Choose an available folder first");return;}
    copy(a->room_folder,sizeof(a->room_folder),a->room_folder_base);
    folder_browser_close(a);a->room_field=RF_PURPOSE;
}
/* The path as clickable segments, "/ › run › media › …", eliding from the LEFT
 * so the folder you are in is always whole. A segment's hit index is its
 * prefix length in room_folder_base. */
static void folder_browser_crumbs(App *a,int x,int y,int w){
    const char *base=a->room_folder_base;
    if(!base[0]){text(x,y,a->room_folder_browse,AMBER,w);return;}
    int start[128],end[128],n=0;start[n]=0;end[n]=1;n++;
    for(int i=1;base[i]&&n<128;){int j=i;while(base[j]&&base[j]!='/')j++;if(j>i){start[n]=i;end[n]=j;n++;}i=base[j]?j+1:j;}
    int sep=3,first=n-1,used=0;
    for(int k=n-1;k>=0;k--){char seg[256];int len=end[k]-start[k];if(len>255)len=255;memcpy(seg,base+start[k],(size_t)len);seg[len]=0;
        int cells=label_cells(seg)+(k<n-1?sep:0);if(used+cells>w/CELLW-(k?2:0)&&k<n-1)break;used+=cells;first=k;}
    int cx=x;
    if(first>0){text(cx,y,"…",DIM,CELLW);cx+=2*CELLW;}
    for(int k=first;k<n;k++){
        char seg[256];int len=end[k]-start[k];if(len>255)len=255;memcpy(seg,base+start[k],(size_t)len);seg[len]=0;
        int cells=label_cells(seg),room=(x+w-cx)/CELLW;if(room<2)break;
        char shown[256];fit(shown,sizeof shown,seg,room);cells=label_cells(shown);
        lj_rect r={cx,y,cells*CELLW,CELLH};int last=k==n-1,hot=hovered(a,H_FB_CRUMB,end[k],NULL);
        if(hot&&!last)rect(r,SURF3);
        text(cx,y,shown,last?GREEN:hot?TEXT:CYAN,r.w);
        if(!last)hit(a,r,H_FB_CRUMB,end[k],NULL);
        cx+=r.w;
        if(!last&&x+w-cx>=sep*CELLW){text(cx+CELLW,y,"›",DIM,CELLW);cx+=sep*CELLW;}
    }
}
static void folder_browser_render(App *a){
    if(!a->fb_open)return;
    folder_browser_geometry(a);room_folder_read(a);
    lj_rect r=a->fb_rect;
    if(a->ansi)lj_ansi_cover(r.x,r.y,r.w,r.h);
    int layer=a->hit_layer;a->hit_layer=layer+4;
    /* modal: the whole screen is this layer's, so nothing of the dialog below is hovered or clicked */
    hit(a,(lj_rect){0,0,a->w,a->h},H_FB_BODY,0,NULL);
    int wc=r.w/CELLW,hc=r.h/CELLH,ix=r.x+CELLW,iw=wc-2,right=r.x+r.w-CELLW;
    panel(r,PANEL,0);
    int hot=a->fb_dragging||a->fb_resizing;uint32_t line=hot?CYAN:EDGE;
    popup_rule(a,(lj_rect){r.x,r.y,r.w,CELLH},line,0);
    popup_rule(a,(lj_rect){r.x,r.y+r.h-CELLH,r.w,CELLH},line,0);
    popup_rule(a,(lj_rect){r.x,r.y+CELLH,CELLW,r.h-2*CELLH},line,1);
    popup_rule(a,(lj_rect){right,r.y+CELLH,CELLW,r.h-2*CELLH},line,1);
    /* 1 title */
    int y=r.y+CELLH;
    text(ix,y,FB_FOLDER_ICON " CHOOSE ROOM FOLDER",GREEN,(iw-2)*CELLW);
    {lj_rect cr=folder_browser_close_rect(a);int h2=hovered(a,H_FB_CLOSE,0,NULL);if(h2)panel(cr,SURF3,4);
     glyph(cr.x,cr.y,lj_theme_codepoint(LJ_THEME_CLOSE),h2?GREEN:DIM,1);hit(a,cr,H_FB_CLOSE,0,NULL);}
    /* 2 toolbar: every button is its label plus button()'s padding */
    y+=CELLH;int x=ix;
    button(a,(lj_rect){x,y,6*CELLW,CELLH},"▴ UP",H_ROOM_FOLDER_UP,0,NULL,0);x+=7*CELLW;
    button(a,(lj_rect){x,y,8*CELLW,CELLH},"⌂ HOME",H_FB_HOME,0,NULL,0);x+=9*CELLW;
    if(a->project[0]){button(a,(lj_rect){x,y,9*CELLW,CELLH},"PROJECT",H_FB_PROJECT,0,NULL,0);x+=10*CELLW;}
    if(right-10*CELLW>=x)button(a,(lj_rect){right-10*CELLW,y,10*CELLW,CELLH},a->fb_hidden?"● HIDDEN":"○ HIDDEN",H_FB_HIDDEN,0,NULL,a->fb_hidden);
    /* 3 breadcrumb, 4 rule */
    y+=CELLH;folder_browser_crumbs(a,ix,y,iw*CELLW);
    y+=CELLH;popup_rule(a,(lj_rect){ix,y,iw*CELLW,CELLH},EDGE,0);
    /* the list */
    int top=y+CELLH,vis=hc-9;if(vis<1)vis=1;a->fb_rows=vis;
    int n=a->room_folder_count;
    a->fb_sel=clamp(a->fb_sel,0,n?n-1:0);
    if(a->fb_follow){if(a->fb_sel<a->fb_scroll)a->fb_scroll=a->fb_sel;if(a->fb_sel>=a->fb_scroll+vis)a->fb_scroll=a->fb_sel-vis+1;}
    a->fb_scroll=clamp(a->fb_scroll,0,n>vis?n-vis:0);
    int lw=(iw-1)*CELLW;
    for(int row=0;row<vis;row++){
        int i=a->fb_scroll+row;lj_rect rr={ix,top+row*CELLH,lw,CELLH};
        if(i>=n){
            if(row==0&&!n)text(ix+CELLW,rr.y,a->room_folder_error[0]?a->room_folder_error:a->fb_hidden?"No subfolders here":"No subfolders here · Ctrl+H shows hidden ones",
                               a->room_folder_error[0]?AMBER:DIM,lw-2*CELLW);
            continue;
        }
        int sel=i==a->fb_sel,hov=hovered(a,H_ROOM_FOLDER_ENTRY,i,NULL)||hovered(a,H_FB_OPEN,i,NULL);
        rect(rr,sel?SURF2:hov?SURF3:PANEL);
        if(sel)glyph(rr.x,rr.y,0x258E,CYAN,1);
        char name[256],label[300];fit(name,sizeof name,a->room_folder_entries[i],lw/CELLW-6);
        snprintf(label,sizeof label,FB_FOLDER_ICON " %s",name);
        text(rr.x+CELLW,rr.y,label,sel||hov?TEXT:DIM,lw-3*CELLW);
        hit(a,rr,H_ROOM_FOLDER_ENTRY,i,NULL);
        lj_rect ch={rr.x+rr.w-2*CELLW,rr.y,2*CELLW,CELLH};
        glyph(ch.x+CELLW,ch.y,0x203A,hovered(a,H_FB_OPEN,i,NULL)?GREEN:sel?CYAN:DIM,1);
        hit(a,ch,H_FB_OPEN,i,NULL);
    }
    if(n>vis){   /* scrollbar in the list's last column */
        int sx=ix+lw,th=clamp(vis*vis/n,1,vis),tp=(vis-th)*a->fb_scroll/(n-vis);
        for(int row=0;row<vis;row++){int on=row>=tp&&row<tp+th;
            glyph(sx,top+row*CELLH,lj_theme_codepoint(on?LJ_THEME_SCROLL_THUMB:LJ_THEME_SCROLL_TRACK),on?CYAN:EDGE,1);}
    }
    y=top+vis*CELLH;popup_rule(a,(lj_rect){ix,y,iw*CELLW,CELLH},EDGE,0);
    /* PATH: shows the folder; typing "/" or "~" (or a click, or Ctrl+L) edits it */
    y+=CELLH;text(ix,y,"PATH",DIM,5*CELLW);
    {lj_rect box={ix+5*CELLW,y,(iw-5)*CELLW,CELLH};
     panel(box,a->fb_edit?SURF2:BG,0);if(a->fb_edit)border(box,GREEN);
     const char *val=a->fb_edit?a->fb_typed:a->room_folder_base[0]?a->room_folder_base:a->room_folder_browse;
     int cells=box.w/CELLW-2,len=label_cells(val);const char *shown=val;char tail[520];
     if(len>cells&&cells>1){const char *q=val;int skip=len-(cells-1);while(*q&&skip>0)skip-=lj_render_cells_measure(nextcp(&q));snprintf(tail,sizeof tail,"…%s",q);shown=tail;len=cells;}
     text(box.x+CELLW,y,shown,a->fb_edit?TEXT:DIM,box.w-2*CELLW);
     if(a->fb_edit)glyph(box.x+CELLW+len*CELLW,y,lj_theme_codepoint(LJ_THEME_PROMPT_CURSOR),GREEN,1);
     hit(a,box,H_FB_PATH,0,NULL);}
    /* footer: count and the keys on the left, the way out on the right */
    y+=CELLH;
    button(a,(lj_rect){right-12*CELLW,y,12*CELLW,CELLH},"USE FOLDER",H_ROOM_FOLDER_USE,0,NULL,1);
    button(a,(lj_rect){right-22*CELLW,y,9*CELLW,CELLH},"CANCEL",H_FB_CANCEL,0,NULL,0);
    {char status[200],shown[200];
     snprintf(status,sizeof status,"%d folder%s%s · Enter opens · Ctrl+Enter uses this folder",n,n==1?"":"s",a->room_folder_truncated?" (first 500)":"");
     fit(shown,sizeof shown,status,iw-23);text(ix,y,shown,DIM,(iw-23)*CELLW);}
    {lj_rect g=folder_browser_grip_rect(a);
     if(a->ansi)glyph(g.x,g.y,lj_theme_codepoint(LJ_THEME_RESIZE),hot?CYAN:DIM,1);
     else for(int i=0;i<7;i++)rect((lj_rect){g.x+g.w-CELLW/2-i,g.y+g.h-CELLH/2-(6-i),1,7-i},hot?CYAN:DIM);}
    a->hit_layer=layer;
}
static int folder_browser_key(App *a,SDL_Keycode k,int ctrl){
    if(!a->fb_open)return 0;
    room_folder_read(a);int n=a->room_folder_count,page=a->fb_rows>1?a->fb_rows-1:1;
    a->dirty=1;
    if(a->fb_edit){
        if(k==SDLK_ESCAPE)a->fb_edit=0;
        else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){if(room_folder_go_path(a,a->fb_typed))a->fb_edit=0;}
        else if(k==SDLK_BACKSPACE){size_t l=strlen(a->fb_typed);if(l){do{l--;}while(l&&((unsigned char)a->fb_typed[l]&0xc0)==0x80);a->fb_typed[l]=0;}}
        else if(ctrl&&k==SDLK_u)a->fb_typed[0]=0;
        return 1;
    }
    if(k==SDLK_ESCAPE)folder_browser_close(a);
    else if((k==SDLK_RETURN||k==SDLK_KP_ENTER)&&ctrl)folder_browser_use(a);
    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_RIGHT){if(n)room_folder_go(a,a->room_folder_entries[a->fb_sel]);}
    else if(k==SDLK_LEFT||k==SDLK_BACKSPACE){if(a->room_folder_base[0])room_folder_go(a,"..");}
    else if(k==SDLK_DOWN||k==SDLK_UP||k==SDLK_PAGEDOWN||k==SDLK_PAGEUP||k==SDLK_HOME||k==SDLK_END){
        int step=k==SDLK_DOWN?1:k==SDLK_UP?-1:k==SDLK_PAGEDOWN?page:k==SDLK_PAGEUP?-page:0;
        a->fb_sel=k==SDLK_HOME?0:k==SDLK_END?n-1:a->fb_sel+step;
        a->fb_sel=clamp(a->fb_sel,0,n?n-1:0);a->fb_follow=1;a->fb_click=-1;
    }
    else if(ctrl&&k==SDLK_h){a->fb_hidden=!a->fb_hidden;a->room_folder_listed[0]=0;a->fb_follow=1;}
    else if(ctrl&&k==SDLK_l){a->fb_edit=1;copy(a->fb_typed,sizeof a->fb_typed,a->room_folder_base);}
    return 1;   /* modal: no key reaches the form below */
}
static int folder_browser_text(App *a,const char *value){
    if(!a->fb_open)return 0;
    for(const unsigned char *p=(const unsigned char*)value;*p;p++)if(*p<32||*p==127)return 1;
    a->dirty=1;
    if(!a->fb_edit){
        if(value[0]=='/'||value[0]=='~'){a->fb_edit=1;a->fb_typed[0]=0;}
        else if(value[0]){   /* type-ahead: the next folder starting with this letter */
            room_folder_read(a);int n=a->room_folder_count,c=tolower((unsigned char)value[0]);
            for(int d=1;d<=n;d++){int i=(a->fb_sel+d)%n;if(tolower((unsigned char)a->room_folder_entries[i][0])==c){a->fb_sel=i;a->fb_click=-1;a->fb_follow=1;break;}}
            return 1;
        }
    }
    size_t have=strlen(a->fb_typed),add=strlen(value);
    if(have+add>=sizeof a->fb_typed){toast(a,"That path is too long");return 1;}
    memcpy(a->fb_typed+have,value,add+1);
    return 1;
}
/* Pointer events the hit list cannot express: move, resize, wheel, and the
 * modal rule. Clicks on controls return 0 and go through activate(). */
static int folder_browser_event(App *a,SDL_Event *e){
    if(e->type==SDL_MOUSEBUTTONUP&&a->fb_mouse){a->fb_mouse=a->fb_dragging=a->fb_resizing=0;a->dirty=1;return 1;}
    if(!a->fb_open)return 0;
    folder_browser_geometry(a);lj_rect r=a->fb_rect;
    if(e->type==SDL_MOUSEMOTION){
        a->mousex=e->motion.x;a->mousey=e->motion.y;
        if(a->fb_resizing){
            a->fb_rect.w=clamp(a->mousex+a->fb_dx-r.x,FB_MIN_COLS*CELLW,a->w-r.x);
            a->fb_rect.h=clamp(a->mousey+a->fb_dy-r.y,FB_MIN_ROWS*CELLH,a->h-r.y);
            folder_browser_geometry(a);a->dirty=1;return 1;
        }
        if(a->fb_dragging){a->fb_rect.x=a->mousex-a->fb_dx;a->fb_rect.y=a->mousey-a->fb_dy;folder_browser_geometry(a);a->dirty=1;return 1;}
        return 0;
    }
    if(e->type==SDL_MOUSEWHEEL){
        int dy=e->wheel.y;if(e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED)dy=-dy;
        if(inside(r,a->mousex,a->mousey)){a->fb_scroll-=dy*3;a->fb_follow=0;a->dirty=1;}
        return 1;
    }
    if(e->type==SDL_MOUSEBUTTONDOWN){
        a->mousex=e->button.x;a->mousey=e->button.y;a->dirty=1;
        if(!inside(r,a->mousex,a->mousey)||e->button.button!=SDL_BUTTON_LEFT)return 1;
        if(inside(folder_browser_close_rect(a),a->mousex,a->mousey)){folder_browser_close(a);return 1;}
        if(inside(folder_browser_grip_rect(a),a->mousex,a->mousey)){a->fb_resizing=a->fb_mouse=1;a->fb_dx=r.x+r.w-a->mousex;a->fb_dy=r.y+r.h-a->mousey;return 1;}
        if(inside(folder_browser_title_rect(a),a->mousex,a->mousey)){a->fb_dragging=a->fb_mouse=1;a->fb_dx=a->mousex-r.x;a->fb_dy=a->mousey-r.y;return 1;}
        return 0;
    }
    return 0;
}
/* ── the block list ───────────────────────────────────────────────────────
 * ⚠ These are the SAME bodies the strcmp chain ran, moved verbatim and not
 * rewritten; the locals each one used are re-bound from the context below so
 * the moved text did not have to be touched at all. `label` was one scratch
 * buffer shared by the whole of render(); every arm writes it before reading
 * it, so a per-block buffer is the same program. */
typedef struct {
    lj_rect r; lj_dock_tile *tile;
    int focused, head, headtext, headbtn, compact_session;
} BlockCtx;
static void draw_block_room(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext; char label[512];
            Session *peer=is_dm(a)?session(a,conversation(a)):NULL;Room *chatroom=room_by_id(a,conversation(a));
            if(is_dm(a))snprintf(label,sizeof(label),"%s %s / %.8s",lj_theme_glyph(LJ_THEME_LOCK),peer?peer->agent:"Agent",conversation(a));
            else snprintf(label,sizeof(label),"%s %s",lj_theme_glyph(LJ_THEME_CHAT),chatroom?chatroom->name:"Public lobby");
            text(r.x+12,headtext,label,GREEN,r.w-35);draw_room(a,r);return;
}
static void draw_block_git(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext;
            text(r.x+12,headtext,"Git DAG",GREEN,r.w-55);
            lj_rect body={r.x+8,r.y+42,r.w-16,r.h-50};hit(a,body,H_GIT_BODY,0,NULL);
            a->git_scroll=clamp(a->git_scroll,0,a->git_height>body.h?a->git_height-body.h:0);
            a->git_height=lj_git_dag_draw(jget(a->review_panel,"git_dag"),body.x,body.y,scrollbar_body(body).w,body.h,a->git_scroll,a->ansi);
            draw_scrollbar(body,a->git_scroll,a->git_height,body.h,scrollbar_active(a,"git",body));
}
static void draw_block_load(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int head=c->head; int headtext=c->headtext;
            /* ⚠ A PLOT OF A MISSING SOURCE IS A LIE. c_metrics reports NAN for
             * anything it cannot read — a card with no NVML, or the CPU before
             * two /proc reads exist — and those rows print the reason instead
             * of a flat line at zero, which would read as an idle machine. */
            text(r.x+12,headtext,"LOAD",GREEN,r.w-13*CELLW);
            char cost[48];snprintf(cost,sizeof(cost),"%.0fus/poll",lj_metrics_poll_us());
            text(r.x+r.w-((int)strlen(cost)+3)*CELLW,headtext,cost,DIM,12*CELLW);
            lj_rect body={r.x+CELLW,r.y+head,r.w-2*CELLW,r.h-head-CELLH};
            hit(a,body,H_LOAD,0,NULL);
            int series=LJ_METRIC_COUNT,each=body.h/CELLH/series;
            if(each<1)each=1;
            for(int m=0;m<series;m++){
                int gy=body.y+m*each*CELLH;
                if(gy+CELLH>body.y+body.h)break;
                uint32_t lcol=m==LJ_METRIC_GPU||m==LJ_METRIC_GPU_MEM?CYAN:GREEN;
                /* Header and LOAD share completed 100ms means. */
                const float *hist=NULL;int n=lj_metrics_display_history(m,&hist);
                int px,py,cols,rows=each-1;
                if(rows>=1){
                    text(body.x,gy,lj_metrics_display_label(m),lcol,body.w);
                    px=body.x;py=gy+CELLH;cols=body.w/CELLW;
                }else{
                    /* ⚠ A LABEL WITH NO PLOT IS NOT A LOAD TILE. Four series in
                     * a short tile give each series ONE cell row, so rows was 0
                     * and the plot loop below was skipped ENTIRELY — the tile
                     * drew "CPU 34%" four times and not one graph. the operator looked
                     * at it and said there is no CPU graph, and he was right.
                     * When there is no room BELOW the label the sparkline goes
                     * ON the label's row, to its right. A graph, always. */
                    enum { LOAD_LABEL_CELLS = 16 };
                    text(body.x,gy,lj_metrics_display_label(m),lcol,LOAD_LABEL_CELLS*CELLW);
                    px=body.x+LOAD_LABEL_CELLS*CELLW;py=gy;
                    cols=(body.x+body.w-px)/CELLW;rows=1;
                }
                if(cols<1||rows<1||!hist)continue;
                /* Newest on the right, one column per sample, height by value.
                 * Eighth-blocks give eight levels inside a single row, so a
                 * one-row plot still shows a shape. */
                static const uint32_t BLK[8]={0x2581,0x2582,0x2583,0x2584,0x2585,0x2586,0x2587,0x2588};
                for(int c=0;c<cols;c++){
                    int idx=n-cols+c;
                    if(idx<0)continue;
                    float v=hist[idx];
                    if(v!=v)continue;                    /* NAN: draw nothing */
                    if(v<0)v=0;if(v>1)v=1;
                    float fill=v*(float)rows;
                    for(int rr=0;rr<rows;rr++){
                        float lvl=fill-(float)(rows-1-rr);
                        int yy=py+rr*CELLH;
                        if(yy+CELLH>body.y+body.h)break;
                        if(lvl>=1.0f)glyph(px+c*CELLW,yy,BLK[7],v>0.85f?RED:v>0.6f?AMBER:GREEN,1);
                        else if(lvl>0)glyph(px+c*CELLW,yy,BLK[(int)(lvl*7.99f)],v>0.85f?RED:v>0.6f?AMBER:GREEN,1);
                    }
                }
            }
}
static void draw_block_files(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int head=c->head; int headtext=c->headtext; int headbtn=c->headbtn; char label[512];
            /* The room's working folder, browsable. Read-only: a directory
             * navigates, a file does not pretend to open. Every failure prints
             * the reason the backend gave, because a blank list reads as an
             * empty folder and that is a different fact. */
            const char *root=a->files?jstr(a->files,"root"):"";
            const char *rel=a->files?jstr(a->files,"path"):"";
            /* ⚠ SAY WHEN YOU HAVE LEFT THE ROOM. Browsing is unconfined now
             * (the operator wants to walk anywhere and open a room on a folder), so
             * the one thing the header must never lose is whether what you are
             * looking at is still this room's folder. */
            /* ⚠ "IS THE ROOM ROOT" IS NOT "IS INSIDE THE ROOM". is_room_root is
             * true only when the current directory IS exactly the room folder,
             * so using it as the marker flagged every ordinary subfolder as
             * outside. The honest test is whether the current root is still
             * under room_root, which does not move as you walk down. */
            const char *rroot=a->files?jstr(a->files,"room_root"):"";
            size_t rlen=strlen(rroot);
            int inside=rroot[0]&&!strncmp(root,rroot,rlen)&&(root[rlen]==0||root[rlen]=='/');
            int marker=a->files&&!inside;
            /* ⚠ EVERY PIECE OF THE HEADER GETS ITS OWN COLUMNS. The path, the
             * outside-the-room marker and the buttons were all clipped against
             * the tile width instead of against each other, so on a half-width
             * tile they overprinted and the path read "/r↑ outside tUP". Lay
             * them out right to left and give the path what is left. */
            /* ⚠ THE PATH OUTRANKS THE MARKER. Laid out right to left with a
             * fixed 12-cell marker, a half-width tile had nothing left for the
             * path and the header said only where you were NOT. The marker
             * shrinks to a single ↑ before the path gives up a cell. */
            /* Budget in CELLS, right to left, each part with its own columns:
             *   [ FILES <path> ] [ marker ] [ UP ] [ × ]
             * Nothing is clipped against the tile width any more, which is how
             * the path ended up printed underneath the buttons. */
            int cells=r.w/CELLW;
            int up_at=cells-13, mark_w=marker?(cells>48?13:2):0;
            int mark_at=up_at-mark_w-1, path_w=(marker?mark_at:up_at)-2;
            button(a,(lj_rect){r.x+up_at*CELLW,headbtn,5*CELLW,CELLH},"UP",H_FILE_UP,0,NULL,0);
            if(marker){char up[64];snprintf(up,sizeof up,"%s%s",lj_theme_glyph(LJ_THEME_UP_ARROW),mark_w>2?" above room":"");text(r.x+mark_at*CELLW,headtext,up,AMBER,mark_w*CELLW);}
            /* Elided from the LEFT: the tail of a path identifies it. */
            char full[1024];snprintf(full,sizeof(full),"%s%s%s",root[0]?root:lj_theme_glyph(LJ_THEME_ELLIPSIS),rel[0]?"/":"",rel);
            /* ⚠ IDENTITY AND LOCATION, NEVER ONE WITHOUT THE OTHER.
             *
             * First cut dropped the PATH on a narrow tile (`if(path_w>8)` and
             * nothing else), so the browser said nothing about where it was —
             * codex's files-800.png. My repair then dropped the WORD instead,
             * on the reasoning that the path is the information and "FILES" is
             * redundant in a tile that IS the browser. codex measured that too
             * and disagreed: at ~190px the tile loses its identity and reads as
             * an anonymous list of names. He is right, and the disagreement was
             * real rather than a bug — a header that says only "…ox/liljack_app"
             * could belong to anything.
             *
             * His design, and it is better than either of ours: keep a STABLE
             * identity that costs almost nothing, and give every remaining
             * column to the path. "FILES " when there is room for the word,
             * "F:" when there is not — the tile is always identifiable and the
             * path is never absent. Same header row, UP and close untouched.
             *
             * ⚠ The path elides from the LEFT because the TAIL of a path is
             * what identifies it; eliding the tail would leave every folder
             * looking like "/home/user/projects…". */
            files_header(label,sizeof(label),full,path_w);
            if(label[0])text(r.x+CELLW,headtext,label,inside?GREEN:AMBER,path_w*CELLW);
            lj_rect body={r.x+CELLW,r.y+head,r.w-2*CELLW,r.h-head-(0)};hit(a,body,H_FILES_BODY,0,NULL);
            lj_rect viewport=body;body=scrollbar_body(body);
            const char *err=a->files?jstr(a->files,"error"):"";
            json_object *entries=a->files?jget(a->files,"entries"):NULL;
            if(!a->files){text(body.x,body.y,"Reading the folder…",DIM,body.w);return;}
            if(*err){wrapped(body.x,body.y,body.w,body.y,body.y+body.h,err,RED);return;}
            size_t n=entries?json_object_array_length(entries):0;
            if(!n){text(body.x,body.y,"This folder is empty",DIM,body.w);return;}
            int rows=body.h/CELLH;
            a->files_height=(int)n*CELLH;
            a->files_scroll=clamp(a->files_scroll,0,(int)n>rows?((int)n-rows)*CELLH:0);
            int first=a->files_scroll/CELLH;
            for(int k=first;k<(int)n&&k-first<rows;k++){
                json_object *e=json_object_array_get_idx(entries,(size_t)k);
                int dir=json_object_get_boolean(jget(e,"dir"));
                const char *name=jstr(e,"name");
                char child[1024];snprintf(child,sizeof(child),"%s%s%s",rel,rel[0]?"/":"",name);
                lj_rect row={body.x,body.y+(k-first)*CELLH,body.w,CELLH};
                {char raw[1024],cut[1024];
                 snprintf(raw,sizeof(raw),"%s %s%s%s",dir?lj_theme_glyph(LJ_THEME_DIRECTORY):" ",name,
                          json_object_get_boolean(jget(e,"symlink"))?" ":"",json_object_get_boolean(jget(e,"symlink"))?lj_theme_glyph(LJ_THEME_SYMLINK):"");
                 text(row.x,row.y,fit(cut,sizeof(cut),raw,(row.w-9*CELLW)/CELLW),
                      dir?CYAN:TEXT,row.w-9*CELLW);}
                if(!dir){char size[32];snprintf(size,sizeof(size),"%lld",(long long)json_object_get_int64(jget(e,"size")));
                    text(row.x+row.w-(int)strlen(size)*CELLW,row.y,size,DIM,(int)strlen(size)*CELLW);}
                hit(a,row,H_FILE_ENTRY,dir,child);
            }
            if(json_object_get_boolean(jget(a->files,"truncated")))
                text(body.x,body.y+body.h-CELLH,"… listing truncated",AMBER,body.w);
            draw_scrollbar(viewport,a->files_scroll,a->files_height,rows*CELLH,scrollbar_active(a,"files",viewport));
}
static void draw_block_review(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext; int headbtn=c->headbtn;
            text(r.x+12,headtext,"App review",GREEN,r.w-55);
            button(a,(lj_rect){r.x+150,headbtn,96,CELLH},"Git DAG",H_GIT,0,NULL,0);
            lj_rect body={r.x+8,r.y+42,r.w-28,r.h-50};hit(a,body,H_REVIEW_BODY,0,NULL);
            a->review_view_height=body.h;
            a->review_scroll=clamp(a->review_scroll,0,a->review_height>body.h?a->review_height-body.h:0);
            a->review_height=lj_review_draw(a->review_panel,body.x,body.y,body.w,body.h,a->review_scroll,a->ansi);
            int maxscroll=a->review_height>body.h?a->review_height-body.h:0;
            if(a->review_scroll>maxscroll){
                a->review_scroll=maxscroll;
                a->review_height=lj_review_draw(a->review_panel,body.x,body.y,body.w,body.h,a->review_scroll,a->ansi);
            }
            draw_scrollbar((lj_rect){body.x,body.y,body.w+12,body.h},a->review_scroll,a->review_height,body.h,scrollbar_active(a,"review",body));
}
static void draw_block_session(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; lj_dock_tile *tile=c->tile; int focused=c->focused; int headtext=c->headtext; int compact_session=c->compact_session; char label[512];
        Session *s=session(a,tile->id);
        int dead=session_dead(s);
        /* ⚠ A STATE BADGE IS A TOKEN, NOT PROSE — IT MAY NEVER BE TRUNCATED.
         * ENDED used to be glued onto the end of the label and clipped with it,
         * so a narrow tile rendered "· ENDE" (codex's ended-800.png, 2026-09-10)
         * and at one cell narrower it would have read "· END". the operator asked for
         * exactly this state to be identifiable after /exit, so when the header
         * is too narrow for everything, the SESSION ID gives way and the badge
         * does not: it is drawn right-aligned against the ROLE control first,
         * and the name is clipped to whatever is left. */
        /* ⚠ AND IT MUST NOT TOUCH THE CONTROL EITHER. The first cut sized the
         * slot at 6 cells but drew with maxw badge_w+CELLW, so "· ENDED" (7
         * cells) ran from the slot into ROLE's 8px gap and rendered
         * "ENDEDROLE" — the clipping was fixed and the SPACING was not. Size
         * the slot to the string and keep a whole cell between them. */
        int badge_w=dead?7*CELLW:0;                       /* "· ENDED" is 7 cells */
        int badge_x=r.x+r.w-132-CELLW-badge_w;
        snprintf(label,sizeof(label),"%s %s  /  %.8s",s?icon(s->agent):lj_theme_glyph(LJ_THEME_DISCONNECTED),s?s->agent:"disconnected",
                 tile->id+(*tile->id?2:0));
        char shown[512];
        if(compact_session){
            /* Identity gets the row first; two cells each keep role and close
             * reachable even at the legal 140px minimum. ENDED stays whole. */
            int width=r.w-5*CELLW;
            if(dead){
                badge_x=r.x+r.w-9*CELLW;
                text(badge_x,headtext,"ENDED",lj_theme_rgb(LJ_THEME_ENDED),5*CELLW);
                width-=6*CELLW;
            }
            snprintf(label,sizeof(label),"%s",s?s->agent:"session");
            text(r.x+CELLW,headtext,fit(shown,sizeof(shown),label,width/CELLW),
                 dead?lj_theme_rgb(LJ_THEME_ENDED):focused||(s&&!strcmp(s->role,"lead"))?GREEN:TEXT,width);
        }else{
            int width=dead?badge_x-(r.x+12)-CELLW:r.w-150;
            text(r.x+12,headtext,fit(shown,sizeof(shown),label,width/CELLW),
                 dead?lj_theme_rgb(LJ_THEME_ENDED):focused?GREEN:TEXT,width);
            if(dead)text(badge_x,headtext,"· ENDED",lj_theme_rgb(LJ_THEME_ENDED),badge_w);
        }
        if(!s||(!s->managed&&!s->vt)){text(r.x+14,r.y+61,"Observed session",AMBER,r.w-28);wrapped(r.x+14,r.y+91,r.w-28,r.y+40,r.y+r.h-25,"No app-owned terminal. Open a session from the sidebar to interact here.",DIM);return;}
        if(!s->vt&&!a->demo)attach(a,s);if(!s->vt)return;
        s->grid=(lj_rect){r.x,r.y+CELLH,r.w,r.h-2*CELLH};
        resize_terminal(s,s->grid.w/CELLW,s->grid.h/CELLH);
        /* ⚠ A TILE IS A VIEWPORT, NOT THE TERMINAL'S SIZE. resize_terminal used
         * to push whatever the tile measured straight down the PTY, floor 1 —
         * so tiling nine agents in one column handed each harness an 82x5
         * terminal. Measured 2026-09-09: a second view attaching squeezed live
         * Codex/Claude/DeepSeek panes from 129x24 to 82x5, and they recovered
         * the instant it detached, because tmux sizes a window to its SMALLEST
         * client and every tile is a client. A harness TUI cannot lay out in
         * five rows; that is the operator's "two rooms at once went really bad".
         * The PTY now never goes below LJ_TERM_MIN, and a viewport smaller
         * than that shows the BOTTOM of the buffer — which is the part of a
         * terminal anyone wants to see. */
        int vrows=clamp(s->grid.h/CELLH,1,s->rows),vcols=clamp(s->grid.w/CELLW,1,s->cols);
        /* ⚠ THE VIEWPORT MUST END AT THE LAST THING WORTH SEEING, which is the
         * lower of the cursor and the last non-blank row — not the bottom of
         * the buffer and not the cursor alone. Codex's 800x560 render caught
         * both mistakes: anchored at the bottom every tile was black, and
         * anchored at the cursor a tile whose cursor sits below its output
         * showed blank rows under the text it was supposed to be displaying.
         * The scan runs from the bottom and stops at the first row with ink,
         * so it costs a row or two, not the buffer. */
        int lastink=0;
        for(int y=s->rows-1;y>=0;y--){
            const lj_cell *rw=lj_vt_row(s->vt,y);int ink=0;
            for(int x=0;x<s->cols&&!ink;x++)if(rw[x].ch&&rw[x].ch!=' ')ink=1;
            if(ink){lastink=y;break;}
        }
        int curline=clamp(lj_vt_state(s->vt,1),0,s->rows-1);
        int anchor=curline>lastink?lastink:curline;   /* whichever is HIGHER up */
        if(lastink<vrows&&curline<vrows)anchor=0;     /* it all fits: show the top */
        s->view_row=anchor>=vrows?anchor-vrows+1:0;
        hit(a,s->grid,H_TERMINAL,0,s->id);rect(s->grid,BG);
        if(!a->ansi)lj_render_text_trim(2);
        int selon=(a->selecting||a->sel_shown)&&!strcmp(a->selection_id,s->id),selb=0,sele=-1;uint32_t selbg=0;
        if(selon){int sh=sel_shift(a,s);selb=(a->selr0-sh)*s->cols+a->selc0;sele=(a->selr1-sh)*s->cols+a->selc1;if(selb>sele){int t=selb;selb=sele;sele=t;}
            /* Reverse video like the cursor and owkTerm: TEXT ground, BG glyphs. The
             * earlier 30% cyan-over-panel wash read as "pale blue" (the operator 2026-09-15). */
            selbg=TEXT;}
        for(int y=s->view_row;y<s->view_row+vrows;y++){
            const lj_cell *row=lj_vt_row(s->vt,y);
            /* Paint every cell's ground before any wide glyph: a continuation
             * cell's fill must not erase the glyph's right half. Keep each
             * cell's own background, including at a clipped viewport edge. */
            /* SELECTION IS A BACKGROUND, never a thin rect: a 1 px rule under the
             * row hit lj_ansi_rect's h<10 branch and became ─ glyphs that REPLACED
             * the selected text (the operator: "it's ------ crossing what was selected"),
             * the same trap as the cursor below. Paint it under the glyphs here. */
            for(int x=0;x<vcols;x++){
                uint32_t bg=row[x].bg?cellcolor(row[x].bg):0;
                if(selon){int idx=y*s->cols+x;if(idx>=selb&&idx<=sele)bg=selbg;}
                if(bg)rect((lj_rect){s->grid.x+x*CELLW,s->grid.y+(y-s->view_row)*CELLH,CELLW,CELLH},bg);
            }
            for(int x=0;x<vcols;x++){
                int xx=s->grid.x+x*CELLW,yy=s->grid.y+(y-s->view_row)*CELLH;
                if(row[x].ch&&row[x].ch!=' '){int cells=lj_render_cells_for(row[x].ch);
                    uint32_t fg=dead?lj_theme_rgb(LJ_THEME_DISABLED):cellcolor(row[x].fg);
                    if(selon){int idx=y*s->cols+x;if(idx>=selb&&idx<=sele)fg=BG;}
                    if(x+cells>vcols)glyph(xx,yy,0xfffd,fg,1);
                    else {glyph(xx,yy,row[x].ch,fg,cells);for(int m=0;m<3;m++)if(row[x].marks[m])glyph(xx,yy,row[x].marks[m],fg,cells);}}
            }
        }
        if(!a->ansi)lj_render_text_trim(0);
        if(focused&&lj_vt_state(s->vt,2)&&!lj_vt_state(s->vt,8)){
            int x=clamp(lj_vt_state(s->vt,0),0,s->cols-1),y=clamp(lj_vt_state(s->vt,1),0,s->rows-1);
            if(y>=s->view_row&&y<s->view_row+vrows&&x<vcols){
                int cx=s->grid.x+x*CELLW,cy=s->grid.y+(y-s->view_row)*CELLH;
                /* ⚠ In the TUI the cursor is REVERSE VIDEO, never a rect. A
                 * 9x2 underline hits lj_ansi_rect's h<10 branch and becomes a
                 * ─ glyph that REPLACES the character under the cursor, and
                 * spills into the next cell whenever the grid origin is not a
                 * multiple of the cell width: the operator's "double block covering
                 * what it's selecting +1 char". */
                if(render_ansi){const lj_cell *cr=lj_vt_row(s->vt,y);lj_ansi_cursor(cx,cy,clamp(cr[x].ch?lj_render_cells_for(cr[x].ch):1,1,vcols-x));}
                else {const lj_cell *cr=lj_vt_row(s->vt,y);int cells=clamp(cr[x].ch?lj_render_cells_for(cr[x].ch):1,1,vcols-x);
                    rect((lj_rect){cx,cy,CELLW*cells,CELLH},TEXT);   /* block cursor, glyph inverted */
                    if(cr[x].ch&&cr[x].ch!=' '){glyph(cx,cy,cr[x].ch,BG,cells);for(int m=0;m<3;m++)if(cr[x].marks[m])glyph(cx,cy,cr[x].marks[m],BG,cells);}}
            }
        }
        if(dead)snprintf(label,sizeof(label),"session ended · × closes this tile · the tmux session is gone");
        else if(focused)snprintf(label,sizeof(label),"%s · %d×%d · Ctrl+] focus",a->demo?"demo replay":s->connected?"attached":"disconnected",s->cols,s->rows);
        else snprintf(label,sizeof(label),"%s · %d×%d",a->demo?"demo replay":s->connected?"attached":"disconnected",s->cols,s->rows);text(r.x+(2*CELLW),r.y+r.h-(CELLH),label,s->connected||a->demo?DIM:RED,r.w-2*CELLW);
}
static void draw_block_media(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext;
        text(r.x+12,headtext,a->media.title[0]?a->media.title:"Media",TEXT,r.w-55);text(r.x+12,r.y+r.h-26,a->media.status,DIM,r.w-24);
}
/* ⚠ THE PICTURE KEEPS ITS SHAPE. Strokes are stored 0..1 over the picture, so if
 * the picture simply filled the tile body a circle drawn fullscreen became an
 * ellipse in a narrow tile ("squeezed"). The picture is the largest 16:9 rect
 * centred in the body, in every layout. */
static lj_rect canvas_picture(lj_rect body){
    if(body.w<2||body.h<2)return body;
    int w=body.w,h=(int)((long)body.w*9/16);if(h>body.h){h=body.h;w=(int)((long)body.h*16/9);}
    return (lj_rect){body.x+(body.w-w)/2,body.y+(body.h-h)/2,w,h};
}
static void canvas_source_size(int w,int h,int drawing,int *sw,int *sh){
    *sw=w;*sh=h;
    /* ⚠ NO SMALLER SOURCE WHILE DRAWING. A 160k-pixel drag preview was tried
     * against the seconds of lag; it only made the pen blurry ("resolution is
     * getting lower"): the cost follows the TARGET (physical cells), not the
     * source. The lag is fixed where it lives: the canvas reports what changed
     * (lj_canvas_take_damage), the presenter sends just those cells and waits
     * for the terminal's frame ACK (c_ansi.c, tests/test_liljack_sixel_damage.c). */
    (void)drawing;
    const double cap=1000000.0;
    if((double)*sw**sh>cap){double k=sqrt(cap/((double)*sw**sh));*sw=(int)(*sw*k);*sh=(int)(*sh*k);if(*sw<1)*sw=1;if(*sh<1)*sh=1;}
}
static void draw_block_canvas(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext;
    text(r.x+12,headtext,"Canvas",GREEN,r.w-55);
    char status[160];snprintf(status,sizeof(status),"%d strokes · drag to draw · agents: liljack room --draw",a->canvas.nstrokes);
    text(r.x+12,r.y+r.h-26,a->has_canvas?status:"Canvas closed",DIM,r.w-24);
    a->canvas_body=canvas_picture((lj_rect){r.x+10,r.y+46,r.w-20,r.h-80});hit(a,a->canvas_body,H_CANVAS_BODY,0,"canvas");
}
/* The home surface. Data comes aggregated from the backend (`dashboard` key);
 * the drawing lives in c_dash.c, which uses the shared primitives only. */
static void draw_block_dash(void *app,void *vctx){
    App *a=app; BlockCtx *c=vctx; lj_rect r=c->r; int headtext=c->headtext;
    text(r.x+12,headtext,"Dashboard",GREEN,r.w-55);
    lj_rect body={r.x+8,r.y+42,r.w-28,r.h-50};hit(a,body,H_DASH_BODY,0,NULL);
    a->dash_view_height=body.h;
    a->dash_scroll=clamp(a->dash_scroll,0,a->dash_height>body.h?a->dash_height-body.h:0);
    a->dash_height=lj_dash_draw(a->dashboard,body.x,body.y,body.w,body.h,a->dash_scroll,a->ansi);
    draw_scrollbar((lj_rect){body.x,body.y,body.w+12,body.h},a->dash_scroll,a->dash_height,body.h,scrollbar_active(a,"dash",body));
}
static const lj_block LJ_BLOCKS[]={
    {"dash",   LJ_CHROME_CLOSE,                draw_block_dash},
    /* chrome is the ordering the strcmp chain used to imply — see c_blocks.h */
    {"room",   0,                              draw_block_room},
    {"git",    LJ_CHROME_CLOSE,                draw_block_git},
    {"load",   LJ_CHROME_CLOSE,                draw_block_load},
    {"files",  LJ_CHROME_CLOSE,                draw_block_files},
    {"review", LJ_CHROME_CLOSE,                draw_block_review},
    {"media",  LJ_CHROME_CLOSE,                draw_block_media},
    {"canvas", LJ_CHROME_CLOSE,                draw_block_canvas},
    {NULL,     LJ_CHROME_CLOSE|LJ_CHROME_ROLE, draw_block_session},
};
#define LJ_BLOCK_COUNT ((int)(sizeof(LJ_BLOCKS)/sizeof(LJ_BLOCKS[0])))
static void draw_gallery(App *a);
static void render(App *a){
    scrollbar_frame++;
    /* Resolve hover BEFORE the hit list is cleared: last frame's rects are what
     * the pointer is actually over right now. */
    {
        int best=-1;long long area=0;
        for(int i=a->nhits-1;i>=0;i--){Hit *h=&a->hits[i];
            if(!inside(h->r,a->mousex,a->mousey))continue;
            long long z=(long long)h->r.w*h->r.h;
            if(best<0||z<area){best=i;area=z;}}
        if(best>=0){a->hover_kind=a->hits[best].kind;a->hover_index=a->hits[best].index;
                    copy(a->hover_id,sizeof(a->hover_id),a->hits[best].id);}
        else {a->hover_kind=0;a->hover_index=0;a->hover_id[0]=0;}
    }
    geometry(a);a->nhits=0;a->hit_layer=0;a->nlead_border=0;render_ansi=a->ansi;render_anim=a->anim;if(a->ansi)lj_ansi_begin(a->w,a->h);rect((lj_rect){0,0,a->w,a->h},BG);
    for(int i=0;i<a->nsession;i++)a->sessions[i].grid=(lj_rect){0,0,0,0};
    if(a->gallery){draw_gallery(a);return;}
    int side=a->w<1100?190:244;
    int fullscreen=a->ansi&&a->full[0];
    if(a->ansi&&!fullscreen)panel((lj_rect){0,0,a->w,2*CELLH},PANEL,0);
    else {panel((lj_rect){16,16,a->w-32,72},EDGE,16);panel((lj_rect){17,17,a->w-34,70},PANEL,15);}
    /* ⚠ NO WORDMARK, NO PATH (thin-HUI §1, the operator: "lilJack and /demo are dead
     * weight"). The Files tile already says where you are and the mark says
     * what this is. What the header carries instead is the one thing nowhere
     * else shows: the machine's load, as pictures. */
    /* ⚠ ROW 0, IN BOTH PATHS. The window path used to draw a 72px header band
     * at y=16 — under the tab row, which starts at y=20 and paints later. That
     * is why the scaled wordmark never appeared in a --screenshot (controls
     * review P3): it was drawn, then buried. The chrome is three CELL rows in
     * every mode, so the header is row 0 and everything in it is one row tall. */
    char label[512];
    int mark_s=CELLH>64?64:CELLH;                       /* one row tall, everywhere */
    int mark_x=0, mark_y=0;
    lj_rect menu_mark={mark_x,mark_y,2*CELLW,CELLH};
    /* one hover rule (item 2): the pointer lifts to SURF3; an open menu/ribbon
     * shows as the raised SURF2, never as the hover colour */
    uint32_t mark_surface=hovered(a,H_BURGER,0,NULL)?SURF3:(a->ribbon_open||a->menu_kind==MENU_LOGO)?SURF2:PANEL;
    panel(menu_mark,mark_surface,0);
    hit(a,menu_mark,H_BURGER,0,NULL);
    {
        static uint32_t mbuf[64*64];
        if(mark_s>64)mark_s=64;
        mark_argb(mbuf,mark_s);
        for(int i=0;i<mark_s*mark_s;i++)if(!(mbuf[i]>>24))mbuf[i]=0xff000000u|mark_surface;
        if(!a->ansi)blit_argb(a,mark_x,mark_y,mbuf,mark_s,mark_s);
        else {
            /* cells first: a two-cell glyph mark in the icon's own colours;
             * then the real picture on top where the terminal can show one. */
            glyph(mark_x,mark_y,lj_theme_codepoint(LJ_THEME_PROMPT),lj_theme_rgb(LJ_THEME_AMBER),1);
            glyph(mark_x+CELLW,mark_y,lj_theme_codepoint(LJ_THEME_PROMPT_CURSOR),lj_theme_rgb(LJ_THEME_AMBER),1);
            if(lj_ansi_images_available())lj_ansi_image(mark_x,mark_y,2*CELLW,CELLH,mbuf,mark_s,mark_s);
        }
    }
    {
        Header hdr=header_layout(a);
        const char *mood=a->mood[0]?a->mood:"ready";
        snprintf(label,sizeof(label),a->demo?"DEMO · %s":"%s",mood);
        text(hdr.status_x,0,label,GREEN,a->w-hdr.status_x-CELLW);
        /* ── the load graphs: GPU · CPU · MEM ────────────────────────────
         * Right-anchored before the status word, each is
         * a label plus a picture, and the picture is a real plot wherever the
         * output can show one (window pixels, or sixel over the cells); on a
         * plain terminal it is the eighth-block spark, one column per sample.
         *
         * ⚠ A DESIGNED ABBREVIATION, NEVER A TRUNCATION. Three rungs still:
         * label+plot, label alone, nothing. "CPU 34" cut from "CPU 34%" is a
         * different number, so a rung that does not fit is not drawn at all.
         * ⚠ NAN IS NOT ZERO: an unavailable source keeps its "—" label and its
         * plot draws nothing — never a flat bar that reads as an idle card. */
        {
            static const uint32_t BAR[8]={0x2581,0x2582,0x2583,0x2584,0x2585,0x2586,0x2587,0x2588};
            const int which[3]={LJ_METRIC_GPU,LJ_METRIC_CPU,LJ_METRIC_MEM};
            int gx=hdr.graphs_x,gy=0;
            int spark=LJ_RIBBON_SPARK,labw=9*CELLW;
            int rung=hdr.rung,per=hdr.per*CELLW;
            for(int m=0;rung&&m<3;m++){
                const float *hist=NULL;int n=lj_metrics_paced_history(which[m],&hist);   /* paced: steps REPAINT/25 columns per refresh */
                int x=gx+m*per;
                text(x,gy,lj_metrics_display_label(which[m]),DIM,labw);  /* carries value or "—" */
                if(rung<2)continue;
                int sx=x+labw,pw=spark*CELLW,ph=CELLH;
                uint32_t *pbuf=a->header_plot[m];
                if(ph>40)ph=40;
                /* ONE PIXEL COLUMN PER BUCKET (the operator 2026-09-12; 50 ms default since 2026-09-13, lj_metrics_display_ms()): the spark
                 * is pw pixels = pw buckets, newest at the right. It used to
                 * stretch one bucket across a whole cell, which read as a
                 * character-wide bar. The cell glyph fallback below cannot do
                 * better than one glyph per cell, so each glyph carries the
                 * mean of the CELLW buckets under it. */
                /* STACKED columns (the operator 2026-09-13): part rings from the
                 * sampler, paced with `hist` so both step together. A CPU with
                 * no cpuN lines (0 parts) keeps the plain aggregate bar. */
                {
                    const float *parts[LJ_METRIC_CORES_MAX];uint32_t colours[LJ_METRIC_CORES_MAX];
                    int np=lj_metrics_part_count(which[m]);if(np>LJ_METRIC_CORES_MAX)np=LJ_METRIC_CORES_MAX;
                    static const float MEM_HUE[3]={215,175,40},GPU_HUE[2]={130,275};   /* used blue, cached teal, swap amber; compute green, memory violet */
                    for(int p=0;p<np;p++){
                        if(!lj_metrics_paced_part_history(which[m],p,&parts[p]))parts[p]=NULL;
                        colours[p]=which[m]==LJ_METRIC_CPU?core_rgb(p,np):hsv_rgb(which[m]==LJ_METRIC_MEM?MEM_HUE[p]:GPU_HUE[p],.65f,.9f);
                    }
                    if(np>0)plot_stacked_argb(pbuf,pw,ph,hist,n,parts,np,which[m]==LJ_METRIC_CPU?np:1+(which[m]==LJ_METRIC_MEM),colours);
                    else plot_argb(pbuf,pw,ph,hist,n);
                }
                int ink=0;
                for(int i=0;i<pw*ph;i++)ink|=(int)(pbuf[i]>>24);
                if(!a->ansi)blit_argb(a,sx,gy,pbuf,pw,ph);
                else {
                    for(int c=0;c<spark;c++){
                        float sum=0;int cnt=0;
                        for(int k=0;k<CELLW;k++){int idx=n-pw+c*CELLW+k;if(idx<0||!hist)continue;
                            float v=hist[idx];if(!isfinite(v))continue;sum+=v;cnt++;}
                        if(!cnt)continue;
                        float v=sum/cnt;if(v<0)v=0;if(v>1)v=1;
                        glyph(sx+c*CELLW,gy,BAR[(int)(v*7.99f)],v>0.85f?RED:v>0.6f?AMBER:GREEN,1);
                    }
                    if(ink&&lj_ansi_images_available())lj_ansi_image(sx,gy,pw,ph,pbuf,pw,ph);
                }
            }
        }
    }
    int stop=ANSI_SIDEBAR_TOP;
    int bmarg=ANSI_MARGIN;
    if(!fullscreen){
        if(a->ansi)hairline((lj_rect){0,ANSI_DOCK_TOP-1,a->w,1},EDGE,0);   /* one rule under the chrome */
        draw_tabs(a,(lj_rect){bmarg,stop,a->w-2*bmarg,CELLH});
        draw_agentbar(a,(lj_rect){bmarg,ANSI_AGENT_TOP,a->w-2*bmarg,CELLH});
    }
    for(int i=0;i<a->dock.tile_count;i++){
        lj_dock_tile *tile=&a->dock.tiles[i];lj_rect r=tile->rect;if(r.w<40||r.h<60)continue;
        /* ⚠ The header was 36px of fill on top of a 1px frame, which the cell
         * grid rounds up to THREE rows before a single line of agent output.
         * Stacked four tiles high that is a third of the screen spent on
         * chrome. In the TUI the header is exactly one row and the frame is
         * not drawn at all — a cell grid has no sub-pixel border, so `border`
         * there costs a full row per edge and buys nothing. */
        int focused=!strcmp(a->focus,tile->id),head=CELLH;
        if(render_ansi)panel(r,PANEL,0);
        else {panel(r,focused?GREEN:EDGE,10);panel((lj_rect){r.x+1,r.y+1,r.w-2,r.h-2},PANEL,9);}
        Session *lead=session(a,tile->id);
        int live_lead=render_ansi&&lead&&!strcmp(lead->role,"lead")&&!session_dead(lead);
        int lead_frame=live_lead&&r.w>=4*CELLW&&r.h>=3*CELLH
            &&a->nlead_border<(int)(sizeof(a->lead_border)/sizeof(a->lead_border[0]));
        int compact_session=lead&&r.w<26*CELLW;
        if(lead_frame){
            /* ⚠ THE RING OWNS ITS CELLS (ring-gutter-reservation, 2026-09-13).
             * Measured in both terminals: text repainted into a cell erases the
             * sixel there (WezTerm), and a sixel pixel on a text cell replaces
             * the text (owkTerm). A lead tile flush with the dock top put the
             * ring's outer strip on the TAB ROW (row 2: "_ _ _" between the
             * tabs, chips flickering) and a tile at the window edge shared its
             * own text cells with the ring ("border under filled glyphs",
             * "doubled"). So the tile's outer cell on every side is a gutter
             * that only the ring may paint: the content is inset one cell/row,
             * always on the ANSI path, images or not. The ring rect stays the
             * whole tile: dotted_border puts the placeholders on its outer
             * cells and hands lj_ansi_border the inset rect, so the sixel
             * strip lands in exactly those gutter cells. */
            if(!(getenv("LILJACK_ANSI_MARGIN")&&!strcmp(getenv("LILJACK_ANSI_MARGIN"),"0"))){
                /* uniform margin: the ring's OUTER rect is the tile grown by one cell
                 * into the window margin / divider cells; the content is NOT inset */
                lj_rect br={r.x-CELLW,r.y-CELLH,r.w+2*CELLW,r.h+2*CELLH};
                a->lead_border[a->nlead_border++]=br;lj_ansi_trace_lead(lead->id,br.x,br.y,br.w,br.h);
            } else {
            a->lead_border[a->nlead_border++]=r;lj_ansi_trace_lead(lead->id,r.x,r.y,r.w,r.h);
            r.x+=CELLW;r.w-=2*CELLW;r.y+=CELLH;r.h-=2*CELLH;
            }
        }
        panel((lj_rect){r.x,r.y,r.w,head},focused?SURF2:PANEL,0);
        /* ⚠ A HAIRLINE NEEDS ITS OWN ROW; there is no space between rows to put
         * one in. Drawn across the header it landed among the title and the ×.
         * So the element is framed by what is already there: the header bar is
         * the top, and the last row becomes a rule with the status written into
         * it (`── attached · 80x27 ─────`). Focus is shown by the WEIGHT of that
         * same line turning yellow, never by a heavier one. */
        /* The LEAD tile gets the same status rule as every other tile (the operator
         * 2026-09-13: "identical on the leader"; visual-polish-chrome item 3).
         * The !lead_frame guard dated from the ring living in the tile's own
         * outer cells; with the uniform margin the ring is in the margin/divider
         * cells and the bottom row is the tile's. */
        if(render_ansi)hairline((lj_rect){r.x,r.y+r.h-1,r.w,1},focused?GREEN:EDGE,0);
        int headtext=r.y+(0),headbtn=r.y+(0);
        /* A lead tile's header hit also covers its gutter (the row above and
         * the columns beside the inset header), so a title press on the tile's
         * top edge still arms the drag and opens the menu as on every tile. */
        {lj_rect hr={r.x,r.y,r.w-34,head};
         if(lead_frame){hr.x-=CELLW;hr.y-=CELLH;hr.w+=2*CELLW;hr.h+=CELLH;}
         hit(a,hr,H_HEADER,0,tile->id);}
        /* ONE dispatch. The kind decides what is drawn; the chrome flags decide
         * what the loop draws around it, in the order the chain used to. */
        const lj_block *blk=lj_block_for(LJ_BLOCKS,LJ_BLOCK_COUNT,tile->id);
        if(!blk)continue;               /* only reachable if the table loses its
                                         * NULL-kind default; a tile drawing
                                         * nothing beats dereferencing nothing */
        BlockCtx ctx={r,tile,focused,head,headtext,headbtn,compact_session};
        /* Layout the trailing controls once in cells, then paint and hit the
         * exact same rectangles in both presenters. */
        int role_cols=compact_session?2:10,gap_cols=compact_session?0:1;
        int widths[2]={role_cols,2};hui_cell_rect controls[2];
        int end_col=floordiv(r.x+r.w,CELLW),row=floordiv(r.y,CELLH);
        int have_role=(blk->chrome&LJ_CHROME_ROLE)!=0;
        int count=have_role?2:1;
        const int *sizes=have_role?widths:&widths[1];
        int start_col=end_col-(have_role?role_cols+gap_cols+2:2);
        if(hui_cell_row_layout(start_col,row,CELLW,CELLH,sizes,count,gap_cols,controls)){
            hui_cell_rect close=controls[count-1];
            if(blk->chrome&LJ_CHROME_CLOSE)
                button(a,(lj_rect){close.x,close.y,close.w,close.h},lj_theme_glyph(LJ_THEME_CLOSE),H_HIDE,0,tile->id,0);
            if(have_role){
                int open=a->menu_kind==MENU_ROLE&&!strcmp(a->menu_target,tile->id);
                hui_cell_rect role=controls[0];
                char role_label[40];snprintf(role_label,sizeof role_label,"%s%s",compact_session?"":"ROLE ",lj_theme_glyph(open?LJ_THEME_UP:LJ_THEME_DOWN));
                button(a,(lj_rect){role.x,role.y,role.w,role.h},
                       role_label,
                       H_CONTROLS,0,tile->id,open);
            }
        }
        blk->draw(a,&ctx);
    }
    /* ⚠ THE MEDIA TILE USED TO DRAW NOTHING IN --tui, AND THAT IS WHY THE
     * RICKROLL HAD NOWHERE TO PLAY. The blit below writes into the HUI ARGB
     * framebuffer, which the window path presented and the terminal path never
     * had. the operator removed the window, so this branch WAS the feature.
     * Terminal path: hand the decoded frame to the sixel presenter instead.
     * lj_ansi_image is a no-op on a terminal without sixel, so the cell-drawn
     * title/status underneath stays as the honest fallback. */
    if(a->has_media&&!a->team)for(int i=0;i<a->dock.tile_count;i++)if(!strcmp(a->dock.tiles[i].id,"media")){
        lj_rect r=a->dock.tiles[i].rect;
        if(!a->ansi)lj_media_blit(&a->media,lj_render_pixels(),a->w,a->h,r.x+10,r.y+46,r.w-20,r.h-80);
        else if(a->media.pixels&&a->media.w>0&&a->media.h>0)
            lj_ansi_image(r.x+10,r.y+46,r.w-20,r.h-80,a->media.pixels,a->media.w,a->media.h);
    }
    /* Canvas: rendered 1:1 at the body size, sixel-first through the same presenter. */
    if(a->has_canvas&&!a->team)for(int i=0;i<a->dock.tile_count;i++)if(!strcmp(a->dock.tiles[i].id,"canvas")){
        lj_rect r=a->dock.tiles[i].rect;lj_rect b=canvas_picture((lj_rect){r.x+10,r.y+46,r.w-20,r.h-80});
        /* ⚠ THE PRESENTER DROPS ANY SOURCE OVER 4 MiB (c_ansi.c IMAGE_COPY_CAP), and
         * the terminal keeps whatever pixels the previous, smaller image left in the
         * cells it never repainted. A fullscreen tile on the operator's 254-column owkTerm is
         * 2520x1060 = 10 MB of source, so the image was refused and the old one stayed
         * on the left: "when it's full the right side is empty". Reproduced headlessly
         * (docs/codex/reports/2026-09-21-interactive-room-survey/, fullscreen addendum).
         * Render at most 1M pixels, aspect kept; the presenter scales to the cells. */
        if(b.w>1&&b.h>1){int sw,sh;canvas_source_size(b.w,b.h,a->canvas_drawing,&sw,&sh);
            const uint32_t *px=lj_canvas_render(&a->canvas,sw,sh,BG);
            if(px&&a->canvas_drawing&&a->canvas_npts)lj_canvas_preview(&a->canvas,a->canvas_pts,a->canvas_npts,lj_canvas_author_colour("operator"),4);
            /* Only the pen's new segments changed: tell the presenter, which then
             * re-sends a few cells instead of the whole picture per mouse move. */
            float dmg[4];int damaged=lj_canvas_take_damage(&a->canvas,dmg);
            if(px&&a->ansi){
                if(lj_ansi_image(b.x,b.y,b.w,b.h,px,sw,sh)&&damaged&&!(dmg[0]<=0&&dmg[1]<=0&&dmg[2]>=1&&dmg[3]>=1))
                    lj_ansi_image_damage((int)(dmg[0]*sw),(int)(dmg[1]*sh),(int)((dmg[2]-dmg[0])*sw)+2,(int)((dmg[3]-dmg[1])*sh)+2);
            }
            else if(px){lj_media frame={.pixels=(uint32_t*)px,.w=sw,.h=sh};lj_media_blit(&frame,lj_render_pixels(),a->w,a->h,b.x,b.y,b.w,b.h);}}
    }
    /* Drawn always, so the splits are visible before you go looking for one;
     * highlighted and 3 physical pixels while hovered or dragged. */
    /* Active scope paints last, including the crossing cell at a T-junction. */
    for(int pass=0;pass<2;pass++)for(int i=0;i<a->dock.divider_count;i++){
        lj_rect d=a->dock.dividers[i].rect;
        int hot=divider_hot(a,i);if(hot!=pass)continue;
        int vertical=a->dock.nodes[a->dock.dividers[i].node].axis==1;
        if(a->ansi)lj_ansi_separator_hot(d.x,d.y,d.w,d.h,divider_colour(hot),vertical,hot);
        else {hui_divider_rect line=hui_divider_stroke((hui_divider_rect){d.x,d.y,d.w,d.h},vertical);
            if(hot){if(vertical){line.x--;line.w=3;}else{line.y--;line.h=3;}}
            rect((lj_rect){line.x,line.y,line.w,line.h},divider_colour(hot));}
    }
    /* The lead's dotted frame, emitted after every tile has painted its own
     * content so the terminal grid cannot overwrite its side columns. */
    for(int i=0;i<a->nlead_border;i++)dotted_border(a->lead_border[i],GREEN,CYAN);
    a->nlead_border=0;
    if(a->dragging)for(int i=0;i<a->dock.tile_count;i++){
        lj_dock_tile *t=&a->dock.tiles[i];if(!inside(t->rect,a->mousex,a->mousey)||!strcmp(t->id,a->drag))continue;
        lj_rect r=t->rect;lj_dock_edge edge=lj_dock_edge_at(r,a->mousex,a->mousey);
        if(!drop_preview(a,t->id,edge,&r)||r.w<2*CELLW||r.h<2*CELLH)continue;
        if(a->ansi){
            /* The hypothetical split can pass THROUGH current text. Dots only
             * decorate unowned cells, including protection for text spaces. */
            int x0=floordiv(r.x,CELLW),x1=floordiv(r.x+r.w-1,CELLW);
            int y0=floordiv(r.y,CELLH),y1=floordiv(r.y+r.h-1,CELLH);
            for(int x=x0;x<=x1;x++){lj_ansi_dot(x*CELLW,y0*CELLH,CYAN);lj_ansi_dot(x*CELLW,y1*CELLH,CYAN);}
            for(int y=y0+1;y<y1;y++){lj_ansi_dot(x0*CELLW,y*CELLH,CYAN);lj_ansi_dot(x1*CELLW,y*CELLH,CYAN);}
        }else{
            /* Pixel preview marks the target's OUTER gutter, not its text. */
            lj_rect target=t->rect;
            border((lj_rect){target.x-1,target.y-1,target.w+2,target.h+2},CYAN);
        }
    }
    if(fullscreen){
        char hint[160];snprintf(hint,sizeof(hint),"FULL SCREEN · %s · Esc or F5 returns to the workspace",a->full);
        panel((lj_rect){0,a->h-CELLH,a->w,CELLH},SURF2,0);
        text(CELLW,a->h-CELLH,hint,GREEN,a->w-CELLW);
        draw_menu(a);
        if(a->ansi)lj_ansi_effects(a->effects);
        a->dirty=0;return;
    }
    /* the footer (toast row + status row) is ONE control, H_STATUS: the whole
     * of it lifts under the pointer like every other control (item 2) */
    if(hovered(a,H_STATUS,0,NULL))panel((lj_rect){0,a->h-ANSI_FOOTER_H,a->w,ANSI_FOOTER_H},SURF3,0);
    const char *muted=heartbeat_mute(a);
    text(20,a->h-(2*CELLH),
         a->copy_pause?"Copy paused: host selection enabled; F8 resumes. Plain drag copies via OSC 52; Ctrl+Shift+C copies selection.":
         muted?muted:a->toast,muted?RED:TEXT,a->w-360);text(a->w-328,a->h-(2*CELLH),"F6 room · F7 team · F8 copy",DIM,315);
    json_object *fields=jget(a->status_ribbon,"fields");
    int ribbon_x=20,ribbon_y=a->h-(CELLH);
    text(a->w-90,ribbon_y,"Details",CYAN,80);
    /* Footer hit must match the drawn footer and stay inside the window. */
    hit(a,(lj_rect){0,a->h-ANSI_FOOTER_H,a->w,ANSI_FOOTER_H},H_STATUS,0,NULL);
    if(!fields||!json_object_array_length(fields))text(ribbon_x,ribbon_y,a->demo?"DEMO status · live fields appear in your workspace":"Status starting…",DIM,a->w-125);
    else for(size_t i=0;i<json_object_array_length(fields);i++){
        json_object *f=json_object_array_get_idx(fields,i);const char *value=jstr(f,"text");
        int remaining=a->w-125-ribbon_x;if(remaining<CELLW*4)break;
        text(ribbon_x,ribbon_y,value,json_object_get_boolean(jget(f,"stale"))?AMBER:TEXT,remaining);
        int cells=0;for(const char *q=value;*q;)cells+=lj_render_cells_measure(nextcp(&q));
        ribbon_x+=(cells+3)*CELLW;
    }
    if(a->team){
        a->nhits=0;int w=a->w<1100?a->w-60:1040,h=clamp(160+a->norder*100,260,a->h-80);lj_rect r={(a->w-w)/2,(a->h-h)/2,w,h};rect(r,PANEL);border(r,GREEN);
        text(r.x+22,r.y+18,"Team · change roles and control each agent",GREEN,r.w-78);
        text(r.x+22,r.y+47,"Make room lead hands off the room. Stop ends that agent session.",DIM,r.w-44);
        button(a,(lj_rect){r.x+r.w-43,r.y+12,30,30},lj_theme_glyph(LJ_THEME_CLOSE),H_TEAM_CLOSE,0,NULL,0);
        int fit=(r.h-140)/100;if(fit<1)fit=1;
        a->team_page=clamp(a->team_page,0,a->norder?(a->norder-1)/fit:0);
        for(int i=a->team_page*fit;i<a->norder&&i<(a->team_page+1)*fit;i++){Session *s=&a->sessions[a->order[i]];int y=r.y+80+(i-a->team_page*fit)*100;
            snprintf(label,sizeof(label),"%s %s  %.8s",icon(s->agent),s->agent,s->id+2);text(r.x+22,y+7,label,TEXT,r.w-460);
            int top_row=(y+CELLH-1)/CELLH;
            int right_col=(r.x+r.w)/CELLW-2;
            int meta_widths[2]={12,24};hui_cell_rect meta[2];
            if(hui_cell_row_layout(right_col-38,top_row,CELLW,CELLH,meta_widths,2,2,meta)){
                button(a,(lj_rect){meta[0].x,meta[0].y,meta[0].w,meta[0].h},s->role[0]?s->role:"worker",H_ROLE,0,s->id,0);
                Session *parent=session(a,s->parent);
                if(parent)snprintf(label,sizeof(label),"%s / %.8s",parent->agent,parent->id+2);
                else copy(label,sizeof(label),"under the operator");
                button(a,(lj_rect){meta[1].x,meta[1].y,meta[1].w,meta[1].h},label,H_PARENT,0,s->id,0);
            }
            int col=(r.x+2*CELLW)/CELLW,available=right_col-col;
            int count=s->room_id[0]?6:5,usable=available-(count-1),widths[6];
            for(int k=0;k<count;k++)widths[k]=usable*(k+1)/count-usable*k/count;
            hui_cell_rect row[6];
            if(hui_cell_row_layout(col,top_row+2,CELLW,CELLH,widths,count,1,row)){
                const char *names[]={"Focus","DM","Submit","Interrupt",
                    !strcmp(a->stop_confirm,s->id)?"Confirm":"Stop","Make lead"};
                const int kinds[]={H_AGENT_FOCUS,H_AGENT_DM,H_CONTROL,H_CONTROL,H_CONTROL,H_LEAD};
                const int indices[]={0,0,0,1,2,0};
                for(int k=0;k<count;k++)button(a,(lj_rect){row[k].x,row[k].y,row[k].w,row[k].h},
                    names[k],kinds[k],indices[k],s->id,0);
            }
        }
        button(a,(lj_rect){r.x+22,r.y+r.h-44,100,30},"Previous",H_TEAM_PAGE,-1,NULL,0);
        snprintf(label,sizeof(label),"Page %d / %d",a->team_page+1,a->norder?(a->norder-1)/fit+1:1);text(r.x+136,r.y+r.h-38,label,DIM,240);
        button(a,(lj_rect){r.x+r.w-122,r.y+r.h-44,100,30},"Next",H_TEAM_PAGE,1,NULL,0);
    }

    if(a->status_open){
        a->nhits=0;int w=clamp(a->w-80,300,1200);lj_rect r={(a->w-w)/2,70,w,a->h-140};rect(r,PANEL);border(r,GREEN);
        text(r.x+20,r.y+15,"lilJack status",GREEN,r.w-80);button(a,(lj_rect){r.x+r.w-45,r.y+10,30,30},lj_theme_glyph(LJ_THEME_CLOSE),H_STATUS,0,NULL,0);
        int total=0;if(fields)for(size_t i=0;i<json_object_array_length(fields);i++)total+=CELLH+wrapped_lines(jstr(json_object_array_get_idx(fields,i),"text"),(r.w-40)/CELLW)*CELLH+10;
        a->status_scroll=clamp(a->status_scroll,0,total>r.h-80?total-(r.h-80):0);
        int y=r.y+55-a->status_scroll;
        if(!fields||!json_object_array_length(fields))text(r.x+20,y,"Status starting…",DIM,r.w-40);
        else for(size_t i=0;i<json_object_array_length(fields);i++){
            json_object *f=json_object_array_get_idx(fields,i);snprintf(label,sizeof(label),"%s%s",jstr(f,"kind"),json_object_get_boolean(jget(f,"stale"))?" · stale":"");
            if(y>=r.y+45&&y+CELLH<=r.y+r.h-20)text(r.x+20,y,label,CYAN,r.w-40);y+=CELLH;
            wrapped(r.x+20,y,r.w-40,r.y+45,r.y+r.h-20,jstr(f,"text"),TEXT);y+=wrapped_lines(jstr(f,"text"),(r.w-40)/CELLW)*CELLH+10;
        }
        draw_scrollbar((lj_rect){r.x+20,r.y+55,r.w-30,r.h-80},a->status_scroll,total,r.h-80,1);
    }
    if(a->settings_open)draw_settings(a);
    if(a->about_open){
        a->nhits=0;
        int rowh=CELLH;
        int w=clamp(a->w-8*CELLW,44*CELLW,76*CELLW),h=13*rowh;
        lj_rect m={((a->w-w)/2)/CELLW*CELLW,((a->h-h)/2)/CELLH*CELLH,w,h};
        panel(m,PANEL,0);border(m,EDGE);
        int lx=m.x+CELLW,y=m.y;
        text(lx,y,"ABOUT lilJack",GREEN,w-2*CELLW);y+=rowh;
        hairline((lj_rect){m.x,y,w,1},EDGE,0);y+=rowh;
        text(lx,y,"A workspace for the agents that work this machine.",TEXT,w-2*CELLW);y+=rowh;
        text(lx,y,"C · HUI · owkterm · tmux. the operator's, and ours.",DIM,w-2*CELLW);y+=rowh;y+=rowh/2;
        text(lx,y,"BUILT BY",CYAN,w-2*CELLW);y+=rowh;
        text(lx,y,"operator     what it is for, and every call that mattered",DIM,w-2*CELLW);y+=rowh;
        text(lx,y,"codex     workspace, rooms, lifecycle, the Python side",DIM,w-2*CELLW);y+=rowh;
        text(lx,y,"claude    renderer, terminal, layout and input",DIM,w-2*CELLW);y+=rowh;
        text(lx,y,"deepseek  review, metrics, the file browser, this popup",DIM,w-2*CELLW);y+=rowh;y+=rowh/2;
        const char *st=lj_popup_is_open(&a->popup)?lj_popup_status(&a->popup)
                      :lj_popup_window_only();
        text(lx,y,st,AMBER,w-2*CELLW);y+=rowh;
        button(a,(lj_rect){m.x+w-25*CELLW,y,14*CELLW,rowh},"NEVER GONNA",H_ABOUT_PLAY,0,NULL,lj_popup_is_open(&a->popup));
        button(a,(lj_rect){m.x+w-11*CELLW,y,10*CELLW,rowh},"CLOSE",H_ABOUT_CLOSE,0,NULL,0);
    }
    if(a->room_dialog){
        /* ⚠ CREATING A ROOM IS THE WHOLE SETUP, NOT A NAME PROMPT. Name, working
         * folder, purpose, the agents WITH their roles, and a first todo — the
         * todo because it is what arms the heartbeat, the purpose and folder
         * because they are what the intro tells each agent when it starts. */
        a->nhits=0;
        int rowh=CELLH,pad=CELLW;
        int w=clamp(a->w-8*CELLW,40*CELLW,90*CELLW);
        /* Each agent+role needs 17 cells plus its button padding and gap.
         * Use more rows when necessary; never clip a role into another word. */
        int agent_space=w-2*pad-11*CELLW;
        int agent_cols=agent_space>=4*19*CELLW?4:agent_space>=2*19*CELLW?2:1;
        int agent_rows=(4+agent_cols-1)/agent_cols;
        int h=(14+agent_rows)*rowh;
        lj_rect modal={((a->w-w)/2)/CELLW*CELLW,((a->h-h)/2)/CELLH*CELLH,w,h};
        panel(modal,PANEL,0);if(!render_ansi)panel((lj_rect){modal.x+1,modal.y+1,modal.w-2,modal.h-2},PANEL,11);
        border(modal,EDGE);
        int y=modal.y+rowh,lx=modal.x+pad,fx=lx+11*CELLW,fw=modal.x+w-pad-fx;
        text(lx,modal.y,"NEW ROOM",GREEN,w-2*pad);
        const char *flabel[RF_COUNT]={"NAME","FOLDER","PURPOSE","TODO 1","TODO 2","TODO 3"};
        const char *fhint[RF_COUNT]={"what this room is called","the working folder every agent starts in · Ctrl+O browses",
                                     "one line: what this room is for","the first task — this is what arms the heartbeat",
                                     "optional","optional"};
        for(int f=0;f<RF_COUNT;f++){
            if(f==RF_TODO0){text(lx,y,"FIRST TODO",CYAN,w-2*pad);y+=rowh;}
            int on=a->room_field==f;const char *val=room_field_buf(a,f,NULL);
            text(lx,y,flabel[f],on?GREEN:DIM,11*CELLW);
            lj_rect box={fx,y,f==RF_FOLDER?fw-12*CELLW:fw,rowh};
            panel(box,on?SURF2:BG,0);if(on)border(box,GREEN);
            /* ⚠ THE FOLDER IS PICKED IN A WINDOW, NOT IN FOUR ROWS OF THE FORM
             * (the operator 2026-09-16: "I can't really browse in a room folder
             * selector"). The field stays typeable; BROWSE opens the popup. */
            if(f==RF_FOLDER)button(a,(lj_rect){fx+fw-11*CELLW,y,11*CELLW,rowh},FB_FOLDER_ICON " BROWSE",H_ROOM_FOLDER_BROWSE,0,NULL,0);
            /* ⚠ AN INPUT FIELD SCROLLS TO ITS CARET. Measured 2026-09-12 by the
             * popup fixture at 800x560: a long room NAME was cut mid-word at the
             * box edge with the caret drawn at the cut, so typing past the width
             * showed nothing. Show the TAIL that fits when the value overflows —
             * what you are typing is what you see — and mark the hidden head
             * with a leading ellipsis so the cut is honest. */
            {   const char *shown=val[0]?val:fhint[f];
                int cells=(box.w-CELLW)/CELLW;
                if(val[0]&&cells>1){
                    int len=0;for(const char *q=val;*q;)
                        len+=lj_render_cells_measure(nextcp(&q));
                    if(len>cells){
                        const char *q=val;int skip=len-(cells-1);
                        while(*q&&skip>0)skip-=lj_render_cells_measure(nextcp(&q));
                        static char tail[256];
                        snprintf(tail,sizeof tail,"\u2026%s",q);
                        shown=tail;
                    }
                }
                text(box.x+CELLW/2,y,shown,val[0]?TEXT:DIM,box.w-CELLW);
            }
            hit(a,box,H_ROOM_FIELD,f,NULL);
            y+=rowh;
        }
        y+=rowh;
        text(lx,y,"AGENTS",CYAN,11*CELLW);
        const char *kinds[4]={"CLAUDE","CODEX","DEEPSEEK","SHELL"};
        const char *rtag[4]={"— off —","WORKER","REVIEWER","LEAD"};
        int aw=(fw/agent_cols)/CELLW*CELLW;
        for(int i=0;i<4;i++){
            lj_rect cell={fx+(i%agent_cols)*aw,y+(i/agent_cols)*rowh,aw-CELLW,rowh};
            int r=a->room_agent[i];
            panel(cell,r?SURF2:BG,0);
            char one[64];snprintf(one,sizeof(one),"%s %s",kinds[i],rtag[r]);
            text(cell.x+CELLW/2,cell.y,one,r==3?GREEN:r?TEXT:DIM,cell.w-CELLW);
            hit(a,cell,H_ROOM_AGENT,i,NULL);
        }
        y+=agent_rows*rowh;
        text(lx,y,room_lead_count(a)==1?"click an agent to cycle off · worker · reviewer · lead"
                                       :"pick exactly ONE lead — the lead owns every todo in the room",
             room_lead_count(a)==1?DIM:AMBER,w-2*pad);
        y+=rowh;
        button(a,(lj_rect){modal.x+w-pad-22*CELLW,y,9*CELLW,rowh},"CANCEL",H_ROOM_CANCEL,0,NULL,0);
        button(a,(lj_rect){modal.x+w-pad-12*CELLW,y,12*CELLW,rowh},a->room_starting?"CREATING\u2026":"CREATE",H_ROOM_CREATE,0,NULL,!a->room_starting);
        folder_browser_render(a);
    }
    int ribbon_moving=fullscreen?0:draw_ribbon(a,0);
    if(a->dragging){
        for(int i=0;i<a->dock.tile_count;i++){
            lj_dock_tile *t=&a->dock.tiles[i];if(!inside(t->rect,a->mousex,a->mousey)||!strcmp(t->id,a->drag))continue;
            lj_dock_edge edge=lj_dock_edge_at(t->rect,a->mousex,a->mousey);
            const char *names[]={"Swap","Snap left","Snap right","Snap top","Snap bottom"};
            char cue[180];snprintf(cue,sizeof cue,"%s · release to place",names[edge]);
            rect((lj_rect){0,a->h-CELLH,a->w,CELLH},PANEL);
            text(CELLW,a->h-CELLH,cue,CYAN,a->w-2*CELLW);break;
        }
    }
    draw_menu(a);                               /* a flyout sits ABOVE its ribbon */
    popup_render(a);
    if(a->ansi)lj_ansi_effects(a->effects);
    if(a->effects&&!a->ansi){uint32_t *p=lj_render_pixels();for(int y=2;y<a->h;y+=4)for(int x=0;x<a->w;x++){uint32_t c=p[(size_t)y*a->w+x];p[(size_t)y*a->w+x]=0xff000000|(((c&0xfefefe)>>1)+((c&0xfcfcfc)>>2)+((c&0xf8f8f8)>>3));}}
    a->dirty=ribbon_moving;                     /* only the slide keeps it alive */
}

static void post(App *a,int stage){
    if(!a->draft_len){toast(a,a->answer_qid[0]?"Write the answer first":"Write a message first");return;}
    /* ⚠ ANSWERING A QUESTION IS ITS OWN ACTION. It unblocks the asker's todo and
     * returns the answer to them; posting the same words as a message does
     * neither, and leaves a room that looks answered but is still stopped. */
    if(a->answer_qid[0]&&!stage){
        json_object *o=json_object_new_object();
        jadd(o,"action","room_answer");jadd(o,"question",a->answer_qid);jadd(o,"answer",a->draft);
        if(request(a,o)){copy(a->posting_dest,sizeof(a->posting_dest),conversation(a));
            copy(a->posting_text,sizeof(a->posting_text),a->draft);
            a->answer_qid[0]=0;toast(a,"Answer sent — the asker's todo unblocks");}
        return;
    }
    if(a->answer_dm[0]&&!stage){
        json_object *o=json_object_new_object();
        jadd(o,"action","room_dm_reply");jadd(o,"room",a->answer_dm_room);jadd(o,"mail",a->answer_dm);jadd(o,"text",a->draft);
        if(request(a,o)){copy(a->posting_dest,sizeof(a->posting_dest),conversation(a));
            copy(a->posting_text,sizeof(a->posting_text),a->draft);
            a->answer_dm[0]=0;a->answer_dm_room[0]=0;toast(a,"Reply sent to the asking session");}
        return;
    }
    const char *dest=conversation(a);
    if(stage&&!is_dm(a)){toast(a,"Open the agent DM before staging a private request");return;}
    if(!*dest){toast(a,"Select a recipient session first");return;}
    if(stage){Session *s=session(a,dest);if(!s||!s->vt||!strcmp(s->agent,"shell")||!s->connected||!lj_vt_state(s->vt,6)){toast(a,"Stage needs a live agent with bracketed paste enabled");return;}}
    json_object *o=json_object_new_object();jadd(o,"action",stage?"stage":"post");jadd(o,"destination",dest);jadd(o,"text",a->draft);if(a->reply[0])jadd(o,"reply_to",a->reply);
    char target[81];copy(target,sizeof(target),dest);if(request(a,o)){copy(a->posting_dest,sizeof(a->posting_dest),target);copy(a->posting_text,sizeof(a->posting_text),a->draft);}
}
static void activate(App *a,Hit *h){
    if(h->kind==H_RETILE)retile(a);
    else if(h->kind==H_GIT){
        if(!dock_has(a,"git")&&!lj_dock_drop(&a->dock,"git","room",LJ_DOCK_RIGHT)){toast(a,"No space for Git DAG tile");return;}
        copy(a->full,sizeof(a->full),"git");copy(a->focus,sizeof(a->focus),"git");a->team=a->status_open=0;save_layout(a);
    }
    else if(h->kind==H_GIT_BODY)copy(a->focus,sizeof(a->focus),"git");
    else if(h->kind==H_REVIEW){
        /* ⚠ Review lives IN the room tile now, as a tab. the operator: "review is
         * reorganised properly in that tile not as a separate tile". */
        a->room_tab=RT_REVIEW;copy(a->focus,sizeof(a->focus),"room");
        a->team=a->status_open=0;a->review_scroll=0;
    }
    else if(h->kind==H_REVIEW_BODY)copy(a->focus,sizeof(a->focus),"review");
    else if(h->kind==H_FILES){
        if(!dock_has(a,"files")&&!lj_dock_drop(&a->dock,"files","room",LJ_DOCK_RIGHT)){toast(a,"No space for the Files tile");return;}
        copy(a->focus,sizeof(a->focus),"files");a->team=a->status_open=0;
        if(!a->files)request_files(a,"");
        save_layout(a);
    }
    else if(h->kind==H_FILES_BODY)copy(a->focus,sizeof(a->focus),"files");
    else if(h->kind==H_FILE_UP){
        const char *parent=a->files?jstr(a->files,"parent"):"";
        if(a->files&&json_object_is_type(jget(a->files,"parent"),json_type_string))request_files(a,parent);
        else toast(a,"Already at the room folder");
    }
    else if(h->kind==H_FILE_ENTRY){
        /* Only directories navigate. Opening a file is not this round's job and
         * pretending otherwise would be a dead click. */
        if(h->index)request_files(a,h->id);
        else toast(a,"Files are read-only here · open it in a terminal");
    }
    else if(h->kind==H_TEAM){a->team=!a->team;a->status_open=0;}
    else if(h->kind==H_STATUS){a->room_tab=RT_STATUS;copy(a->focus,sizeof(a->focus),"room");
        a->team=a->status_open=0;a->status_scroll=0;}
    else if(h->kind==H_TEAM_CLOSE){a->team=0;a->stop_confirm[0]=0;}
    else if(h->kind==H_TEAM_PAGE){a->team_page+=h->index;a->stop_confirm[0]=0;}
    else if(h->kind==H_CONTROLS){
        /* A dropdown hanging off the button, not a modal that hides the room. */
        if(a->menu_kind==MENU_ROLE&&!strcmp(a->menu_target,h->id))menu_close(a);
        else menu_open(a,MENU_ROLE,h->r,h->id);
    }
    else if(h->kind==H_LOAD){
        if(!dock_has(a,"load")&&!lj_dock_drop(&a->dock,"load","room",LJ_DOCK_RIGHT)){toast(a,"No space for the Load tile");return;}
        copy(a->focus,sizeof(a->focus),"load");a->team=a->status_open=0;save_layout(a);
    }
    else if(h->kind==H_OPEN_HERE){
        if(a->menu_kind==MENU_OPEN_HERE)menu_close(a);else menu_open(a,MENU_OPEN_HERE,h->r,"");
    }
    else if(h->kind==H_STATE_FILTER){
        if(a->menu_kind==MENU_STATE)menu_close(a);else menu_open(a,MENU_STATE,h->r,"");
    }
    /* ⚠ THE BURGER TOGGLES THE RIBBON, IT DOES NOT OPEN A MENU. menu_kind stays
     * free for the flyouts the ribbon opens; if the burger owned it, clicking
     * FILES would close the very ribbon the flyout hangs off. */
    /* ⚠ THE BURGER OPENS THE LOGO MENU (item 5, lead: B). The ribbon code stays
     * for the MORE/TOOLBAR path and the fixtures; the burger no longer unfolds it. */
    else if(h->kind==H_BURGER){if(a->menu_kind==MENU_LOGO)menu_close(a);else menu_open(a,MENU_LOGO,h->r,"");a->ribbon_open=0;a->dirty=1;}
    /* Click-away dismisses BOTH — the flyout if one is up, and the ribbon, which
     * then slides back rather than vanishing. */
    else if(h->kind==H_MENU_SCRIM){menu_close(a);a->ribbon_open=0;a->dirty=1;}
    else if(h->kind==H_RIBBON_MORE){
        char start[16];snprintf(start,sizeof(start),"%d",h->index);
        menu_open(a,MENU_TOOLBAR,h->r,start);a->ribbon_open=1;
    }
    else if(h->kind==H_RIBBON_ITEM){
        MenuRow rows[16];int n=toolbar_rows(a,rows,16);
        if(h->index<0||h->index>=n||!rows[h->index].action)return;
        int action=rows[h->index].action;
        /* FILES is the one item with somewhere further to go. Its flyout is
         * anchored on the item and the ribbon stays out underneath it. */
        if(action==H_FILES){
            char t[256];snprintf(t,sizeof(t),"d%s",a->files_path);
            if(a->menu_kind==MENU_FILES)menu_close(a);
            else {menu_open(a,MENU_FILES,h->r,t);a->ribbon_open=1;}
            return;
        }
        /* TODO(menu): ATTACHMENT POINT 3 of 4 — the remaining eight are direct
         * actions today. To give one children, do exactly what FILES does above:
         * menu_open(a,MENU_<X>,h->r,target) and keep a->ribbon_open=1 so the
         * parent stays out underneath its flyout, then add a MENU_<X> branch to
         * menu_rows(). draw_menu() renders it and H_MENU_ITEM dispatches it, so
         * a sub-menu costs a rows branch and nothing else. */
        menu_close(a);a->ribbon_open=0;
        Hit forward={.kind=action,.index=rows[h->index].index};forward.id[0]=0;
        activate(a,&forward);return;
    }
    else if(h->kind==H_MENU_ITEM){
        MenuRow rows[24];int n=menu_rows(a,rows,24);
        if(h->index<0||h->index>=n||rows[h->index].disabled||!rows[h->index].action)return;
        int action=rows[h->index].action,index=rows[h->index].index,kind=a->menu_kind;
        char target[256];copy(target,sizeof(target),a->menu_target);
        menu_close(a);
        if(kind==MENU_TOOLBAR){
            Hit forward={.kind=H_RIBBON_ITEM,.index=index,.r=h->r};
            activate(a,&forward);return;
        }
        if(kind==MENU_ROOMS&&action==H_GROUP){
            switch_workspace(a,index<0||index>=a->nroom?"":a->rooms[index].id);return;
        }
        if(kind==MENU_LOGO){
            /* Files nests its flyout on the row; everything else is the H_ kind itself. */
            if(action==H_FILES){char t[256];snprintf(t,sizeof(t),"d%s",a->files_path);menu_open(a,MENU_FILES,h->r,t);return;}
            Hit forward={.kind=action,.index=index,.r=h->r};forward.id[0]=0;
            activate(a,&forward);return;
        }
        if(kind==MENU_ROLE&&action==H_ROLE){
            Session *s=session(a,target);if(!s)return;
            const char *roles[3]={"worker","reviewer","lead"};
            json_object *o=json_object_new_object();jadd(o,"action","assign");
            jadd(o,"session",s->id);jadd(o,"role",roles[clamp(index,0,2)]);
            jadd(o,"parent",s->parent);request(a,o);return;
        }
        if(kind==MENU_STATE&&action==H_STATE_FILTER){
            copy(a->room_filter,sizeof(a->room_filter),ROOM_STATES[clamp(index,0,5)]);
            /* A room that just left the visible state must not stay open. */
            Room *cur=room_by_id(a,a->active_room);
            if(cur&&strcmp(cur->state[0]?cur->state:"active",a->room_filter))switch_workspace(a,"");
            return;
        }
        if(kind==MENU_ROOM&&action==H_ROOM_STATE){
            json_object *o=json_object_new_object();jadd(o,"action","room_state");
            jadd(o,"room",target);jadd(o,"state",ROOM_STATES[clamp(index,0,5)]);
            json_object_object_add(o,"force",json_object_new_boolean(0));request(a,o);return;
        }
        if(kind==MENU_ROOM&&action==H_ROOM_PURGE){
            json_object *o=json_object_new_object();jadd(o,"action","room_purge");
            jadd(o,"room",target);request(a,o);return;
        }
        if(kind==MENU_ROOM&&action==H_ROOM_ACTION){
            const char *acts[6]={"room_view","room_status","room_review","room_files","room_tools","room_move_all"};
            json_object *o=json_object_new_object();jadd(o,"action",acts[clamp(index,0,5)]);
            jadd(o,"room",target);
            if(index==5)jadd(o,"target_room",a->active_room);
            request(a,o);
            if(index==3){copy(a->focus,sizeof(a->focus),"files");
                if(!dock_has(a,"files"))lj_dock_drop(&a->dock,"files","room",LJ_DOCK_RIGHT);}
            if(index==2){copy(a->focus,sizeof(a->focus),"review");
                if(!dock_has(a,"review"))lj_dock_drop(&a->dock,"review","room",LJ_DOCK_RIGHT);}
            if(index==1)a->status_open=1;
            if(index==0)switch_workspace(a,target);
            return;
        }
        if((kind==MENU_FILE||kind==MENU_FILES)&&action==H_NEW_ROOM){
            room_dialog_open(a);
            /* the operator: right-click a folder, start a room on it, agents launch
             * there. The form opens already pointed at that folder. */
            copy(a->room_folder,sizeof(a->room_folder),target+1);
            a->room_field=RF_NAME;return;
        }
        Hit forward={.kind=action,.index=index};copy(forward.id,sizeof(forward.id),target);
        if(action==H_FILE_ENTRY)copy(forward.id,sizeof(forward.id),a->files_path);
        activate(a,&forward);return;
    }
    else if(h->kind==H_AGENT_FOCUS||h->kind==H_AGENT_DM){Session *s=session(a,h->id);if(!s)return;a->team=0;copy(a->selected,sizeof(a->selected),s->id);switch_workspace(a,s->room_id);if(h->kind==H_AGENT_DM){set_conversation(a,s->id);copy(a->focus,sizeof(a->focus),"room");}else {if(!terminal_tile(s)){copy(a->focus,sizeof(a->focus),"room");toast(a,"Observed session · no terminal to tile");return;}copy(a->focus,sizeof(a->focus),s->id);if(!dock_has(a,s->id))lj_dock_drop(&a->dock,s->id,"room",LJ_DOCK_LEFT);save_layout(a);}}
    else if(h->kind==H_LEAD){Session *s=session(a,h->id);if(!s||!s->room_id[0])return;json_object *o=json_object_new_object();jadd(o,"action","room_lead");jadd(o,"room",s->room_id);jadd(o,"session",s->id);request(a,o);}
    else if(h->kind==H_CONTROL||h->kind==H_STOP_SESSION){
        if(h->kind==H_CONTROL&&h->index==2&&strcmp(a->stop_confirm,h->id)){copy(a->stop_confirm,sizeof(a->stop_confirm),h->id);a->dirty=1;return;}
        json_object *o=json_object_new_object();jadd(o,"action","control");jadd(o,"session",h->id);jadd(o,"operation",h->index==0?"submit":h->index==1?"interrupt":"stop");if(h->index==2)json_object_object_add(o,"confirmed",json_object_new_boolean(1));a->stop_confirm[0]=0;request(a,o);
    }
    else if(h->kind==H_FULL){if(a->full[0])a->full[0]=0;else copy(a->full,sizeof(a->full),a->focus[0]?a->focus:a->selected[0]?a->selected:"room");}
    else if(h->kind==H_HIDE){lj_dock_remove(&a->dock,h->id);if(!strcmp(h->id,"media")){lj_media_close(&a->media);a->has_media=0;}if(!strcmp(h->id,"canvas")){lj_canvas_close(&a->canvas);a->has_canvas=0;a->canvas_drawing=0;}if(!strcmp(a->focus,h->id))copy(a->focus,sizeof(a->focus),"room");a->full[0]=0;save_layout(a);}
    else if(h->kind==H_CANVAS)open_canvas(a);
    else if(h->kind==H_CREATE){const char *agents[]={"claude","codex","deepseek","shell"};json_object *o=json_object_new_object();jadd(o,"action","create");jadd(o,"agent",agents[h->index]);jadd(o,"room",a->active_room);request(a,o);}
    else if(h->kind==H_ROOM)copy(a->focus,sizeof(a->focus),"room");
    else if(h->kind==H_ROOM_TAB){a->room_tab=clamp(h->index,0,RT_COUNT-1);
        copy(a->focus,sizeof(a->focus),"room");a->room_scroll=0;}
    else if(h->kind==H_ANSWER){
        copy(a->answer_qid,sizeof(a->answer_qid),h->id);
        /* The lead answers in the composer: the draft becomes the answer to
         * this question rather than a message, so nobody has to learn a second
         * place to type. */
        copy(a->reply,sizeof(a->reply),h->id);
        copy(a->focus,sizeof(a->focus),"room");a->room_tab=RT_ACTION;
        toast(a,"Type the answer and press Send — it answers this question");
    }
    else if(h->kind==H_ANSWER_DM){
        copy(a->answer_dm,sizeof(a->answer_dm),h->id);
        Room *rm=room_by_id(a,a->active_room);
        copy(a->answer_dm_room,sizeof(a->answer_dm_room),rm?rm->id:"");
        copy(a->focus,sizeof(a->focus),"room");a->room_tab=RT_ACTION;
        toast(a,"Type the answer and press Send — it replies to this DM");
    }
    else if(h->kind==H_DEST){if(h->index&&!a->selected[0]){toast(a,"Select an agent first");return;}set_conversation(a,h->index?a->selected:room_destination(a));copy(a->focus,sizeof(a->focus),"room");}
    else if(h->kind==H_GROUP)switch_workspace(a,h->id);
    else if(h->kind==H_ROOM_MORE){if(a->menu_kind==MENU_ROOMS)menu_close(a);else menu_open(a,MENU_ROOMS,h->r,"");a->dirty=1;}
    else if(h->kind==H_COLLAPSE)collapse_room(a,h->id);
    /* ⚠ THE SAME EXIT AS Ctrl+Q, NOT A SECOND ONE. One line, one meaning: set
     * running=0 and let the normal shutdown run. A separate teardown path here
     * is how a UI ends up with two ways to leave that clean up differently. */
    else if(h->kind==H_QUIT)a->running=0;
    else if(h->kind==H_ABOUT){a->about_open=!a->about_open;a->team=a->status_open=0;}
    else if(h->kind==H_ABOUT_CLOSE){a->about_open=0;}
    else if(h->kind==H_SETTINGS){a->settings_open=!a->settings_open;a->settings_sel=1;a->settings_scroll=0;a->settings_hex[0]=0;a->about_open=a->team=a->status_open=0;}
    else if(h->kind==H_SET_CLOSE){a->settings_open=0;a->settings_hex[0]=0;}
    else if(h->kind==H_SET_ROW){a->settings_sel=h->index;a->settings_hex[0]=0;}
    else if(h->kind==H_SET_DEC){a->settings_sel=h->index;theme_row_step(a,h->index,-1);}
    else if(h->kind==H_SET_INC){a->settings_sel=h->index;theme_row_step(a,h->index,+1);}
    else if(h->kind==H_SET_RESET){theme_reset_all(a);toast(a,"Theme reset to compiled defaults · SAVE to keep");}
    else if(h->kind==H_SET_SAVE){char err[256];if(lj_theme_save_user(err,sizeof err)>0)toast(a,"Theme saved");else{char m[300];snprintf(m,sizeof m,"Theme not saved: %s",err);toast(a,m);}}
    else if(h->kind==H_POPUP_CLOSE){lj_popup_request_close(&a->popup);}
    else if(h->kind==H_ABOUT_PLAY){
        if(!lj_popup_is_open(&a->popup)&&!lj_popup_open(&a->popup,LJ_RICKROLL))toast(a,lj_popup_status(&a->popup));
        else {a->about_open=0;a->menu_kind=MENU_NONE;a->popup_placed=0;
            a->drag[0]=0;a->dragging=a->selecting=0;a->split=-1;popup_geometry(a);}
    }
    else if(h->kind==H_NEW_ROOM)room_dialog_open(a);
    else if(h->kind==H_ROOM_FIELD)a->room_field=clamp(h->index,0,RF_COUNT-1);
    else if(h->kind==H_ROOM_FOLDER_BROWSE)folder_browser_open(a);
    else if(h->kind==H_ROOM_FOLDER_ENTRY){
        /* click selects; a click on the selected row opens it (a double click) */
        if(h->index>=0&&h->index<a->room_folder_count){
            /* ⚠ ONLY A CLICK-SELECTED ROW OPENS ON THE NEXT CLICK. Entering a folder
             * pre-selects row 0 for the keyboard; "click on the selected row opens"
             * turned one click on that row into a jump. */
            if(h->index==a->fb_click&&h->index==a->fb_sel)room_folder_go(a,a->room_folder_entries[h->index]);
            else {a->fb_sel=a->fb_click=h->index;a->fb_edit=0;}}
    }
    else if(h->kind==H_FB_OPEN){if(h->index>=0&&h->index<a->room_folder_count)room_folder_go(a,a->room_folder_entries[h->index]);}
    else if(h->kind==H_ROOM_FOLDER_UP){if(a->room_folder_base[0])room_folder_go(a,"..");}
    else if(h->kind==H_FB_HOME){const char *home=getenv("HOME");if(home)room_folder_go_path(a,home);}
    else if(h->kind==H_FB_PROJECT){if(a->project[0])room_folder_go_path(a,a->project);}
    else if(h->kind==H_FB_HIDDEN){a->fb_hidden=!a->fb_hidden;a->room_folder_listed[0]=0;a->fb_follow=1;}
    else if(h->kind==H_FB_CRUMB){char prefix[512];int n=clamp(h->index,1,(int)sizeof(prefix)-1);
        if((size_t)n<=strlen(a->room_folder_base)){memcpy(prefix,a->room_folder_base,(size_t)n);prefix[n]=0;room_folder_go_path(a,prefix);}}
    else if(h->kind==H_FB_PATH){if(!a->fb_edit){a->fb_edit=1;copy(a->fb_typed,sizeof a->fb_typed,a->room_folder_base);}}
    else if(h->kind==H_FB_CANCEL||h->kind==H_FB_CLOSE)folder_browser_close(a);
    else if(h->kind==H_ROOM_FOLDER_USE)folder_browser_use(a);
    else if(h->kind==H_ROOM_AGENT){int i=clamp(h->index,0,3);
        a->room_agent[i]=(a->room_agent[i]+1)%4;
        /* Exactly one lead: promoting one demotes the previous. */
        if(a->room_agent[i]==3)for(int k=0;k<4;k++)if(k!=i&&a->room_agent[k]==3)a->room_agent[k]=1;
    }
    else if(h->kind==H_ROOM_CANCEL)a->room_dialog=0;
    else if(h->kind==H_ROOM_CREATE)create_room(a);
    else if(h->kind==H_POST)post(a,0);
    else if(h->kind==H_STAGE)post(a,1);
    else if(h->kind==H_ROLE||h->kind==H_PARENT){
        Session *s=session(a,h->id);if(!s)return;json_object *o=json_object_new_object();jadd(o,"action","assign");jadd(o,"session",s->id);
        const char *role=s->role,*parent=s->parent;
        if(h->kind==H_ROLE)role=!strcmp(role,"worker")?"reviewer":!strcmp(role,"reviewer")?"lead":"worker";
        else {int found=!parent[0];const char *next="";for(int i=0;i<a->norder;i++){Session *p=&a->sessions[a->order[i]];if(strcmp(p->role,"lead")||p==s)continue;if(found){next=p->id;break;}if(!strcmp(p->id,parent))found=1;}parent=next;}
        jadd(o,"role",role);jadd(o,"parent",parent);request(a,o);
    }a->dirty=1;
}
static int control_at(App *a,int x,int y){
    for(int i=a->nhits-1;i>=0;i--)if(!surface_kind(a->hits[i].kind)&&inside(a->hits[i].r,x,y))return 1;
    return 0;
}
static void mouse_send(App *a,Session *s,int x,int y,int b,int release){
    if(!s||!s->vt||!lj_vt_state(s->vt,4))return;int cx=clamp((x-s->grid.x)/CELLW+1,1,s->cols),cy=clamp((y-s->grid.y)/CELLH+1+s->view_row,1,s->rows);char buf[64];
    if(lj_vt_state(s->vt,5)){int n=snprintf(buf,sizeof(buf),"\033[<%d;%d;%d%c",b,cx,cy,release?'m':'M');send_bytes(a,s,buf,(size_t)n);}
    else if(cx<224&&cy<224){unsigned char raw[]={27,'[','M',(unsigned char)(32+(release?3:b)),(unsigned char)(32+cx),(unsigned char)(32+cy)};send_bytes(a,s,(char*)raw,6);}
}
/* A selection is anchored to the TEXT (the operator 2026-09-13: "not scrolling with the
 * text it selects"): rows are stored at press time; every use subtracts the rows
 * that scrolled off the top since (LJ_VT_SCROLLED), so the highlight moves with
 * the lines and clips when they leave the screen. */
static int sel_shift(App *a,Session *s){return lj_vt_state(s->vt,LJ_VT_SCROLLED)-a->sel_base;}
static void copy_selection(App *a){
    Session *s=session(a,a->selection_id);if(!s||!s->vt)return;
    int shift=sel_shift(a,s);
    int start=(a->selr0-shift)*s->cols+a->selc0,end=(a->selr1-shift)*s->cols+a->selc1;if(start>end){int t=start;start=end;end=t;}
    if(end<0)return;if(start<0)start=0;                     /* lines that scrolled out of the screen are gone */
    char *out=malloc((size_t)(end-start+2)*20);if(!out)return;size_t n=0;
    for(int idx=start;idx<=end;idx++){int y=idx/s->cols,x=idx%s->cols;const lj_cell *row=lj_vt_row(s->vt,y);if(!row)break;
        uint32_t cps[4]={row[x].ch,row[x].marks[0],row[x].marks[1],row[x].marks[2]};
        for(int j=0;j<4;j++){uint32_t cp=cps[j];if(!cp)continue;if(cp<128)out[n++]=(char)cp;else if(cp<2048){out[n++]=(char)(192|(cp>>6));out[n++]=(char)(128|(cp&63));}else if(cp<65536){out[n++]=(char)(224|(cp>>12));out[n++]=(char)(128|((cp>>6)&63));out[n++]=(char)(128|(cp&63));}else{out[n++]=(char)(240|(cp>>18));out[n++]=(char)(128|((cp>>12)&63));out[n++]=(char)(128|((cp>>6)&63));out[n++]=(char)(128|(cp&63));}}
        if(x==s->cols-1&&idx<end)out[n++]='\n';
    }out[n]=0;
    if(a->ansi)toast(a,lj_ansi_copy(out)?"Clipboard requested · if blocked, F8 enables host selection":"Clipboard request failed · F8 enables host selection");
    else toast(a,SDL_SetClipboardText(out)==0?"Selection copied":"Could not copy selection");
    free(out);
}
static void add_draft(App *a,const char *s){size_t n=strlen(s);if(a->draft_len+n>=sizeof(a->draft)){toast(a,"Message exceeds 16 KB editor limit");return;}memcpy(a->draft+a->draft_len,s,n+1);a->draft_len+=n;a->dirty=1;}
static int message_input(App *a){
    return !a->copy_pause&&!a->about_open&&!a->settings_open&&!a->room_dialog&&!a->team&&!a->status_open&&
        !strcmp(a->focus,"room")&&(a->room_tab==RT_ROOM||a->room_tab==RT_ACTION);
}
static int visual_path(const char *value,char *out,size_t cap){
    if(!value||!out||cap<2)return 0;
    while(isspace((unsigned char)*value))value++;
    const char *end=value+strlen(value);while(end>value&&isspace((unsigned char)end[-1]))end--;
    if(end-value>1&&((*value=='\''&&end[-1]=='\'')||(*value=='"'&&end[-1]=='"'))){value++;end--;}
    if((size_t)(end-value)>=7&&!strncmp(value,"file://",7))value+=7;
    size_t n=0;
    while(value<end&&n+1<cap){
        if(*value=='%'&&end-value>=3&&isxdigit((unsigned char)value[1])&&isxdigit((unsigned char)value[2])){
            char hex[3]={value[1],value[2],0};out[n++]=(char)strtoul(hex,NULL,16);value+=3;
        }else out[n++]=*value++;
    }
    out[n]=0;if(value!=end||!out[0])return 0;
    struct stat st;if(stat(out,&st)||!S_ISREG(st.st_mode))return 0;
    const char *dot=strrchr(out,'.');if(!dot)return 0;
    static const char *exts[]={".png",".jpg",".jpeg",".gif",".bmp",".tga",".webp",".mp4",".webm",".mkv",".mov",NULL};
    for(int i=0;exts[i];i++)if(!strcasecmp(dot,exts[i]))return 1;
    return 0;
}
static int image_path(const char *path){
    const char *dot=strrchr(path,'.');if(!dot)return 0;
    static const char *exts[]={".png",".jpg",".jpeg",".gif",".bmp",".tga",".webp",NULL};
    for(int i=0;exts[i];i++)if(!strcasecmp(dot,exts[i]))return 1;
    return 0;
}
static int import_room_media(App *a,const char *path,char *copy_path,size_t cap){
    char root[PATH_MAX];copy(root,sizeof root,a->root);
    if(!root[0]){const char *home=getenv("HOME");snprintf(root,sizeof root,"%s/.cache/liljack/workspace",home&&*home?home:"/tmp");}
    lj_canvas c;lj_canvas_init(&c);int ok=lj_canvas_open(&c,root,a->active_room)&&lj_canvas_import(&c,path,copy_path,cap);
    if(!ok&&c.status[0])toast(a,c.status);lj_canvas_close(&c);return ok;
}
static void visual_ingest(App *a,const char *value){
    char path[PATH_MAX];if(!visual_path(value,path,sizeof path))return;
    if(!strcmp(a->focus,"canvas")&&a->has_canvas&&image_path(path)){
        if(lj_canvas_append_image(&a->canvas,path,0,0,0,0,"operator")){
            lj_canvas_poll(&a->canvas);toast(a,"Image placed on the shared canvas");
        }else toast(a,a->canvas.status[0]?a->canvas.status:"Could not place image on canvas");
        return;
    }
    open_media(a,path);
    if(!strcmp(a->focus,"room")&&message_input(a)){
        char durable[PATH_MAX];if(!import_room_media(a,path,durable,sizeof durable))return;
        char line[PATH_MAX+16];snprintf(line,sizeof line,"media: %s",durable);add_draft(a,line);
    }
}
static void input_paste(App *a,const char *value,int brackets){
    if(a->copy_pause||a->about_open||a->settings_open)return;
    if(a->room_dialog){room_name_input(a,value);return;}
    if(a->team||a->status_open)return;
    char path[PATH_MAX];if((!strcmp(a->focus,"canvas")||!strcmp(a->focus,"room"))&&visual_path(value,path,sizeof path)){
        visual_ingest(a,path);return;
    }
    if(!strcmp(a->focus,"room")){
        if(message_input(a))add_draft(a,value);
        else toast(a,"Select ROOM or ACTIONABLE to paste a message");
    }else paste(a,session(a,a->focus),value,brackets);
}
static void clipboard_paste(App *a){
    if(a->ansi){toast(a,"Paste using your host terminal’s shortcut (usually Ctrl+Shift+V)");return;}
    char *value=SDL_GetClipboardText();
    if(value){input_paste(a,value,0);SDL_free(value);}
    else toast(a,"Could not read clipboard");
}
static void key(App *a,SDL_KeyboardEvent *e){
    SDL_Keycode k=e->keysym.sym;int mod=e->keysym.mod,ctrl=mod&KMOD_CTRL,shift=mod&KMOD_SHIFT,alt=mod&KMOD_ALT;
    if(ctrl&&k==SDLK_q&&(shift||a->ansi)){a->running=0;return;}
    if(a->ansi&&k==SDLK_F8){
        if(!lj_ansi_mouse(a->copy_pause)){toast(a,"Could not change terminal mouse capture");return;}
        a->copy_pause=!a->copy_pause;a->copy_pause_painted=0;
        a->selecting=a->dragging=0;a->drag[0]=0;a->split=-1;
        toast(a,a->copy_pause?"Host selection enabled · F8 resumes":"App mouse restored");return;
    }
    if(a->copy_pause)return;
    if(k==SDLK_F12){
        if(!a->active_room[0]){toast(a,"Open a room before capturing a shared screenshot");return;}
        char root[PATH_MAX],dir[PATH_MAX],path[PATH_MAX];copy(root,sizeof root,a->root);
        if(!root[0]){const char *home=getenv("HOME");snprintf(root,sizeof root,"%s/.cache/liljack/workspace",home&&*home?home:"/tmp");}
        snprintf(dir,sizeof dir,"%s/rooms/%s/media",root,a->active_room);
        char rooms[PATH_MAX],room[PATH_MAX];snprintf(rooms,sizeof rooms,"%s/rooms",root);snprintf(room,sizeof room,"%s/rooms/%s",root,a->active_room);
        mkdir(rooms,0700);mkdir(room,0700);mkdir(dir,0700);
        struct timespec shot_time;clock_gettime(CLOCK_REALTIME,&shot_time);
        snprintf(path,sizeof path,"%s/shot-%lld-%09ld.png",dir,(long long)shot_time.tv_sec,shot_time.tv_nsec);
        int was_ansi=a->ansi,was_render=render_ansi;a->ansi=0;render_ansi=0;render(a);
        int ok=lj_render_save_png(path)==0;a->ansi=was_ansi;render_ansi=was_render;a->dirty=1;
        if(ok){char line[PATH_MAX+16];snprintf(line,sizeof line,"media: %s",path);
            json_object *o=json_object_new_object();jadd(o,"action","post");jadd(o,"destination",room_destination(a));jadd(o,"text",line);
            request(a,o);toast(a,"Workspace screenshot shared with the room");}
        else toast(a,"Could not save workspace screenshot");
        return;
    }
    if(a->ansi&&ctrl&&shift&&k==SDLK_c){copy_selection(a);return;}
    if(k==SDLK_ESCAPE&&lj_popup_is_open(&a->popup)){lj_popup_request_close(&a->popup);a->dirty=1;return;}
    if(a->menu_kind==MENU_LOGO){
        /* Keyboard for the logo menu: Up/Down skip headers and disabled rows,
         * Enter activates, Right opens the nested Files flyout, Esc closes. */
        MenuRow rows[24];int n=menu_rows(a,rows,24);
        if(k==SDLK_ESCAPE){menu_close(a);return;}
        if(k==SDLK_DOWN||k==SDLK_UP){
            int step=k==SDLK_DOWN?1:-1,i=a->menu_sel<0?(step>0?-1:n):a->menu_sel;
            for(int t=0;t<n;t++){i=(i+step+n)%n;if(rows[i].action&&!rows[i].disabled)break;}
            a->menu_sel=i;a->dirty=1;return;
        }
        if((k==SDLK_RETURN||k==SDLK_RIGHT)&&a->menu_sel>=0&&a->menu_sel<n){
            if(k==SDLK_RIGHT&&rows[a->menu_sel].action!=H_FILES)return;
            Hit h={.kind=H_MENU_ITEM,.index=a->menu_sel};copy(h.id,sizeof(h.id),a->menu_target);
            h.r=(lj_rect){a->menu_at.x,a->menu_at.y+a->menu_at.h+a->menu_sel*CELLH,14*CELLW,CELLH};
            activate(a,&h);a->dirty=1;return;
        }
    }
    if(a->settings_open){
        ThemeRow rows[THEME_ROWS_MAX];int n=theme_rows(rows,THEME_ROWS_MAX);
        int hexing=a->settings_hex[0]!=0;
        if(k==SDLK_ESCAPE){if(hexing)a->settings_hex[0]=0;else a->settings_open=0;}
        else if(k==SDLK_RETURN){
            if(hexing&&strlen(a->settings_hex)==6&&a->settings_sel<n&&rows[a->settings_sel].kind==TR_COLOUR){
                colour_set(a,rows[a->settings_sel].index,(uint32_t)strtoul(a->settings_hex,NULL,16));a->settings_hex[0]=0;
            } else if(hexing)toast(a,"Type six hex digits, then Enter"); else a->settings_open=0;
        }
        else if(k==SDLK_DOWN||k==SDLK_UP){int step=k==SDLK_DOWN?1:-1,i=a->settings_sel;for(int t=0;t<n;t++){i=(i+step+n)%n;if(rows[i].kind!=TR_HEADER)break;}a->settings_sel=i;a->settings_hex[0]=0;}
        else if(k==SDLK_PAGEDOWN||k==SDLK_PAGEUP){int step=k==SDLK_PAGEDOWN?10:-10;a->settings_sel=clamp(a->settings_sel+step,0,n-1);if(rows[a->settings_sel].kind==TR_HEADER&&a->settings_sel+1<n)a->settings_sel++;a->settings_hex[0]=0;}
        else if(k==SDLK_TAB)a->settings_chan=(a->settings_chan+1)%3;
        else if(k==SDLK_LEFT||k==SDLK_MINUS||k==SDLK_KP_MINUS)theme_row_step(a,a->settings_sel,-1);
        else if(k==SDLK_RIGHT||k==SDLK_EQUALS||k==SDLK_PLUS||k==SDLK_KP_PLUS)theme_row_step(a,a->settings_sel,+1);
        else if(k==SDLK_BACKSPACE){size_t l=strlen(a->settings_hex);if(l)a->settings_hex[l-1]=0;}
        a->dirty=1;return;
    }
    if(a->about_open){
        if(k==SDLK_ESCAPE||k==SDLK_RETURN)a->about_open=0;
        a->dirty=1;return;
    }
    if((ctrl&&k==SDLK_v)||(shift&&k==SDLK_INSERT)){clipboard_paste(a);return;}
    if(a->room_dialog){
        if(folder_browser_key(a,k,ctrl))return;
        if(ctrl&&k==SDLK_o){folder_browser_open(a);return;}
        if(k==SDLK_ESCAPE)a->room_dialog=0;
        else if(k==SDLK_RETURN||k==SDLK_KP_ENTER)create_room(a);
        else if(k==SDLK_TAB)a->room_field=(a->room_field+(shift?RF_COUNT-1:1))%RF_COUNT;
        else if(k==SDLK_DOWN)a->room_field=(a->room_field+1)%RF_COUNT;
        else if(k==SDLK_UP)a->room_field=(a->room_field+RF_COUNT-1)%RF_COUNT;
        else if(k==SDLK_BACKSPACE)room_field_backspace(a);
        else if(ctrl&&k==SDLK_u){room_field_buf(a,a->room_field,NULL)[0]=0;}
        a->dirty=1;return;
    }
    if(k==SDLK_F11){if(a->ansi){toast(a,"Resize or maximize your host terminal");return;}a->fullscreen=!a->fullscreen;SDL_SetWindowFullscreen(a->window,a->fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);return;}
    if(k==SDLK_F7){a->team=!a->team;a->dirty=1;return;}
    if(a->status_open){if(k==SDLK_ESCAPE)a->status_open=0;else if(k==SDLK_DOWN||k==SDLK_PAGEDOWN)a->status_scroll+=CELLH*3;else if(k==SDLK_UP||k==SDLK_PAGEUP)a->status_scroll-=CELLH*3;a->dirty=1;return;}
    if(a->team){if(k==SDLK_ESCAPE){a->team=0;a->dirty=1;}return;}
    if(k==SDLK_F5){Hit f={.kind=H_FULL};activate(a,&f);return;}
    if(a->full[0]&&k==SDLK_ESCAPE){a->full[0]=0;a->dirty=1;return;}
    /* F6 is "take me to the room chat", so it selects a tab you can type on —
     * landing on STATUS or REVIEW would focus the tile and silently swallow
     * everything typed next. */
    if(k==SDLK_F6){copy(a->focus,sizeof(a->focus),"room");a->room_tab=RT_ROOM;a->dirty=1;return;}
    if(ctrl&&k==SDLK_RIGHTBRACKET){a->focus[0]=0;a->dirty=1;return;}
    if(!strcmp(a->focus,"git")){
        if(k==SDLK_ESCAPE){a->full[0]=0;copy(a->focus,sizeof(a->focus),"room");}
        else if(k==SDLK_DOWN||k==SDLK_PAGEDOWN)a->git_scroll+=CELLH*3;
        else if(k==SDLK_UP||k==SDLK_PAGEUP)a->git_scroll-=CELLH*3;
        else if(k==SDLK_HOME)a->git_scroll=0;
        else if(k==SDLK_END)a->git_scroll=a->git_height;
        a->dirty=1;return;
    }
    if(!strcmp(a->focus,"files")){
        if(k==SDLK_ESCAPE){a->full[0]=0;copy(a->focus,sizeof(a->focus),"room");}
        else if(k==SDLK_DOWN||k==SDLK_PAGEDOWN)a->files_scroll+=CELLH*(k==SDLK_DOWN?1:8);
        else if(k==SDLK_UP||k==SDLK_PAGEUP)a->files_scroll-=CELLH*(k==SDLK_UP?1:8);
        else if(k==SDLK_HOME)a->files_scroll=0;
        else if(k==SDLK_END)a->files_scroll=a->files_height;
        else if(k==SDLK_BACKSPACE){Hit up={.kind=H_FILE_UP};activate(a,&up);}
        a->dirty=1;return;
    }
    if(!strcmp(a->focus,"review")){
        if(k==SDLK_ESCAPE){a->full[0]=0;copy(a->focus,sizeof(a->focus),"room");}
        else if(k==SDLK_DOWN||k==SDLK_PAGEDOWN)a->review_scroll+=CELLH*3;
        else if(k==SDLK_UP||k==SDLK_PAGEUP)a->review_scroll-=CELLH*3;
        else if(k==SDLK_HOME)a->review_scroll=0;
        else if(k==SDLK_END)a->review_scroll=a->review_height;
        a->dirty=1;return;
    }

    if(!strcmp(a->focus,"room")){
        /* ⚠ ONLY THE TABS WITH A COMPOSER TAKE TYPING. STATUS and REVIEW have no
         * input line, and without this a keystroke there filled a draft the
         * screen never showed — it would surface later, on another tab, as text
         * nobody remembers typing. Those tabs scroll instead. */
        if(a->room_tab!=RT_ROOM&&a->room_tab!=RT_ACTION){
            if(k==SDLK_DOWN||k==SDLK_PAGEDOWN){if(a->room_tab==RT_REVIEW)a->review_scroll+=CELLH*3;else a->status_scroll+=CELLH*3;}
            else if(k==SDLK_UP||k==SDLK_PAGEUP){if(a->room_tab==RT_REVIEW)a->review_scroll-=CELLH*3;else a->status_scroll-=CELLH*3;}
            else if(k==SDLK_HOME){a->review_scroll=a->status_scroll=0;}
            else if(k==SDLK_ESCAPE)a->room_tab=RT_ROOM;
            a->dirty=1;return;
        }
        /* ⚠ ONE KEY, because a question is the room stopped on its lead and
         * hunting for the right row is the wrong amount of friction. `a` arms
         * the OLDEST open question — oldest, because the one that has been
         * waiting longest is the one costing the most. */
        if(k==SDLK_a&&!ctrl&&!alt&&!a->draft_len&&!a->answer_qid[0]&&!a->answer_dm[0]){
            size_t nd=a->room_dms?json_object_array_length(a->room_dms):0;
            for(size_t i=0;i<nd;i++){json_object *d=json_object_array_get_idx(a->room_dms,i);
                if(d&&!json_object_get_boolean(jget(d,"read"))){copy(a->answer_dm,sizeof(a->answer_dm),jstr(d,"id"));
                    Room *rm=room_by_id(a,a->active_room);copy(a->answer_dm_room,sizeof(a->answer_dm_room),rm?rm->id:"");
                    a->room_tab=RT_ACTION;char t[220];snprintf(t,sizeof(t),"Replying to %s",jstr(d,"agent"));
                    toast(a,t);a->dirty=1;return;}}
            if(a->room_questions&&json_object_array_length(a->room_questions)){
                json_object *q=json_object_array_get_idx(a->room_questions,0);
                copy(a->answer_qid,sizeof(a->answer_qid),jstr(q,"id"));
                a->room_tab=RT_ACTION;
                char t[220];snprintf(t,sizeof(t),"Answering: %.160s",jstr(q,"question"));
                toast(a,t);a->dirty=1;return;
            }
        }
        if(k==SDLK_ESCAPE&&(a->answer_qid[0]||a->answer_dm[0])){a->answer_qid[0]=a->answer_dm[0]=a->answer_dm_room[0]=0;toast(a,"Answer cancelled");a->dirty=1;return;}
        if(k==SDLK_RETURN||k==SDLK_KP_ENTER){if(shift)add_draft(a,"\n");else post(a,0);}
        else if(k==SDLK_BACKSPACE&&a->draft_len){do{a->draft_len--;}while(a->draft_len&&((unsigned char)a->draft[a->draft_len]&0xc0)==0x80);a->draft[a->draft_len]=0;}
        else if(ctrl&&k==SDLK_u){a->draft[0]=0;a->draft_len=0;}a->dirty=1;return;
    }
    Session *s=session(a,a->focus);if(!s||!s->vt)return;char c;const char *seq=NULL;
    if(ctrl&&k>=SDLK_a&&k<=SDLK_z){c=(char)(k-SDLK_a+1);send_bytes(a,s,&c,1);return;}
    if(ctrl&&(k==SDLK_SPACE||k==SDLK_AT)){c=0;send_bytes(a,s,&c,1);return;}
    if(ctrl||shift||alt){
        char final=k==SDLK_UP?'A':k==SDLK_DOWN?'B':k==SDLK_RIGHT?'C':k==SDLK_LEFT?'D':k==SDLK_HOME?'H':k==SDLK_END?'F':0;
        if(final){char encoded[24];int n=snprintf(encoded,sizeof(encoded),"\033[1;%d%c",1+(shift?1:0)+(alt?2:0)+(ctrl?4:0),final);send_bytes(a,s,encoded,(size_t)n);return;}
    }
    if(k==SDLK_RETURN||k==SDLK_KP_ENTER)seq="\r";else if(k==SDLK_BACKSPACE)seq="\177";else if(k==SDLK_TAB)seq=shift?"\033[Z":"\t";else if(k==SDLK_ESCAPE)seq="\033";
    else if(k==SDLK_UP)seq=lj_vt_state(s->vt,7)?"\033OA":"\033[A";else if(k==SDLK_DOWN)seq=lj_vt_state(s->vt,7)?"\033OB":"\033[B";
    else if(k==SDLK_RIGHT)seq=lj_vt_state(s->vt,7)?"\033OC":"\033[C";else if(k==SDLK_LEFT)seq=lj_vt_state(s->vt,7)?"\033OD":"\033[D";
    else if(k==SDLK_HOME)seq="\033[H";else if(k==SDLK_END)seq="\033[F";else if(k==SDLK_DELETE)seq="\033[3~";else if(k==SDLK_INSERT)seq="\033[2~";else if(k==SDLK_PAGEUP)seq="\033[5~";else if(k==SDLK_PAGEDOWN)seq="\033[6~";
    else if(k>=SDLK_F1&&k<=SDLK_F12){const char *keys[]={"\033OP","\033OQ","\033OR","\033OS","\033[15~","\033[17~","\033[18~","\033[19~","\033[20~","\033[21~","\033[23~","\033[24~"};seq=keys[k-SDLK_F1];}
    if(seq){if(alt)send_bytes(a,s,"\033",1);send_bytes(a,s,seq,strlen(seq));}
}
static void open_media(App *a,const char *path){
    /* A direct owkTerm host already owns a native HUI canvas and a 24-fps
     * floating player. Hand it the source instead of decoding into lilJack's
     * framebuffer and then encoding the whole terminal surface as sixel.
     * tmux breaks the direct child/host relationship, so it keeps the ordinary
     * in-process 12-fps sixel/block path below. */
    if(a->ansi&&getenv("OWKTERM")&&!getenv("TMUX")){
        fflush(stdout);
        if(lj_media_delegate_owkterm(STDOUT_FILENO,path)){
            toast(a,"Media opened in owkTerm's native player");a->dirty=1;return;
        }
        toast(a,"Media source contains unsupported terminal controls");a->dirty=1;return;
    }
    /* ⚠ THIS REFUSED EVERY IMAGE AND VIDEO IN --tui AND POINTED AT A WINDOW
     * THE OPERATOR DELETED. The refusal was correct only while a terminal had no way
     * to show pixels; it has one now (the sixel presenter, 0f4a312). Leaving it
     * here is what made "where is a rick roll video" unanswerable — the button
     * existed, the decoder existed, the presenter existed, and this line said
     * no. Refuse now ONLY when the terminal itself cannot show an image, and
     * say which terminal fact decided that. */
    /* ⚠ NO LONGER A REFUSAL. This returned early on any terminal without sixel,
     * which meant the media tile drew nothing for anyone whose terminal does
     * not advertise DA1 attribute 4 — mate-terminal among them, which is what
     * the operator runs. There is now a half-block path (U+2580, fg=upper pixel,
     * bg=lower) that works in any truecolor terminal, so the honest answer is
     * to SHOW IT AT LOWER RESOLUTION and say which path is in use, rather than
     * to show nothing and explain why. */
    if(a->ansi&&!lj_ansi_images_available())
        toast(a,"No sixel here — drawing at half resolution with block glyphs");
    if(lj_media_open(&a->media,path)){
        if(!lj_dock_drop(&a->dock,"media","room",LJ_DOCK_TOP)){
            lj_media_close(&a->media);a->has_media=0;toast(a,"No space for a media tile");return;
        }
        a->has_media=1;a->full[0]=0;toast(a,"Media opened · drag its title to arrange it");
    }
    else toast(a,a->media.status[0]?a->media.status:"Media could not be opened");a->dirty=1;
}
static void canvas_point(App *a,int x,int y){
    lj_rect b=a->canvas_body;if(b.w<2||b.h<2||a->canvas_npts>=LJ_CANVAS_MAX_POINTS)return;
    float nx=(float)(x-b.x)/(b.w-1),ny=(float)(y-b.y)/(b.h-1);if(nx<0)nx=0;if(nx>1)nx=1;if(ny<0)ny=0;if(ny>1)ny=1;
    int n=a->canvas_npts;if(n&&a->canvas_pts[(n-1)*2]==nx&&a->canvas_pts[(n-1)*2+1]==ny)return;
    a->canvas_pts[n*2]=nx;a->canvas_pts[n*2+1]=ny;a->canvas_npts=n+1;
}
static void open_canvas(App *a){
    if(!a->active_room[0]){toast(a,"Open a room first · the canvas belongs to a room");return;}
    /* --root / LILJACK_WORKSPACE_ROOT may both be absent (the operator launches plain
     * ./liljack); the Python side then defaults to ~/.cache/liljack/workspace,
     * and the canvas file must land where `liljack room --draw` writes it. */
    char root[512];copy(root,sizeof(root),a->root);
    if(!root[0]){const char *home=getenv("HOME");snprintf(root,sizeof(root),"%s/.cache/liljack/workspace",home&&*home?home:"/tmp");}
    if(!lj_canvas_open(&a->canvas,root,a->active_room)){toast(a,a->canvas.status);return;}
    if(!dock_has(a,"canvas")&&!lj_dock_drop(&a->dock,"canvas","room",LJ_DOCK_TOP)){lj_canvas_close(&a->canvas);toast(a,"No space for a canvas tile");return;}
    a->has_canvas=1;a->canvas_drawing=0;a->canvas_npts=0;a->full[0]=0;copy(a->focus,sizeof(a->focus),"canvas");save_layout(a);
    if(a->ansi&&!lj_ansi_images_available())toast(a,"No sixel here — canvas drawn with block glyphs");
    else toast(a,"Canvas opened · drag to draw · agents draw with liljack room --draw");
    lj_canvas_poll(&a->canvas);a->dirty=1;
}
static void event(App *a,SDL_Event *e){
    if(e->type==SDL_USEREVENT&&(e->user.code==LJ_ANSI_PASTE_CODE||e->user.code==LJ_ANSI_ERROR_CODE)){
        char *data=e->user.data1;
        if(a->copy_pause){free(data);return;}
        /* ⚠ A host paste is NEVER dropped. It used to require the tile's app to
         * have enabled bracketed paste and otherwise toasted "type directly in
         * its terminal" — the operator: "I can't paste in tile terminals anything".
         * paste() still wraps in ESC[200~/201~ when the app asked for it. */
        if(data){if(e->user.code==LJ_ANSI_ERROR_CODE)toast(a,data);else input_paste(a,data,0);free(data);}return;
    }
    if(e->type==SDL_QUIT){a->running=0;return;}
    if(e->type==SDL_WINDOWEVENT){if(e->window.event==SDL_WINDOWEVENT_SIZE_CHANGED){
            if(e->window.data1<=0||e->window.data2<=0){a->minimised=1;return;}
            int w=clamp(e->window.data1,1,7680),h=clamp(e->window.data2,1,4320);a->minimised=0;
            SDL_Texture *next=a->renderer?SDL_CreateTexture(a->renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h):NULL;
            if(a->renderer&&!next){toast(a,"Could not resize presentation buffer");return;}
            SDL_DestroyTexture(a->texture);a->texture=next;a->w=w;a->h=h;a->split=-1;lj_render_resize(w,h);a->dirty=1;
        }
        if(e->window.event==SDL_WINDOWEVENT_MINIMIZED)a->minimised=1;
        if(e->window.event==SDL_WINDOWEVENT_RESTORED){a->minimised=0;a->dirty=1;}
        if(e->window.event==SDL_WINDOWEVENT_FOCUS_LOST){a->drag[0]=0;a->dragging=a->selecting=0;a->split=-1;a->popup_dragging=a->popup_mouse=a->popup_resizing=0;}return;}
    if(e->type==SDL_DROPFILE){visual_ingest(a,e->drop.file);SDL_free(e->drop.file);return;}
    if(e->type==SDL_KEYDOWN){if(a->sel_shown){a->sel_shown=0;a->dirty=1;}key(a,&e->key);return;}
    if(a->copy_pause)return;
    if(folder_browser_event(a,e))return;
    if(popup_event(a,e))return;
    if(!a->ansi&&e->type==SDL_MOUSEBUTTONDOWN&&e->button.button==SDL_BUTTON_MIDDLE){
        for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_ROOM&&inside(a->hits[i].r,e->button.x,e->button.y)){
            copy(a->focus,sizeof a->focus,"room");clipboard_paste(a);return;
        }
    }
    if(e->type==SDL_TEXTINPUT&&a->settings_open){settings_text(a,e->text.text);return;}   /* the theme editor owns typed text (hex entry); nothing reaches a tile */
    if(e->type==SDL_TEXTINPUT&&!a->team&&!a->status_open){if(a->room_dialog){room_name_input(a,e->text.text);return;}if(!strcmp(a->focus,"room")){if(message_input(a))add_draft(a,e->text.text);return;}else {Session *s=session(a,a->focus);if(s){if(modifiers()&KMOD_ALT)send_bytes(a,s,"\033",1);send_bytes(a,s,e->text.text,strlen(e->text.text));}}return;}
    if(e->type==SDL_MOUSEMOTION){int prior=-1,next=-1;for(int i=0;i<a->dock.divider_count;i++){if(inside(hitrect(a->dock.dividers[i].rect),a->mousex,a->mousey))prior=i;if(inside(hitrect(a->dock.dividers[i].rect),e->motion.x,e->motion.y))next=i;}if(prior!=next&&!a->team&&!a->status_open&&!a->room_dialog&&!a->about_open)a->dirty=1;
        if(e->motion.x/CELLW!=a->mousex/CELLW||e->motion.y/CELLH!=a->mousey/CELLH)a->dirty=1;  /* hover follows the pointer */
        a->mousex=e->motion.x;a->mousey=e->motion.y;if(a->team||a->status_open||a->room_dialog||a->about_open||a->settings_open)return;
        if(a->canvas_drawing){canvas_point(a,a->mousex,a->mousey);a->dirty=1;return;}
        if(a->split>=0){lj_dock_drag_move(&a->dock,&a->split_drag,a->mousex,a->mousey);a->dirty=1;return;}
        if(a->drag[0]&&(abs(a->mousex-a->dragx)>6||abs(a->mousey-a->dragy)>6)){a->dragging=1;a->dirty=1;return;}
        if(a->selecting){Session *s=session(a,a->selection_id);if(s){a->selc1=clamp((a->mousex-s->grid.x)/CELLW,0,s->cols-1);a->selr1=clamp((a->mousey-s->grid.y)/CELLH+s->view_row+sel_shift(a,s),0,s->rows-1+sel_shift(a,s));}a->dirty=1;return;}
        Session *s=session(a,a->focus);if(s&&inside(s->grid,a->mousex,a->mousey)&&s->vt){int mode=lj_vt_state(s->vt,4);if(mode==1003||(mode==1002&&e->motion.state))mouse_send(a,s,a->mousex,a->mousey,32+(e->motion.state&SDL_BUTTON_LMASK?0:e->motion.state&SDL_BUTTON_MMASK?1:e->motion.state&SDL_BUTTON_RMASK?2:3),0);}return;}
    if(e->type==SDL_MOUSEBUTTONDOWN){a->mousex=e->button.x;a->mousey=e->button.y;if(a->sel_shown){a->sel_shown=0;a->dirty=1;}
        if(!a->team&&!a->status_open&&!a->room_dialog&&!a->about_open&&!a->settings_open&&e->button.button==SDL_BUTTON_LEFT&&!control_at(a,a->mousex,a->mousey)){
            int i=divider_at(a,a->mousex,a->mousey);
            if(i>=0&&lj_dock_drag_begin(&a->dock,a->dock.dividers[i].node,a->mousex,a->mousey,&a->split_drag)){
                a->split=a->dock.dividers[i].node;a->dirty=1;return;
            }
        }
        /* ⚠ SMALLEST CONTAINING TARGET WINS, not the last one drawn. Snapping a
         * hit area out to whole cells lets a wide button (Controls) and the
         * narrow × beside it claim the same cell; last-drawn then handed the
         * cell to whichever happened to be painted later, so the × was dead on
         * the very cell showing its glyph. Area order is stable and needs no
         * draw-order bookkeeping, and it is the rule a reader expects: the
         * more specific control is the one you meant. */
        int best=-1,bestlayer=-1;long long bestarea=0;
        for(int i=a->nhits-1;i>=0;i--){Hit *probe=&a->hits[i];
            if(!inside(probe->r,a->mousex,a->mousey))continue;
            long long area=(long long)probe->r.w*probe->r.h;
            /* Higher layer always wins; area only breaks ties within a layer. */
            if(best<0||probe->layer>bestlayer||(probe->layer==bestlayer&&area<bestarea)){
                best=i;bestlayer=probe->layer;bestarea=area;}}
        if(getenv("LILJACK_DEBUG_HITS")){
            fprintf(stderr,"click %d,%d -> best=%d kind=%d id=%s rect=%d,%d,%d,%d\n",a->mousex,a->mousey,best,
                    best>=0?a->hits[best].kind:-1,best>=0?a->hits[best].id:"",
                    best>=0?a->hits[best].r.x:0,best>=0?a->hits[best].r.y:0,
                    best>=0?a->hits[best].r.w:0,best>=0?a->hits[best].r.h:0);
            for(int i=0;i<a->nhits;i++)if(inside(a->hits[i].r,a->mousex,a->mousey))
                fprintf(stderr,"   candidate i=%d kind=%d id=%s rect=%d,%d,%d,%d area=%d\n",i,a->hits[i].kind,a->hits[i].id,
                        a->hits[i].r.x,a->hits[i].r.y,a->hits[i].r.w,a->hits[i].r.h,a->hits[i].r.w*a->hits[i].r.h);
            for(int i=0;i<a->dock.divider_count;i++){lj_rect d=hitrect(a->dock.dividers[i].rect);
                fprintf(stderr,"   divider %d raw=%d,%d,%d,%d snap=%d,%d,%d,%d hot=%d\n",i,
                        a->dock.dividers[i].rect.x,a->dock.dividers[i].rect.y,a->dock.dividers[i].rect.w,a->dock.dividers[i].rect.h,
                        d.x,d.y,d.w,d.h,inside(d,a->mousex,a->mousey));}
            fflush(stderr);
        }
        if(best>=0&&a->hits[best].kind==H_CANVAS_BODY&&e->button.button==SDL_BUTTON_LEFT&&a->has_canvas){
            a->canvas_drawing=1;a->canvas_npts=0;canvas_point(a,a->mousex,a->mousey);copy(a->focus,sizeof(a->focus),"canvas");a->dirty=1;return;}
        if(best>=0){Hit *h=&a->hits[best];
            if(h->kind==H_SESSION||h->kind==H_HEADER){if(e->button.button!=SDL_BUTTON_LEFT)return;
                if(strcmp(h->id,"room")&&strcmp(h->id,"media")&&strcmp(h->id,"review"))copy(a->selected,sizeof(a->selected),h->id);
                copy(a->drag,sizeof(a->drag),h->id);a->dragx=a->mousex;a->dragy=a->mousey;a->dragging=0;
                if(h->kind==H_SESSION){Session *member=session(a,h->id);
                    if(member&&!in_workspace(a,member)){char id[81];copy(id,sizeof(id),member->room_id);switch_workspace(a,id);geometry(a);}
                    int found=0;for(int j=0;j<a->dock.tile_count;j++)if(!strcmp(a->dock.tiles[j].id,h->id))found=1;
                    Session *s=session(a,h->id);if(s&&!a->demo&&s->managed&&!s->connected)attach(a,s);
                    if(terminal_tile(s)){if(!found){lj_dock_drop(&a->dock,h->id,"room",LJ_DOCK_LEFT);save_layout(a);}copy(a->focus,sizeof(a->focus),h->id);}else {copy(a->focus,sizeof(a->focus),"room");toast(a,"Observed session · no terminal to tile");}}
                a->dirty=1;return;
            }
            if(h->kind==H_TERMINAL){Session *s=session(a,h->id);copy(a->focus,sizeof(a->focus),h->id);copy(a->selected,sizeof(a->selected),h->id);
                if(s&&e->button.button==SDL_BUTTON_LEFT){a->selecting=1;a->sel_base=lj_vt_state(s->vt,LJ_VT_SCROLLED);copy(a->selection_id,sizeof(a->selection_id),s->id);a->selc0=a->selc1=clamp((a->mousex-s->grid.x)/CELLW,0,s->cols-1);a->selr0=a->selr1=clamp((a->mousey-s->grid.y)/CELLH+s->view_row,0,s->rows-1);}
                else mouse_send(a,s,a->mousex,a->mousey,e->button.button==SDL_BUTTON_LEFT?0:e->button.button==SDL_BUTTON_MIDDLE?1:2,0);a->dirty=1;return;}
            /* ⚠ ONE RECT, ONE TARGET. A right-click menu used to be registered
             * as a SECOND hit over the same rectangle as the thing it belongs
             * to. Equal areas make the smallest-target rule a coin toss decided
             * by draw order, and the menu won — so left-clicking a room tab
             * opened its burger instead of opening the room, and left-clicking
             * a folder did nothing at all. The right button now dispatches off
             * the primary hit; nothing is registered twice. */
            if(e->button.button==SDL_BUTTON_RIGHT){
                if(h->kind==H_FILE_ENTRY){
                    char t[256];
                    if(h->index){const char *root=a->files?jstr(a->files,"root"):"";
                        snprintf(t,sizeof(t),"d%s%s%s",root,h->id[0]?"/":"",h->id);}
                    else snprintf(t,sizeof(t),"f%s",h->id);
                    menu_open(a,MENU_FILE,h->r,t);
                }
                else if(h->kind==H_GROUP&&h->id[0])menu_open(a,MENU_ROOM,h->r,h->id);
                else if(h->kind==H_SESSION||h->kind==H_HEADER)menu_open(a,MENU_ROLE,h->r,h->id);
                return;
            }
            if(e->button.button==SDL_BUTTON_LEFT)activate(a,h);return;
        }
    }
    if(e->type==SDL_MOUSEBUTTONUP){
        if(a->canvas_drawing){a->canvas_drawing=0;canvas_point(a,e->button.x,e->button.y);
            if(a->canvas_npts>=1)lj_canvas_append_line(&a->canvas,a->canvas_pts,a->canvas_npts,lj_canvas_author_colour("operator"),4,"operator");
            a->canvas_npts=0;lj_canvas_poll(&a->canvas);a->dirty=1;return;}
        if(a->team||a->status_open||a->room_dialog||a->about_open||a->settings_open){a->drag[0]=0;a->dragging=a->selecting=0;a->split=-1;return;}
        if(a->split>=0){a->split=-1;save_layout(a);a->dirty=1;return;}
        if(a->selecting){a->selecting=0;a->dirty=1;
            /* A plain click (press and release in one cell) selects nothing: it used to
             * leave one highlighted cell behind and fire a clipboard request. */
            if(a->selr0==a->selr1&&a->selc0==a->selc1){a->sel_shown=0;return;}
            copy_selection(a);a->sel_shown=1;return;}
        if(a->dragging&&session(a,a->drag)){
            for(int i=a->nhits-1;i>=0;i--){Hit *target=&a->hits[i];if(target->kind==H_GROUP&&inside(target->r,e->button.x,e->button.y)){
                char id[81],room[81];copy(id,sizeof(id),a->drag);copy(room,sizeof(room),target->id);move_session(a,id,room);a->drag[0]=0;a->dragging=0;a->dirty=1;return;
            }}
        }
        if(a->dragging&&(!session(a,a->drag)||terminal_tile(session(a,a->drag)))){for(int i=0;i<a->dock.tile_count;i++){lj_dock_tile *t=&a->dock.tiles[i];if(inside(t->rect,e->button.x,e->button.y)){char target[81];copy(target,sizeof(target),t->id);lj_dock_drop(&a->dock,a->drag,target,lj_dock_edge_at(t->rect,e->button.x,e->button.y));break;}}save_layout(a);}
        else if(a->drag[0]&&(!session(a,a->drag)||terminal_tile(session(a,a->drag))))copy(a->focus,sizeof(a->focus),a->drag);
        else {Session *s=session(a,a->focus);if(s)mouse_send(a,s,e->button.x,e->button.y,e->button.button==SDL_BUTTON_LEFT?0:e->button.button==SDL_BUTTON_MIDDLE?1:2,1);}
        a->drag[0]=0;a->dragging=0;a->dirty=1;
    }
    if(e->type==SDL_MOUSEWHEEL&&a->status_open){int dy=e->wheel.y;if(e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED)dy=-dy;a->status_scroll-=dy*CELLH*2;a->dirty=1;return;}
    if(e->type==SDL_MOUSEWHEEL&&a->settings_open){int dy=e->wheel.y;if(e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED)dy=-dy;a->settings_scroll-=dy*3;if(a->settings_scroll<0)a->settings_scroll=0;a->dirty=1;return;}
    if(e->type==SDL_MOUSEWHEEL&&!a->team&&!a->status_open&&!a->room_dialog&&!a->about_open&&!a->settings_open){
        int dy=e->wheel.y;if(e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED)dy=-dy;
        for(int i=0;i<a->dock.tile_count;i++)if(inside(a->dock.tiles[i].rect,a->mousex,a->mousey)){
            const char *id=a->dock.tiles[i].id;
            if(!strcmp(id,"room")){
                /* ⚠ TWO PANES, TWO SCROLLS. The todo column shares the room
                 * tile, and routing every wheel event to the chat made the list
                 * unreachable the moment it grew past the tile. */
                if((a->room_tab==RT_ACTION||a->room_tab==RT_REVIEW)&&inside(a->todo_rect,a->mousex,a->mousey))a->todo_scroll-=dy*CELLH*3;
                /* ⚠ THE STATUS TAB HAD NO SCROLL OF ITS OWN. The handler knew
                 * about the todo column and routed EVERYTHING ELSE to
                 * room_scroll — which the STATUS panel does not read, so a long
                 * ribbon simply could not be reached: room_scroll moved, the
                 * view did not. codex measured status_scroll stuck at 0 while
                 * room_scroll went to -200. Status has its own offset; use it. */
                else if(a->room_tab==RT_STATUS)a->status_scroll-=dy*CELLH*2;
                else a->room_scroll+=dy*40;
            }
            else if(!strcmp(id,"review"))a->review_scroll-=dy*CELLH*3;
            else if(!strcmp(id,"files"))a->files_scroll-=dy*CELLH*3;
            else if(!strcmp(id,"git"))a->git_scroll-=dy*CELLH*3;
            else {Session *s=session(a,id);if(s&&s->vt){if(lj_vt_state(s->vt,4)&&!(modifiers()&KMOD_SHIFT))mouse_send(a,s,a->mousex,a->mousey,dy>0?64:65,0);else lj_vt_scroll(s->vt,dy*3);}}break;
        }a->dirty=1;
    }
}
static int screenshot(App *a,const char *path){
    render(a);return lj_render_save_png(path)==0;
}
static void draw_gallery(App *a){
    static const char *names[]={"BG","PANEL","EDGE","SURF2","SURF3","TEXT","DIM","CYAN","AMBER","RED","GREEN","tile frame","lead dotted","focus","divider H","divider V","T junction","button default","button hover","button pressed","button disabled","tabs","badge","OPEN HERE","agent live","agent busy","agent ended","graph sixel","graph ansi","BAR glyph","marks","icons","spinner","dialog","toast","scrollbar"};
    const uint32_t cols[]={BG,PANEL,EDGE,SURF2,SURF3,TEXT,DIM,CYAN,AMBER,RED,GREEN,CYAN,GREEN,GREEN,EDGE,EDGE,EDGE,PANEL,SURF3,GREEN,DIM,SURF2,CYAN,CYAN,TEXT,DIM,RED,CYAN,CYAN,GREEN,TEXT,CYAN,DIM,PANEL,GREEN,DIM};
    int n=(int)(sizeof(names)/sizeof(names[0])),per=a->w/CELLW>=36?3:2,colw=a->w/per;
    panel((lj_rect){0,0,a->w,a->h},BG,0);text(CELLW,0,"STYLE GALLERY · every visual token",GREEN,a->w-2*CELLW);
    for(int i=0;i<n;i++){int x=(i%per)*colw+CELLW,y=CELLH+(i/per)*CELLH*2;if(y+CELLH>=a->h)break;panel((lj_rect){x,y,2*CELLW,CELLH},cols[i],0);text(x+3*CELLW,y,names[i],DIM,colw-4*CELLW);}
    text(CELLW,a->h-CELLH,"--gallery · labelled surfaces, controls, glyphs and states",DIM,a->w-2*CELLW);
}
static void help(void){puts("lilJack · native C / HUI / owkterm workspace\n\n  ./liljack [--fullscreen] [--effects] [--media FILE_OR_URL]\n  ./liljack --tui                    inside your terminal / SSH\n  ./liljack --window                 HUI window with media rendering\n  ./liljack --demo                   isolated visual preview\n  ./liljack --screenshot FILE.png    headless demo render\n  ./liljack owkterm [-- command]    native sixel terminal\n  ./liljack room|session ...         existing workspace CLI\n\nOptions: --project DIR --root DIR --width N --height N --frames N\nRooms: + Room creates a named group; drop agent cards on room titles or Standalone.\nMouse: select session; drag titles/cards to snap; drag dividers to resize.\nShift-drag terminal copies; Ctrl+Shift+V pastes; F6 room; F7 team; F11 full.\nCtrl+] releases terminal focus; Ctrl+Shift+Q detaches (Ctrl+Q in --tui).\nWindow media: images and muted 12-fps video, CPU decode; YouTube needs yt-dlp.\nSessions persist in the dedicated liljack-app tmux server after closing.");}
int main(int argc,char **argv){
    setlocale(LC_CTYPE,"");signal(SIGPIPE,SIG_IGN);signal(SIGTERM,quit_signal);signal(SIGINT,quit_signal);
    App *a=calloc(1,sizeof(*a));if(!a)return 1;a->w=1440;a->h=900;a->running=a->dirty=1;a->split=-1;a->backend_in=a->backend_out=-1;lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);lj_metrics_init();
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    copy(a->repo,sizeof(a->repo),getenv("LILJACK_REPO"));if(!a->repo[0])getcwd(a->repo,sizeof(a->repo));getcwd(a->project,sizeof(a->project));
    copy(a->root,sizeof(a->root),getenv("LILJACK_WORKSPACE_ROOT"));
    a->ansi=1;                      /* the only mode; kept as a field for --screenshot */
    const char *shot=NULL,*media=NULL;
    if(argc>1&&(!strcmp(argv[1],"room")||!strcmp(argv[1],"session"))){const char *toolbox_root=getenv("LILJACK_TOOLBOX_ROOT");char pythonpath[3*PATH_MAX+64];snprintf(pythonpath,sizeof(pythonpath),"%s:%s/toolbox%s%s/toolbox",a->repo,a->repo,toolbox_root?":":"",toolbox_root?toolbox_root:"");setenv("PYTHONPATH",pythonpath,1);char **args=calloc((size_t)argc+5,sizeof(char*));if(!args)return 1;args[0]="python3";args[1]="-m";args[2]="liljack_app.cli";for(int i=1;i<argc;i++)args[i+2]=argv[i];execvp(args[0],args);perror("lilJack CLI");return 1;}
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){help();free(a);return 0;}
        else if(!strcmp(argv[i],"--demo"))a->demo=1;
        else if(!strcmp(argv[i],"--gallery"))a->gallery=1,a->demo=1;
        /* ⚠ TUI ONLY. the operator, 2026-09-09: "remove not tui implementation what so
         * ever. from liljack". --tui is accepted and ignored because every
         * script and habit already types it; --window and --fullscreen are
         * refused rather than silently doing nothing, so nobody is left
         * wondering why a window did not appear. */
        else if(!strcmp(argv[i],"--tui")){}
        else if(!strcmp(argv[i],"--window")||!strcmp(argv[i],"--fullscreen")){
            fprintf(stderr,"lilJack is terminal-only now: %s was removed. Just run ./liljack.\n",argv[i]);
            free(a);return 1;
        }
        else if(!strcmp(argv[i],"--effects"))a->effects=1;
        else if(!strcmp(argv[i],"--width")&&i+1<argc)a->w=clamp(atoi(argv[++i]),800,7680);
        else if(!strcmp(argv[i],"--height")&&i+1<argc)a->h=clamp(atoi(argv[++i]),560,4320);
        else if(!strcmp(argv[i],"--frames")&&i+1<argc)a->frames=clamp(atoi(argv[++i]),1,100000);
        else if(!strcmp(argv[i],"--project")&&i+1<argc){char resolved[PATH_MAX];if(!realpath(argv[++i],resolved)){perror("project");return 1;}copy(a->project,sizeof(a->project),resolved);}
        else if(!strcmp(argv[i],"--root")&&i+1<argc)copy(a->root,sizeof(a->root),argv[++i]);
        else if(!strcmp(argv[i],"--media")&&i+1<argc)media=argv[++i];
        else if(!strcmp(argv[i],"--screenshot")&&i+1<argc){shot=argv[++i];a->demo=1;}
        else {fprintf(stderr,"Unknown/incomplete option: %s\n",argv[i]);help();free(a);return 1;}
    }
    char theme_error[256];if(lj_theme_load_default(theme_error,sizeof theme_error)<0)
        fprintf(stderr,"Theme: %s; using compiled defaults\n",theme_error);
    theme_apply_all(a,1);                         /* theme values for the plots/effects; a valid env override (or --effects) still wins at init */
    if(shot)a->ansi=0;
    if(a->ansi&&!lj_ansi_open(&a->w,&a->h)){free(a);return 1;}
    render_ansi=a->ansi;
    if(lj_render_init(a->w,a->h)<0){if(a->ansi)lj_ansi_close();fprintf(stderr,"HUI renderer initialization failed\n");free(a);return 1;}
    if(a->demo)demo(a);else {copy(a->focus,sizeof(a->focus),"room");toast(a,"🌿 Open a session below · drag titles to make this workspace yours");if(!backend_start(a))toast(a,"Could not start workspace helper");else action(a,"refresh");}
    if(media)open_media(a,media);
    int result=0;
    if(shot){a->ansi=0;
        /* --frames N with --screenshot: let the media helper and the canvas
         * settle for up to N frames first, so a headless render shows a
         * decoded video frame instead of "Reading media duration…". Without
         * --frames the render is immediate, exactly as before. */
        for(int i=0;a->frames&&i<a->frames;i++){int busy=0;
            if(a->has_media)busy|=lj_media_poll(&a->media);
            if(a->has_canvas)busy|=lj_canvas_poll(&a->canvas);
            popup_poll(a);SDL_Delay(16);(void)busy;}
        if(!screenshot(a,shot)){fprintf(stderr,"Could not write screenshot\n");result=1;}
             else printf("Headless render: %s\n",shot);goto cleanup;}
    while(a->running&&!quitting){SDL_Event e;
        for(;;){lj_ansi_message_input(message_input(a));if(!lj_ansi_poll(&e))break;event(a,&e);}
        for(int i=0;i<a->nsession;i++)a->dirty|=poll_terminal(a,&a->sessions[i]);
        if(!a->demo)a->dirty|=backend_poll(a);
        if(a->has_media)a->dirty|=lj_media_poll(&a->media);
        if(a->has_canvas)a->dirty|=lj_canvas_poll(&a->canvas);
        popup_poll(a);
        uint32_t now=SDL_GetTicks();if(!a->demo&&!a->busy&&a->backend_out>=0&&now-a->last_refresh>3000)request_room_log(a,a->room_log_before);
        /* Preserve the existing decoration clock; meter values have their own
         * completed-bucket revision below. No placeholder wave animation. */
        {uint32_t tick=now/120;if(tick!=a->anim){a->anim=tick;a->dirty=1;}}
        /* ⚠ SAMPLE WHENEVER SOMETHING WILL SHOW IT. Polling was tied to the
         * LOAD TILE alone, so the STATUS panel's MACHINE section could never
         * have live numbers unless a different tile happened to be docked. */
        /* ⚠ STOP CONDITIONING THE SAMPLE ON WHICH SURFACE IS OPEN. This
         * predicate has been wrong twice: first it named only the LOAD tile, so
         * the STATUS panel showed dashes; then it gained status_open but not
         * the room's STATUS TAB, which codex found. Every new surface that
         * shows a metric has to remember to extend a boolean somewhere else —
         * and the header ribbon is on screen ALWAYS, so the condition is now
         * simply true.
         *
         * It is also free: lj_metrics_poll() is internally rate-limited to
         * LJ_METRICS_INTERVAL_MS (25ms) and returns early between intervals,
         * and a real sample measured 178us. The `dirty` flag stays tied to the
         * surfaces that actually animate, so an idle UI does not repaint at
         * 50Hz just because the sampler ticked. */
        lj_metrics_poll();
        {uint64_t revision=lj_metrics_paced_revision();
         if(revision!=a->metrics_revision){a->metrics_revision=revision;a->dirty=1;}}
        if(!a->minimised&&(!a->copy_pause||!a->copy_pause_painted)&&(a->dirty||a->frames)&&now-a->last_paint>=16u){render(a);
            if(!lj_ansi_present()){a->running=0;result=1;}
            a->copy_pause_painted=a->copy_pause;a->last_paint=now;a->rendered++;if(a->frames&&a->rendered>=a->frames)break;}
        /* Border-only tick: never forces a full controller repaint. */
        if(!a->minimised&&!a->copy_pause&&!lj_ansi_border_tick(now/LJ_BORDER_TICK_MS)){a->running=0;result=1;}
        SDL_Delay(8);
    }
cleanup:
    save_layout(a);for(int i=0;i<a->nsession;i++)terminal_close(&a->sessions[i]);lj_media_close(&a->media);lj_popup_close(&a->popup);free(a->popup_pixels);lj_metrics_close();
    if(a->backend_in>=0)close(a->backend_in);if(a->backend_out>=0)close(a->backend_out);child_stop(a->backend_pid);free(a->response);if(a->messages)json_object_put(a->messages);if(a->status_ribbon)json_object_put(a->status_ribbon);if(a->review_panel)json_object_put(a->review_panel);if(a->room_log)json_object_put(a->room_log);if(a->files)json_object_put(a->files);if(a->room_states)json_object_put(a->room_states);if(a->room_questions)json_object_put(a->room_questions);if(a->room_dms)json_object_put(a->room_dms);if(a->room_todos)json_object_put(a->room_todos);if(a->all_tasks)json_object_put(a->all_tasks);free(a->pending);drafts_close(a);
    SDL_Quit();lj_render_close();lj_ansi_close();free(a);return result;
}
