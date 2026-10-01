/*
 * hui_cum.h — Color Universal Model
 *
 * CUM is hui's built-in theming palette.  Every demo, template, plugin and
 * build tool uses CUM_ names for colours so the entire ecosystem shares one
 * consistent visual language and can be re-themed by swapping one define.
 *
 * Usage (default theme — Ristretto × warmer · darker · blue-yellow):
 *   #include "hui.h"          ← pulls in hui_cum.h automatically
 *
 * Custom theme — define your own values before including:
 *   #define CUM_CUSTOM
 *   #define CUM_BG0  HUI_RGB(...)
 *   ... (define all CUM_* you need)
 *   #include "hui.h"
 *
 * Theme selection macros (mutually exclusive, define before hui.h):
 *   CUM_THEME_RISTRETTO   — warm-dark, blue-yellow focus (default)
 *   CUM_THEME_DARK        — neutral dark (classic ImGui dark)
 *   CUM_CUSTOM            — all CUM_* provided by the user
 *
 * Semantic aliases (always defined, map to accent slots above):
 *   CUM_ACCENT   CUM_BLUE   — primary interactive highlight
 *   CUM_ACTIVE   CUM_YELL   — active / selected state
 *   CUM_OK       CUM_GREEN  — success / pass
 *   CUM_WARN     CUM_ORANG  — warning
 *   CUM_ERR      CUM_PINK   — error / fail
 *   CUM_INFO     CUM_CYAN   — informational
 */

#ifndef HUI_CUM_H
#define HUI_CUM_H

/* HUI_RGB is defined in hui_math.h — must be included before this file. */

/* ---- Theme: Ristretto (default) ---- */
/* Monokai Pro Ristretto × warmer · darker · blue-yellow shift            */
#if !defined(CUM_CUSTOM) && \
    (!defined(CUM_THEME_DARK) && !defined(CUM_THEME_RISTRETTO))
#  define CUM_THEME_RISTRETTO
#endif

#ifdef CUM_THEME_RISTRETTO

/* Backgrounds — 5 levels, warm near-black → lifted surface */
#define CUM_BG0   HUI_RGB( 18, 14, 14)   /* #120e0e  deepest   */
#define CUM_BG1   HUI_RGB( 26, 20, 20)   /* #1a1414  titlebar  */
#define CUM_BG2   HUI_RGB( 34, 27, 27)   /* #221b1b  main bg   */
#define CUM_BG3   HUI_RGB( 50, 40, 40)   /* #322828  lifted    */
#define CUM_BG4   HUI_RGB( 62, 50, 48)   /* #3e3230  hover     */

/* Foreground — 3 levels */
#define CUM_FG    HUI_RGB(237,222,198)   /* #eddec6  cream     */
#define CUM_FG2   HUI_RGB(148,130,112)   /* #948270  mid       */
#define CUM_FG3   HUI_RGB( 88, 76, 68)   /* #584c44  dim       */

/* Accents — blue-yellow focus */
#define CUM_BLUE  HUI_RGB( 90,158,210)   /* #5a9ed2  sky blue  */
#define CUM_YELL  HUI_RGB(242,194, 68)   /* #f2c244  gold      */
#define CUM_GREEN HUI_RGB(141,196, 92)   /* #8dc45c  sage      */
#define CUM_CYAN  HUI_RGB( 94,194,184)   /* #5ec2b8  teal      */
#define CUM_PINK  HUI_RGB(232, 85,110)   /* #e8556e  coral     */
#define CUM_ORANG HUI_RGB(232,133, 90)   /* #e8855a  amber     */
#define CUM_PURP  HUI_RGB(136,144,224)   /* #8890e0  indigo    */

#endif /* CUM_THEME_RISTRETTO */

/* ---- Theme: Dark (neutral) ---- */
#ifdef CUM_THEME_DARK

#define CUM_BG0   HUI_RGB( 15, 15, 15)
#define CUM_BG1   HUI_RGB( 22, 22, 22)
#define CUM_BG2   HUI_RGB( 30, 30, 30)
#define CUM_BG3   HUI_RGB( 46, 46, 46)
#define CUM_BG4   HUI_RGB( 60, 60, 60)

#define CUM_FG    HUI_RGB(220,220,220)
#define CUM_FG2   HUI_RGB(140,140,140)
#define CUM_FG3   HUI_RGB( 80, 80, 80)

#define CUM_BLUE  HUI_RGB( 70,130,200)
#define CUM_YELL  HUI_RGB(230,180, 60)
#define CUM_GREEN HUI_RGB(100,190, 80)
#define CUM_CYAN  HUI_RGB( 80,190,190)
#define CUM_PINK  HUI_RGB(220, 80,100)
#define CUM_ORANG HUI_RGB(220,130, 70)
#define CUM_PURP  HUI_RGB(140,110,220)

#endif /* CUM_THEME_DARK */

/* ---- Semantic aliases (theme-independent names) ---- */
#define CUM_ACCENT  CUM_BLUE
#define CUM_ACTIVE  CUM_YELL
#define CUM_OK      CUM_GREEN
#define CUM_WARN    CUM_ORANG
#define CUM_ERR     CUM_PINK
#define CUM_INFO    CUM_CYAN

#endif /* HUI_CUM_H */
