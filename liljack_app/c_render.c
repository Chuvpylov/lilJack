/* c_render.c — see c_render.h. Owner: claude, delegated by codex 2026-09-08. */
#define _XOPEN_SOURCE 700
#define HUI_IMPLEMENTATION
#define HUI_BACKEND_HEADLESS
#include "hui.h"

#include "c_render.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Known-installed Noto paths. Deliberately hardcoded: codex asked for no shell
 * font subprocess in the module (fc-match belongs in tests only). A path that
 * is absent is skipped, not fatal. */
/* Font slots, as CANDIDATE LISTS: the first path that opens wins, so a machine
 * missing one face degrades to the next rather than to nothing. Windows and
 * Linux differ in every slot, which is why this is a table and not a string.
 * ⚠ No fontconfig / no shell-out at runtime (codex's constraint), so these are
 * the well-known system paths for each platform. */
#define MONO_FACE  0
#define CJK_FACE   1
#define EMOJI_FACE 2
#define SYM_FACE   3   /* symbols the mono face lacks: ✻ ✽ ✢, arrows (Claude Code's spinner) */
#define SYM2_FACE  4   /* braille and the rest: DejaVu Sans (no mono face on this box has braille) */
#define SYM3_FACE  5   /* media controls ⏵⏸⏹, geometric shapes: Noto Sans Symbols 2 / Adwaita Mono */
#define NERD_FACE  6   /* Nerd Fonts private-use icons (U+E000-F8FF, U+F0000+): starship/lsd/editor prompts */
#define NFACE      7
#define MAX_CAND   4

/* "$HOME/.local/share/fonts/SymbolsNerdFontMono-Regular.ttf", resolved once at
 * open time (no fontconfig, but a user-installed font must still count). */
static char nerd_user_path[512];
#define NERD_USER_PATH nerd_user_path
static const char *FACE_CAND[NFACE][MAX_CAND] = {
#ifdef _WIN32
    { "C:\\Windows\\Fonts\\consola.ttf",       /* Consolas: latin + cyrillic  */
      "C:\\Windows\\Fonts\\lucon.ttf",
      "C:\\Windows\\Fonts\\cour.ttf", NULL },
    { "C:\\Windows\\Fonts\\YuGothM.ttc",       /* Yu Gothic Medium: ja        */
      "C:\\Windows\\Fonts\\msgothic.ttc",      /* MS Gothic                   */
      "C:\\Windows\\Fonts\\meiryo.ttc", NULL },
    { "C:\\Windows\\Fonts\\seguiemj.ttf", NULL, NULL, NULL },  /* Segoe UI Emoji */
    { "C:\\Windows\\Fonts\\seguisym.ttf", "C:\\Windows\\Fonts\\DejaVuSansMono.ttf", NULL, NULL },  /* Segoe UI Symbol */
    { "C:\\Windows\\Fonts\\segoeui.ttf", NULL, NULL, NULL },
    { "C:\\Windows\\Fonts\\seguisym.ttf", NULL, NULL, NULL },
    { "C:\\Windows\\Fonts\\SymbolsNerdFontMono-Regular.ttf", NULL, NULL, NULL },
#else
    { "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", NULL },
    { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", NULL, NULL },
    { "/usr/share/fonts/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", NULL, NULL },
    /* ⚠ SYMBOL FALLBACK (the operator 2026-09-12: "when claude thinks some characters
     * are just squares"). JetBrains Mono (owkTerm's default) has no ✻ ✽ ✢ and
     * Noto CJK only some; DejaVu Sans Mono covers them plus braille/arrows. */
    { "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf", NULL },
    { "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL, NULL },
    { "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf", "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
      "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf" },
    /* ⚠ NERD ICONS (the operator 2026-09-15: "rectangle pictograms"). The PUA icon
     * ranges have no glyph in any stock face; the ▪ substitute below stays as
     * the last resort when this face is absent too. User dir first: the
     * package (ttf-nerd-fonts-symbols) needs root, ~/.local/share/fonts does not. */
    { NERD_USER_PATH, "/usr/share/fonts/TTF/SymbolsNerdFontMono-Regular.ttf",
      "/usr/share/fonts/nerd-fonts-symbols/SymbolsNerdFontMono-Regular.ttf",
      "/usr/share/fonts/truetype/nerd-fonts/SymbolsNerdFontMono-Regular.ttf" },
#endif
};

/* Runtime grid. Defaults are the compile-time cell; lj_render_set_font may
 * re-derive them from a real face (owkTerm). Not part of R: survives close(). */
static int lj_cw = LJ_CELL_W, lj_lh = LJ_LINE_H;
static int lj_px = 0;                 /* explicit mono raster px (0 = lj_lh - 5) */
static int lj_base = LJ_LINE_H - 5;   /* baseline offset from the cell top */
static const char *lj_override[NFACE];
static double lj_line_scale = 1.0;
void lj_render_set_line_scale(double scale) { lj_line_scale = scale > 0.5 && scale < 3.0 ? scale : 1.0; }
static int mono_px(void) { return lj_px > 0 ? lj_px : lj_lh - 5; }
int  lj_render_cell_w(void) { return lj_cw; }
int  lj_render_line_h(void) { return lj_lh; }
void lj_render_set_font(const char *mono_path, const char *emoji_path, const char *cjk_path, int px) {
    lj_override[MONO_FACE] = mono_path; lj_override[EMOJI_FACE] = emoji_path; lj_override[CJK_FACE] = cjk_path;
    lj_px = px > 0 ? px : 0;
    if (!lj_px) { lj_cw = LJ_CELL_W; lj_lh = LJ_LINE_H; lj_base = LJ_LINE_H - 5; }
}

#define CACHE_BITS 10
#define CACHE_N    (1 << CACHE_BITS)

typedef struct {                 /* one rasterised glyph, ARGB or A8 */
    uint32_t cp;                 /* 0 = empty slot */
    int      scale;              /* cache is keyed by (cp, scale, trim) */
    int      trim;               /* raster px removed; advance is NOT affected */
    int      w, h, left, top;    /* bitmap box, FT pen-relative */
    int      colour;             /* 1 = BGRA artwork (emoji), 0 = A8 coverage */
    uint8_t *bits;               /* w*h (A8) or w*h*4 (BGRA) */
} glyph_t;

static struct {
    int        ready;
    FT_Library ft;
    FT_Face    face[NFACE];
    int        have[NFACE];
    int        nfaces;
    int        bitmap_only;   /* every FT face missing -> HUI 8x8 atlas */
    int        text_trim;     /* raster px removed from text; advance unchanged */
    glyph_t    cache[CACHE_N];
} R;

/* ── small helpers ─────────────────────────────────────────────────────── */

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp; int n;
    if (*s < 0x80)            { cp = *s;         n = 1; }
    else if ((*s & 0xE0)==0xC0){ cp = *s & 0x1F; n = 2; }
    else if ((*s & 0xF0)==0xE0){ cp = *s & 0x0F; n = 3; }
    else if ((*s & 0xF8)==0xF0){ cp = *s & 0x07; n = 4; }
    else                      { *p += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

/* ⚠ VENDORED wcwidth, DERIVED not hand-written. MSVC's CRT has no wcwidth and
 * the POSIX one is locale-dependent, so the same input could measure
 * differently on two machines — and for a terminal CELL GRID that means the
 * grid and the renderer disagree about where a glyph ends.
 *
 * These tables were GENERATED from this system's own wcwidth over all of
 * Unicode (see the note below), so on Linux they agree with libc by
 * construction and Windows gets that identical answer. A hand-written
 * approximation was tried first and disagreed on 6.8% of codepoints —
 * 13,211 of them, including U+231A ⌚ and the U+23E9 media controls — which
 * is exactly the desync this exists to prevent.
 * Regenerate: see /tmp/genw.c pattern in the session log; runs of width 2 and
 * width 0 emitted as sorted ranges, then binary-searched here. */
typedef struct { uint32_t lo, hi; } lj_range;

static const lj_range LJ_WIDE[] = {
    {0x1100,0x115F},{0x231A,0x231B},{0x2329,0x232A},{0x23E9,0x23EC},{0x23F0,0x23F0},
    {0x23F3,0x23F3},{0x25FD,0x25FE},{0x2614,0x2615},{0x2630,0x2637},{0x2648,0x2653},
    {0x267F,0x267F},{0x268A,0x268F},{0x2693,0x2693},{0x26A1,0x26A1},{0x26AA,0x26AB},
    {0x26BD,0x26BE},{0x26C4,0x26C5},{0x26CE,0x26CE},{0x26D4,0x26D4},{0x26EA,0x26EA},
    {0x26F2,0x26F3},{0x26F5,0x26F5},{0x26FA,0x26FA},{0x26FD,0x26FD},{0x2705,0x2705},
    {0x270A,0x270B},{0x2728,0x2728},{0x274C,0x274C},{0x274E,0x274E},{0x2753,0x2755},
    {0x2757,0x2757},{0x2795,0x2797},{0x27B0,0x27B0},{0x27BF,0x27BF},{0x2B1B,0x2B1C},
    {0x2B50,0x2B50},{0x2B55,0x2B55},{0x2E80,0x2E99},{0x2E9B,0x2EF3},{0x2F00,0x2FD5},
    {0x2FF0,0x3029},{0x302E,0x303E},{0x3041,0x3096},{0x309B,0x30FF},{0x3105,0x312F},
    {0x3131,0x3163},{0x3165,0x318E},{0x3190,0x31E5},{0x31EF,0x321E},{0x3220,0xA48C},
    {0xA490,0xA4C6},{0xA960,0xA97C},{0xAC00,0xD7A3},{0xF900,0xFA6D},{0xFA70,0xFAD9},
    {0xFE10,0xFE19},{0xFE30,0xFE52},{0xFE54,0xFE66},{0xFE68,0xFE6B},{0xFF01,0xFF60},
    {0xFFE0,0xFFE6},{0x16FE0,0x16FE3},{0x16FF0,0x16FF6},{0x17000,0x18CD5},{0x18CFF,0x18D1E},
    {0x18D80,0x18DF2},{0x1AFF0,0x1AFF3},{0x1AFF5,0x1AFFB},{0x1AFFD,0x1AFFE},
    {0x1B000,0x1B122},{0x1B132,0x1B132},{0x1B150,0x1B152},{0x1B155,0x1B155},
    {0x1B164,0x1B167},{0x1B170,0x1B2FB},{0x1D300,0x1D356},{0x1D360,0x1D376},
    {0x1F004,0x1F004},{0x1F0CF,0x1F0CF},{0x1F18E,0x1F18E},{0x1F191,0x1F19A},
    {0x1F200,0x1F202},{0x1F210,0x1F23B},{0x1F240,0x1F248},{0x1F250,0x1F251},
    {0x1F260,0x1F265},{0x1F300,0x1F320},{0x1F32D,0x1F335},{0x1F337,0x1F37C},
    {0x1F37E,0x1F393},{0x1F3A0,0x1F3CA},{0x1F3CF,0x1F3D3},{0x1F3E0,0x1F3F0},
    {0x1F3F4,0x1F3F4},{0x1F3F8,0x1F43E},{0x1F440,0x1F440},{0x1F442,0x1F4FC},
    {0x1F4FF,0x1F53D},{0x1F54B,0x1F54E},{0x1F550,0x1F567},{0x1F57A,0x1F57A},
    {0x1F595,0x1F596},{0x1F5A4,0x1F5A4},{0x1F5FB,0x1F64F},{0x1F680,0x1F6C5},
    {0x1F6CC,0x1F6CC},{0x1F6D0,0x1F6D2},{0x1F6D5,0x1F6D8},{0x1F6DC,0x1F6DF},
    /* ⚠ THE ICON SET MUST BE ONE WIDTH (tab-icons-flicker, 2026-09-12). The
     * table had a hole at U+1F6E0..U+1F6EA, so codex's hammer-and-wrench (U+1F6E0)
     * measured 1 cell while brain/magnifier/herb (U+1F9E0/U+1F50E/U+1F33F) measured
     * 2. The emoji sub-blocks on either side are already wide here, and every
     * terminal that draws the other three icons as wide draws this one wide too,
     * so the hole was an omission, not a Unicode East-Asian-Width judgement. A
     * per-agent width difference makes a chip's reserved width disagree with what
     * is painted and lets the neighbour's panel erase the icon. */
    {0x1F6E0,0x1F6EA},{0x1F6EB,0x1F6EC},{0x1F6F4,0x1F6FC},{0x1F7E0,0x1F7EB},{0x1F7F0,0x1F7F0},
    {0x1F90C,0x1F93A},{0x1F93C,0x1F945},{0x1F947,0x1F9FF},{0x1FA70,0x1FA7C},
    {0x1FA80,0x1FA8A},{0x1FA8E,0x1FAC6},{0x1FAC8,0x1FAC8},{0x1FACD,0x1FADC},
    {0x1FADF,0x1FAEA},{0x1FAEF,0x1FAF8},{0x20000,0x2A6DF},{0x2A700,0x2B81D},
    {0x2B820,0x2CEAD},{0x2CEB0,0x2EBE0},{0x2EBF0,0x2EE5D},{0x2F800,0x2FA1D},
    {0x30000,0x3134A},{0x31350,0x33479},
};
static const lj_range LJ_ZERO[] = {
    {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},{0x05C1,0x05C2},
    {0x05C4,0x05C5},{0x05C7,0x05C7},{0x0610,0x061A},{0x061C,0x061C},{0x064B,0x065F},
    {0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},{0x06E7,0x06E8},{0x06EA,0x06ED},
    {0x0711,0x0711},{0x0730,0x074A},{0x07A6,0x07B0},{0x07EB,0x07F3},{0x07FD,0x07FD},
    {0x0816,0x0819},{0x081B,0x0823},{0x0825,0x0827},{0x0829,0x082D},{0x0859,0x085B},
    {0x0897,0x089F},{0x08CA,0x08E1},{0x08E3,0x0902},{0x093A,0x093A},{0x093C,0x093C},
    {0x0941,0x0948},{0x094D,0x094D},{0x0951,0x0957},{0x0962,0x0963},{0x0981,0x0981},
    {0x09BC,0x09BC},{0x09C1,0x09C4},{0x09CD,0x09CD},{0x09E2,0x09E3},{0x09FE,0x09FE},
    {0x0A01,0x0A02},{0x0A3C,0x0A3C},{0x0A41,0x0A42},{0x0A47,0x0A48},{0x0A4B,0x0A4D},
    {0x0A51,0x0A51},{0x0A70,0x0A71},{0x0A75,0x0A75},{0x0A81,0x0A82},{0x0ABC,0x0ABC},
    {0x0AC1,0x0AC5},{0x0AC7,0x0AC8},{0x0ACD,0x0ACD},{0x0AE2,0x0AE3},{0x0AFA,0x0AFF},
    {0x0B01,0x0B01},{0x0B3C,0x0B3C},{0x0B3F,0x0B3F},{0x0B41,0x0B44},{0x0B4D,0x0B4D},
    {0x0B55,0x0B56},{0x0B62,0x0B63},{0x0B82,0x0B82},{0x0BC0,0x0BC0},{0x0BCD,0x0BCD},
    {0x0C00,0x0C00},{0x0C04,0x0C04},{0x0C3C,0x0C3C},{0x0C3E,0x0C40},{0x0C46,0x0C48},
    {0x0C4A,0x0C4D},{0x0C55,0x0C56},{0x0C62,0x0C63},{0x0C81,0x0C81},{0x0CBC,0x0CBC},
    {0x0CBF,0x0CBF},{0x0CC6,0x0CC6},{0x0CCC,0x0CCD},{0x0CE2,0x0CE3},{0x0D00,0x0D01},
    {0x0D3B,0x0D3C},{0x0D41,0x0D44},{0x0D4D,0x0D4D},{0x0D62,0x0D63},{0x0D81,0x0D81},
    {0x0DCA,0x0DCA},{0x0DD2,0x0DD4},{0x0DD6,0x0DD6},{0x0E31,0x0E31},{0x0E34,0x0E3A},
    {0x0E47,0x0E4E},{0x0EB1,0x0EB1},{0x0EB4,0x0EBC},{0x0EC8,0x0ECE},{0x0F18,0x0F19},
    {0x0F35,0x0F35},{0x0F37,0x0F37},{0x0F39,0x0F39},{0x0F71,0x0F7E},{0x0F80,0x0F84},
    {0x0F86,0x0F87},{0x0F8D,0x0F97},{0x0F99,0x0FBC},{0x0FC6,0x0FC6},{0x102D,0x1030},
    {0x1032,0x1037},{0x1039,0x103A},{0x103D,0x103E},{0x1058,0x1059},{0x105E,0x1060},
    {0x1071,0x1074},{0x1082,0x1082},{0x1085,0x1086},{0x108D,0x108D},{0x109D,0x109D},
    {0x1160,0x11FF},{0x135D,0x135F},{0x1712,0x1714},{0x1732,0x1733},{0x1752,0x1753},
    {0x1772,0x1773},{0x17B4,0x17B5},{0x17B7,0x17BD},{0x17C6,0x17C6},{0x17C9,0x17D3},
    {0x17DD,0x17DD},{0x180B,0x180F},{0x1885,0x1886},{0x18A9,0x18A9},{0x1920,0x1922},
    {0x1927,0x1928},{0x1932,0x1932},{0x1939,0x193B},{0x1A17,0x1A18},{0x1A1B,0x1A1B},
    {0x1A56,0x1A56},{0x1A58,0x1A5E},{0x1A60,0x1A60},{0x1A62,0x1A62},{0x1A65,0x1A6C},
    {0x1A73,0x1A7C},{0x1A7F,0x1A7F},{0x1AB0,0x1ADD},{0x1AE0,0x1AEB},{0x1B00,0x1B03},
    {0x1B34,0x1B34},{0x1B36,0x1B3A},{0x1B3C,0x1B3C},{0x1B42,0x1B42},{0x1B6B,0x1B73},
    {0x1B80,0x1B81},{0x1BA2,0x1BA5},{0x1BA8,0x1BA9},{0x1BAB,0x1BAD},{0x1BE6,0x1BE6},
    {0x1BE8,0x1BE9},{0x1BED,0x1BED},{0x1BEF,0x1BF1},{0x1C2C,0x1C33},{0x1C36,0x1C37},
    {0x1CD0,0x1CD2},{0x1CD4,0x1CE0},{0x1CE2,0x1CE8},{0x1CED,0x1CED},{0x1CF4,0x1CF4},
    {0x1CF8,0x1CF9},{0x1DC0,0x1DFF},{0x200B,0x200F},{0x202A,0x202E},{0x2060,0x2064},
    {0x2066,0x206F},{0x20D0,0x20F0},{0x2CEF,0x2CF1},{0x2D7F,0x2D7F},{0x2DE0,0x2DFF},
    {0x302A,0x302D},{0x3099,0x309A},{0x3164,0x3164},{0xA66F,0xA672},{0xA674,0xA67D},
    {0xA69E,0xA69F},{0xA6F0,0xA6F1},{0xA802,0xA802},{0xA806,0xA806},{0xA80B,0xA80B},
    {0xA825,0xA826},{0xA82C,0xA82C},{0xA8C4,0xA8C5},{0xA8E0,0xA8F1},{0xA8FF,0xA8FF},
    {0xA926,0xA92D},{0xA947,0xA951},{0xA980,0xA982},{0xA9B3,0xA9B3},{0xA9B6,0xA9B9},
    {0xA9BC,0xA9BD},{0xA9E5,0xA9E5},{0xAA29,0xAA2E},{0xAA31,0xAA32},{0xAA35,0xAA36},
    {0xAA43,0xAA43},{0xAA4C,0xAA4C},{0xAA7C,0xAA7C},{0xAAB0,0xAAB0},{0xAAB2,0xAAB4},
    {0xAAB7,0xAAB8},{0xAABE,0xAABF},{0xAAC1,0xAAC1},{0xAAEC,0xAAED},{0xAAF6,0xAAF6},
    {0xABE5,0xABE5},{0xABE8,0xABE8},{0xABED,0xABED},{0xD7B0,0xD7C6},{0xD7CB,0xD7FB},
    {0xFB1E,0xFB1E},{0xFE00,0xFE0F},{0xFE20,0xFE2F},{0xFEFF,0xFEFF},{0xFFA0,0xFFA0},
    {0x101FD,0x101FD},{0x102E0,0x102E0},{0x10376,0x1037A},{0x10A01,0x10A03},
    {0x10A05,0x10A06},{0x10A0C,0x10A0F},{0x10A38,0x10A3A},{0x10A3F,0x10A3F},
    {0x10AE5,0x10AE6},{0x10D24,0x10D27},{0x10D69,0x10D6D},{0x10EAB,0x10EAC},
    {0x10EFA,0x10EFF},{0x10F46,0x10F50},{0x10F82,0x10F85},{0x11001,0x11001},
    {0x11038,0x11046},{0x11070,0x11070},{0x11073,0x11074},{0x1107F,0x11081},
    {0x110B3,0x110B6},{0x110B9,0x110BA},{0x110C2,0x110C2},{0x11100,0x11102},
    {0x11127,0x1112B},{0x1112D,0x11134},{0x11173,0x11173},{0x11180,0x11181},
    {0x111B6,0x111BE},{0x111C9,0x111CC},{0x111CF,0x111CF},{0x1122F,0x11231},
    {0x11234,0x11234},{0x11236,0x11237},{0x1123E,0x1123E},{0x11241,0x11241},
    {0x112DF,0x112DF},{0x112E3,0x112EA},{0x11300,0x11301},{0x1133B,0x1133C},
    {0x11340,0x11340},{0x11366,0x1136C},{0x11370,0x11374},{0x113BB,0x113C0},
    {0x113CE,0x113CE},{0x113D0,0x113D0},{0x113D2,0x113D2},{0x113E1,0x113E2},
    {0x11438,0x1143F},{0x11442,0x11444},{0x11446,0x11446},{0x1145E,0x1145E},
    {0x114B3,0x114B8},{0x114BA,0x114BA},{0x114BF,0x114C0},{0x114C2,0x114C3},
    {0x115B2,0x115B5},{0x115BC,0x115BD},{0x115BF,0x115C0},{0x115DC,0x115DD},
    {0x11633,0x1163A},{0x1163D,0x1163D},{0x1163F,0x11640},{0x116AB,0x116AB},
    {0x116AD,0x116AD},{0x116B0,0x116B5},{0x116B7,0x116B7},{0x1171D,0x1171D},
    {0x1171F,0x1171F},{0x11722,0x11725},{0x11727,0x1172B},{0x1182F,0x11837},
    {0x11839,0x1183A},{0x1193B,0x1193C},{0x1193E,0x1193E},{0x11943,0x11943},
    {0x119D4,0x119D7},{0x119DA,0x119DB},{0x119E0,0x119E0},{0x11A01,0x11A0A},
    {0x11A33,0x11A38},{0x11A3B,0x11A3E},{0x11A47,0x11A47},{0x11A51,0x11A56},
    {0x11A59,0x11A5B},{0x11A8A,0x11A96},{0x11A98,0x11A99},{0x11B60,0x11B60},
    {0x11B62,0x11B64},{0x11B66,0x11B66},{0x11C30,0x11C36},{0x11C38,0x11C3D},
    {0x11C3F,0x11C3F},{0x11C92,0x11CA7},{0x11CAA,0x11CB0},{0x11CB2,0x11CB3},
    {0x11CB5,0x11CB6},{0x11D31,0x11D36},{0x11D3A,0x11D3A},{0x11D3C,0x11D3D},
    {0x11D3F,0x11D45},{0x11D47,0x11D47},{0x11D90,0x11D91},{0x11D95,0x11D95},
    {0x11D97,0x11D97},{0x11EF3,0x11EF4},{0x11F00,0x11F01},{0x11F36,0x11F3A},
    {0x11F40,0x11F40},{0x11F42,0x11F42},{0x11F5A,0x11F5A},{0x13440,0x13440},
    {0x13447,0x13455},{0x1611E,0x16129},{0x1612D,0x1612F},{0x16AF0,0x16AF4},
    {0x16B30,0x16B36},{0x16F4F,0x16F4F},{0x16F8F,0x16F92},{0x16FE4,0x16FE4},
    {0x1BC9D,0x1BC9E},{0x1BCA0,0x1BCA3},{0x1CF00,0x1CF2D},{0x1CF30,0x1CF46},
    {0x1D167,0x1D169},{0x1D173,0x1D182},{0x1D185,0x1D18B},{0x1D1AA,0x1D1AD},
    {0x1D242,0x1D244},{0x1DA00,0x1DA36},{0x1DA3B,0x1DA6C},{0x1DA75,0x1DA75},
    {0x1DA84,0x1DA84},{0x1DA9B,0x1DA9F},{0x1DAA1,0x1DAAF},{0x1E000,0x1E006},
    {0x1E008,0x1E018},{0x1E01B,0x1E021},{0x1E023,0x1E024},{0x1E026,0x1E02A},
    {0x1E08F,0x1E08F},{0x1E130,0x1E136},{0x1E2AE,0x1E2AE},{0x1E2EC,0x1E2EF},
    {0x1E4EC,0x1E4EF},{0x1E5EE,0x1E5EF},{0x1E6E3,0x1E6E3},{0x1E6E6,0x1E6E6},
    {0x1E6EE,0x1E6EF},{0x1E6F5,0x1E6F5},{0x1E8D0,0x1E8D6},{0x1E944,0x1E94A},
    {0xE0001,0xE0001},{0xE0020,0xE007F},{0xE0100,0xE01EF},
};

static int in_ranges(uint32_t cp, const lj_range *r, int n) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cp < r[m].lo) hi = m - 1;
        else if (cp > r[m].hi) lo = m + 1;
        else return 1;
    }
    return 0;
}

static int lj_wcwidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return -1;
    if (in_ranges(cp, LJ_ZERO, (int)(sizeof(LJ_ZERO)/sizeof(LJ_ZERO[0])))) return 0;
    if (in_ranges(cp, LJ_WIDE, (int)(sizeof(LJ_WIDE)/sizeof(LJ_WIDE[0])))) return 2;
    return 1;
}

int lj_render_cells_for(uint32_t cp) {
    return lj_wcwidth(cp) == 2 ? 2 : 1;    /* ADVANCE: width 0 and -1 both occupy one */
}

/* MEASURE, not advance. ⚠ A COMBINING MARK OCCUPIES NO COLUMN. The paint pass
 * (c_ansi.c:371) attaches a zero-width codepoint to the PREVIOUS cell as a mark
 * and consumes no column, while lj_render_cells_for counts it as one — so a
 * label carrying a combining mark or a variation selector was measured one cell
 * too wide per mark, the chip reserved more than it painted, and the layout
 * disagreed with the canvas frame to frame (the same class of defect as the
 * U+1F6E0 width gap in tab-icons-flicker, found 2026-09-12 while measuring the
 * icon set: U+FE0F measured 1 here and 0 in the paint pass). Use this wherever
 * a STRING IS SIZED; keep lj_render_cells_for where a cursor is advanced. */
int lj_render_cells_measure(uint32_t cp) {
    int w = lj_wcwidth(cp);
    return w == 2 ? 2 : w == 0 ? 0 : 1;
}

static void blend(int x, int y, uint8_t a, uint32_t rgb) {
    if (a == 0) return;
    if (x < 0 || y < 0 || x >= hui__fb.w || y >= hui__fb.h) return;
    uint32_t *px = &hui__fb.pixels[(size_t)y * hui__fb.w + x];
    uint32_t d = *px;
    unsigned sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    unsigned dr = (d   >> 16) & 0xFF, dg = (d   >> 8) & 0xFF, db = d   & 0xFF;
    unsigned ia = 255u - a;
    *px = 0xFF000000u
        | (((sr * a + dr * ia) / 255u) << 16)
        | (((sg * a + dg * ia) / 255u) << 8)
        |  ((sb * a + db * ia) / 255u);
}

/* ── face chain ────────────────────────────────────────────────────────── */

/* Emoji presentation: a pictograph that has colour artwork is drawn from the
 * colour face even when the CJK face also carries a monochrome outline for it.
 * That is what WezTerm/xterm-with-fontconfig do, and it is why lilJack's icons
 * were colour inside WezTerm but grey inside owkTerm and the native window. */
static int prefers_colour(uint32_t cp) {
    return cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) || cp == 0x231A || cp == 0x231B || (cp >= 0x23E9 && cp <= 0x23F3);
}
static int face_for(uint32_t cp, FT_UInt *gi_out) {
    if (R.bitmap_only) return -1;             /* the 8x8 path handles it */
    /* Only for DOUBLE-width pictographs: a width-1 symbol such as 🛠 U+1F6E0 keeps
     * its text presentation (as WezTerm does), because colour artwork needs the
     * two cells the grid does not give it. */
    if (R.have[EMOJI_FACE] && prefers_colour(cp) && lj_render_cells_for(cp) == 2) {
        FT_UInt gi = FT_Get_Char_Index(R.face[EMOJI_FACE], cp);
        if (gi) { if (gi_out) *gi_out = gi; return EMOJI_FACE; }
    }
    static const int order[NFACE] = { MONO_FACE, SYM_FACE, SYM2_FACE, SYM3_FACE, NERD_FACE, CJK_FACE, EMOJI_FACE };
    for (int k = 0; k < NFACE; k++) {
        int i = order[k];
        if (!R.have[i]) continue;
        FT_UInt gi = FT_Get_Char_Index(R.face[i], cp);
        if (gi) { if (gi_out) *gi_out = gi; return i; }
    }
    return -1;
}

int lj_render_has_codepoint(uint32_t cp) {
    if (R.bitmap_only) return cp >= 32 && cp < 127;   /* the atlas is ASCII */
    return face_for(cp, NULL) >= 0;
}
int lj_render_faces(void)                { return R.nfaces; }
int lj_render_width(void)                { return R.ready ? hui__fb.w : 0; }
int lj_render_height(void)               { return R.ready ? hui__fb.h : 0; }

/* Nearest fixed strike for a bitmap-only (CBDT/CBLC) face. */
static void select_bitmap_strike(FT_Face f, int target_px);

/* ⚠ The colour-emoji face is NOT the same kind of font on both platforms:
 *   Linux   NotoColorEmoji  CBDT/CBLC  -> fixed bitmap strikes, NOT scalable
 *   Windows Segoe UI Emoji  COLR/CPAL  -> scalable vector layers
 * FreeType renders both to BGRA under FT_LOAD_COLOR, but a scalable face has
 * no strikes to select, so calling FT_Select_Size on it fails and leaves the
 * size unset. Size by whichever the face actually is. */
static void size_emoji_face(FT_Face f, int target_px) {
    if (f->num_fixed_sizes > 0) { select_bitmap_strike(f, target_px); return; }
    FT_Set_Pixel_Sizes(f, 0, target_px);            /* COLR/CPAL: scalable */
}

static void select_bitmap_strike(FT_Face f, int target_px) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < f->num_fixed_sizes; i++) {
        int d = f->available_sizes[i].height - target_px;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    if (f->num_fixed_sizes > 0) FT_Select_Size(f, best);
}

/* Window text size is DELIBERATELY separate from the layout cell. lj_cw and
 * lj_lh stay the grid: advance, wrapping and hit-testing must not move, or a
 * terminal pane stops lining up with its own VT. This trims only the RASTER, so
 * a pane's text can read a point smaller than the chrome around it while sitting
 * on exactly the same cells. Clamped, and glyphs never rasterise below 6px. */
void lj_render_text_trim(int px) {
    if (px < 0) px = 0;
    if (px > lj_lh / 2) px = lj_lh / 2;
    R.text_trim = px;
}

/* ── glyph cache ───────────────────────────────────────────────────────── */

static glyph_t *rasterise(uint32_t cp, int scale) {
    if (scale < 1) scale = 1;
    int trim = R.text_trim;   /* raster-only: the cell advance never changes */
    unsigned h = ((cp * 2654435761u) ^ (unsigned)(scale * 40503u)
                  ^ (unsigned)(trim * 2246822519u)) >> (32 - CACHE_BITS);
    for (unsigned probe = 0; probe < 8; probe++) {
        glyph_t *g = &R.cache[(h + probe) & (CACHE_N - 1)];
        if (g->cp == cp && g->scale == scale && g->trim == trim && g->bits) return g;
        if (g->cp == 0) break;
    }
    FT_UInt gi = 0;
    int fi = face_for(cp, &gi);
    /* ⚠ PRIVATE-USE ICONS (nerd fonts, U+E000–F8FF, U+F0000+) have no face on a
     * stock box; agents' CLIs still print them. A hollow tofu box is what the operator
     * called "empty rectangles" (2026-09-12). Draw a small filled square instead:
     * visibly an icon slot, never a box. */
    if (fi < 0 && ((cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0xF0000)) { cp = 0x25AA; fi = face_for(cp, &gi); }
    if (fi < 0) return NULL;
    FT_Face f = R.face[fi];
    int colour = (fi == EMOJI_FACE);
    FT_Int32 flags = FT_LOAD_RENDER | (colour ? FT_LOAD_COLOR : 0);
    int px = ((fi == CJK_FACE) ? lj_cw * 2 - 2 : mono_px()) * scale - trim;
    if (px < 6) px = 6;                     /* never rasterise into illegibility */
    if (colour) size_emoji_face(f, lj_lh * scale);   /* emoji stay cell-sized */
    else FT_Set_Pixel_Sizes(f, 0, px);
    if (FT_Load_Glyph(f, gi, flags) != 0) return NULL;
    FT_Bitmap *bm = &f->glyph->bitmap;
    if (bm->width == 0 || bm->rows == 0) return NULL;

    /* find a home: exact, empty, or evict the first probe */
    glyph_t *g = NULL;
    for (unsigned probe = 0; probe < 8 && !g; probe++) {
        glyph_t *c = &R.cache[(h + probe) & (CACHE_N - 1)];
        if (c->cp == 0 || (c->cp == cp && c->scale == scale && c->trim == trim)) g = c;
    }
    if (!g) g = &R.cache[h & (CACHE_N - 1)];
    free(g->bits);

    if (colour) {
        /* CBDT artwork is a fixed strike; box-downscale it into the cells the
         * GRID gives this codepoint (2 for a wide pictograph, 1 for a width-1
         * symbol such as 🛠 U+1F6E0), preserving aspect. Sizing every emoji to
         * two cells drew width-1 symbols across the neighbour cell, where the
         * next glyph then cut them in half. */
        int cells = lj_render_cells_for(cp); if (cells < 1) cells = 1;
        int boxw = lj_cw * cells * scale, boxh = lj_lh * scale;
        int dw = boxw, dh = boxh;
        if ((long)bm->width * boxh > (long)bm->rows * boxw) dh = (int)((long)boxw * bm->rows / (bm->width ? bm->width : 1));
        else dw = (int)((long)boxh * bm->width / (bm->rows ? bm->rows : 1));
        if ((int)bm->width < dw) dw = bm->width;
        if ((int)bm->rows  < dh) dh = bm->rows;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        uint8_t *out = (uint8_t *)calloc((size_t)dw * dh, 4);
        if (!out) return NULL;
        /* Area-average (box filter) downscale in premultiplied space: the
         * previous nearest-neighbour pick from a 109 px strike into ~34 px made
         * every emoji look bulky and blurry-jagged (the operator, 2026-09-13). */
        for (int yy = 0; yy < dh; yy++) {
            int sy0 = (int)((long)yy * bm->rows / dh), sy1 = (int)((long)(yy + 1) * bm->rows / dh); if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int xx = 0; xx < dw; xx++) {
                int sx0 = (int)((long)xx * bm->width / dw), sx1 = (int)((long)(xx + 1) * bm->width / dw); if (sx1 <= sx0) sx1 = sx0 + 1;
                unsigned long b = 0, g = 0, r = 0, a = 0, n = 0;
                for (int y2 = sy0; y2 < sy1 && y2 < (int)bm->rows; y2++)
                    for (int x2 = sx0; x2 < sx1 && x2 < (int)bm->width; x2++) {
                        const uint8_t *s = bm->buffer + (size_t)y2 * bm->pitch + (size_t)x2 * 4;
                        b += s[0]; g += s[1]; r += s[2]; a += s[3]; n++;      /* BGRA, premultiplied in CBDT */
                    }
                uint8_t *d = out + ((size_t)yy * dw + xx) * 4;
                if (n) { d[0] = (uint8_t)(b / n); d[1] = (uint8_t)(g / n); d[2] = (uint8_t)(r / n); d[3] = (uint8_t)(a / n); }
            }
        }
        g->cp=cp; g->scale=scale; g->trim=trim; g->w=dw; g->h=dh; g->colour=1; g->bits=out;
        g->left = (boxw - dw) / 2;                 /* centre narrow art in its cell(s) */
        /* Vertically centre the artwork in the line: gy = baseline - top must
         * land at y + (line - dh)/2. The old (line + dh)/2 put the top of every
         * emoji ABOVE the cell, so the glyph clip cut its head off (visible on
         * the 💬 and 👤 icons in both the native window and owkTerm). */
        g->top  = lj_base * scale - (lj_lh * scale - dh) / 2;
    } else {
        size_t n = (size_t)bm->width * bm->rows;
        uint8_t *out = (uint8_t *)malloc(n ? n : 1);
        if (!out) return NULL;
        for (unsigned yy = 0; yy < bm->rows; yy++)
            memcpy(out + (size_t)yy * bm->width,
                   bm->buffer + (size_t)yy * bm->pitch, bm->width);
        g->cp=cp; g->scale=scale; g->trim=trim; g->w=bm->width; g->h=bm->rows; g->colour=0; g->bits=out;
        g->left = f->glyph->bitmap_left;
        g->top  = f->glyph->bitmap_top;
    }
    return g;
}

/* HUI's built-in 8x8 ASCII atlas, scaled x2 into the 10x20 cell. Only used
 * when NO FreeType face opened — the last line of the fallback chain. */
static int draw_bitmap_glyph(int x, int y, uint32_t cp, uint32_t rgb, int cw) {
    if (cp < 32 || cp >= 127) return 0;
    const uint8_t *g = hui_font_glyph((unsigned char)cp);
    if (!g) return 0;
    int oy = y + (lj_lh - 16) / 2;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (g[r] & (1u << c))
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int px = x + c * 2 + dx, py = oy + r * 2 + dy;
                        if (px >= x && px < x + cw) blend(px, py, 255, rgb);
                    }
    return 1;
}

/* Hollow box for a codepoint no face covers — visible, never silent. */
static void draw_tofu(int x, int y, int cells, uint32_t rgb) {
    int w = lj_cw * cells - 2, h = lj_lh - 6;
    for (int i = 0; i < w; i++) { blend(x+1+i, y+3, 160, rgb); blend(x+1+i, y+3+h-1, 160, rgb); }
    for (int j = 0; j < h; j++) { blend(x+1, y+3+j, 160, rgb); blend(x+1+w-1, y+3+j, 160, rgb); }
}

/* ── public API ────────────────────────────────────────────────────────── */

int lj_render_init(int width, int height) {
    if (R.ready) lj_render_close();
    memset(&R, 0, sizeof(R));
    /* No setlocale: width comes from the vendored tables, so rendering is
     * LOCALE-INDEPENDENT — the same string measures and draws identically
     * whatever LC_CTYPE the host happens to have. That is worth more than
     * portability alone: it removes an invisible environment dependency.  */
    if (FT_Init_FreeType(&R.ft) != 0) return -1;
    /* Test hook: LJ_RENDER_NO_FONTS=1 simulates every font being absent, so the
     * bitmap fallback is exercised rather than assumed. An untested fallback is
     * not a fallback. */
    const int no_fonts = getenv("LJ_RENDER_NO_FONTS") != NULL;
    if (!nerd_user_path[0]) {
        const char *home = getenv("HOME");
        if (home && *home) snprintf(nerd_user_path, sizeof nerd_user_path, "%s/.local/share/fonts/SymbolsNerdFontMono-Regular.ttf", home);
    }
    for (int i = 0; i < NFACE && !no_fonts; i++) {
        int opened = 0;
        if (lj_override[i] && *lj_override[i])
            opened = (FT_New_Face(R.ft, lj_override[i], 0, &R.face[i]) == 0);
        for (int k = 0; k < MAX_CAND && FACE_CAND[i][k] && !opened; k++)
            opened = (FT_New_Face(R.ft, FACE_CAND[i][k], 0, &R.face[i]) == 0);
        if (!opened) continue;
        /* Runtime grid (owkTerm): derive the cell from the mono face itself so
         * the terminal's grid is the font's grid, as every real terminal does. */
        if (i == MONO_FACE && lj_px > 0 && FT_Set_Pixel_Sizes(R.face[i], 0, lj_px) == 0) {
            FT_Size_Metrics *m = &R.face[i]->size->metrics;
            int adv = 0;
            if (FT_Load_Char(R.face[i], 'M', FT_LOAD_DEFAULT) == 0) adv = (int)((R.face[i]->glyph->advance.x + 63) >> 6);
            int lh = (int)((m->height + 63) >> 6), asc = (int)((m->ascender + 63) >> 6);
            lh = (int)(lh * lj_line_scale + 0.5);
            if (adv > 0 && lh > 0) {
                lj_cw = adv; lj_lh = lh;
                lj_base = asc + (lh - (asc + (int)((-m->descender + 63) >> 6))) / 2;   /* centre ascent+descent in the line */
                if (lj_base > lh) lj_base = lh;
            }
        }
        /* Per-face pixel size. CJK is drawn LARGER than Latin on purpose: a
         * Han glyph is designed to fill a square em, so at the Latin size it
         * sits undersized in its 2-cell (20x20) box with a visible gap, while
         * Latin at the CJK size would overflow the 20px line. Verified by eye
         * against the rendered evidence, not assumed. */
        if (i == EMOJI_FACE) size_emoji_face(R.face[i], lj_lh);
        else {
            int px = (i == CJK_FACE) ? lj_cw * 2 - 2 : mono_px();   /* SYM sized like mono */
            if (FT_Set_Pixel_Sizes(R.face[i], 0, px) != 0) {
                FT_Done_Face(R.face[i]); continue;
            }
        }
        R.have[i] = 1; R.nfaces++;
    }
    /* ⚠ NO TOTAL FAILURE (codex, 2026-09-08). If every font file is missing we
     * fall back to HUI's built-in 8x8 ASCII atlas (hui_font.h) rather than
     * refusing to start. Latin still reads; anything above U+007F draws tofu,
     * so the degradation is VISIBLE rather than a blank window. */
    if (R.nfaces == 0) {
        FT_Done_FreeType(R.ft);
        R.ft = NULL;
        R.bitmap_only = 1;
    }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    hui_headless_init(width, height);
    if (!hui__fb.pixels) { lj_render_close(); return -3; }
    R.ready = 1;
    return 0;
}

void lj_render_resize(int w, int h) {
    if (!R.ready) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    hui_headless_resize(w, h);               /* invalidates lj_render_pixels() */
}

void lj_render_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!R.ready) return;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= hui__fb.h) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= hui__fb.w) continue;
            hui__fb.pixels[(size_t)yy * hui__fb.w + xx] = 0xFF000000u | (rgb & 0xFFFFFFu);
        }
    }
}

void lj_render_glyph(int x, int y, uint32_t cp, uint32_t rgb, int cells) {
    if (!R.ready) return;
    if (cells < 1) cells = 1;
    int cw = lj_cw * cells;
    if (R.bitmap_only) {
        if (!draw_bitmap_glyph(x, y, cp, rgb, cw)) draw_tofu(x, y, cells, rgb);
        return;
    }
    glyph_t *g = rasterise(cp, 1);
    if (!g) { draw_tofu(x, y, cells, rgb); return; }
    int baseline = y + lj_base;
    int gx = x + g->left, gy = baseline - g->top;
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if (py < y || py >= y + lj_lh) continue;          /* clip to line */
        for (int i = 0; i < g->w; i++) {
            int px = gx + i;
            if (px < x || px >= x + cw) continue;             /* clip to cells */
            if (g->colour) {
                const uint8_t *s = g->bits + ((size_t)j * g->w + i) * 4;
                /* ⚠ emoji keeps its OWN colours; rgb is deliberately ignored */
                blend(px, py, s[3], ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0]);
            } else {
                blend(px, py, g->bits[(size_t)j * g->w + i], rgb);
            }
        }
    }
}

void lj_render_text(int x, int y, const char *utf8, uint32_t rgb, int max_width) {
    if (!R.ready || !utf8) return;
    const char *p = utf8;
    int pen = 0;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int cells = lj_render_cells_for(cp);
        int adv = lj_cw * cells;
        if (max_width > 0 && pen + adv > max_width) break;
        if (cp != ' ') lj_render_glyph(x + pen, y, cp, rgb, cells);
        pen += adv;
    }
}

uint32_t *lj_render_pixels(void) { return R.ready ? hui__fb.pixels : NULL; }

int lj_render_save_ppm(const char *path) {
    if (!R.ready) return -1;
    hui_headless_save_ppm(path);
    return 0;
}
int lj_render_save_png(const char *path) {
    if (!R.ready) return -1;
    return hui_headless_save_png(path);
}
int lj_render_save_png_region(const char *path, int x, int y, int w, int h, int scale) {
    const uint32_t *px = lj_render_pixels(); int fw = lj_render_width(), fh = lj_render_height();
    if (!px || !path || scale < 1) return -1;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > fw) w = fw - x; if (y + h > fh) h = fh - y;
    if (w <= 0 || h <= 0) return -1;
    int ow = w * scale, oh = h * scale; uint32_t *out = malloc((size_t)ow * oh * sizeof *out); if (!out) return -1;
    for (int yy = 0; yy < oh; yy++) for (int xx = 0; xx < ow; xx++) out[(size_t)yy * ow + xx] = px[(size_t)(y + yy / scale) * fw + x + xx / scale];
    int rc = hui__save_argb_png(path, out, ow, oh); free(out); return rc;
}

void lj_render_close(void) {
    for (int i = 0; i < CACHE_N; i++) { free(R.cache[i].bits); R.cache[i].bits = NULL; R.cache[i].cp = 0; }
    if (R.ready) hui_headless_free();
    for (int i = 0; i < NFACE; i++) if (R.have[i]) FT_Done_Face(R.face[i]);
    if (R.ft) FT_Done_FreeType(R.ft);          /* NULL in bitmap_only mode */
    memset(&R, 0, sizeof(R));
}

/* ── panel + scaled text (codex, 2026-09-08: "island beauty, not box grid") ── */

void lj_render_panel(int x, int y, int w, int h, int radius, uint32_t rgb) {
    if (!R.ready || w <= 0 || h <= 0) return;
    int m = (w < h ? w : h) / 2;
    if (radius > m) radius = m;
    if (radius <= 0) { lj_render_rect(x, y, w, h, rgb); return; }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= hui__fb.h) continue;
        /* distance into a corner band, if any */
        int dy = (j < radius) ? (radius - 1 - j) : (j >= h - radius ? j - (h - radius) : -1);
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= hui__fb.w) continue;
            int dx = (i < radius) ? (radius - 1 - i) : (i >= w - radius ? i - (w - radius) : -1);
            uint8_t a = 255;
            if (dx >= 0 && dy >= 0) {
                /* anti-alias the corner arc: coverage from the radius boundary */
                float d = (float)dx * dx + (float)dy * dy;
                float r = (float)radius - 0.5f;
                float rr = r * r;
                if (d > rr + r) continue;                    /* fully outside */
                if (d > rr - r) {
                    float t = (rr + r - d) / (2.0f * r);      /* 0..1 across the edge */
                    a = (uint8_t)(t < 0 ? 0 : t > 1 ? 255 : t * 255.0f);
                }
            }
            if (a == 255) hui__fb.pixels[(size_t)py * hui__fb.w + px] = 0xFF000000u | (rgb & 0xFFFFFFu);
            else blend(px, py, a, rgb);
        }
    }
}

void lj_render_text_scaled(int x, int y, const char *utf8, uint32_t rgb,
                           int max_width, int scale) {
    if (!R.ready || !utf8) return;
    if (scale < 1) scale = 1;
    if (scale == 1) { lj_render_text(x, y, utf8, rgb, max_width); return; }
    const char *p = utf8;
    int pen = 0, cw = lj_cw * scale, lh = lj_lh * scale;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int cells = lj_render_cells_for(cp);
        int adv = cw * cells;
        if (max_width > 0 && pen + adv > max_width) break;
        if (cp != ' ') {
            if (R.bitmap_only) {
                draw_bitmap_glyph(x + pen, y, cp, rgb, adv);
            } else {
                glyph_t *g = rasterise(cp, scale);
                if (!g) { draw_tofu(x + pen, y, cells * scale, rgb); }
                else {
                    int baseline = y + lj_base * scale;
                    int gx = x + pen + g->left, gy = baseline - g->top;
                    for (int j = 0; j < g->h; j++) {
                        int py = gy + j;
                        if (py < y || py >= y + lh) continue;
                        for (int i = 0; i < g->w; i++) {
                            int px = gx + i;
                            if (px < x + pen || px >= x + pen + adv) continue;
                            if (g->colour) {
                                const uint8_t *s = g->bits + ((size_t)j * g->w + i) * 4;
                                blend(px, py, s[3], ((uint32_t)s[2]<<16)|((uint32_t)s[1]<<8)|s[0]);
                            } else blend(px, py, g->bits[(size_t)j * g->w + i], rgb);
                        }
                    }
                }
            }
        }
        pen += adv;
    }
}
