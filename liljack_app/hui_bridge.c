/* HUI's software renderer, presented by SDL (no GL/CUDA renderer). */
#define HUI_IMPLEMENTATION
#define HUI_BACKEND_HEADLESS
#include "hui.h"

static int width, height;
void lj_hui_begin(int w, int h) {
    if (w != width || h != height) {
        if (width) hui_shutdown();
        hui_init(w,h); width=w; height=h;
    }
    hui_begin_frame();
}
void lj_hui_rect(int x,int y,int w,int h,unsigned color,int radius) {
    hui_color c={(color>>16)&255,(color>>8)&255,color&255,255};
    hui_rect_fill(hui_rect_make(x,y,w,h),c,radius);
}
void *lj_hui_end(void) {
    hui_end_frame();
    return hui__fb.pixels;
}
