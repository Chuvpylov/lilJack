/*
 * backends/hui_sdl2.h — SDL2 windowed backend for hui
 *
 * Uses the headless software rasterizer (virtual canvas),
 * then presents via SDL_Renderer + SDL_Texture with integer-step scaling.
 * Mouse coordinates are transformed to virtual canvas space.
 *
 * Select with: #define HUI_BACKEND_SDL2 before #include "hui.h"
 * Links: $(pkg-config --cflags --libs sdl2) -lm
 */

#ifndef HUI_SDL2_H
#define HUI_SDL2_H

#ifndef HUI_HEADLESS_H
#  define HUI_HEADLESS_NO_FLUSH
#  include "hui_headless.h"
#endif

#include <SDL2/SDL.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

static SDL_Window   *hui__sdl_win = NULL;
static SDL_Renderer *hui__sdl_ren = NULL;
static SDL_Texture  *hui__sdl_tex = NULL;
static char          hui__sdl_capture_path[512] = {0};
static int hui_sdl_save_ppm(const char *path);
static int hui_sdl_save_png(const char *path);

/* Public input state — virtual (canvas) coords */
int  hui_sdl_mouse_x   = 0;
int  hui_sdl_mouse_y   = 0;
int  hui_sdl_mouse_btn = 0;
bool hui_sdl_key[256]  = {0};
int  hui_sdl_scroll_dy = 0;                /* scroll delta this frame (+up/-down) */
char hui_sdl_text[32]  = {0};             /* printable chars typed this frame */
int  hui_sdl_text_len  = 0;

/* Translate SDL_Keycode → hui key index (HUI_KEY_* or printable ASCII) */
static int hui__sdl_key_to_hui(SDL_Keycode k) {
    switch (k) {
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB:       return 0x09;
    case SDLK_RETURN:    return 0x0D;
    case SDLK_ESCAPE:    return 0x1B;
    case SDLK_DELETE:    return 0x7F;
    case SDLK_LEFT:      return 0x80;
    case SDLK_RIGHT:     return 0x81;
    case SDLK_UP:        return 0x82;
    case SDLK_DOWN:      return 0x83;
    case SDLK_HOME:      return 0x84;
    case SDLK_END:       return 0x85;
    case SDLK_PAGEUP:    return 0x86;
    case SDLK_PAGEDOWN:  return 0x87;
    default:
        if (k >= 32 && k < 128) return (int)k;
        return -1;
    }
}

static int hui_sdl_open_scaled(int vw, int vh, int ww, int wh, const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[hui_sdl] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    hui__sdl_win = SDL_CreateWindow(
        title ? title : "hui",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ww, wh,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!hui__sdl_win) {
        fprintf(stderr, "[hui_sdl] CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    hui__sdl_ren = SDL_CreateRenderer(
        hui__sdl_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!hui__sdl_ren)
        hui__sdl_ren = SDL_CreateRenderer(hui__sdl_win, -1, 0);
    if (!hui__sdl_ren) {
        fprintf(stderr, "[hui_sdl] CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    /* Integer-step nearest-neighbor scaling with letterbox */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetIntegerScale(hui__sdl_ren, SDL_TRUE);
    SDL_RenderSetLogicalSize(hui__sdl_ren, vw, vh);

    hui__sdl_tex = SDL_CreateTexture(
        hui__sdl_ren,
        SDL_PIXELFORMAT_ARGB8888,   /* matches hui__fb: 0xFFRRGGBB */
        SDL_TEXTUREACCESS_STREAMING,
        vw, vh);
    if (!hui__sdl_tex) {
        fprintf(stderr, "[hui_sdl] CreateTexture: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

static int hui_sdl_open(int vw, int vh, const char *title) {
    return hui_sdl_open_scaled(vw, vh, vw, vh, title);
}

/* Returns false when the window should close.
 * Call once per frame BEFORE hui_begin_frame(). Resets scroll_dy and text_typed. */
static bool hui_sdl_poll(void) {
    /* Reset per-frame accumulators */
    hui_sdl_scroll_dy = 0;
    hui_sdl_text_len  = 0;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN: {
            if (e.key.keysym.sym == SDLK_ESCAPE) return false;
            int hk = hui__sdl_key_to_hui(e.key.keysym.sym);
            if (hk >= 0 && hk < 256) hui_sdl_key[hk] = true;
            break;
        }
        case SDL_KEYUP: {
            int hk = hui__sdl_key_to_hui(e.key.keysym.sym);
            if (hk >= 0 && hk < 256) hui_sdl_key[hk] = false;
            break;
        }
        case SDL_TEXTINPUT: {
            /* UTF-8 printable chars — copy up to remaining space */
            int rem = (int)sizeof(hui_sdl_text) - 1 - hui_sdl_text_len;
            if (rem > 0) {
                int n = (int)SDL_strlen(e.text.text);
                if (n > rem) n = rem;
                SDL_memcpy(hui_sdl_text + hui_sdl_text_len, e.text.text, (size_t)n);
                hui_sdl_text_len += n;
                hui_sdl_text[hui_sdl_text_len] = '\0';
            }
            break;
        }
        case SDL_MOUSEWHEEL:
            /* positive y = scroll up; SDL convention matches hui (+up/-down) */
            hui_sdl_scroll_dy += e.wheel.y;
            break;
        case SDL_MOUSEMOTION: {
            /* SDL_RenderSetLogicalSize does NOT transform event coords.
               Use SDL_RenderWindowToLogical (SDL >= 2.0.18) instead. */
            float lx, ly;
            SDL_RenderWindowToLogical(hui__sdl_ren,
                                      e.motion.x, e.motion.y, &lx, &ly);
            hui_sdl_mouse_x = (int)lx;
            hui_sdl_mouse_y = (int)ly;
            break;
        }
        case SDL_MOUSEBUTTONDOWN: {
            float lx, ly;
            SDL_RenderWindowToLogical(hui__sdl_ren,
                                      e.button.x, e.button.y, &lx, &ly);
            hui_sdl_mouse_x = (int)lx;
            hui_sdl_mouse_y = (int)ly;
            hui_sdl_mouse_btn |= (1 << (e.button.button - 1));
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            float lx, ly;
            SDL_RenderWindowToLogical(hui__sdl_ren,
                                      e.button.x, e.button.y, &lx, &ly);
            hui_sdl_mouse_x = (int)lx;
            hui_sdl_mouse_y = (int)ly;
            hui_sdl_mouse_btn &= ~(1 << (e.button.button - 1));
            break;
        }
        default: break;
        }
    }
    return true;
}

/* Called by hui_begin_frame() (after snapshotting prev IO) to copy SDL state
 * into hui_g->io. This ensures widgets see same-frame input with zero lag. */
static void hui_sdl_pump_io(void) {
    if (!hui_g) return;
    hui_g->io.mouse_x   = (int16_t)hui_sdl_mouse_x;
    hui_g->io.mouse_y   = (int16_t)hui_sdl_mouse_y;
    hui_g->io.mouse_btn = (uint8_t)hui_sdl_mouse_btn;
    hui_g->io.scroll_dy = (int16_t)hui_sdl_scroll_dy;
    for (int i = 0; i < 256; i++) hui_g->io.keys[i] = hui_sdl_key[i];
    int tlen = hui_sdl_text_len;
    if (tlen > (int)sizeof(hui_g->io.text_typed) - 1)
        tlen = (int)sizeof(hui_g->io.text_typed) - 1;
    for (int i = 0; i < tlen; i++) hui_g->io.text_typed[i] = hui_sdl_text[i];
    hui_g->io.text_typed[tlen] = '\0';
    hui_g->io.text_typed_len   = tlen;
}

static void hui_sdl_request_capture(const char *path) {
    if (!path || !path[0]) {
        hui__sdl_capture_path[0] = '\0';
        return;
    }
    snprintf(hui__sdl_capture_path, sizeof(hui__sdl_capture_path), "%s", path);
}

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    hui__rasterize(cmds, count, strpool, datapool);

    SDL_UpdateTexture(hui__sdl_tex, NULL,
                      hui__fb.pixels, hui__fb.w * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(hui__sdl_ren, 0, 0, 0, 255);
    SDL_RenderClear(hui__sdl_ren);
    SDL_RenderCopy(hui__sdl_ren, hui__sdl_tex, NULL, NULL);
    if (hui__sdl_capture_path[0]) {
        const char *dot = strrchr(hui__sdl_capture_path, '.');
        if (dot && strcmp(dot, ".png") == 0) hui_sdl_save_png(hui__sdl_capture_path);
        else hui_sdl_save_ppm(hui__sdl_capture_path);
        hui__sdl_capture_path[0] = '\0';
    }
    SDL_RenderPresent(hui__sdl_ren);

    memset(hui__fb.pixels, 0,
           (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));
}

static void hui_sdl_close(void) {
    if (hui__sdl_tex) { SDL_DestroyTexture(hui__sdl_tex);  hui__sdl_tex = NULL; }
    if (hui__sdl_ren) { SDL_DestroyRenderer(hui__sdl_ren); hui__sdl_ren = NULL; }
    if (hui__sdl_win) { SDL_DestroyWindow(hui__sdl_win);   hui__sdl_win = NULL; }
    SDL_Quit();
}

static int hui_sdl_save_ppm(const char *path) {
    if (!hui__sdl_win || !path || !hui__fb.pixels || hui__fb.w <= 0 || hui__fb.h <= 0) return -1;
    int w = 0, h = 0;
    SDL_GetWindowSize(hui__sdl_win, &w, &h);
    if (w <= 0 || h <= 0) return -1;
    uint32_t *pixels = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!pixels) return -1;
    hui__scale_argb_letterbox(pixels, w, h,
                              hui__fb.pixels, hui__fb.w, hui__fb.h,
                              0, NULL, NULL, NULL);
    hui__save_argb_ppm(path, pixels, w, h);
    free(pixels);
    return 0;
}

static int hui_sdl_save_png(const char *path) {
    if (!hui__sdl_win || !path || !hui__fb.pixels || hui__fb.w <= 0 || hui__fb.h <= 0) return -1;
    int w = 0, h = 0;
    SDL_GetWindowSize(hui__sdl_win, &w, &h);
    if (w <= 0 || h <= 0) return -1;
    uint32_t *pixels = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!pixels) return -1;
    hui__scale_argb_letterbox(pixels, w, h,
                              hui__fb.pixels, hui__fb.w, hui__fb.h,
                              0, NULL, NULL, NULL);
    int rc = hui__save_argb_png(path, pixels, w, h);
    free(pixels);
    return rc;
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_SDL2_H */
