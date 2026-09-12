#ifndef UI_CHROME_H
#define UI_CHROME_H

#include <SDL.h>
#include "ui_theme.h"

/*
 * Geometry of the window chrome, shared by the renderer (view.c) and the input
 * handler (controller.c).
 *
 * Every clickable rectangle of the rail and the command bar is defined ONCE
 * here. Before this header the same numbers were written twice, so a visual
 * tweak silently moved the mouse targets away from what was drawn.
 */

/* --- left rail: one entry per tool panel, in the order they are stacked --- */
typedef enum {
    UI_TOOL_ASSIGN = 0,
    UI_TOOL_PEAKS,
    UI_TOOL_AVG,
    UI_TOOL_BROAD,
    UI_TOOL_DIP,
    UI_TOOL_CUT,
    UI_TOOL_FILTER,
    UI_TOOL_JUMP,
    UI_TOOL_SPECTRA,
    UI_TOOL_PREDFIT,
    UI_TOOL_COUNT
} UiTool;

#define UI_RAIL_BTN_W 32
#define UI_RAIL_BTN_H 30
#define UI_RAIL_STEP  33
#define UI_RAIL_SEP   13   /* extra space where a group separator is drawn */

/* A separator follows the measurement group and the prediction group. */
static inline int ui_rail_separator_after(int tool) {
    return (tool == UI_TOOL_AVG || tool == UI_TOOL_FILTER || tool == UI_TOOL_SPECTRA);
}

static inline SDL_Rect ui_rail_rect(int tool) {
    int y = UI_CONTENT_Y + 8;
    if (tool < 0 || tool >= UI_TOOL_COUNT) return (SDL_Rect){0, 0, 0, 0};
    for (int i = 0; i < tool; i++) {
        y += UI_RAIL_STEP;
        if (ui_rail_separator_after(i)) y += UI_RAIL_SEP;
    }
    return (SDL_Rect){(UI_RAIL_W - UI_RAIL_BTN_W) / 2, y, UI_RAIL_BTN_W, UI_RAIL_BTN_H};
}

/* --- command bar --- */
typedef enum {
    UI_TOP_BAR = 0,
    UI_TOP_MEASURE,
    UI_TOP_SYNC,
    UI_TOP_DELPEAK,
    UI_TOP_OFFSET,
    UI_TOP_CAT_TEMP,
    UI_TOP_ROT_TEMP,
    UI_TOP_SAVE,
    UI_TOP_EXPORT,
    UI_TOP_HELP,
    UI_TOP_SETTINGS,
    UI_TOP_COUNT
} UiTopItem;

#define UI_TOP_BTN_H 22
#define UI_TOP_BTN_Y (UI_TITLEBAR_H + (UI_TOPBAR_H - UI_TOP_BTN_H) / 2)

/* x of the group separator drawn between the toggles and Delete peak. */
#define UI_TOP_SEP_X 234

static inline SDL_Rect ui_top_rect(int item, int win_w) {
    const int y = UI_TOP_BTN_Y, h = UI_TOP_BTN_H;
    int help_w = 92, export_w = 104, offset_w = 150, temp_w = 132, cat_temp_w = 132;
    int save_w = 112;                     /* the only writer of the session */
    int set_w = 34;                       /* icon only: it is opened rarely */
    int set_x    = win_w - 10 - set_w;
    int help_x   = set_x - 6 - help_w;
    int export_x = help_x - 6 - export_w;
    int save_x   = export_x - 6 - save_w;
    int offset_x = save_x - 14 - offset_w;
    int temp_x   = offset_x - 6 - temp_w;
    int cat_temp_x = temp_x - 6 - cat_temp_w;
    switch (item) {
        case UI_TOP_BAR:     return (SDL_Rect){ 10, y,  58, h};
        case UI_TOP_MEASURE: return (SDL_Rect){ 72, y,  86, h};
        case UI_TOP_SYNC:    return (SDL_Rect){162, y,  64, h};
        case UI_TOP_DELPEAK: return (SDL_Rect){243, y, 104, h};
        case UI_TOP_OFFSET:  return (SDL_Rect){offset_x, y, offset_w, h};
        case UI_TOP_CAT_TEMP:return (SDL_Rect){cat_temp_x, y, cat_temp_w, h};
        case UI_TOP_ROT_TEMP:return (SDL_Rect){temp_x, y, temp_w, h};
        case UI_TOP_SAVE:    return (SDL_Rect){save_x,   y, save_w,   h};
        case UI_TOP_EXPORT:  return (SDL_Rect){export_x, y, export_w, h};
        case UI_TOP_HELP:    return (SDL_Rect){help_x,   y, help_w,   h};
        case UI_TOP_SETTINGS:return (SDL_Rect){set_x,    y, set_w,    h};
        default:             return (SDL_Rect){0, 0, 0, 0};
    }
}

/* The right-hand controls need room; below this width they are hidden and the
 * keyboard shortcuts remain the way to reach them. */
static inline int ui_top_right_visible(int win_w) { return win_w > 1060; }

/* --- line-art icons, drawn from polylines instead of a symbol font --- */
typedef enum {
    UI_ICON_BAR = 0, UI_ICON_MEASURE, UI_ICON_SYNC, UI_ICON_TRASH,
    UI_ICON_LIST, UI_ICON_PEAK, UI_ICON_WAVE, UI_ICON_BELL,
    UI_ICON_DIPOLE, UI_ICON_RANGE, UI_ICON_JUMP, UI_ICON_FILTER, UI_ICON_LAYERS,
    UI_ICON_EXPORT, UI_ICON_HELP, UI_ICON_CLOSE, UI_ICON_EYE, UI_ICON_GEAR,
    UI_ICON_COUNT
} UiIcon;

/* Draws `icon` centred in `box` at its natural 16x16 grid size. */
void ui_draw_icon(SDL_Renderer *ren, int icon, SDL_Rect box, SDL_Color c);

/* The rail tooltip / inspector title of each tool, and its keyboard shortcut. */
const char *ui_tool_title(int tool);
const char *ui_tool_key(int tool);
int         ui_tool_icon(int tool);

#endif
