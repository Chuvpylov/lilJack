/* hui_opengl.h — OpenGL 3.3 backend for hui
 *
 * Strategy: software rasterizer (hui_headless) → RGBA texture → full-screen quad.
 * Reuses 100% of existing draw logic; GL is used only for presentation.
 *
 * Select with: #define HUI_BACKEND_OPENGL before #include "hui.h"
 * Links: -lGL -lX11
 * Requires: OpenGL 3.3 core profile, X11/GLX (Linux)
 *
 * Pixel format note:
 *   hui__fb.pixels is uint32_t ARGB (0xAARRGGBB, native endian).
 *   On little-endian: bytes in memory are [B,G,R,A], so we upload
 *   GL_BGRA / GL_UNSIGNED_BYTE which maps correctly to GL_RGBA8 storage.
 */

#ifndef HUI_OPENGL_H
#define HUI_OPENGL_H

#include <stdbool.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/* ---- Public API ---- */

bool     hui_gl_is_available(void);
Display *hui_gl_display(void);
Window   hui_gl_window(void);
void     hui_gl_set_underlay(void (*draw)(int width,int height));
void     hui_gl_overlay_dirty(bool dirty);
bool     hui_gl_should_close(void);
void     hui_gl_size(int *width,int *height);
void     hui_gl_maximize_toggle(void);
void     hui_gl_set_title(const char *title);
void     hui_gl_poll(void);

/* ---- Implementation ---- */

#ifdef HUI_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Pull in the headless rasterizer; suppress its init/free/resize/flush
 * because we provide our own versions below. */
#define HUI_HEADLESS_NO_INIT
#define HUI_HEADLESS_NO_FLUSH
#include "hui_headless.h"

/* ---- GL context state ---- */

typedef struct {
    Display    *dpy;
    Window      win;
    GLXContext  ctx;
    GLuint      tex;   /* full-screen framebuffer texture */
    GLuint      vao;
    GLuint      vbo;
    GLuint      prog;
    int         w, h;
    bool        ok;    /* false → fallback to headless-only (no window) */
} hui__gl_ctx_t;

static hui__gl_ctx_t hui__gl;
static void (*hui__gl_underlay)(int,int);
static bool hui__gl_dirty=true;
static bool hui__gl_quit;
static bool hui__gl_maximized;

/* ---- IO mirror (populated by event pump) ---- */

static int     hui__gl_mx, hui__gl_my;
static uint8_t hui__gl_btn;
static int16_t hui__gl_scroll;
static bool    hui__gl_keys[256];
static char    hui__gl_text[32];
static int     hui__gl_text_len;
static uint8_t hui__gl_pressed_now, hui__gl_release_pending;

/* ---- Shader source ---- */

static const char *s_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";

static const char *s_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { frag_color = texture(u_tex, v_uv); }\n";

/* NDC quad: two triangles (TRIANGLE_STRIP), pos + uv.
 * UV y-flip: headless rasterizes top-down, GL texture origin is bottom-left,
 * so uv.y = 1-y flips it back. */
static const float s_quad[] = {
    /* pos      uv   */
    -1.0f,-1.0f,  0.0f,1.0f,
     1.0f,-1.0f,  1.0f,1.0f,
    -1.0f, 1.0f,  0.0f,0.0f,
     1.0f, 1.0f,  1.0f,0.0f,
};

/* ---- Shader helper ---- */

static GLuint hui__gl_compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, NULL, log);
        fprintf(stderr, "[hui_opengl] shader compile error: %s\n", log);
    }
    return s;
}

/* ---- X11 keysym → HUI_KEY_* ---- */

static int hui__gl_keysym(KeySym ks) {
    switch (ks) {
        case XK_Left:      return HUI_KEY_LEFT;
        case XK_Right:     return HUI_KEY_RIGHT;
        case XK_Up:        return HUI_KEY_UP;
        case XK_Down:      return HUI_KEY_DOWN;
        case XK_Home:      return HUI_KEY_HOME;
        case XK_End:       return HUI_KEY_END;
        case XK_Page_Up:   return HUI_KEY_PGUP;
        case XK_Page_Down: return HUI_KEY_PGDN;
        case XK_BackSpace: return HUI_KEY_BACKSPACE;
        case XK_Return:    return HUI_KEY_RETURN;
        case XK_Escape:    return HUI_KEY_ESCAPE;
        case XK_Delete:    return HUI_KEY_DELETE;
        case XK_Tab:       return HUI_KEY_TAB;
        default:           return -1;
    }
}

/* ---- Event pump ---- */

static void hui__gl_pump_events(void) {
    if (!hui__gl.ok) return;

    hui__gl_text_len = 0;
    hui__gl_scroll   = 0;
    hui__gl_btn &= (uint8_t)~hui__gl_release_pending;
    hui__gl_release_pending = hui__gl_pressed_now = 0;

    while (XPending(hui__gl.dpy)) {
        XEvent ev;
        XNextEvent(hui__gl.dpy, &ev);

        switch (ev.type) {
        case MotionNotify:
            hui__gl_mx = ev.xmotion.x;
            hui__gl_my = ev.xmotion.y;
            break;

        case ButtonPress:
            switch (ev.xbutton.button) {
            case Button1: hui__gl_btn |=  (1u << 0); hui__gl_pressed_now|=1u<<0; break;
            case Button3: hui__gl_btn |=  (1u << 1); hui__gl_pressed_now|=1u<<1; break;
            case Button2: hui__gl_btn |=  (1u << 2); hui__gl_pressed_now|=1u<<2; break;
            case Button4: hui__gl_scroll =  1; break; /* scroll up   */
            case Button5: hui__gl_scroll = -1; break; /* scroll down */
            }
            break;

        case ButtonRelease:
            switch (ev.xbutton.button) {
            case Button1: if(hui__gl_pressed_now&(1u<<0))hui__gl_release_pending|=1u<<0;else hui__gl_btn&=~(1u<<0); break;
            case Button3: if(hui__gl_pressed_now&(1u<<1))hui__gl_release_pending|=1u<<1;else hui__gl_btn&=~(1u<<1); break;
            case Button2: if(hui__gl_pressed_now&(1u<<2))hui__gl_release_pending|=1u<<2;else hui__gl_btn&=~(1u<<2); break;
            }
            break;

        case KeyPress: {
            /* Translate to HUI_KEY_* for special keys */
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int hk = hui__gl_keysym(ks);
            if (hk >= 0 && hk < 256) {
                hui__gl_keys[hk] = true;
            }
            /* Translate to printable ASCII for text input */
            char buf[8];
            int n = XLookupString(&ev.xkey, buf, (int)sizeof(buf)-1, NULL, NULL);
            if (n > 0) {
                buf[n] = '\0';
                int avail = (int)sizeof(hui__gl_text) - hui__gl_text_len - 1;
                if (avail > 0) {
                    int copy = n < avail ? n : avail;
                    memcpy(hui__gl_text + hui__gl_text_len, buf, (size_t)copy);
                    hui__gl_text_len += copy;
                    hui__gl_text[hui__gl_text_len] = '\0';
                }
            }
            break;
        }

        case KeyRelease: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int hk = hui__gl_keysym(ks);
            if (hk >= 0 && hk < 256) {
                hui__gl_keys[hk] = false;
            }
            break;
        }

        case ConfigureNotify:
            if (ev.xconfigure.width  != hui__gl.w ||
                ev.xconfigure.height != hui__gl.h) {
                hui__gl.w = ev.xconfigure.width;
                hui__gl.h = ev.xconfigure.height;
                hui_resize(hui__gl.w, hui__gl.h);
            }
            break;

        case ClientMessage:
            hui__gl_quit = true;
            break;

        case DestroyNotify:
            hui__gl_quit = true;
            /* The server has already destroyed this XID.  Clearing it keeps
             * shutdown from issuing a second XDestroyWindow request. */
            hui__gl.win = 0;
            break;

        default:
            break;
        }
    }
}

/* ---- GL init ---- */

static void hui__gl_init(int w, int h) {
    hui__gl.w  = w;
    hui__gl.h  = h;
    hui__gl.ok = false;

    /* 1. Open X display */
    hui__gl.dpy = XOpenDisplay(NULL);
    if (!hui__gl.dpy) {
        fprintf(stderr, "[hui_opengl] XOpenDisplay failed\n");
        return;
    }

    /* 2. Choose GLX visual */
    static int attr[] = {
        GLX_RGBA,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };
    XVisualInfo *vi = glXChooseVisual(hui__gl.dpy, DefaultScreen(hui__gl.dpy), attr);
    if (!vi) {
        fprintf(stderr, "[hui_opengl] glXChooseVisual failed\n");
        XCloseDisplay(hui__gl.dpy);
        hui__gl.dpy = NULL;
        return;
    }

    /* 3. Create colormap + window */
    Window root = RootWindow(hui__gl.dpy, vi->screen);
    Colormap cmap = XCreateColormap(hui__gl.dpy, root, vi->visual, AllocNone);

    XSetWindowAttributes swa;
    swa.colormap   = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask;

    hui__gl.win = XCreateWindow(
        hui__gl.dpy, root,
        0, 0, (unsigned)w, (unsigned)h, 0,
        vi->depth, InputOutput, vi->visual,
        CWColormap | CWEventMask, &swa);

    XStoreName(hui__gl.dpy, hui__gl.win, "hui");
    Atom wm_delete=XInternAtom(hui__gl.dpy,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(hui__gl.dpy,hui__gl.win,&wm_delete,1);
    XMapWindow(hui__gl.dpy, hui__gl.win);

    /* 4. Create GL context */
    hui__gl.ctx = glXCreateContext(hui__gl.dpy, vi, NULL, GL_TRUE);
    XFree(vi);
    if (!hui__gl.ctx) {
        fprintf(stderr, "[hui_opengl] glXCreateContext failed\n");
        if (hui__gl.win) XDestroyWindow(hui__gl.dpy, hui__gl.win);
        XCloseDisplay(hui__gl.dpy);
        hui__gl.dpy = NULL;
        return;
    }

    /* 5. Make context current */
    if (!glXMakeCurrent(hui__gl.dpy, hui__gl.win, hui__gl.ctx)) {
        fprintf(stderr, "[hui_opengl] glXMakeCurrent failed\n");
        glXDestroyContext(hui__gl.dpy, hui__gl.ctx);
        XDestroyWindow(hui__gl.dpy, hui__gl.win);
        XCloseDisplay(hui__gl.dpy);
        hui__gl.dpy = NULL;
        return;
    }

    /* 6. Create framebuffer texture */
    glGenTextures(1, &hui__gl.tex);
    glBindTexture(GL_TEXTURE_2D, hui__gl.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Allocate storage; pixels uploaded each frame via glTexSubImage2D */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    /* 7. Compile shaders */
    GLuint vs = hui__gl_compile(GL_VERTEX_SHADER,   s_vert_src);
    GLuint fs = hui__gl_compile(GL_FRAGMENT_SHADER, s_frag_src);

    hui__gl.prog = glCreateProgram();
    glAttachShader(hui__gl.prog, vs);
    glAttachShader(hui__gl.prog, fs);
    glLinkProgram(hui__gl.prog);

    GLint linked = 0;
    glGetProgramiv(hui__gl.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(hui__gl.prog, 512, NULL, log);
        fprintf(stderr, "[hui_opengl] program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    /* Bind texture unit 0 */
    glUseProgram(hui__gl.prog);
    glUniform1i(glGetUniformLocation(hui__gl.prog, "u_tex"), 0);

    /* 8. Create VAO + VBO for full-screen quad */
    glGenVertexArrays(1, &hui__gl.vao);
    glGenBuffers(1, &hui__gl.vbo);

    glBindVertexArray(hui__gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, hui__gl.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(s_quad), s_quad, GL_STATIC_DRAW);

    /* attrib 0: vec2 pos */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          4 * (GLsizei)sizeof(float), (void*)0);
    /* attrib 1: vec2 uv */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          4 * (GLsizei)sizeof(float),
                          (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    hui__gl.ok = true;
}

/* ---- init / free / resize (replaces HUI_HEADLESS_NO_INIT stubs) ---- */

static void hui_headless_init(int w, int h) {
    /* Allocate software pixel buffer */
    hui__fb.w        = w;
    hui__fb.h        = h;
    hui__fb.pixels   = (uint32_t*)calloc((size_t)(w * h), sizeof(uint32_t));
    hui__fb.clip_x0  = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1  = w; hui__fb.clip_y1 = h;
    hui__fb.clip_depth = 0;
    /* Initialise GL window */
    hui__gl_init(w, h);
}

static void hui_headless_free(void) {
    free(hui__fb.pixels);
    hui__fb.pixels = NULL;
    if (hui__gl.ok) {
        glDeleteTextures(1, &hui__gl.tex);
        glDeleteBuffers(1, &hui__gl.vbo);
        glDeleteVertexArrays(1, &hui__gl.vao);
        glDeleteProgram(hui__gl.prog);
        glXMakeCurrent(hui__gl.dpy, None, NULL);
        glXDestroyContext(hui__gl.dpy, hui__gl.ctx);
        XDestroyWindow(hui__gl.dpy, hui__gl.win);
        XCloseDisplay(hui__gl.dpy);
        hui__gl.ok  = false;
        hui__gl.dpy = NULL;
    }
}

static void hui_headless_resize(int w, int h) {
    free(hui__fb.pixels);
    hui__fb.w        = w;
    hui__fb.h        = h;
    hui__fb.pixels   = (uint32_t*)calloc((size_t)(w * h), sizeof(uint32_t));
    hui__fb.clip_x0  = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1  = w; hui__fb.clip_y1 = h;
    hui__fb.clip_depth = 0;
    hui__gl.w = w;
    hui__gl.h = h;
    /* Re-allocate texture storage */
    if (hui__gl.ok) {
        glBindTexture(GL_TEXTURE_2D, hui__gl.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    }
}

/* ---- hui_backend_flush ---- */

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    /* Static UI/labels are cached; GPU underlays may animate independently. */
    if(hui__gl_dirty)hui__rasterize(cmds, count, strpool, datapool);

    if (!hui__gl.ok) {
        /* Fallback: no window — caller can use hui_headless_save_ppm() */
        memset(hui__fb.pixels, 0,
               (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));
        return;
    }

    /* 2. Upload pixels to texture.
     *    hui__fb.pixels is 0xAARRGGBB (little-endian bytes: B,G,R,A)
     *    → GL_BGRA + GL_UNSIGNED_BYTE reads exactly those byte lanes. */
    glBindTexture(GL_TEXTURE_2D, hui__gl.tex);
    if(hui__gl_dirty)glTexSubImage2D(GL_TEXTURE_2D, 0,
                    0, 0, hui__gl.w, hui__gl.h,
                    GL_BGRA, GL_UNSIGNED_BYTE,
                    hui__fb.pixels);

    /* 3. Draw full-screen quad */
    glViewport(0, 0, hui__gl.w, hui__gl.h);
    glClear(GL_COLOR_BUFFER_BIT);
    if(hui__gl_underlay)hui__gl_underlay(hui__gl.w,hui__gl.h);
    glUseProgram(hui__gl.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hui__gl.tex);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glBindVertexArray(hui__gl.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDisable(GL_BLEND);

    /* 4. Present */
    glXSwapBuffers(hui__gl.dpy, hui__gl.win);

    /* 5. Clear software buffer for next frame */
    if(hui__gl_dirty)memset(hui__fb.pixels, 0,
           (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));
    hui__gl_dirty=false;

    /* Input is pumped explicitly before the app update, never after drawing. */
}

void hui_gl_poll(void){
    hui__gl_pump_events();
    if (hui_g) {
        hui_g->io.mouse_x   = (int16_t)hui__gl_mx;
        hui_g->io.mouse_y   = (int16_t)hui__gl_my;
        hui_g->io.mouse_btn = hui__gl_btn;
        hui_g->io.scroll_dy = hui__gl_scroll;
        memcpy(hui_g->io.keys, hui__gl_keys, 256 * sizeof(bool));
        int n = hui__gl_text_len < 31 ? hui__gl_text_len : 31;
        memcpy(hui_g->io.text_typed, hui__gl_text, (size_t)n);
        hui_g->io.text_typed[n]  = '\0';
        hui_g->io.text_typed_len = n;
    }
}

/* ---- Public API ---- */

bool hui_gl_is_available(void) { return hui__gl.ok; }

Display *hui_gl_display(void) { return hui__gl.dpy; }

Window hui_gl_window(void) { return hui__gl.win; }
void hui_gl_set_underlay(void (*draw)(int,int)){hui__gl_underlay=draw;}
void hui_gl_overlay_dirty(bool dirty){if(dirty)hui__gl_dirty=true;}
bool hui_gl_should_close(void){return hui__gl_quit||!hui__gl.ok;}
void hui_gl_size(int*w,int*h){if(w)*w=hui__gl.w;if(h)*h=hui__gl.h;}
void hui_gl_maximize_toggle(void){
 if(!hui__gl.ok)return;
 Atom state=XInternAtom(hui__gl.dpy,"_NET_WM_STATE",False),max_h=XInternAtom(hui__gl.dpy,"_NET_WM_STATE_MAXIMIZED_HORZ",False),max_v=XInternAtom(hui__gl.dpy,"_NET_WM_STATE_MAXIMIZED_VERT",False);XEvent e;memset(&e,0,sizeof e);e.type=ClientMessage;e.xclient.window=hui__gl.win;e.xclient.message_type=state;e.xclient.format=32;e.xclient.data.l[0]=hui__gl_maximized?0:1;e.xclient.data.l[1]=max_h;e.xclient.data.l[2]=max_v;e.xclient.data.l[3]=1;XSendEvent(hui__gl.dpy,DefaultRootWindow(hui__gl.dpy),False,SubstructureRedirectMask|SubstructureNotifyMask,&e);XFlush(hui__gl.dpy);hui__gl_maximized=!hui__gl_maximized;
}
void hui_gl_set_title(const char*title){if(hui__gl.ok&&title)XStoreName(hui__gl.dpy,hui__gl.win,title);}

#endif /* HUI_IMPLEMENTATION */
#endif /* HUI_OPENGL_H */
