/* hui_icons.h -- HUI's code-native icon language.
 *
 * Icons use a 24x24 design grid and only HUI draw primitives, so the same
 * source renders on X11, headless, Canvas/WASM, and SVG-capable draw-list
 * consumers. Header-only; include after hui.h. */
#ifndef HUI_ICONS_H
#define HUI_ICONS_H
#include "hui.h"

typedef enum hui_icon_id {
    HUI_ICON_PLAY, HUI_ICON_STOP, HUI_ICON_PAUSE, HUI_ICON_RECORD,
    HUI_ICON_LOOP, HUI_ICON_SKIP_BACK, HUI_ICON_SKIP_FORWARD,
    HUI_ICON_FOLDER, HUI_ICON_FILE, HUI_ICON_IMPORT, HUI_ICON_EXPORT,
    HUI_ICON_SETTINGS, HUI_ICON_CLOSE, HUI_ICON_MINIMIZE, HUI_ICON_MAXIMIZE,
    HUI_ICON_CHECK, HUI_ICON_INFO, HUI_ICON_WARNING, HUI_ICON_SEARCH,
    HUI_ICON_ARROW_LEFT, HUI_ICON_ARROW_RIGHT, HUI_ICON_ARROW_UP,
    HUI_ICON_ARROW_DOWN, HUI_ICON_CHEVRON_RIGHT, HUI_ICON_CHEVRON_DOWN,
    HUI_ICON_DRAG_HANDLE, HUI_ICON_WAVEFORM, HUI_ICON_MUTE,
    HUI_ICON_VOLUME_LOW, HUI_ICON_VOLUME_HIGH, HUI_ICON_LINK,
    HUI_ICON_LOCK, HUI_ICON_UNLOCK, HUI_ICON_GRID,
    HUI_ICON_COLLAPSE, HUI_ICON_EXPAND,
    /* editor tools (Barva + future creative apps) */
    HUI_ICON_POINTER, HUI_ICON_NODE_EDIT, HUI_ICON_PEN, HUI_ICON_PENCIL,
    HUI_ICON_BRUSH, HUI_ICON_STAMP, HUI_ICON_ERASER, HUI_ICON_TEXT,
    HUI_ICON_TEXT_CURSOR, HUI_ICON_RECT, HUI_ICON_ELLIPSE, HUI_ICON_POLYGON,
    HUI_ICON_RULER, HUI_ICON_GUIDES, HUI_ICON_GRADIENT, HUI_ICON_GLOW,
    HUI_ICON_EYEDROPPER, HUI_ICON_LASSO,
    HUI_ICON_COUNT
} hui_icon_id;

static inline const char *hui_icon_name(hui_icon_id id) {
    static const char *names[HUI_ICON_COUNT] = {
        "play","stop","pause","record","loop","skip-back","skip-forward",
        "folder","file","import","export","settings","close","minimize",
        "maximize","check","info","warning","search","arrow-left",
        "arrow-right","arrow-up","arrow-down","chevron-right","chevron-down",
        "drag-handle","waveform","mute","volume-low","volume-high","link",
        "lock","unlock","grid","collapse","expand",
        "pointer","node-edit","pen","pencil","brush","stamp","eraser",
        "text","text-cursor","rect","ellipse","polygon","ruler","guides",
        "gradient","glow","eyedropper","lasso"
    };
    return (unsigned)id < HUI_ICON_COUNT ? names[id] : "?";
}

static inline void hui_icon_draw(hui_icon_id id, hui_rect r, hui_color c) {
    int u = (r.w < r.h ? r.w : r.h);
    int ox = r.x + (r.w-u)/2, oy = r.y + (r.h-u)/2;
#define IX(v) (ox + ((v) * u + 12) / 24)
#define IY(v) (oy + ((v) * u + 12) / 24)
#define IP(x,y) ((hui_v2i){(int16_t)IX(x),(int16_t)IY(y)})
    uint8_t sw = (uint8_t)(u >= 20 ? 2 : 1);
    int rr = u >= 20 ? 2 : 1;
    switch (id) {
    case HUI_ICON_PLAY:
        hui_triangle_fill(IP(7,4),IP(19,12),IP(7,20),c); break;
    case HUI_ICON_STOP:
        hui_rect_fill(hui_rect_make(IX(6),IY(6),IX(18)-IX(6),IY(18)-IY(6)),c,1); break;
    case HUI_ICON_PAUSE:
        hui_rect_fill(hui_rect_make(IX(6),IY(5),IX(10)-IX(6),IY(19)-IY(5)),c,1);
        hui_rect_fill(hui_rect_make(IX(14),IY(5),IX(18)-IX(14),IY(19)-IY(5)),c,1); break;
    case HUI_ICON_RECORD: hui_circle_fill(IX(12),IY(12),IX(18)-IX(12),c); break;
    case HUI_ICON_LOOP:
        hui_line(IX(5),IY(9),IX(8),IY(6),c,sw); hui_line(IX(5),IY(9),IX(8),IY(12),c,sw);
        hui_line(IX(5),IY(9),IX(17),IY(9),c,sw); hui_line(IX(19),IY(15),IX(7),IY(15),c,sw);
        hui_line(IX(19),IY(15),IX(16),IY(12),c,sw); hui_line(IX(19),IY(15),IX(16),IY(18),c,sw); break;
    case HUI_ICON_SKIP_BACK: case HUI_ICON_SKIP_FORWARD: {
        int flip = id == HUI_ICON_SKIP_FORWARD;
        int bar = flip ? 19 : 5, tip = flip ? 18 : 6, back = flip ? 7 : 17;
        hui_line(IX(bar),IY(5),IX(bar),IY(19),c,sw);
        hui_triangle_fill(IP(tip,12),IP(back,5),IP(back,19),c); break; }
    case HUI_ICON_FOLDER:
        hui_line(IX(3),IY(8),IX(3),IY(19),c,sw); hui_line(IX(3),IY(19),IX(21),IY(19),c,sw);
        hui_line(IX(21),IY(19),IX(21),IY(8),c,sw); hui_line(IX(21),IY(8),IX(11),IY(8),c,sw);
        hui_line(IX(11),IY(8),IX(9),IY(5),c,sw); hui_line(IX(9),IY(5),IX(3),IY(5),c,sw); break;
    case HUI_ICON_FILE:
        hui_line(IX(6),IY(3),IX(15),IY(3),c,sw); hui_line(IX(15),IY(3),IX(20),IY(8),c,sw);
        hui_line(IX(20),IY(8),IX(20),IY(21),c,sw); hui_line(IX(20),IY(21),IX(6),IY(21),c,sw);
        hui_line(IX(6),IY(21),IX(6),IY(3),c,sw); hui_line(IX(15),IY(3),IX(15),IY(8),c,sw);
        hui_line(IX(15),IY(8),IX(20),IY(8),c,sw); break;
    case HUI_ICON_IMPORT: case HUI_ICON_EXPORT: {
        int down = id == HUI_ICON_IMPORT, ya = down ? 5 : 15, yb = down ? 15 : 5;
        hui_line(IX(12),IY(ya),IX(12),IY(yb),c,sw);
        hui_line(IX(12),IY(yb),IX(8),IY(down?11:9),c,sw); hui_line(IX(12),IY(yb),IX(16),IY(down?11:9),c,sw);
        hui_line(IX(5),IY(16),IX(5),IY(20),c,sw); hui_line(IX(5),IY(20),IX(19),IY(20),c,sw); hui_line(IX(19),IY(20),IX(19),IY(16),c,sw); break; }
    case HUI_ICON_SETTINGS:
        hui_circle(IX(12),IY(12),IX(16)-IX(12),c); hui_circle_fill(IX(12),IY(12),rr,c);
        for (int i=0;i<8;i++){ static const int dx[8]={0,5,7,5,0,-5,-7,-5}; static const int dy[8]={-7,-5,0,5,7,5,0,-5}; hui_line(IX(12+dx[i]),IY(12+dy[i]),IX(12+dx[i]*4/5),IY(12+dy[i]*4/5),c,sw); } break;
    case HUI_ICON_CLOSE:
        hui_line(IX(6),IY(6),IX(18),IY(18),c,sw); hui_line(IX(6),IY(18),IX(18),IY(6),c,sw); break;
    case HUI_ICON_MINIMIZE: hui_line(IX(6),IY(17),IX(18),IY(17),c,sw); break;
    case HUI_ICON_MAXIMIZE:
        hui_rect_outline(hui_rect_make(IX(5),IY(5),IX(19)-IX(5),IY(19)-IY(5)),c,0); break;
    case HUI_ICON_CHECK:
        hui_line(IX(4),IY(12),IX(9),IY(17),c,sw); hui_line(IX(9),IY(17),IX(20),IY(6),c,sw); break;
    case HUI_ICON_INFO:
        hui_circle(IX(12),IY(12),IX(20)-IX(12),c); hui_circle_fill(IX(12),IY(8),rr,c); hui_line(IX(12),IY(11),IX(12),IY(17),c,sw); break;
    case HUI_ICON_WARNING:
        hui_triangle(IP(12,3),IP(21,20),IP(3,20),c); hui_line(IX(12),IY(8),IX(12),IY(14),c,sw); hui_circle_fill(IX(12),IY(17),rr,c); break;
    case HUI_ICON_SEARCH:
        hui_circle(IX(10),IY(10),IX(16)-IX(10),c); hui_line(IX(14),IY(14),IX(20),IY(20),c,sw); break;
    case HUI_ICON_ARROW_LEFT: case HUI_ICON_ARROW_RIGHT: case HUI_ICON_ARROW_UP: case HUI_ICON_ARROW_DOWN:
        if(id==HUI_ICON_ARROW_LEFT) hui_triangle_fill(IP(5,12),IP(17,5),IP(17,19),c);
        if(id==HUI_ICON_ARROW_RIGHT) hui_triangle_fill(IP(19,12),IP(7,5),IP(7,19),c);
        if(id==HUI_ICON_ARROW_UP) hui_triangle_fill(IP(12,5),IP(5,17),IP(19,17),c);
        if(id==HUI_ICON_ARROW_DOWN) hui_triangle_fill(IP(12,19),IP(5,7),IP(19,7),c);
        break;
    case HUI_ICON_CHEVRON_RIGHT:
        hui_line(IX(8),IY(5),IX(16),IY(12),c,sw); hui_line(IX(16),IY(12),IX(8),IY(19),c,sw); break;
    case HUI_ICON_CHEVRON_DOWN:
        hui_line(IX(5),IY(8),IX(12),IY(16),c,sw); hui_line(IX(12),IY(16),IX(19),IY(8),c,sw); break;
    case HUI_ICON_DRAG_HANDLE:
        for(int y=8;y<=16;y+=4) {
            for(int x=10;x<=14;x+=4) hui_circle_fill(IX(x),IY(y),rr,c);
        }
        break;
    case HUI_ICON_WAVEFORM:
        hui_line(IX(3),IY(12),IX(6),IY(12),c,sw); hui_line(IX(6),IY(12),IX(8),IY(6),c,sw); hui_line(IX(8),IY(6),IX(11),IY(18),c,sw); hui_line(IX(11),IY(18),IX(14),IY(8),c,sw); hui_line(IX(14),IY(8),IX(17),IY(14),c,sw); hui_line(IX(17),IY(14),IX(21),IY(14),c,sw); break;
    case HUI_ICON_MUTE: case HUI_ICON_VOLUME_LOW: case HUI_ICON_VOLUME_HIGH:
        hui_rect_fill(hui_rect_make(IX(3),IY(10),IX(8)-IX(3),IY(15)-IY(10)),c,0);
        hui_triangle_fill(IP(8,10),IP(14,5),IP(14,20),c);
        if(id==HUI_ICON_MUTE){ hui_line(IX(17),IY(9),IX(22),IY(16),c,sw); hui_line(IX(17),IY(16),IX(22),IY(9),c,sw); }
        else { hui_line(IX(17),IY(9),IX(19),IY(12),c,sw); hui_line(IX(19),IY(12),IX(17),IY(15),c,sw); if(id==HUI_ICON_VOLUME_HIGH){ hui_line(IX(20),IY(6),IX(23),IY(12),c,sw); hui_line(IX(23),IY(12),IX(20),IY(18),c,sw); }} break;
    case HUI_ICON_LINK:
        hui_circle(IX(9),IY(12),IX(14)-IX(9),c); hui_circle(IX(15),IY(12),IX(20)-IX(15),c); break;
    case HUI_ICON_LOCK: case HUI_ICON_UNLOCK:
        hui_rect_outline(hui_rect_make(IX(5),IY(10),IX(19)-IX(5),IY(21)-IY(10)),c,1);
        hui_line(IX(id==HUI_ICON_LOCK?8:11),IY(10),IX(id==HUI_ICON_LOCK?8:11),IY(7),c,sw); hui_line(IX(id==HUI_ICON_LOCK?8:11),IY(7),IX(12),IY(4),c,sw); hui_line(IX(12),IY(4),IX(16),IY(7),c,sw); if(id==HUI_ICON_LOCK) hui_line(IX(16),IY(7),IX(16),IY(10),c,sw); break;
    case HUI_ICON_GRID:
        for(int y=5;y<=15;y+=5) {
            for(int x=5;x<=15;x+=5)
                hui_rect_fill(hui_rect_make(IX(x),IY(y),IX(x+3)-IX(x),IY(y+3)-IY(y)),c,0);
        }
        break;
    case HUI_ICON_COLLAPSE:
        hui_line(IX(4),IY(4),IX(10),IY(10),c,sw); hui_line(IX(4),IY(10),IX(10),IY(10),c,sw); hui_line(IX(10),IY(4),IX(10),IY(10),c,sw);
        hui_line(IX(20),IY(20),IX(14),IY(14),c,sw); hui_line(IX(20),IY(14),IX(14),IY(14),c,sw); hui_line(IX(14),IY(20),IX(14),IY(14),c,sw); break;
    case HUI_ICON_EXPAND:
        hui_line(IX(10),IY(10),IX(4),IY(4),c,sw); hui_line(IX(4),IY(4),IX(10),IY(4),c,sw); hui_line(IX(4),IY(4),IX(4),IY(10),c,sw);
        hui_line(IX(14),IY(14),IX(20),IY(20),c,sw); hui_line(IX(20),IY(20),IX(14),IY(20),c,sw); hui_line(IX(20),IY(20),IX(20),IY(14),c,sw); break;

    /* ---- editor tools ---- */
    case HUI_ICON_POINTER:
        /* classic arrow: filled head + angled tail */
        hui_triangle_fill(IP(7,3),IP(7,18),IP(11,14),c);
        hui_triangle_fill(IP(7,3),IP(11,14),IP(16,13),c);
        hui_triangle_fill(IP(11,14),IP(14,13),IP(16,19),c);
        hui_triangle_fill(IP(11,14),IP(16,19),IP(13,20),c); break;
    case HUI_ICON_NODE_EDIT:
        /* bezier arc, two square anchors, one lifted handle */
        hui_line(IX(4),IY(17),IX(8),IY(9),c,sw);  hui_line(IX(8),IY(9),IX(15),IY(8),c,sw);
        hui_line(IX(15),IY(8),IX(20),IY(14),c,sw);
        hui_line(IX(15),IY(8),IX(20),IY(3),c,1);
        hui_rect_fill(hui_rect_make(IX(2),IY(15),IX(6)-IX(2)+1,IY(19)-IY(15)+1),c,0);
        hui_rect_fill(hui_rect_make(IX(18),IY(12),IX(22)-IX(18)+1,IY(16)-IY(12)+1),c,0);
        hui_circle(IX(20),IY(3),rr+1,c); break;
    case HUI_ICON_PEN:
        /* fountain nib, outline style with slit + breather hole */
        hui_line(IX(8),IY(3),IX(16),IY(3),c,sw);
        hui_line(IX(16),IY(3),IX(17),IY(12),c,sw); hui_line(IX(8),IY(3),IX(7),IY(12),c,sw);
        hui_line(IX(17),IY(12),IX(12),IY(21),c,sw); hui_line(IX(7),IY(12),IX(12),IY(21),c,sw);
        hui_line(IX(12),IY(7),IX(12),IY(13),c,1);
        hui_circle_fill(IX(12),IY(15),rr,c); break;
    case HUI_ICON_PENCIL:
        /* diagonal shaft, cap band, filled tip */
        hui_line(IX(15),IY(4),IX(20),IY(9),c,sw);   /* cap end */
        hui_line(IX(13),IY(6),IX(6),IY(13),c,sw);  hui_line(IX(18),IY(11),IX(11),IY(18),c,sw);
        hui_line(IX(13),IY(6),IX(18),IY(11),c,1);  /* band */
        hui_triangle_fill(IP(6,13),IP(11,18),IP(4,20),c); break;
    case HUI_ICON_BRUSH:
        /* handle + ferrule + bristle drop */
        hui_line(IX(20),IY(3),IX(13),IY(10),c,sw);
        hui_line(IX(12),IY(9),IX(15),IY(12),c,sw+1);
        hui_triangle_fill(IP(12,10),IP(14,12),IP(6,20),c);
        hui_circle_fill(IX(7),IY(18),rr+1,c); break;
    case HUI_ICON_STAMP:
        /* knob, neck, mount, base plate */
        hui_circle_fill(IX(12),IY(6),IX(15)-IX(12),c);
        hui_rect_fill(hui_rect_make(IX(10),IY(8),IX(14)-IX(10),IY(12)-IY(8)),c,0);
        hui_triangle_fill(IP(10,12),IP(14,12),IP(17,16),c);
        hui_triangle_fill(IP(10,12),IP(17,16),IP(7,16),c);
        hui_rect_fill(hui_rect_make(IX(5),IY(18),IX(19)-IX(5),IY(21)-IY(18)),c,1); break;
    case HUI_ICON_ERASER:
        /* tilted block with wear band over a baseline */
        hui_triangle_fill(IP(6,13),IP(13,6),IP(19,12),c);
        hui_triangle_fill(IP(6,13),IP(19,12),IP(12,19),c);
        hui_line(IX(4),IY(21),IX(20),IY(21),c,sw); break;
    case HUI_ICON_TEXT:
        /* block capital T */
        hui_rect_fill(hui_rect_make(IX(5),IY(4),IX(19)-IX(5),IY(8)-IY(4)),c,0);
        hui_rect_fill(hui_rect_make(IX(10),IY(8),IX(14)-IX(10),IY(20)-IY(8)),c,0); break;
    case HUI_ICON_TEXT_CURSOR:
        hui_line(IX(12),IY(4),IX(12),IY(20),c,sw);
        hui_line(IX(9),IY(4),IX(15),IY(4),c,sw); hui_line(IX(9),IY(20),IX(15),IY(20),c,sw); break;
    case HUI_ICON_RECT:
        hui_rect_outline(hui_rect_make(IX(4),IY(6),IX(20)-IX(4),IY(18)-IY(6)),c,(uint8_t)rr); break;
    case HUI_ICON_ELLIPSE:
        hui_circle(IX(12),IY(12),IX(19)-IX(12),c); break;
    case HUI_ICON_POLYGON:
        hui_line(IX(12),IY(3),IX(21),IY(10),c,sw); hui_line(IX(21),IY(10),IX(17),IY(20),c,sw);
        hui_line(IX(17),IY(20),IX(7),IY(20),c,sw); hui_line(IX(7),IY(20),IX(3),IY(10),c,sw);
        hui_line(IX(3),IY(10),IX(12),IY(3),c,sw); break;
    case HUI_ICON_RULER:
        /* diagonal ruler with ticks */
        hui_line(IX(3),IY(16),IX(16),IY(3),c,sw);  hui_line(IX(16),IY(3),IX(21),IY(8),c,sw);
        hui_line(IX(21),IY(8),IX(8),IY(21),c,sw);  hui_line(IX(8),IY(21),IX(3),IY(16),c,sw);
        hui_line(IX(7),IY(12),IX(9),IY(14),c,1);   hui_line(IX(10),IY(9),IX(12),IY(11),c,1);
        hui_line(IX(13),IY(6),IX(15),IY(8),c,1); break;
    case HUI_ICON_GUIDES:
        /* dashed crosshair with a center ring */
        hui_line(IX(2),IY(12),IX(7),IY(12),c,1);  hui_line(IX(17),IY(12),IX(22),IY(12),c,1);
        hui_line(IX(12),IY(2),IX(12),IY(7),c,1);  hui_line(IX(12),IY(17),IX(12),IY(22),c,1);
        hui_circle(IX(12),IY(12),IX(15)-IX(12),c); break;
    case HUI_ICON_GRADIENT:
        /* frame; solid left fading to dots on the right */
        hui_rect_outline(hui_rect_make(IX(3),IY(6),IX(21)-IX(3),IY(18)-IY(6)),c,(uint8_t)rr);
        hui_rect_fill(hui_rect_make(IX(5),IY(8),IX(10)-IX(5),IY(16)-IY(8)),c,0);
        hui_rect_fill(hui_rect_make(IX(11),IY(9),IX(13)-IX(11),IY(15)-IY(9)),c,0);
        hui_circle_fill(IX(15),IY(10),1,c); hui_circle_fill(IX(15),IY(14),1,c);
        hui_circle_fill(IX(18),IY(12),1,c); break;
    case HUI_ICON_GLOW:
        /* radiant core */
        hui_circle_fill(IX(12),IY(12),IX(15)-IX(12),c);
        for (int i=0;i<8;i++){
            static const int gx[8]={0,5,7,5,0,-5,-7,-5};
            static const int gy[8]={-7,-5,0,5,7,5,0,-5};
            hui_line(IX(12+gx[i]*5/7),IY(12+gy[i]*5/7),IX(12+gx[i]),IY(12+gy[i]),c,1);
        } break;
    case HUI_ICON_EYEDROPPER:
        /* bulb, shaft, drop tip */
        hui_circle_fill(IX(17),IY(7),IX(20)-IX(17),c);
        hui_line(IX(15),IY(9),IX(7),IY(17),c,sw+1);
        hui_triangle_fill(IP(7,17),IP(9,19),IP(4,20),c); break;
    case HUI_ICON_LASSO:
        /* dashed loop with a short tail from the bottom */
        hui_line(IX(6),IY(6),IX(12),IY(4),c,sw);  hui_line(IX(12),IY(4),IX(18),IY(7),c,sw);
        hui_line(IX(18),IY(7),IX(19),IY(12),c,sw); hui_line(IX(19),IY(12),IX(14),IY(16),c,sw);
        hui_line(IX(14),IY(16),IX(8),IY(15),c,sw); hui_line(IX(8),IY(15),IX(6),IY(10),c,sw);
        hui_line(IX(6),IY(10),IX(6),IY(6),c,sw);
        hui_line(IX(10),IY(16),IX(9),IY(21),c,sw);   /* tail */
        hui_circle_fill(IX(9),IY(21),rr,c); break;
    default: break;
    }
#undef IP
#undef IY
#undef IX
}

#endif /* HUI_ICONS_H */
