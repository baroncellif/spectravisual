#ifndef LAYOUT_H
#define LAYOUT_H

#include <SDL.h>
#include <SDL_ttf.h>
#include "types.h"

// --- UTILS ---
double nice_tick(double range);
int point_in_rect(int mx, int my, SDL_Rect r);
SDL_Color color_for_pred(char branch, char mu);

/* The one layout calculation used by both the primary window and disposable
 * viewer windows.  Keeping it here prevents a preview from quietly becoming a
 * different plot just because its SDL window has another size. */
void app_compute_layout(AppState *state, Layout *layout, int width, int height);

// --- TYPE ---
// Labels and controls are set in the sans face, every number in the mono face,
// so columns of digits line up and a value never changes width as it updates.
typedef enum {
    UI_FONT_SANS = 0,
    UI_FONT_SANS_SM,
    UI_FONT_TITLE,
    UI_FONT_MONO,
    UI_FONT_MONO_SM,
    UI_FONT_COUNT
} UiFontRole;

// Opens the faces at size * scale, where scale is the backing-store ratio of
// the window (2.0 on a Retina display). Returns 0 if no face could be opened.
int   ui_fonts_init(float scale);
void  ui_fonts_close(void);
TTF_Font *ui_font(int role);
float ui_scale(void);
int   ui_text_w(int role, const char *txt);
int   ui_text_h(int role);

// --- TEXT ---
void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color);
void draw_text_vertical(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color);
void ui_text(SDL_Renderer *ren, int role, const char *txt, int x, int y, SDL_Color c);
void ui_text_right(SDL_Renderer *ren, int role, const char *txt, int right_x, int y, SDL_Color c);
void ui_text_v(SDL_Renderer *ren, int role, const char *txt, int x, SDL_Rect box, SDL_Color c);

// --- PRIMITIVES ---
void fill_rounded_rect(SDL_Renderer *ren, SDL_Rect dst, int radius, SDL_Color c);

// Switch the renderer to device pixels for one shape, so anti-aliasing and
// hairlines are computed on the real pixel grid instead of being stretched.
void ui_dev_begin(SDL_Renderer *ren);
void ui_dev_end(SDL_Renderer *ren);
int  ui_dev(int logical_px);
void ui_fill(SDL_Renderer *ren, SDL_Rect r, SDL_Color c);
void ui_frame(SDL_Renderer *ren, SDL_Rect r, SDL_Color c);
void ui_hline(SDL_Renderer *ren, int x0, int x1, int y, SDL_Color c);
void ui_thick_line(SDL_Renderer *ren, int x1, int y1, int x2, int y2, int width);

// Anti-aliased plot geometry. Coordinates and width are in logical pixels; the
// line itself is built on the device pixel grid, so it keeps the display's full
// resolution instead of being a doubled one-pixel line.
void ui_plot_polyline(SDL_Renderer *ren, const SDL_FPoint *pts, int n, float width, SDL_Color c);
void ui_plot_segments(SDL_Renderer *ren, const SDL_FPoint *pts, int n_points, float width, SDL_Color c);
// The dense min/max envelope, drawn crisp on the pixel grid.
void ui_plot_columns(SDL_Renderer *ren, const SDL_FPoint *pts, int n_points, float width, SDL_Color c);
void ui_vline(SDL_Renderer *ren, int x, int y0, int y1, SDL_Color c);

// --- CONTROLS ---
// Visual roles share the numbering of ButtonStyle in types.h.
#define UI_BTN_QUIET   BTN_NORMAL
#define UI_BTN_DANGER  BTN_DANGER
#define UI_BTN_PRIMARY BTN_PRIMARY
#define UI_BTN_DANGER_QUIET 3

int  ui_button(SDL_Renderer *ren, SDL_Rect r, const char *label, int icon, int kind,
               int active, int mx, int my, int mdown);
void ui_field(SDL_Renderer *ren, SDL_Rect r, const char *label, const char *value, int focused);
void ui_field_u(SDL_Renderer *ren, SDL_Rect r, const char *value, const char *unit, int focused);
void ui_field_ex(SDL_Renderer *ren, SDL_Rect r, const char *label, const char *value,
                 const char *unit, int focused, int caret, int anchor);
int  ui_field_caret_at(SDL_Rect r, const char *value, const char *unit, int mx);
void ui_switch(SDL_Renderer *ren, SDL_Rect r, int on);
int  ui_toggle_row(SDL_Renderer *ren, SDL_Rect r, const char *label, int on, int mx, int my);
void ui_segmented(SDL_Renderer *ren, SDL_Rect r, const char *const *labels, int n, int sel);
void draw_button(SDL_Renderer *ren, TTF_Font *font, Button *btn, int mouse_x, int mouse_y, int mouse_down, int active_state);

double ui_smoothstep(double a);

// --- INSPECTOR ---
// Stacks the open tool panels in the fixed right-hand column, advances their
// open/close animation and fills in l->plot_right so the plot can take the rest
// of the width. Call once per frame BEFORE event handling, so hit-testing uses
// the same rectangles that were drawn.
void update_sidebars(AppState *s, Layout *l);
void draw_inspector_section(SDL_Renderer *ren, DraggableWindow *win, int icon, int mx, int my);
void draw_draggable_window(SDL_Renderer *ren, TTF_Font *font, DraggableWindow *win);

// --- PREDICTION FILTER ---
// Returns 1 if predicted line `idx` should be shown/selectable, applying the
// intensity cut plus (when filter_active) the dipole/branch and quantum-number
// range / delta gates.
int pred_passes_filter(const AppState *s, int idx);

#endif
