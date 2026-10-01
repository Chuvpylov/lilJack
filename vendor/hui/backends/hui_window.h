/*
 * backends/hui_window.h — Minimal X11 windowed display for hui
 *
 * Zero install. Uses dlopen("libX11.so.6") at runtime.
 * Links against: -ldl  (always available)
 *
 * API:
 *   int  hui_win_open(int w, int h, const char *title, unsigned flags);
 *   bool hui_win_poll(void);
 *   void hui_win_blit(const uint32_t *src, int sw, int sh);  // virtual→scaled
 *   void hui_win_move(int x, int y);
 *   void hui_win_close(void);
 *
 * Flags:
 *   HUI_WIN_FLAG_UNDECORATED  — remove WM titlebar (draw your own)
 *
 * Scale:
 *   hui_win_scale_step = 0   → auto integer (largest N where N*sw≤window_w)
 *   hui_win_scale_step = N>0 → fixed N× scale
 *   Read-only after blit: hui_win_scale, hui_win_off_x, hui_win_off_y
 *
 * Input (set by hui_win_poll, in WINDOW pixel coords):
 *   hui_win_mouse_x/y, hui_win_mouse_btn, hui_win_key[256]
 *   hui_win_pos_x/y   — current window position (top-left)
 *
 * Clipboard (X11 CLIPBOARD selection, see the Clipboard section):
 *   hui_win_clipboard_set_text / get_text / get_data(target) / get_image
 *
 * Debug: HUI_TRACE=1 in the environment logs key presses and clipboard
 * transfers (targets, sizes, INCR chunks, timings) to stderr.
 */

#ifndef HUI_WINDOW_H
#define HUI_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Flags ---- */
#define HUI_WIN_FLAG_UNDECORATED (1u<<0)
/* Set EWMH window type to DESKTOP + state BELOW+STICKY.
 * Window sits behind all normal windows + the desktop — renders as animated wallpaper.
 * Implies UNDECORATED. Does NOT receive keyboard/mouse focus under GNOME/KDE. */
#define HUI_WIN_FLAG_DESKTOP     (1u<<1)

/* ---- X11 event field access (Linux x86_64) ---- */
#define HUI_XFLD_INT(ev,off)    (*((int     *)((char*)(ev)+(off))))
#define HUI_XFLD_UINT(ev,off)   (*((unsigned*)((char*)(ev)+(off))))
#define HUI_XFLD_ULONG(ev,off)  (*((unsigned long*)((char*)(ev)+(off))))
#define HUI_XFLD_LONG(ev,off)   (*((long    *)((char*)(ev)+(off))))

#define HUI_XEV_TYPE(ev)        HUI_XFLD_INT(ev,0)
#define HUI_XEV_X(ev)           HUI_XFLD_INT(ev,64)
#define HUI_XEV_Y(ev)           HUI_XFLD_INT(ev,68)
#define HUI_XEV_KEYCODE(ev)     HUI_XFLD_UINT(ev,84)
#define HUI_XEV_BUTTON(ev)      HUI_XFLD_UINT(ev,84)
#define HUI_XEV_STATE(ev)       HUI_XFLD_UINT(ev,80)
#define HUI_XEV_CFG_X(ev)       HUI_XFLD_INT(ev,48)
#define HUI_XEV_CFG_Y(ev)       HUI_XFLD_INT(ev,52)
#define HUI_XEV_CFG_W(ev)       HUI_XFLD_INT(ev,56)
#define HUI_XEV_CFG_H(ev)       HUI_XFLD_INT(ev,60)
#define HUI_XEV_MSG_TYPE(ev)    HUI_XFLD_ULONG(ev,40)
#define HUI_XEV_DATA_L0(ev)     HUI_XFLD_LONG(ev,56)

/* X11 event type numbers */
#define HUI_X_KeyPress          2
#define HUI_X_KeyRelease        3
#define HUI_X_ButtonPress       4
#define HUI_X_ButtonRelease     5
#define HUI_X_MotionNotify      6
#define HUI_X_Expose            12
#define HUI_X_ConfigureNotify   22
#define HUI_X_PropertyNotify    28
#define HUI_X_SelectionClear    29
#define HUI_X_SelectionRequest  30
#define HUI_X_SelectionNotify   31
#define HUI_X_ClientMessage     33
#define HUI_X_ZPixmap           2

/* XSelectInput masks */
#define HUI_X_ExposureMask        (1L<<15)
#define HUI_X_KeyPressMask        (1L<<0)
#define HUI_X_KeyReleaseMask      (1L<<1)
#define HUI_X_ButtonPressMask     (1L<<2)
#define HUI_X_ButtonReleaseMask   (1L<<3)
#define HUI_X_PointerMotionMask   (1L<<6)
#define HUI_X_StructureNotifyMask (1L<<17)
#define HUI_X_PropertyChangeMask  (1L<<22)

/* XSelectionRequestEvent / XSelectionEvent / XSelectionClearEvent /
 * XPropertyEvent field offsets (x86_64 Xlib layout: int type, ulong serial,
 * Bool send_event, Display*, then the per-event fields) */
#define HUI_XEV_SREQ_OWNER(ev)     HUI_XFLD_ULONG(ev,32)
#define HUI_XEV_SREQ_REQUESTOR(ev) HUI_XFLD_ULONG(ev,40)
#define HUI_XEV_SREQ_SELECTION(ev) HUI_XFLD_ULONG(ev,48)
#define HUI_XEV_SREQ_TARGET(ev)    HUI_XFLD_ULONG(ev,56)
#define HUI_XEV_SREQ_PROPERTY(ev)  HUI_XFLD_ULONG(ev,64)
#define HUI_XEV_SREQ_TIME(ev)      HUI_XFLD_ULONG(ev,72)
#define HUI_XEV_SNOT_REQUESTOR(ev) HUI_XFLD_ULONG(ev,32)
#define HUI_XEV_SNOT_SELECTION(ev) HUI_XFLD_ULONG(ev,40)
#define HUI_XEV_SNOT_TARGET(ev)    HUI_XFLD_ULONG(ev,48)
#define HUI_XEV_SNOT_PROPERTY(ev)  HUI_XFLD_ULONG(ev,56)
#define HUI_XEV_SNOT_TIME(ev)      HUI_XFLD_ULONG(ev,64)
#define HUI_XEV_SCLR_SELECTION(ev) HUI_XFLD_ULONG(ev,40)
#define HUI_XEV_PROP_WINDOW(ev)    HUI_XFLD_ULONG(ev,32)
#define HUI_XEV_PROP_ATOM(ev)      HUI_XFLD_ULONG(ev,40)
#define HUI_XEV_PROP_STATE(ev)     HUI_XFLD_INT(ev,56)

/* ---- X11 function pointer table ---- */

typedef struct {
    void  *handle;
    void* (*XOpenDisplay)(const char*);
    int   (*XDefaultScreen)(void*);
    void* (*XDefaultVisual)(void*,int);
    int   (*XDefaultDepth)(void*,int);
    unsigned long (*XRootWindow)(void*,int);
    unsigned long (*XBlackPixel)(void*,int);
    unsigned long (*XWhitePixel)(void*,int);
    int   (*XCloseDisplay)(void*);
    int   (*XFlush)(void*);
    int   (*XSync)(void*,int);
    unsigned long (*XCreateSimpleWindow)(void*,unsigned long,
        int,int,unsigned int,unsigned int,unsigned int,
        unsigned long,unsigned long);
    int   (*XSelectInput)(void*,unsigned long,long);
    int   (*XMapWindow)(void*,unsigned long);
    int   (*XStoreName)(void*,unsigned long,const char*);
    int   (*XDestroyWindow)(void*,unsigned long);
    void* (*XCreateGC)(void*,unsigned long,unsigned long,void*);
    int   (*XFreeGC)(void*,void*);
    void* (*XCreateImage)(void*,void*,unsigned int,int,int,
        char*,unsigned int,unsigned int,int,int);
    int   (*XPutImage)(void*,unsigned long,void*,void*,
        int,int,int,int,unsigned int,unsigned int);
    int   (*XPending)(void*);
    int   (*XNextEvent)(void*,void*);
    unsigned long (*XInternAtom)(void*,const char*,int);
    int   (*XSetWMProtocols)(void*,unsigned long,unsigned long*,int);
    int   (*XChangeProperty)(void*,unsigned long,unsigned long,unsigned long,
        int,int,const unsigned char*,int);
    int   (*XMoveWindow)(void*,unsigned long,int,int);
    int   (*XResizeWindow)(void*,unsigned long,unsigned int,unsigned int);
    int   (*XLookupString)(void*,char*,int,void*,void*);
    int   (*XGetWindowAttributes)(void*,unsigned long,void*);
    int   (*XIconifyWindow)(void*,unsigned long,int);
    int   (*XSendEvent)(void*,unsigned long,int,long,void*);
    int   (*XUngrabPointer)(void*,unsigned long);
    unsigned long (*XCreateFontCursor)(void*,unsigned int);
    int   (*XDefineCursor)(void*,unsigned long,unsigned long);
    int   (*XFree)(void*);
    int   (*XSetSelectionOwner)(void*,unsigned long,unsigned long,unsigned long);
    unsigned long (*XGetSelectionOwner)(void*,unsigned long);
    int   (*XConvertSelection)(void*,unsigned long,unsigned long,unsigned long,
        unsigned long,unsigned long);
    int   (*XGetWindowProperty)(void*,unsigned long,unsigned long,long,long,int,
        unsigned long,unsigned long*,int*,unsigned long*,unsigned long*,
        unsigned char**);
    int   (*XDeleteProperty)(void*,unsigned long,unsigned long);
} hui_x11_t;

/* ---- Global state ---- */

static hui_x11_t  hui__x11;
static void      *hui__x11_dpy    = NULL;
static unsigned long hui__x11_win  = 0;
static void      *hui__x11_gc     = NULL;
static void      *hui__x11_img    = NULL;
static unsigned long hui__x11_wm_del = 0;
static int        hui__x11_w = 0, hui__x11_h = 0;
static uint32_t  *hui__x11_pixels = NULL;
static bool       hui__x11_foreign = false; /* true when window is owned by caller */
static char       hui__x11_capture_path[512] = {0};
static int hui_win_save_ppm(const char *path);
static int hui_win_save_png(const char *path);

/* Public input / scale state — defined once (HUI_IMPLEMENTATION TU), extern elsewhere */
#ifdef HUI_IMPLEMENTATION
int  hui_win_mouse_x   = 0;
int  hui_win_mouse_y   = 0;
int  hui_win_mouse_btn = 0;
bool hui_win_key[256]  = {0};
bool hui_win_keys[256] = {0};
char hui_win_text_buf[32] = {0};
int  hui_win_text_len  = 0;
int  hui_win_pos_x     = 0;
int  hui_win_pos_y     = 0;
int  hui_win_w         = 0;
int  hui_win_h         = 0;
int  hui_win_scale_step= 0;
bool hui_win_quit      = false;
int  hui_win_scroll    = 0;
int  hui_win_scale     = 1;
int  hui_win_off_x     = 0;
int  hui_win_off_y     = 0;
int  hui_win_mods      = 0;
#else
extern int  hui_win_mouse_x;
extern int  hui_win_mouse_y;
extern int  hui_win_mouse_btn;
extern bool hui_win_key[256];
extern bool hui_win_keys[256];
extern char hui_win_text_buf[32];
extern int  hui_win_text_len;
extern int  hui_win_pos_x;
extern int  hui_win_pos_y;
extern int  hui_win_w;
extern int  hui_win_h;
extern int  hui_win_scale_step;
extern bool hui_win_quit;
extern int  hui_win_scroll;
extern int  hui_win_scale;
extern int  hui_win_off_x;
extern int  hui_win_off_y;
extern int  hui_win_mods;
#endif

/* ---- dlopen loader ---- */

#define HUI_XLOAD(fn) \
    { void *_s_ = dlsym(hui__x11.handle,#fn); \
      if (!_s_) { fprintf(stderr,"[hui_win] missing: %s\n",#fn); return -1; } \
      memcpy(&hui__x11.fn,&_s_,sizeof(void*)); }

static int hui__x11_load(void) {
    const char *libs[] = { "libX11.so.6","libX11.so",NULL };
    for (int i=0; libs[i]; i++) {
        hui__x11.handle = dlopen(libs[i], RTLD_LAZY);
        if (hui__x11.handle) break;
    }
    if (!hui__x11.handle) {
        fprintf(stderr,"[hui_win] Cannot open libX11: %s\n",dlerror());
        return -1;
    }
    HUI_XLOAD(XOpenDisplay)
    HUI_XLOAD(XDefaultScreen)
    HUI_XLOAD(XDefaultVisual)
    HUI_XLOAD(XDefaultDepth)
    HUI_XLOAD(XRootWindow)
    HUI_XLOAD(XBlackPixel)
    HUI_XLOAD(XWhitePixel)
    HUI_XLOAD(XCloseDisplay)
    HUI_XLOAD(XFlush)
    HUI_XLOAD(XSync)
    HUI_XLOAD(XCreateSimpleWindow)
    HUI_XLOAD(XSelectInput)
    HUI_XLOAD(XMapWindow)
    HUI_XLOAD(XStoreName)
    HUI_XLOAD(XDestroyWindow)
    HUI_XLOAD(XCreateGC)
    HUI_XLOAD(XFreeGC)
    HUI_XLOAD(XCreateImage)
    HUI_XLOAD(XPutImage)
    HUI_XLOAD(XPending)
    HUI_XLOAD(XNextEvent)
    HUI_XLOAD(XInternAtom)
    HUI_XLOAD(XSetWMProtocols)
    HUI_XLOAD(XChangeProperty)
    HUI_XLOAD(XMoveWindow)
    HUI_XLOAD(XResizeWindow)
    HUI_XLOAD(XLookupString)
    HUI_XLOAD(XGetWindowAttributes)
    HUI_XLOAD(XIconifyWindow)
    HUI_XLOAD(XSendEvent)
    HUI_XLOAD(XUngrabPointer)
    HUI_XLOAD(XCreateFontCursor)
    HUI_XLOAD(XDefineCursor)
    HUI_XLOAD(XFree)
    HUI_XLOAD(XSetSelectionOwner)
    HUI_XLOAD(XGetSelectionOwner)
    HUI_XLOAD(XConvertSelection)
    HUI_XLOAD(XGetWindowProperty)
    HUI_XLOAD(XDeleteProperty)
    return 0;
}

/* Map X11 keysym to HUI_KEY_* code. Returns -1 if not a special key. */
static int hui__keysym_to_hui(unsigned long ks) {
    /* Keep printable shortcuts in the same 8-bit key space as hui_key_pressed.
     * XLookupString reports shifted letters as uppercase; normalize those so
     * Ctrl+K and Ctrl+Shift+K address the conventional lowercase binding. */
    if (ks >= 'A' && ks <= 'Z') return (int)(ks - 'A' + 'a');
    if (ks >= 32 && ks < 127) return (int)ks;
    switch (ks) {
        case 0xFF08: return 0x08; /* HUI_KEY_BACKSPACE */
        case 0xFF09: return 0x09; /* HUI_KEY_TAB */
        case 0xFF0D: return 0x0D; /* HUI_KEY_RETURN */
        case 0xFF1B: return 0x1B; /* HUI_KEY_ESCAPE */
        case 0xFFFF: return 0x7F; /* HUI_KEY_DELETE */
        case 0xFF51: return 0x80; /* HUI_KEY_LEFT */
        case 0xFF53: return 0x81; /* HUI_KEY_RIGHT */
        case 0xFF52: return 0x82; /* HUI_KEY_UP */
        case 0xFF54: return 0x83; /* HUI_KEY_DOWN */
        case 0xFF50: return 0x84; /* HUI_KEY_HOME */
        case 0xFF57: return 0x85; /* HUI_KEY_END */
        case 0xFF55: return 0x86; /* HUI_KEY_PGUP */
        case 0xFF56: return 0x87; /* HUI_KEY_PGDN */
        case 0xFFBE: return 0x88; /* HUI_KEY_F1  */
        case 0xFFBF: return 0x89; /* HUI_KEY_F2  */
        case 0xFFC0: return 0x8A; /* HUI_KEY_F3  */
        case 0xFFC1: return 0x8B; /* HUI_KEY_F4  */
        case 0xFFC2: return 0x8C; /* HUI_KEY_F5  */
        case 0xFFC3: return 0x8D; /* HUI_KEY_F6  */
        case 0xFFC4: return 0x8E; /* HUI_KEY_F7  */
        case 0xFFC5: return 0x8F; /* HUI_KEY_F8  */
        case 0xFFC6: return 0x90; /* HUI_KEY_F9  */
        case 0xFFC7: return 0x91; /* HUI_KEY_F10 */
        case 0xFFC8: return 0x92; /* HUI_KEY_F11 */
        case 0xFFC9: return 0x93; /* HUI_KEY_F12 */
        default:     return -1;
    }
}

/* ---- Public API ---- */

static void hui_win_request_capture(const char *path) {
    if (!path || !path[0]) {
        hui__x11_capture_path[0] = '\0';
        return;
    }
    snprintf(hui__x11_capture_path, sizeof(hui__x11_capture_path), "%s", path);
}

static int hui_win_open(int w, int h, const char *title, unsigned flags) {
    if (hui__x11_load() != 0) return -1;

    hui__x11_dpy = hui__x11.XOpenDisplay(NULL);
    if (!hui__x11_dpy) {
        fprintf(stderr,"[hui_win] XOpenDisplay failed\n");
        return -1;
    }

    int scr          = hui__x11.XDefaultScreen(hui__x11_dpy);
    unsigned long root  = hui__x11.XRootWindow(hui__x11_dpy,scr);
    unsigned long black = hui__x11.XBlackPixel(hui__x11_dpy,scr);

    hui__x11_w = w; hui__x11_h = h;
    hui_win_w  = w; hui_win_h  = h;

    hui__x11_win = hui__x11.XCreateSimpleWindow(
        hui__x11_dpy, root, 0, 0, (unsigned)w, (unsigned)h,
        1, black, black);

    /* WM_DELETE_WINDOW */
    hui__x11_wm_del = hui__x11.XInternAtom(hui__x11_dpy,"WM_DELETE_WINDOW",0);
    unsigned long wm_prot = hui__x11.XInternAtom(hui__x11_dpy,"WM_PROTOCOLS",0);
    (void)wm_prot;
    hui__x11.XSetWMProtocols(hui__x11_dpy,hui__x11_win,&hui__x11_wm_del,1);

    /* Remove WM decorations */
    if (flags & (HUI_WIN_FLAG_UNDECORATED | HUI_WIN_FLAG_DESKTOP)) {
        unsigned long motif[5] = {2,0,0,0,0}; /* MWM_HINTS_DECORATIONS=2, deco=0 */
        unsigned long matom = hui__x11.XInternAtom(
            hui__x11_dpy,"_MOTIF_WM_HINTS",0);
        hui__x11.XChangeProperty(
            hui__x11_dpy, hui__x11_win, matom, matom, 32,
            0 /*PropModeReplace*/, (const unsigned char*)motif, 5);
    }

    /* Borderless application windows are still ordinary NORMAL windows.
     * Declaring this explicitly prevents some WMs from treating them as an
     * unmanaged/full-desktop surface when maximizing on mixed monitors. */
    if ((flags & HUI_WIN_FLAG_UNDECORATED) && !(flags & HUI_WIN_FLAG_DESKTOP)) {
        unsigned long wtype = hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_WINDOW_TYPE",0);
        unsigned long normal = hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_WINDOW_TYPE_NORMAL",0);
        hui__x11.XChangeProperty(hui__x11_dpy,hui__x11_win,wtype,4,32,0,
            (const unsigned char*)&normal,1);
    }

    /* EWMH desktop window: sits behind all normal windows (animated wallpaper) */
    if (flags & HUI_WIN_FLAG_DESKTOP) {
        /* _NET_WM_WINDOW_TYPE = _NET_WM_WINDOW_TYPE_DESKTOP */
        unsigned long wtype =
            hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_WINDOW_TYPE", 0);
        unsigned long wtype_desktop =
            hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", 0);
        hui__x11.XChangeProperty(hui__x11_dpy, hui__x11_win, wtype,
            4 /*XA_ATOM*/, 32, 0 /*PropModeReplace*/,
            (const unsigned char*)&wtype_desktop, 1);
        /* _NET_WM_STATE = { _NET_WM_STATE_BELOW, _NET_WM_STATE_STICKY } */
        unsigned long wstate =
            hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_STATE", 0);
        unsigned long states[2] = {
            hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_STATE_BELOW",  0),
            hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_STATE_STICKY", 0),
        };
        hui__x11.XChangeProperty(hui__x11_dpy, hui__x11_win, wstate,
            4 /*XA_ATOM*/, 32, 0 /*PropModeReplace*/,
            (const unsigned char*)states, 2);
    }

    hui__x11.XSelectInput(hui__x11_dpy, hui__x11_win,
        HUI_X_ExposureMask | HUI_X_KeyPressMask | HUI_X_KeyReleaseMask |
        HUI_X_ButtonPressMask | HUI_X_ButtonReleaseMask |
        HUI_X_PointerMotionMask | HUI_X_StructureNotifyMask |
        HUI_X_PropertyChangeMask);

    hui__x11.XStoreName(hui__x11_dpy,hui__x11_win,title?title:"hui");
    hui__x11.XMapWindow(hui__x11_dpy,hui__x11_win);

    hui__x11_gc = hui__x11.XCreateGC(hui__x11_dpy,hui__x11_win,0,NULL);

    hui__x11_pixels = (uint32_t*)calloc((size_t)(w*h),sizeof(uint32_t));

    void *visual = hui__x11.XDefaultVisual(hui__x11_dpy,scr);
    int   depth  = hui__x11.XDefaultDepth(hui__x11_dpy,scr);
    hui__x11_img = hui__x11.XCreateImage(
        hui__x11_dpy, visual, (unsigned)depth,
        HUI_X_ZPixmap, 0, (char*)hui__x11_pixels,
        (unsigned)w, (unsigned)h, 32, 0);
    if (!hui__x11_img) {
        fprintf(stderr,"[hui_win] XCreateImage failed\n");
        return -1;
    }

    hui__x11.XFlush(hui__x11_dpy);
    return 0;
}

/*
 * Blit virtual canvas (sw×sh) to window with integer-step nearest-neighbor
 * scaling + letterboxing.  Steppiness comes from the integer snap: content
 * jumps from 1× to 2× to 3× as you widen the window.
 */
/* Damage-based present: only the bounding box of pixels that changed since the
 * previous frame is sent with XPutImage. A mostly static 4K dashboard then costs
 * a few rows per frame instead of a 32 MB upload (which stalls when the GPU is
 * busy). Set hui_win_damage = false to always push the full frame. */
static uint32_t *hui__x11_prev = NULL;
static int       hui__x11_prev_w = 0, hui__x11_prev_h = 0;
static bool      hui__x11_need_full = true;
/* App-settable flag + diagnostics. Defined ONCE, in the HUI_IMPLEMENTATION TU,
 * and declared extern elsewhere: hui_window.h is included by every TU via
 * hui_x11.h (hui.h:449-450 pulls in the backend for any -DHUI_BACKEND_X11), so
 * non-static definitions here collide on GCC>=10 (-fno-common is the default)
 * and break every multi-TU X11 build. */
#ifdef HUI_IMPLEMENTATION
bool             hui_win_damage = true;
long             hui__x11_damage_px = 0; int hui__x11_damage_bands = 0;   /* last frame, for diagnostics */
#else
extern bool      hui_win_damage;
extern long      hui__x11_damage_px; extern int hui__x11_damage_bands;
#endif
static void hui_win_blit(const uint32_t *src, int sw, int sh) {
    if (!hui__x11_dpy || !hui__x11_img || !src) return;
    int dw = hui__x11_w, dh = hui__x11_h;
    hui__scale_argb_letterbox(hui__x11_pixels, dw, dh, src, sw, sh,
                              hui_win_scale_step,
                              &hui_win_scale,
                              &hui_win_off_x,
                              &hui_win_off_y);
    bool full = true;
    if (hui_win_damage) {
        if (!hui__x11_prev || hui__x11_prev_w != dw || hui__x11_prev_h != dh) {
            free(hui__x11_prev);
            hui__x11_prev = (uint32_t*)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
            hui__x11_prev_w = dw; hui__x11_prev_h = dh; hui__x11_need_full = true;
        }
        if (!hui__x11_need_full && hui__x11_prev) full = false;
    }
    if (hui__x11_capture_path[0]) {
        const char *dot = strrchr(hui__x11_capture_path, '.');
        if (dot && strcmp(dot, ".png") == 0) hui_win_save_png(hui__x11_capture_path);
        else hui_win_save_ppm(hui__x11_capture_path);
        hui__x11_capture_path[0] = '\0';
    }


    if (full) {
        if (hui__x11_prev) { memcpy(hui__x11_prev, hui__x11_pixels, (size_t)dw * (size_t)dh * sizeof(uint32_t)); hui__x11_need_full = false; }
        hui__x11.XPutImage(hui__x11_dpy,hui__x11_win,hui__x11_gc,hui__x11_img, 0,0,0,0,(unsigned)dw,(unsigned)dh);
        hui__x11_damage_px = (long)dw * dh; hui__x11_damage_bands = 1;
    } else {
        /* each contiguous run of dirty rows becomes one XPutImage with its own x-range */
        hui__x11_damage_px = 0; hui__x11_damage_bands = 0;
        int y = 0;
        while (y < dh) {
            const uint32_t *a = hui__x11_pixels + (size_t)y * (size_t)dw, *bp = hui__x11_prev + (size_t)y * (size_t)dw;
            if (memcmp(a, bp, (size_t)dw * 4) == 0) { y++; continue; }
            int y0 = y, x0 = dw, x1 = -1;
            while (y < dh) {
                a = hui__x11_pixels + (size_t)y * (size_t)dw; bp = hui__x11_prev + (size_t)y * (size_t)dw;
                if (memcmp(a, bp, (size_t)dw * 4) == 0) break;
                int l = 0; while (l < x0 && a[l] == bp[l]) l++;
                int r = dw - 1; while (r > x1 && a[r] == bp[r]) r--;
                if (l < x0) x0 = l; if (r > x1) x1 = r;
                y++;
            }
            int y1 = y - 1;
            for (int yy = y0; yy <= y1; yy++)
                memcpy(hui__x11_prev + (size_t)yy * (size_t)dw + (size_t)x0, hui__x11_pixels + (size_t)yy * (size_t)dw + (size_t)x0, (size_t)(x1 - x0 + 1) * 4);
            hui__x11.XPutImage(hui__x11_dpy,hui__x11_win,hui__x11_gc,hui__x11_img, x0,y0,x0,y0,(unsigned)(x1 - x0 + 1),(unsigned)(y1 - y0 + 1));
            hui__x11_damage_px += (long)(x1 - x0 + 1) * (y1 - y0 + 1); hui__x11_damage_bands++;
        }
    }
    hui__x11.XFlush(hui__x11_dpy);
}

static int hui_win_save_ppm(const char *path) {
    if (!hui__x11_pixels || !path || hui__x11_w <= 0 || hui__x11_h <= 0) return -1;
    hui__save_argb_ppm(path, hui__x11_pixels, hui__x11_w, hui__x11_h);
    return 0;
}

static int hui_win_save_png(const char *path) {
    if (!hui__x11_pixels || !path || hui__x11_w <= 0 || hui__x11_h <= 0) return -1;
    return hui__save_argb_png(path, hui__x11_pixels, hui__x11_w, hui__x11_h);
}

/* Attach to an existing X11 window (e.g. one provided by xwinwrap via --wid WID).
 * Call instead of hui_win_open(). The window already exists and is mapped —
 * we just create a GC + XImage to draw into it.
 * w/h should match the window's actual pixel dimensions. */
static int hui_win_attach(unsigned long wid, int w, int h) __attribute__((unused));
static int hui_win_attach(unsigned long wid, int w, int h) {
    if (hui__x11_load() != 0) return -1;

    hui__x11_dpy = hui__x11.XOpenDisplay(NULL);
    if (!hui__x11_dpy) {
        fprintf(stderr, "[hui_win] XOpenDisplay failed\n");
        return -1;
    }

    hui__x11_win     = wid;
    hui__x11_w       = w;
    hui__x11_h       = h;
    hui_win_w        = w;
    hui_win_h        = h;
    hui__x11_foreign = true;

    /* WM_DELETE_WINDOW (best-effort on foreign window) */
    hui__x11_wm_del = hui__x11.XInternAtom(hui__x11_dpy, "WM_DELETE_WINDOW", 0);

    hui__x11.XSelectInput(hui__x11_dpy, hui__x11_win,
        HUI_X_ExposureMask | HUI_X_KeyPressMask | HUI_X_KeyReleaseMask |
        HUI_X_ButtonPressMask | HUI_X_ButtonReleaseMask |
        HUI_X_PointerMotionMask | HUI_X_StructureNotifyMask |
        HUI_X_PropertyChangeMask);

    hui__x11_gc = hui__x11.XCreateGC(hui__x11_dpy, hui__x11_win, 0, NULL);

    hui__x11_pixels = (uint32_t*)calloc((size_t)(w * h), sizeof(uint32_t));

    int   scr    = hui__x11.XDefaultScreen(hui__x11_dpy);
    void *visual = hui__x11.XDefaultVisual(hui__x11_dpy, scr);
    int   depth  = hui__x11.XDefaultDepth(hui__x11_dpy, scr);
    hui__x11_img = hui__x11.XCreateImage(
        hui__x11_dpy, visual, (unsigned)depth,
        HUI_X_ZPixmap, 0, (char*)hui__x11_pixels,
        (unsigned)w, (unsigned)h, 32, 0);
    if (!hui__x11_img) {
        fprintf(stderr, "[hui_win] XCreateImage failed\n");
        return -1;
    }
    hui__x11.XFlush(hui__x11_dpy);
    return 0;
}

/* Query the root window (display) dimensions — use before hui_win_open() to
 * get the screen size for fullscreen or desktop-mode windows.
 * Falls back to 1920×1080 if display cannot be opened. */
static void hui_win_screen_size(int *out_w, int *out_h) __attribute__((unused));
static void hui_win_screen_size(int *out_w, int *out_h) {
    *out_w = 1920; *out_h = 1080; /* fallback */
    bool need_close = (hui__x11_dpy == NULL);
    if (need_close && hui__x11_load() != 0) return;
    void *dpy = hui__x11_dpy;
    if (!dpy) dpy = hui__x11.XOpenDisplay(NULL);
    if (!dpy) return;
    int scr = hui__x11.XDefaultScreen(dpy);
    unsigned long root = hui__x11.XRootWindow(dpy, scr);
    /* XWindowAttributes layout: x(int@0), y(int@4), width(int@8), height(int@12) */
    char attrs[512] = {0};
    hui__x11.XGetWindowAttributes(dpy, root, attrs);
    int rw = HUI_XFLD_INT(attrs, 8);
    int rh = HUI_XFLD_INT(attrs, 12);
    if (rw > 0 && rh > 0) { *out_w = rw; *out_h = rh; }
    if (need_close && dpy != hui__x11_dpy) hui__x11.XCloseDisplay(dpy);
}

/* Move window (for custom titlebar drag) */
static void hui_win_move(int x, int y) __attribute__((unused));
static void hui_win_move(int x, int y) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    hui__x11.XMoveWindow(hui__x11_dpy,hui__x11_win,x,y);
    hui__x11.XFlush(hui__x11_dpy);
}

/* (hui_win_resize already defined later in this header — reused as-is.) */

/* Minimize via the WM (XIconifyWindow sends WM_CHANGE_STATE internally) */
static void hui_win_minimize(void) __attribute__((unused));
static void hui_win_minimize(void) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    int scr = hui__x11.XDefaultScreen(hui__x11_dpy);
    hui__x11.XIconifyWindow(hui__x11_dpy, hui__x11_win, scr);
    hui__x11.XFlush(hui__x11_dpy);
}

/* EWMH _NET_WM_MOVERESIZE direction codes */
#define HUI_WMMR_SIZE_TOPLEFT     0
#define HUI_WMMR_SIZE_TOP         1
#define HUI_WMMR_SIZE_TOPRIGHT    2
#define HUI_WMMR_SIZE_RIGHT       3
#define HUI_WMMR_SIZE_BOTTOMRIGHT 4
#define HUI_WMMR_SIZE_BOTTOM      5
#define HUI_WMMR_SIZE_BOTTOMLEFT  6
#define HUI_WMMR_SIZE_LEFT        7
#define HUI_WMMR_MOVE             8

/* Hand an interactive move/resize to the WM via _NET_WM_MOVERESIZE. Works
   regardless of whether the WM reparents the (borderless) window, and gives
   OS-level edge snapping. Call once on the button-press; the WM follows the
   pointer until release. root_x/root_y = pointer position in root coords
   (best-effort; most WMs use the live pointer). XEvent built by byte offset —
   same 64-bit Xlib layout HUI's read macros already assume. */
static void hui_win_wm_moveresize(int root_x, int root_y, int direction) __attribute__((unused));
static void hui_win_wm_moveresize(int root_x, int root_y, int direction) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    unsigned long atom = hui__x11.XInternAtom(hui__x11_dpy, "_NET_WM_MOVERESIZE", 0);
    hui__x11.XUngrabPointer(hui__x11_dpy, 0 /*CurrentTime*/);
    char ev[192]; memset(ev, 0, sizeof ev);
    HUI_XFLD_INT(ev, 0)    = 33;               /* ClientMessage */
    HUI_XFLD_ULONG(ev, 32) = hui__x11_win;     /* .window        */
    HUI_XFLD_ULONG(ev, 40) = atom;             /* .message_type  */
    HUI_XFLD_INT(ev, 48)   = 32;               /* .format        */
    HUI_XFLD_LONG(ev, 56)  = root_x;           /* data.l[0]      */
    HUI_XFLD_LONG(ev, 64)  = root_y;           /* data.l[1]      */
    HUI_XFLD_LONG(ev, 72)  = direction;        /* data.l[2]      */
    HUI_XFLD_LONG(ev, 80)  = 1;                /* data.l[3] button 1 */
    HUI_XFLD_LONG(ev, 88)  = 1;                /* data.l[4] source: app */
    unsigned long root = hui__x11.XRootWindow(hui__x11_dpy,
                              hui__x11.XDefaultScreen(hui__x11_dpy));
    hui__x11.XSendEvent(hui__x11_dpy, root, 0, (1L<<20)|(1L<<19), ev);
    hui__x11.XFlush(hui__x11_dpy);
}

/* Set the window's pointer shape (X cursorfont value). Cached per shape;
   only re-applied when the shape changes. */
static void hui_win_set_cursor(int shape) __attribute__((unused));
static void hui_win_set_cursor(int shape) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    static int last_shape = -1;
    static unsigned long cache[160];
    /* 0 is XC_X_cursor in cursorfont — never what an app wants as "default".
     * Treat <=0 (and out-of-range) as the normal arrow. */
    if (shape <= 0 || shape >= 160) shape = 68;    /* XC_left_ptr */
    if (shape == last_shape) return;
    last_shape = shape;
    if (!cache[shape])
        cache[shape] = hui__x11.XCreateFontCursor(hui__x11_dpy, (unsigned)shape);
    hui__x11.XDefineCursor(hui__x11_dpy, hui__x11_win, cache[shape]);
    hui__x11.XFlush(hui__x11_dpy);
}

/* Toggle maximized state inside the physical monitor containing the window.
 * Several WMs maximize undecorated windows against the complete X11 root
 * (all monitors), which puts custom controls off-screen on asymmetric layouts. */
static void hui_win_maximize_toggle(void) __attribute__((unused));
static int hui__win_manual_maximized=0;
static int hui__win_restore_x=100,hui__win_restore_y=100,hui__win_restore_w=1280,hui__win_restore_h=800;
static int hui__win_restore_pending=0;
static void hui__win_remove_wm_max_state(void){
    unsigned long st=hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_STATE",0);
    unsigned long mv=hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_STATE_MAXIMIZED_VERT",0);
    unsigned long mh=hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_STATE_MAXIMIZED_HORZ",0);
    unsigned long fs=hui__x11.XInternAtom(hui__x11_dpy,"_NET_WM_STATE_FULLSCREEN",0);
    unsigned long root=hui__x11.XRootWindow(hui__x11_dpy,hui__x11.XDefaultScreen(hui__x11_dpy));
    unsigned long pairs[2][2]={{mv,mh},{fs,0}};
    for(int i=0;i<2;i++){
        char ev[192];memset(ev,0,sizeof ev);HUI_XFLD_INT(ev,0)=33;
        HUI_XFLD_ULONG(ev,32)=hui__x11_win;HUI_XFLD_ULONG(ev,40)=st;HUI_XFLD_INT(ev,48)=32;
        HUI_XFLD_LONG(ev,56)=0; /* _NET_WM_STATE_REMOVE */
        HUI_XFLD_LONG(ev,64)=(long)pairs[i][0];HUI_XFLD_LONG(ev,72)=(long)pairs[i][1];HUI_XFLD_LONG(ev,80)=1;
        hui__x11.XSendEvent(hui__x11_dpy,root,0,(1L<<20)|(1L<<19),ev);
    }
    hui__x11.XSync(hui__x11_dpy,0);
}
static void hui_win_restore_if_maximized(void) __attribute__((unused));
static void hui_win_restore_if_maximized(void){
    if(!hui__win_manual_maximized||!hui__x11_dpy||!hui__x11_win)return;
    if(hui__win_restore_pending)return;
    hui__win_remove_wm_max_state();
    /* The WM handles ClientMessage asynchronously — and may hold a move grab
     * (titlebar double-click) that clobbers a one-shot restore. The poll loop
     * reasserts the restore geometry every frame until the WM complies, for
     * up to this many frames (~1s at 60fps). */
    hui__win_restore_pending=60;
}
static void hui_win_maximize_toggle(void) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    if(hui__win_manual_maximized){ hui_win_restore_if_maximized(); return; }

    int mx=hui_win_pos_x+hui_win_w/2, my=hui_win_pos_y+hui_win_h/2;
    int tx=0,ty=0,tw=0,th=0;
    typedef struct { unsigned long name; int primary,automatic,noutput,x,y,width,height,mwidth,mheight; unsigned long *outputs; } HuiRRMonitor;
    void *xh=dlopen("libXrandr.so.2",RTLD_LAZY|RTLD_LOCAL);
    if(xh){
        HuiRRMonitor *(*query)(void*,unsigned long,int,int*)=
            (HuiRRMonitor*(*)(void*,unsigned long,int,int*))dlsym(xh,"XRRGetMonitors");
        void (*release)(HuiRRMonitor*)=(void(*)(HuiRRMonitor*))dlsym(xh,"XRRFreeMonitors");
        if(query&&release){
            int n=0; unsigned long root=hui__x11.XRootWindow(hui__x11_dpy,hui__x11.XDefaultScreen(hui__x11_dpy));
            HuiRRMonitor *ss=query(hui__x11_dpy,root,1,&n); int best=-1; long best_d=0x7fffffffL;
            for(int i=0;i<n;i++){
                if(mx>=ss[i].x&&mx<ss[i].x+ss[i].width&&my>=ss[i].y&&my<ss[i].y+ss[i].height){best=i;break;}
                long dx=(long)mx-(ss[i].x+ss[i].width/2),dy=(long)my-(ss[i].y+ss[i].height/2),d=dx*dx+dy*dy;
                if(d<best_d){best_d=d;best=i;}
            }
            if(best>=0){tx=ss[best].x;ty=ss[best].y;tw=ss[best].width;th=ss[best].height;}
            if(ss)release(ss);
        }
        dlclose(xh);
    }
    /* Failure is deliberately a no-op: spanning the root is never a safe
     * maximize fallback on multi-monitor desktops. */
    if(tw<=0||th<=0)return;
    /* Recover even if internal state was lost across a resize/event boundary:
     * actual monitor-sized geometry is authoritative. */
    const int safe_inset=8;
    int max_x=tx+safe_inset,max_y=ty+safe_inset;
    int max_w=tw-safe_inset*2,max_h=th-safe_inset*2;
    if(abs(hui_win_w-max_w)<=4 && abs(hui_win_h-max_h)<=4){
        hui__win_manual_maximized=1;
        if(hui__win_restore_w<320||hui__win_restore_h<240){
            hui__win_restore_w=1280;hui__win_restore_h=800;
            hui__win_restore_x=tx+(tw-hui__win_restore_w)/2;
            hui__win_restore_y=ty+(th-hui__win_restore_h)/2;
        }
        hui_win_restore_if_maximized(); return;
    }
    hui__win_restore_x=hui_win_pos_x;hui__win_restore_y=hui_win_pos_y;
    hui__win_restore_w=hui_win_w;hui__win_restore_h=hui_win_h;
    /* A tiny inset prevents WMs from auto-classifying the borderless window
     * as maximized and subsequently refusing application restore geometry. */
    hui__x11.XMoveWindow(hui__x11_dpy,hui__x11_win,max_x,max_y);
    hui__x11.XResizeWindow(hui__x11_dpy,hui__x11_win,(unsigned)max_w,(unsigned)max_h);
    hui__win_manual_maximized=1;
    hui__x11.XFlush(hui__x11_dpy);
}

static bool hui__x11_handle_event(char *ev);
static bool hui__clip_owned = false;   /* we currently own CLIPBOARD */

/* A key pressed AND released inside one poll (a fast tap, xdotool, key
 * repeat bursts) would never be seen down by any frame. Releases of keys
 * pressed in the current poll are deferred to the start of the next one so
 * every tap is visible for at least one frame. */
static int  hui__x11_trace = -1;               /* HUI_TRACE=1: log keys + clipboard to stderr */
static int  hui__trace(void) {
    if (hui__x11_trace < 0) hui__x11_trace = getenv("HUI_TRACE") != NULL;
    return hui__x11_trace;
}
static bool hui__key_pressed_now[256] = {0};   /* pressed during this poll */
static bool hui__key_release_pend[256] = {0};  /* release deferred to next poll */

/* Preserve a press/release pair until one rendered frame observes the press. */
static unsigned hui__mouse_pressed_now, hui__mouse_release_pend;
static void hui__mouse_begin_poll(void) {
    hui_win_mouse_btn &= ~(int)hui__mouse_release_pend;
    hui__mouse_release_pend = hui__mouse_pressed_now = 0;
}

/* Pump events. Returns false to quit. */
static bool hui_win_poll(void) {
    /* Custom titlebars set hui_win_quit from inside the previous frame.  Check
     * it here as well as in WM_DELETE_WINDOW handling so their close buttons
     * actually terminate the next loop iteration even when X has no pending
     * events. */
    if (!hui__x11_dpy || hui_win_quit) return false;
    if(hui__win_restore_pending>0){
        /* success test first: once the WM applied our size, stop asserting
         * (position tolerance is loose — WMs adjust it during unmap races) */
        if(abs(hui_win_w-hui__win_restore_w)<=4 && abs(hui_win_h-hui__win_restore_h)<=4){
            hui__win_restore_pending=0;
            hui__win_manual_maximized=0;
        } else {
            hui__win_restore_pending--;
            /* skip the first 2 frames: the WM is still processing the
             * _NET_WM_STATE removal; then reassert every frame until it
             * complies — this outlasts any move-grab clobber */
            if(hui__win_restore_pending<58){
                hui__x11.XResizeWindow(hui__x11_dpy,hui__x11_win,(unsigned)hui__win_restore_w,(unsigned)hui__win_restore_h);
                hui__x11.XMoveWindow(hui__x11_dpy,hui__x11_win,hui__win_restore_x,hui__win_restore_y);
                hui__x11.XFlush(hui__x11_dpy);
            }
            if(hui__win_restore_pending==0)
                hui__win_manual_maximized=0;   /* gave up; state stays consistent */
        }
    }
    char ev[192];
    hui_win_text_len = 0;  /* reset typed chars each poll */
    hui__mouse_begin_poll();
    for (int k = 0; k < 256; k++) {
        if (hui__key_release_pend[k]) { hui_win_keys[k] = false; hui__key_release_pend[k] = false; }
        hui__key_pressed_now[k] = false;
    }

    while (hui__x11.XPending(hui__x11_dpy) > 0) {
        hui__x11.XNextEvent(hui__x11_dpy, ev);
        if (hui__x11_handle_event(ev)) return false;
    }
    return true;
}

/* Clipboard owner-side reply to a SelectionRequest (we own CLIPBOARD). */
static void hui__x11_clip_serve(char *ev);

/* Dispatch one X event into the hui_win_* input state. Returns true when the
 * event asked the app to quit (WM_DELETE_WINDOW). Shared by hui_win_poll and
 * the synchronous clipboard fetch, which must keep pumping input while it
 * waits for the selection owner to answer. */
static bool hui__x11_handle_event(char *ev) {
    {
        switch (HUI_XEV_TYPE(ev)) {

        case HUI_X_KeyPress: {
            unsigned kc = HUI_XEV_KEYCODE(ev);
            if (kc < 256) hui_win_key[kc] = true;
            /* Capture modifier state: ShiftMask=0x1, ControlMask=0x4, Mod1Mask=0x8 */
            unsigned st = HUI_XEV_STATE(ev);
            hui_win_mods = 0;
            if (st & 0x0001) hui_win_mods |= 1;  /* Shift */
            if (st & 0x0004) hui_win_mods |= 2;  /* Ctrl  */
            if (st & 0x0008) hui_win_mods |= 4;  /* Alt   */
            /* Translate keysym: printable + ctrl chars → text_buf, special keys → hui_win_keys */
            char ch[8] = {0};
            unsigned long ks = 0;
            int n = hui__x11.XLookupString(ev, ch, 7, &ks, NULL);
            if (hui__trace()) fprintf(stderr, "[key] press kc=%u ks=%lx st=%x mods=%d\n", kc, ks, st, hui_win_mods);
            if (n > 0) {
                unsigned char c0 = (unsigned char)ch[0];
                /* Pass printable chars; also pass ctrl chars (1-31) generated by Ctrl+key,
                 * but exclude chars already handled as special keys (BS, CR, ESC, DEL) */
                int ctrl_ok = (c0 >= 1 && c0 <= 31 && c0 != 0x08 && c0 != 0x0D && c0 != 0x1B
                               && (st & 0x0004));
                int print_ok = (c0 >= 32 && c0 < 127);
                if (ctrl_ok || print_ok) {
                    for (int i = 0; i < n && hui_win_text_len < 31; i++)
                        hui_win_text_buf[hui_win_text_len++] = ch[i];
                }
            }
            int hk = hui__keysym_to_hui(ks);
            if (hk >= 0 && hk < 256) {
                hui_win_keys[hk] = true;
                hui__key_pressed_now[hk] = true;
                hui__key_release_pend[hk] = false;
            }
            break;
        }
        case HUI_X_KeyRelease: {
            unsigned kc = HUI_XEV_KEYCODE(ev);
            if (kc < 256) hui_win_key[kc] = false;
            char ch[8] = {0};
            unsigned long ks = 0;
            hui__x11.XLookupString(ev, ch, 7, &ks, NULL);
            int hk = hui__keysym_to_hui(ks);
            if (hk >= 0 && hk < 256) {
                if (hui__key_pressed_now[hk]) hui__key_release_pend[hk] = true;  /* tap: hold one frame */
                else hui_win_keys[hk] = false;
            }
            break;
        }
        case HUI_X_ButtonPress:
            hui_win_mouse_x = HUI_XEV_X(ev);
            hui_win_mouse_y = HUI_XEV_Y(ev);
            {
                int btn = HUI_XEV_BUTTON(ev);
                if      (btn == 4) hui_win_scroll += 1;
                else if (btn == 5) hui_win_scroll -= 1;
                else if (btn >= 1 && btn <= 8) {
                    unsigned mask = 1u << (btn - 1);
                    hui_win_mouse_btn |= (int)mask;
                    hui__mouse_pressed_now |= mask;
                    hui__mouse_release_pend &= ~mask;
                }
            }
            break;
        case HUI_X_ButtonRelease:
            hui_win_mouse_x = HUI_XEV_X(ev);
            hui_win_mouse_y = HUI_XEV_Y(ev);
            {
                int btn = HUI_XEV_BUTTON(ev);
                if (btn >= 1 && btn <= 8 && btn != 4 && btn != 5) {
                    unsigned mask = 1u << (btn - 1);
                    if (hui__mouse_pressed_now & mask) hui__mouse_release_pend |= mask;
                    else hui_win_mouse_btn &= ~(int)mask;
                }
            }
            break;
        case HUI_X_MotionNotify:
            hui_win_mouse_x = HUI_XEV_X(ev);
            hui_win_mouse_y = HUI_XEV_Y(ev);
            break;

        case 12: /* Expose */
            hui__x11_need_full = true;
            break;
        case HUI_X_ConfigureNotify: {
            int nw = HUI_XEV_CFG_W(ev), nh = HUI_XEV_CFG_H(ev);
            hui_win_pos_x = HUI_XEV_CFG_X(ev);
            hui_win_pos_y = HUI_XEV_CFG_Y(ev);
            if (nw>0 && nh>0 && (nw!=hui__x11_w || nh!=hui__x11_h)) {
                hui__x11_w = nw; hui__x11_h = nh;
                hui_win_w  = nw; hui_win_h  = nh;
                /* The X11 window may resize independently of the logical HUI
                 * framebuffer. Only rebuild the presentation buffer here. */
                free(hui__x11_pixels);
                hui__x11_pixels = (uint32_t*)calloc(
                    (size_t)(nw*nh),sizeof(uint32_t));
                *((char**)(((char*)hui__x11_img)+16)) = NULL;
                void *vis = hui__x11.XDefaultVisual(hui__x11_dpy,
                    hui__x11.XDefaultScreen(hui__x11_dpy));
                int dep = hui__x11.XDefaultDepth(hui__x11_dpy,
                    hui__x11.XDefaultScreen(hui__x11_dpy));
                hui__x11_img = hui__x11.XCreateImage(
                    hui__x11_dpy, vis, (unsigned)dep,
                    HUI_X_ZPixmap, 0, (char*)hui__x11_pixels,
                    (unsigned)nw,(unsigned)nh,32,0);
            }
            break;
        }
        case HUI_X_ClientMessage:
            if ((unsigned long)HUI_XEV_DATA_L0(ev) == hui__x11_wm_del) {
                hui_win_quit = true;
                return true;
            }
            break;
        case HUI_X_SelectionRequest:
            hui__x11_clip_serve(ev);
            break;
        case HUI_X_SelectionClear:
            /* another client took CLIPBOARD; keep our copy for local pastes */
            hui__clip_owned = false;
            break;
        default: break;
        }
    }
    return false;
}


/* ======================================================================
 * Clipboard (X11 CLIPBOARD selection)
 *
 *   hui_win_clipboard_set_text(text)        — become owner, serve UTF8/STRING
 *   hui_win_clipboard_get_text()            — malloc'd UTF-8 or NULL
 *   hui_win_clipboard_get_data(target,&len) — malloc'd bytes for any target
 *                                             ("image/png", ...) or NULL
 *   hui_win_clipboard_get_image(&len,&mime) — first of image/png, image/jpeg,
 *                                             image/bmp that the owner serves
 *
 * Fetches are synchronous: they pump X events (through the normal handler,
 * so input is not lost) until the owner answers or a timeout expires. INCR
 * transfers — how every owner hands over anything bigger than the server's
 * max request size, i.e. most screenshots — are reassembled transparently.
 * ====================================================================== */

static char  *hui__clip_text     = NULL;   /* our offered text (owner side) */
static size_t hui__clip_text_len = 0;

static unsigned long hui__x11_atom(const char *name) {
    return hui__x11.XInternAtom(hui__x11_dpy, name, 0);
}

static double hui__x11_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void hui__x11_sleep_ms(int ms) {
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

/* Read the whole of property `prop` on our window (any type). Returns a
 * malloc'd buffer (NUL-padded by one byte), sets type, format and len.
 * del=1 deletes the property afterwards (this is what advances an INCR). */
static unsigned char *hui__x11_read_prop(unsigned long prop, int del,
                                         unsigned long *type_out, int *fmt_out,
                                         size_t *len_out) {
    unsigned char *acc = NULL; size_t acc_len = 0;
    unsigned long type = 0, nitems = 0, after = 0; int fmt = 0;
    long off = 0;
    for (;;) {
        unsigned char *data = NULL;
        int r = hui__x11.XGetWindowProperty(hui__x11_dpy, hui__x11_win, prop,
                    off, 0x1000000L, 0, 0 /*AnyPropertyType*/,
                    &type, &fmt, &nitems, &after, &data);
        if (r != 0 /*Success*/ || !data) { if (data) hui__x11.XFree(data); break; }
        size_t unit = (fmt == 32) ? sizeof(long) : (fmt == 16 ? 2u : 1u);
        size_t bytes = (size_t)nitems * unit;
        unsigned char *n = (unsigned char*)realloc(acc, acc_len + bytes + 1);
        if (!n) { hui__x11.XFree(data); free(acc); acc = NULL; acc_len = 0; break; }
        acc = n; memcpy(acc + acc_len, data, bytes); acc_len += bytes; acc[acc_len] = 0;
        hui__x11.XFree(data);
        if (after == 0) break;
        off += (long)(bytes / 4);
    }
    if (del) hui__x11.XDeleteProperty(hui__x11_dpy, hui__x11_win, prop);
    if (type_out) *type_out = type;
    if (fmt_out)  *fmt_out  = fmt;
    if (len_out)  *len_out  = acc_len;
    if (!acc) { acc = (unsigned char*)calloc(1, 1); if (len_out) *len_out = 0; }
    return acc;
}

/* Wait (pumping other events) until an event of `type` arrives that passes
 * `match`; copies it into ev_out. Returns false on timeout. */
static bool hui__x11_wait_event(int type, bool (*match)(char*, void*), void *ctx,
                                char *ev_out, double timeout_ms) {
    double t0 = hui__x11_now_ms();
    char ev[192];
    for (;;) {
        while (hui__x11.XPending(hui__x11_dpy) > 0) {
            hui__x11.XNextEvent(hui__x11_dpy, ev);
            if (HUI_XEV_TYPE(ev) == type && match(ev, ctx)) {
                memcpy(ev_out, ev, sizeof ev); return true;
            }
            hui__x11_handle_event(ev);
        }
        if (hui__x11_now_ms() - t0 > timeout_ms) return false;
        hui__x11_sleep_ms(1);
    }
}

typedef struct { unsigned long selection, prop; } hui__clip_wait_t;
static bool hui__x11_match_selnotify(char *ev, void *ctx) {
    hui__clip_wait_t *w = (hui__clip_wait_t*)ctx;
    return HUI_XEV_SNOT_REQUESTOR(ev) == hui__x11_win &&
           HUI_XEV_SNOT_SELECTION(ev) == w->selection;
}
static bool hui__x11_match_propnew(char *ev, void *ctx) {
    hui__clip_wait_t *w = (hui__clip_wait_t*)ctx;
    if (hui__trace()) fprintf(stderr, "[clip]   PropertyNotify win=%lx (ours %lx) atom=%lu (want %lu) state=%d\n",
        HUI_XEV_PROP_WINDOW(ev), hui__x11_win, HUI_XEV_PROP_ATOM(ev), w->prop, HUI_XEV_PROP_STATE(ev));
    return HUI_XEV_PROP_WINDOW(ev) == hui__x11_win &&
           HUI_XEV_PROP_ATOM(ev) == w->prop &&
           HUI_XEV_PROP_STATE(ev) == 0 /*PropertyNewValue*/;
}

/* Fetch CLIPBOARD converted to `target`. Returns malloc'd bytes (+1 NUL
 * pad) or NULL if nobody owns the clipboard / owner refuses the target. */
static unsigned char *hui_win_clipboard_get_data(const char *target, size_t *len_out)
    __attribute__((unused));
static unsigned char *hui_win_clipboard_get_data(const char *target, size_t *len_out) {
    if (len_out) *len_out = 0;
    if (!hui__x11_dpy || !hui__x11_win || !target) return NULL;
    unsigned long sel  = hui__x11_atom("CLIPBOARD");
    unsigned long tgt  = hui__x11_atom(target);
    unsigned long prop = hui__x11_atom("HUI_CLIP");
    unsigned long incr = hui__x11_atom("INCR");
    int trace = hui__trace();
    if (hui__x11.XGetSelectionOwner(hui__x11_dpy, sel) == 0) { if (trace) fprintf(stderr, "[clip] %s: no owner\n", target); return NULL; }
    double t0 = hui__x11_now_ms();

    hui__x11.XDeleteProperty(hui__x11_dpy, hui__x11_win, prop);
    hui__x11.XConvertSelection(hui__x11_dpy, sel, tgt, prop, hui__x11_win, 0 /*CurrentTime*/);
    hui__x11.XFlush(hui__x11_dpy);

    hui__clip_wait_t w = { sel, prop };
    char ev[192];
    if (!hui__x11_wait_event(HUI_X_SelectionNotify, hui__x11_match_selnotify, &w, ev, 1500.0)) {
        if (trace) fprintf(stderr, "[clip] %s: timeout after %.0f ms\n", target, hui__x11_now_ms()-t0);
        return NULL;
    }
    if (HUI_XEV_SNOT_PROPERTY(ev) == 0) { if (trace) fprintf(stderr, "[clip] %s: refused (%.0f ms)\n", target, hui__x11_now_ms()-t0); return NULL; }

    unsigned long type = 0; int fmt = 0; size_t len = 0;
    unsigned char *data = hui__x11_read_prop(prop, 1, &type, &fmt, &len);
    if (type != incr) { if (trace) fprintf(stderr, "[clip] %s: %zu bytes direct (%.0f ms)\n", target, len, hui__x11_now_ms()-t0); if (len_out) *len_out = len; return data; }

    /* INCR: the property we just deleted kicked off chunked delivery. Each
     * chunk arrives as PropertyNewValue on `prop`; an empty chunk ends it. */
    free(data);
    unsigned char *acc = NULL; size_t acc_len = 0;
    for (;;) {
        if (!hui__x11_wait_event(HUI_X_PropertyNotify, hui__x11_match_propnew, &w, ev, 3000.0)) {
            if (trace) fprintf(stderr, "[clip] %s: INCR chunk timeout after %zu bytes (%.0f ms)\n", target, acc_len, hui__x11_now_ms()-t0);
            free(acc); return NULL;
        }
        size_t clen = 0;
        unsigned char *chunk = hui__x11_read_prop(prop, 1, &type, &fmt, &clen);
        if (trace) fprintf(stderr, "[clip]   chunk %zu bytes\n", clen);
        if (clen == 0) { free(chunk); break; }
        unsigned char *n = (unsigned char*)realloc(acc, acc_len + clen + 1);
        if (!n) { free(chunk); free(acc); return NULL; }
        acc = n; memcpy(acc + acc_len, chunk, clen); acc_len += clen; acc[acc_len] = 0;
        free(chunk);
    }
    if (!acc) { acc = (unsigned char*)calloc(1, 1); acc_len = 0; }
    if (trace) fprintf(stderr, "[clip] %s: %zu bytes via INCR (%.0f ms)\n", target, acc_len, hui__x11_now_ms()-t0);
    if (len_out) *len_out = acc_len;
    return acc;
}

/* Convenience: UTF-8 text (UTF8_STRING, then STRING). malloc'd or NULL. */
static char *hui_win_clipboard_get_text(void) __attribute__((unused));
static char *hui_win_clipboard_get_text(void) {
    if (hui__clip_owned && hui__clip_text) {
        char *t = (char*)malloc(hui__clip_text_len + 1);
        if (!t) return NULL;
        memcpy(t, hui__clip_text, hui__clip_text_len + 1); return t;
    }
    size_t len = 0;
    unsigned char *d = hui_win_clipboard_get_data("UTF8_STRING", &len);
    if (!d) d = hui_win_clipboard_get_data("STRING", &len);
    return (char*)d;   /* NUL-padded by hui__x11_read_prop */
}

/* First encoded image the owner offers. *mime_out names the format. */
static unsigned char *hui_win_clipboard_get_image(size_t *len_out, const char **mime_out)
    __attribute__((unused));
static unsigned char *hui_win_clipboard_get_image(size_t *len_out, const char **mime_out) {
    static const char *mimes[] = { "image/png", "image/jpeg", "image/bmp", NULL };
    for (int i = 0; mimes[i]; i++) {
        size_t len = 0;
        unsigned char *d = hui_win_clipboard_get_data(mimes[i], &len);
        if (d && len > 0) { if (len_out) *len_out = len; if (mime_out) *mime_out = mimes[i]; return d; }
        free(d);
    }
    if (len_out) *len_out = 0;
    return NULL;
}

/* Become CLIPBOARD owner offering `text`. Returns 0 on success. */
static int hui_win_clipboard_set_text(const char *text) __attribute__((unused));
static int hui_win_clipboard_set_text(const char *text) {
    if (!text) return -1;
    size_t len = strlen(text);
    char *copy = (char*)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, text, len + 1);
    free(hui__clip_text); hui__clip_text = copy; hui__clip_text_len = len;
    if (!hui__x11_dpy || !hui__x11_win) { hui__clip_owned = true; return 0; }
    unsigned long sel = hui__x11_atom("CLIPBOARD");
    hui__x11.XSetSelectionOwner(hui__x11_dpy, sel, hui__x11_win, 0 /*CurrentTime*/);
    hui__clip_owned = (hui__x11.XGetSelectionOwner(hui__x11_dpy, sel) == hui__x11_win);
    hui__x11.XFlush(hui__x11_dpy);
    return hui__clip_owned ? 0 : -1;
}

/* Answer a SelectionRequest for the text we own. */
static void hui__x11_clip_serve(char *ev) {
    unsigned long requestor = HUI_XEV_SREQ_REQUESTOR(ev);
    unsigned long selection = HUI_XEV_SREQ_SELECTION(ev);
    unsigned long target    = HUI_XEV_SREQ_TARGET(ev);
    unsigned long property  = HUI_XEV_SREQ_PROPERTY(ev);
    unsigned long tm        = HUI_XEV_SREQ_TIME(ev);
    if (property == 0) property = target;   /* obsolete requestors */

    unsigned long a_targets = hui__x11_atom("TARGETS");
    unsigned long a_utf8    = hui__x11_atom("UTF8_STRING");
    unsigned long a_text    = hui__x11_atom("TEXT");
    unsigned long a_string  = 31;   /* XA_STRING */
    unsigned long a_atom    = 4;    /* XA_ATOM */
    bool ok = false;
    if (selection == hui__x11_atom("CLIPBOARD") && hui__clip_text) {
        if (target == a_targets) {
            unsigned long atoms[4] = { a_targets, a_utf8, a_text, a_string };
            hui__x11.XChangeProperty(hui__x11_dpy, requestor, property, a_atom, 32, 0,
                                     (const unsigned char*)atoms, 4);
            ok = true;
        } else if (target == a_utf8 || target == a_text || target == a_string) {
            unsigned long type = (target == a_string) ? a_string : a_utf8;
            hui__x11.XChangeProperty(hui__x11_dpy, requestor, property, type, 8, 0,
                                     (const unsigned char*)hui__clip_text,
                                     (int)hui__clip_text_len);
            ok = true;
        }
    }
    char rep[192]; memset(rep, 0, sizeof rep);
    HUI_XEV_TYPE(rep) = HUI_X_SelectionNotify;
    *((void**)(rep + 24)) = hui__x11_dpy;      /* display */
    HUI_XEV_SNOT_REQUESTOR(rep) = requestor;
    HUI_XEV_SNOT_SELECTION(rep) = selection;
    HUI_XEV_SNOT_TARGET(rep)    = target;
    HUI_XEV_SNOT_PROPERTY(rep)  = ok ? property : 0;
    HUI_XEV_SNOT_TIME(rep)      = tm;
    hui__x11.XSendEvent(hui__x11_dpy, requestor, 0, 0, rep);
    hui__x11.XFlush(hui__x11_dpy);
}

static void hui_win_resize(int w, int h) __attribute__((unused));
static void hui_win_resize(int w, int h) {
    if (!hui__x11_dpy || !hui__x11_win) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    hui__x11.XResizeWindow(hui__x11_dpy, hui__x11_win,
                           (unsigned)w, (unsigned)h);
    hui__x11.XFlush(hui__x11_dpy);
}

static void hui_win_close(void) {
    if (!hui__x11_dpy) return;
    if (hui__x11_img) {
        *((char**)(((char*)hui__x11_img)+16)) = NULL;
        hui__x11_img = NULL;
    }
    free(hui__x11_pixels); hui__x11_pixels = NULL;
    if (hui__x11_gc)  { hui__x11.XFreeGC(hui__x11_dpy,hui__x11_gc); hui__x11_gc=NULL; }
    if (hui__x11_win && !hui__x11_foreign) {
        hui__x11.XDestroyWindow(hui__x11_dpy,hui__x11_win);
    }
    hui__x11_win = 0; hui__x11_foreign = false;
    hui__x11.XCloseDisplay(hui__x11_dpy);
    hui__x11_dpy = NULL;
    if (hui__x11.handle) { dlclose(hui__x11.handle); hui__x11.handle=NULL; }
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_WINDOW_H */
