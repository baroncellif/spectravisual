#include "layout.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include <string.h>
#include <math.h>
#include <stdio.h>


// --- UTILS ---

double nice_tick(double range) {
    if (range <= 0) return 1.0;
    double expv = floor(log10(range)), base = pow(10,expv), frac = range/base;
    if (frac < 2) return base*0.2;
    if (frac < 5) return base*0.5;
    return base*1.0;
}

int point_in_rect(int mx, int my, SDL_Rect r) {
    return (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h);
}

SDL_Color color_for_pred(char branch, char mu) {
    // Standard prediction colors
    if (branch=='R' && mu=='a') return (SDL_Color){255, 80, 80, 255};
    if (branch=='R' && mu=='b') return (SDL_Color){ 80,255, 80, 255};
    if (branch=='R' && mu=='c') return (SDL_Color){ 80, 80,255, 255};
    if (branch=='Q' && mu=='a') return (SDL_Color){255,150, 50, 255};
    if (branch=='Q' && mu=='b') return (SDL_Color){ 50,255,200, 255};
    if (branch=='Q' && mu=='c') return (SDL_Color){200, 50,255, 255};
    if (branch=='P' && mu=='a') return (SDL_Color){200, 40, 40, 255};
    if (branch=='P' && mu=='b') return (SDL_Color){ 40,200, 40, 255};
    if (branch=='P' && mu=='c') return (SDL_Color){ 40, 40,200, 255};
    return (SDL_Color){180,180,180,255};
}

// Returns 1 if predicted line `idx` passes the active filters. The intensity
// cut always applies; the dipole/branch and quantum-number gates only apply
// when filter_active is set.
int pred_passes_filter(const AppState *s, int idx) {
    const PredLine *p = &s->pred_lines[idx];

    // Intensity cut (always on).
    if (p->lgint < s->pred_min_log_int) return 0;
    if (p->lgint > s->pred_max_log_int) return 0;

    if (!s->filter_active) return 1;

    // Dipole type (mu = a/b/c).
    int mui = (p->mu == 'a') ? 0 : (p->mu == 'b') ? 1 : (p->mu == 'c') ? 2 : -1;
    if (mui >= 0 && !s->filt_mu[mui]) return 0;

    // Branch (P/Q/R).
    int bri = (p->branch == 'P') ? 0 : (p->branch == 'Q') ? 1 : (p->branch == 'R') ? 2 : -1;
    if (bri >= 0 && !s->filt_br[bri]) return 0;

    // Upper-state quantum-number ranges.
    if (s->filt_use_range) {
        if (p->Ju  < s->filt_j_min  || p->Ju  > s->filt_j_max)  return 0;
        if (p->Kau < s->filt_ka_min || p->Kau > s->filt_ka_max) return 0;
        if (p->Kcu < s->filt_kc_min || p->Kcu > s->filt_kc_max) return 0;
    }

    // Quantum-number jumps (upper - lower).
    if (s->filt_use_delta) {
        if ((p->Ju  - p->Jl)  != s->filt_dj)  return 0;
        if ((p->Kau - p->Kal) != s->filt_dka) return 0;
        if ((p->Kcu - p->Kcl) != s->filt_dkc) return 0;
    }

    return 1;
}

// --- HELPER: Anti-Aliased Rounded Rect ---
// This makes the corners look smooth, not jagged.
void fill_rounded_rect(SDL_Renderer *ren, SDL_Rect dst, int radius, SDL_Color c) {
    // 1. Draw Body
    SDL_Rect r_mid = {dst.x, dst.y + radius, dst.w, dst.h - 2 * radius};
    SDL_Rect r_top = {dst.x + radius, dst.y, dst.w - 2 * radius, radius};
    SDL_Rect r_bot = {dst.x + radius, dst.y + dst.h - radius, dst.w - 2 * radius, radius};

    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren, &r_mid);
    SDL_RenderFillRect(ren, &r_top);
    SDL_RenderFillRect(ren, &r_bot);

    // 2. Draw Smooth Corners
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    
    // Safety check for radius
    if (radius < 1) return;

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            double dist = sqrt((dx + 0.5) * (dx + 0.5) + (dy + 0.5) * (dy + 0.5));
            double alpha = 1.0; 

            if (dist > radius) alpha = 0.0;
            else if (dist > radius - 1.0) alpha = 1.0 - (dist - (radius - 1.0));
            
            if (alpha > 0.0) {
                SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, (Uint8)(c.a * alpha));
                // TL, TR, BL, BR
                SDL_RenderDrawPoint(ren, dst.x + radius - 1 - dx, dst.y + radius - 1 - dy);
                SDL_RenderDrawPoint(ren, dst.x + dst.w - radius + dx, dst.y + radius - 1 - dy);
                SDL_RenderDrawPoint(ren, dst.x + radius - 1 - dx, dst.y + dst.h - radius + dy);
                SDL_RenderDrawPoint(ren, dst.x + dst.w - radius + dx, dst.y + dst.h - radius + dy);
            }
        }
    }
}


// ============================================================================
//  FONTS
//  One sans face for labels and controls, one mono face for every number, in
//  four sizes. Faces are opened at size * backing_scale and drawn back at
//  logical size, so text is rendered at the display's real pixel density
//  instead of being scaled up from a 1x bitmap.
// ============================================================================
static TTF_Font *g_fonts[UI_FONT_COUNT];
static float     g_scale = 1.0f;

static const int UI_FONT_SIZE[UI_FONT_COUNT] = {
    12,   /* UI_FONT_SANS    labels, buttons        */
    11,   /* UI_FONT_SANS_SM captions, chips        */
    13,   /* UI_FONT_TITLE   window + section title */
    12,   /* UI_FONT_MONO    values, tables         */
    10    /* UI_FONT_MONO_SM axis ticks             */
};

static TTF_Font *open_first(const char *const *paths, int n, int px) {
    for (int i = 0; i < n; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], px);
        if (f) return f;
    }
    return NULL;
}

int ui_fonts_init(float scale) {
    static const char *sans[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    };
    static const char *mono[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    };
    g_scale = (scale > 0.1f) ? scale : 1.0f;
    for (int r = 0; r < UI_FONT_COUNT; r++) {
        int px = (int)lround(UI_FONT_SIZE[r] * g_scale);
        int is_mono = (r == UI_FONT_MONO || r == UI_FONT_MONO_SM);
        g_fonts[r] = is_mono ? open_first(mono, 3, px) : open_first(sans, 4, px);
        if (!g_fonts[r]) g_fonts[r] = open_first(sans, 4, px);
        if (!g_fonts[r]) return 0;
    }
    return 1;
}

void ui_fonts_close(void) {
    for (int r = 0; r < UI_FONT_COUNT; r++) {
        if (g_fonts[r]) { TTF_CloseFont(g_fonts[r]); g_fonts[r] = NULL; }
    }
}

TTF_Font *ui_font(int role) {
    if (role < 0 || role >= UI_FONT_COUNT) role = UI_FONT_SANS;
    return g_fonts[role];
}

float ui_scale(void) { return g_scale; }

// Width of `txt` in logical pixels (the font is oversized by g_scale).
int ui_text_w(int role, const char *txt) {
    TTF_Font *f = ui_font(role);
    if (!f || !txt || !*txt) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, txt, &w, &h);
    return (int)lround(w / (double)g_scale);
}

int ui_text_h(int role) {
    TTF_Font *f = ui_font(role);
    if (!f) return 0;
    return (int)lround(TTF_FontHeight(f) / (double)g_scale);
}

// --- DRAWING FUNCTIONS ---

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color) {
    if (!txt || !*txt) return;
    if (!font) font = ui_font(UI_FONT_SANS);
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, color);
    if (!surf) return;
    SDL_Texture *tex  = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = {x, y,
                    (int)lround(surf->w / (double)g_scale),
                    (int)lround(surf->h / (double)g_scale)};
    SDL_FreeSurface(surf);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

// Same as draw_text but picks the face by role, which is how the UI code should
// ask for type: numbers always mono, labels always sans.
void ui_text(SDL_Renderer *ren, int role, const char *txt, int x, int y, SDL_Color c) {
    draw_text(ren, ui_font(role), txt, x, y, c);
}

// Draws text right-aligned on `right_x`. Numeric readouts line up on their last
// digit, so a changing value does not make the whole row twitch.
void ui_text_right(SDL_Renderer *ren, int role, const char *txt, int right_x, int y, SDL_Color c) {
    draw_text(ren, ui_font(role), txt, right_x - ui_text_w(role, txt), y, c);
}

// Vertically centres one line of text in `box`.
void ui_text_v(SDL_Renderer *ren, int role, const char *txt, int x, SDL_Rect box, SDL_Color c) {
    int h = ui_text_h(role);
    draw_text(ren, ui_font(role), txt, x, box.y + (box.h - h) / 2, c);
}

void draw_text_vertical(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color) {
    if (!txt || !*txt) return;
    if (!font) font = ui_font(UI_FONT_SANS);
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, color);
    if (!surf) return;
    SDL_Texture *tex  = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = {x, y,
                    (int)lround(surf->w / (double)g_scale),
                    (int)lround(surf->h / (double)g_scale)};
    SDL_Point center = {0, dst.h};
    SDL_RenderCopyEx(ren, tex, NULL, &dst, -90.0, &center, SDL_FLIP_NONE);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(tex);
}

// --- PRIMITIVES ---

void ui_fill(SDL_Renderer *ren, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren, &r);
}

void ui_frame(SDL_Renderer *ren, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(ren, &r);
}

void ui_hline(SDL_Renderer *ren, int x0, int x1, int y, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(ren, x0, y, x1, y);
}

void ui_vline(SDL_Renderer *ren, int x, int y0, int y1, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(ren, x, y0, x, y1);
}

// 1px border drawn as a rounded outline with the fill inset by one pixel.
static void filled_border(SDL_Renderer *ren, SDL_Rect r, int radius, SDL_Color border, SDL_Color fill, int has_fill) {
    fill_rounded_rect(ren, r, radius, border);
    if (has_fill) {
        SDL_Rect in = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
        fill_rounded_rect(ren, in, radius > 0 ? radius - 1 : 0, fill);
    }
}

// --- CONTROLS ---

// A control button. `kind` picks the visual role, `active` lights a toggle.
// Returns 1 when the pointer is over it, so callers can show a tooltip.
int ui_button(SDL_Renderer *ren, SDL_Rect r, const char *label, int icon, int kind,
              int active, int mx, int my, int mdown) {
    int hover = point_in_rect(mx, my, r);
    int pressed = hover && mdown;
    SDL_Color fill = UI_RAISED, border = UI_LINE, text = UI_DIM;
    int has_fill = 1, has_border = 1;

    if (active) {
        fill = UI_ACCENT_SOFT; border = UI_ACCENT_LINE; text = UI_ACCENT_TEXT;
    } else if (kind == UI_BTN_DANGER) {
        has_fill = hover; has_border = hover;
        fill = UI_DANGER_BG; border = (SDL_Color){58, 37, 35, 255}; text = hover ? UI_DANGER_TEXT : UI_DIM;
    } else if (kind == UI_BTN_PRIMARY) {
        fill = (SDL_Color){29, 62, 99, 255}; border = (SDL_Color){44, 90, 140, 255}; text = (SDL_Color){187, 216, 255, 255};
    } else {                       /* quiet: only a hover surface */
        has_fill = hover; has_border = hover;
        text = hover ? UI_TEXT : UI_DIM;
    }
    if (pressed && has_fill) {
        fill.r = (Uint8)(fill.r * 0.85); fill.g = (Uint8)(fill.g * 0.85); fill.b = (Uint8)(fill.b * 0.85);
    }

    if (has_border) filled_border(ren, r, UI_RADIUS, border, fill, has_fill);
    else if (has_fill) fill_rounded_rect(ren, r, UI_RADIUS, fill);

    int iw = (icon >= 0) ? 16 : 0;
    int lw = (label && *label) ? ui_text_w(UI_FONT_SANS, label) : 0;
    int gap = (iw && lw) ? 6 : 0;
    int x = r.x + (r.w - (iw + gap + lw)) / 2;
    if (icon >= 0) {
        ui_draw_icon(ren, icon, (SDL_Rect){x, r.y + (r.h - 16) / 2, 16, 16}, text);
        x += iw + gap;
    }
    if (lw) ui_text_v(ren, UI_FONT_SANS, label, x, r, text);
    return hover;
}

// Numeric input field. Shows the live edit buffer with a caret when focused.
void ui_field(SDL_Renderer *ren, SDL_Rect r, const char *label, const char *value, int focused) {
    filled_border(ren, r, UI_RADIUS, focused ? UI_ACCENT_LINE : UI_LINE, UI_INPUT, 1);
    int x = r.x + 8;
    if (label && *label) {
        ui_text_v(ren, UI_FONT_SANS_SM, label, x, r, UI_FAINT);
        x += ui_text_w(UI_FONT_SANS_SM, label) + 8;
        ui_vline(ren, x - 4, r.y + 1, r.y + r.h - 2, UI_LINE);
    }
    /* the value is right-aligned so digits stay in place while typing */
    int h = ui_text_h(UI_FONT_MONO);
    ui_text_right(ren, UI_FONT_MONO, value, r.x + r.w - 8, r.y + (r.h - h) / 2,
                  focused ? UI_TEXT : UI_TEXT);
}

// Small on/off switch, used for the "enabled" state of a tool.
void ui_switch(SDL_Renderer *ren, SDL_Rect r, int on) {
    SDL_Rect track = {r.x, r.y + (r.h - 16) / 2, 28, 16};
    fill_rounded_rect(ren, track, 8, on ? (SDL_Color){36, 62, 96, 255} : (SDL_Color){43, 45, 51, 255});
    SDL_Rect knob = {track.x + (on ? 14 : 2), track.y + 2, 12, 12};
    fill_rounded_rect(ren, knob, 6, on ? UI_ACCENT : (SDL_Color){138, 141, 148, 255});
}

// A row that reads "label            [switch]" and toggles the whole row.
int ui_toggle_row(SDL_Renderer *ren, SDL_Rect r, const char *label, int on, int mx, int my) {
    int hover = point_in_rect(mx, my, r);
    if (hover) fill_rounded_rect(ren, r, UI_RADIUS, UI_RAISED);
    ui_text_v(ren, UI_FONT_SANS, label, r.x + 2, r, on ? UI_TEXT : UI_DIM);
    ui_switch(ren, (SDL_Rect){r.x + r.w - 30, r.y, 28, r.h}, on);
    return hover;
}

// Segmented control: `n` options sharing one track, `sel` is highlighted.
void ui_segmented(SDL_Renderer *ren, SDL_Rect r, const char *const *labels, int n, int sel) {
    filled_border(ren, r, UI_RADIUS, UI_LINE, UI_INPUT, 1);
    int seg_w = (r.w - 4) / (n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        SDL_Rect s = {r.x + 2 + i * seg_w, r.y + 2, seg_w, r.h - 4};
        if (i == sel) fill_rounded_rect(ren, s, 2, UI_RAISED_HI);
        int w = ui_text_w(UI_FONT_SANS, labels[i]);
        ui_text_v(ren, UI_FONT_SANS, labels[i], s.x + (s.w - w) / 2, s, i == sel ? UI_TEXT : UI_DIM);
    }
}

// Legacy Button wrapper kept so existing panel code reads the same.
void draw_button(SDL_Renderer *ren, TTF_Font *font, Button *btn, int mx, int my, int m_down, int active_state) {
    (void)font;
    ui_button(ren, btn->rect, btn->label, btn->icon_id, btn->style,
              btn->is_toggle ? active_state : 0, mx, my, m_down);
}

// Smooth ease (smoothstep) used for the inspector slide-in.
double ui_smoothstep(double a) {
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a * a * (3.0 - 2.0 * a);
}

// ============================================================================
//  INSPECTOR
//  The tool windows are no longer free-floating: the open ones are stacked in a
//  fixed column on the right, in rail order, and the plot gets the rest of the
//  width. Each panel keeps its own internal layout, so the hit-testing in
//  controller.c still lines up with what is drawn.
// ============================================================================
void update_sidebars(AppState *s, Layout *l) {
    DraggableWindow *panels[UI_TOOL_COUNT] = {
        &s->win_as, &s->win_pf, &s->win_avg, &s->win_br,
        &s->win_cut, &s->win_filt, &s->win_jump, &s->win_spec
    };
    const int TOP = UI_CONTENT_Y;
    const float SPEED = 0.26f;

    int vp_bottom = l->win_h - UI_STATUS_H;
    if (vp_bottom < TOP + 60) vp_bottom = TOP + 60;

    int any = 0;
    double total = 0.0;
    double e_arr[UI_TOOL_COUNT];
    for (int i = 0; i < UI_TOOL_COUNT; i++) {
        DraggableWindow *p = panels[i];
        double target = p->visible ? 1.0 : 0.0;
        p->anim += (float)((target - p->anim) * SPEED);
        if (target > 0.5 && p->anim > 0.999f) p->anim = 1.0f;
        if (target < 0.5 && p->anim < 0.001f) p->anim = 0.0f;
        e_arr[i] = ui_smoothstep(p->anim);
        if (e_arr[i] > 0.01) { any = 1; total += p->rect.h * e_arr[i]; }
    }

    int view_h = vp_bottom - TOP;
    double max_scroll = total - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (s->sidebar_scroll < 0) s->sidebar_scroll = 0;
    if (s->sidebar_scroll > max_scroll) s->sidebar_scroll = max_scroll;

    int col_x = l->win_w - UI_INSPECTOR_W;
    double y = (double)TOP - s->sidebar_scroll;

    for (int i = 0; i < UI_TOOL_COUNT; i++) {
        DraggableWindow *p = panels[i];
        double e = e_arr[i];
        p->rect.w = UI_INSPECTOR_W;
        if (e < 0.01) {                      /* closed: park off-screen */
            p->rect.x = l->win_w + 80;
            p->rect.y = TOP;
            p->clip = (SDL_Rect){0, 0, 0, 0};
            continue;
        }
        p->rect.x = col_x;
        p->rect.y = (int)y;

        int cy = (int)y, ch = p->rect.h;
        if (cy < TOP) { ch -= (TOP - cy); cy = TOP; }
        if (cy + ch > vp_bottom) ch = vp_bottom - cy;
        if (ch < 0) ch = 0;
        /* slide in from the right while opening */
        int vis_w = (int)(UI_INSPECTOR_W * e);
        p->clip = (SDL_Rect){l->win_w - vis_w, cy, vis_w, ch};

        y += p->rect.h * e;
    }

    l->inspector_open = any;
    l->plot_right = any ? col_x : l->win_w;
}

// Header of one inspector section: icon, title, close button, hairline.
void draw_inspector_section(SDL_Renderer *ren, DraggableWindow *win, int icon, int mx, int my) {
    double a = ui_smoothstep(win->anim);
    if (a <= 0.01) return;

    SDL_Rect body = win->rect;
    ui_fill(ren, body, UI_PANEL);

    SDL_Rect head = {body.x, body.y, body.w, UI_SECTION_HEAD_H};
    ui_fill(ren, head, UI_CHROME);
    ui_hline(ren, head.x, head.x + head.w, head.y + head.h - 1, UI_LINE_SOFT);
    ui_hline(ren, body.x, body.x + body.w, body.y + body.h - 1, UI_LINE);
    ui_vline(ren, body.x, body.y, body.y + body.h, UI_LINE);

    ui_draw_icon(ren, icon, (SDL_Rect){head.x + 10, head.y + (head.h - 16) / 2, 16, 16}, UI_FAINT);
    ui_text_v(ren, UI_FONT_TITLE, win->title, head.x + 32, head, UI_TEXT);

    SDL_Rect close = {head.x + head.w - 26, head.y + (head.h - 18) / 2, 18, 18};
    int hover = point_in_rect(mx, my, close);
    if (hover) fill_rounded_rect(ren, close, 3, UI_RAISED);
    ui_draw_icon(ren, UI_ICON_CLOSE, (SDL_Rect){close.x + 3, close.y + 3, 12, 12},
                 hover ? UI_TEXT : UI_FAINT);
}

// Kept for source compatibility: the panels are drawn by the call above.
void draw_draggable_window(SDL_Renderer *ren, TTF_Font *font, DraggableWindow *win) {
    (void)font;
    int mx, my; SDL_GetMouseState(&mx, &my);
    draw_inspector_section(ren, win, UI_ICON_LIST, mx, my);
}
