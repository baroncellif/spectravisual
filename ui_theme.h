#ifndef UI_THEME_H
#define UI_THEME_H

#include <SDL.h>

/*
 * Visual language of the workbench UI (see docs/ui-workbench.md).
 *
 * Two rules make the palette readable at a glance:
 *   - the chrome is neutral graphite, so the spectrum is the only saturated
 *     thing on screen;
 *   - blue means focus or selection, amber means a measurement, green means an
 *     assignment. No colour is used decoratively.
 *
 * The predicted-line colour code (color_for_pred) is deliberately NOT part of
 * this palette: it is the official P/Q/R x mu code and must not drift.
 */

/* --- surfaces --- */
static const SDL_Color UI_GROUND      = { 11,  12,  13, 255};  /* window ground        */
static const SDL_Color UI_TITLEBAR    = { 30,  32,  35, 255};  /* macOS-style top bar  */
static const SDL_Color UI_CHROME      = { 26,  27,  30, 255};  /* command bar, rail    */
static const SDL_Color UI_PANEL       = { 19,  20,  22, 255};  /* inspector body       */
static const SDL_Color UI_PLOT        = { 17,  18,  20, 255};  /* plot ground          */
static const SDL_Color UI_RAISED      = { 32,  34,  38, 255};  /* hover / raised ctrl  */
static const SDL_Color UI_RAISED_HI   = { 38,  40,  45, 255};  /* pressed / selected   */
static const SDL_Color UI_INPUT       = { 13,  14,  16, 255};  /* field ground         */

/* --- hairlines --- */
static const SDL_Color UI_LINE        = { 42,  44,  49, 255};
static const SDL_Color UI_LINE_SOFT   = { 31,  33,  38, 255};

/* --- type --- */
static const SDL_Color UI_TEXT        = {231, 232, 234, 255};
static const SDL_Color UI_DIM         = {147, 149, 156, 255};
static const SDL_Color UI_FAINT       = { 99, 102, 109, 255};

/* --- meaning --- */
static const SDL_Color UI_ACCENT      = { 76, 154, 255, 255};  /* focus, selection     */
static const SDL_Color UI_ACCENT_SOFT = { 30,  47,  74, 255};  /* accent fill (opaque) */
static const SDL_Color UI_ACCENT_LINE = { 47,  85, 140, 255};
static const SDL_Color UI_ACCENT_TEXT = {156, 198, 255, 255};
static const SDL_Color UI_OK          = { 70, 196, 138, 255};  /* assigned             */
static const SDL_Color UI_WARN        = {227, 179,  65, 255};  /* peaks, bar, measure  */
static const SDL_Color UI_DANGER      = {229,  83,  75, 255};  /* destructive          */
static const SDL_Color UI_DANGER_BG   = { 34,  23,  24, 255};
static const SDL_Color UI_DANGER_TEXT = {240, 138, 131, 255};

/* --- plot marks --- */
static const SDL_Color UI_GRID        = { 30,  32,  36, 255};
static const SDL_Color UI_AXIS        = { 58,  61,  68, 255};
static const SDL_Color UI_TRACE       = {201, 207, 214, 255};

/* --- metrics (logical pixels) --- */
#define UI_TITLEBAR_H     30
#define UI_TOPBAR_H       34
#define UI_CONTENT_Y      (UI_TITLEBAR_H + UI_TOPBAR_H)
#define UI_RAIL_W         46
#define UI_INSPECTOR_W    302
#define UI_STATUS_H       26
#define UI_PANEL_HEADER_H 26
#define UI_SECTION_HEAD_H 28
#define UI_FIELD_H        24
#define UI_RADIUS         4
#define UI_PLOT_GUTTER    62   /* left margin inside a plot pane for Y labels */
#define UI_PRED_AXIS_H    22   /* shared frequency axis under the prediction  */

#endif
