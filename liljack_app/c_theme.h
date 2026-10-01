#ifndef LILJACK_C_THEME_H
#define LILJACK_C_THEME_H
#include <stddef.h>
#include <stdint.h>
/* Single UI thread; getters return stable table storage until the next load. */
typedef enum {
    LJ_THEME_BG,
    LJ_THEME_PANEL,
    LJ_THEME_EDGE,
    LJ_THEME_SURF2,
    LJ_THEME_SURF3,
    LJ_THEME_GREEN,
    LJ_THEME_TEXT,
    LJ_THEME_DIM,
    LJ_THEME_CYAN,
    LJ_THEME_AMBER,
    LJ_THEME_RED,
    LJ_THEME_BUTTON_ACTIVE,
    LJ_THEME_BUTTON_IDLE,
    LJ_THEME_ENDED,
    LJ_THEME_UNAVAILABLE_BG,
    LJ_THEME_DISABLED,
    LJ_THEME_TASK_ACTIVE,
    LJ_THEME_TASK_META,
    LJ_THEME_DEPENDENCY,
    LJ_THEME_CANVAS_FG,
    LJ_THEME_CANVAS_BG,
    LJ_THEME_NOTICE_BG,
    LJ_THEME_NOTICE_FG,
    LJ_THEME_SCROLLBAR,
    LJ_THEME_GRAPH_LOW,
    LJ_THEME_GRAPH_MID,
    LJ_THEME_GRAPH_HIGH,
    LJ_THEME_ANSI_0,
    LJ_THEME_ANSI_1,
    LJ_THEME_ANSI_2,
    LJ_THEME_ANSI_3,
    LJ_THEME_ANSI_4,
    LJ_THEME_ANSI_5,
    LJ_THEME_ANSI_6,
    LJ_THEME_ANSI_7,
    LJ_THEME_ANSI_8,
    LJ_THEME_ANSI_9,
    LJ_THEME_ANSI_10,
    LJ_THEME_ANSI_11,
    LJ_THEME_ANSI_12,
    LJ_THEME_ANSI_13,
    LJ_THEME_ANSI_14,
    LJ_THEME_ANSI_15,
    LJ_THEME_ICON_CLAUDE,
    LJ_THEME_ICON_CODEX,
    LJ_THEME_ICON_DEEPSEEK,
    LJ_THEME_ICON_SHELL,
    LJ_THEME_ICON_USER,
    LJ_THEME_ROOM,
    LJ_THEME_STANDALONE,
    LJ_THEME_CHECK,
    LJ_THEME_ACTIVE,
    LJ_THEME_BLOCKED,
    LJ_THEME_EMPTY,
    LJ_THEME_CLOSE,
    LJ_THEME_RESIZE,
    LJ_THEME_DISCONNECTED,
    LJ_THEME_UP,
    LJ_THEME_DOWN,
    LJ_THEME_ELLIPSIS,
    LJ_THEME_RING_DOT,
    LJ_THEME_RULE_H,
    LJ_THEME_RULE_V,
    LJ_THEME_SCROLL_THUMB,
    LJ_THEME_SCROLL_TRACK,
    LJ_THEME_PROMPT,
    LJ_THEME_PROMPT_CURSOR,
    LJ_THEME_RETURN_MARK,
    LJ_THEME_RULE_FILLED,
    LJ_THEME_LAB_BG,
    LJ_THEME_LAB_PANEL,
    LJ_THEME_LAB_EDGE,
    LJ_THEME_LAB_SURF2,
    LJ_THEME_LAB_SURF3,
    LJ_THEME_LAB_TEXT,
    LJ_THEME_LAB_DIM,
    LJ_THEME_LAB_BLUE,
    LJ_THEME_LAB_GOLD,
    LJ_THEME_LAB_RED,
    LJ_THEME_LAB_GREEN,
    LJ_THEME_LAB_GRAPH_BG,
    LJ_THEME_LAB_HAIRLINE,
    LJ_THEME_LAB_WHITE,
    LJ_THEME_WARNING,
    LJ_THEME_LOCK,
    LJ_THEME_CHAT,
    LJ_THEME_UP_ARROW,
    LJ_THEME_DIRECTORY,
    LJ_THEME_SYMLINK,
    LJ_THEME_TREE_LAST,
    LJ_THEME_TREE_MID,
    LJ_THEME_DIVIDER,
    LJ_THEME_DIVIDER_HOT,
    LJ_THEME_COUNT
} lj_theme_id;
typedef struct { const char *name; uint32_t rgb; unsigned ansi256,ansi16; char glyph[32]; } lj_theme_token;
const lj_theme_token *lj_theme_get(lj_theme_id id);
int lj_theme_find(const char *name);
uint32_t lj_theme_rgb(lj_theme_id id);
/* Applied 24-bit colour, remaps ANSI palette entries; invalid id returns 0. */
uint32_t lj_theme_set_rgb(lj_theme_id id,uint32_t rgb);
const char *lj_theme_glyph(lj_theme_id id);
uint32_t lj_theme_codepoint(lj_theme_id id);
void lj_theme_reset(void);
/* 1 loaded; 0 absent; -1 invalid/I/O error. Errors never modify the table. */
int lj_theme_load(const char *path,char *error,size_t capacity);
int lj_theme_load_default(char *error,size_t capacity);
/* Writes the compiled defaults, not the user's current overrides. */
int lj_theme_write_defaults(const char *path);
typedef enum { LJ_THEME_GRAPH_BUCKET_MS, LJ_THEME_GRAPH_REPAINT_MS,
               LJ_THEME_METRICS_POLL_MS, LJ_THEME_METRICS_LABEL_HOLD_MS,
               LJ_THEME_DIVIDER_IDLE_PX, LJ_THEME_DIVIDER_HOT_PX,
               LJ_THEME_TAB_BADGE_MIN_CELLS, LJ_THEME_EFFECTS_SCANLINES,
               LJ_THEME_SETTING_COUNT } lj_theme_setting_id;
typedef struct {
    const char *name, *unit;
    int value, min, max, step, default_value;
} lj_theme_setting;
const lj_theme_setting *lj_theme_setting_get(lj_theme_setting_id id);
int lj_theme_int(lj_theme_setting_id id);
/* Returns applied value; invalid ID returns -1. Repaint never falls below bucket. */
int lj_theme_set_int(lj_theme_setting_id id,int value);
/* Atomic current-theme save to the same path load_default selects; 1/-1. */
int lj_theme_save_user(char *error,size_t capacity);
#endif
