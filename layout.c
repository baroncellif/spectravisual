#include "layout.h"
#include <math.h>
#include <stdio.h>

// --- COLORS (The one you liked + Minimalist accents) ---
const SDL_Color WIN_BG          = {24, 27, 32, 246};
const SDL_Color BORDER_GLOW     = {64, 190, 215, 255};
const SDL_Color BORDER_IDLE     = {70, 78, 88, 255};
const SDL_Color TXT_BRIGHT      = {232, 238, 245, 255};
const SDL_Color TXT_DIM         = {145, 153, 164, 255};

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

// --- DRAWING FUNCTIONS ---

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color) {
    if (!txt || !*txt) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, color);
    if (!surf) return;
    SDL_Texture *tex  = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

void draw_text_vertical(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color) {
    if (!txt || !*txt) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, color);
    if (!surf) return;
    SDL_Texture *tex  = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_Point center = {0, surf->h};
    SDL_RenderCopyEx(ren, tex, NULL, &dst, -90.0, &center, SDL_FLIP_NONE);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(tex);
}

// Smooth ease (smoothstep) used for panel slide-in / push animation.
double ui_smoothstep(double a) {
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a * a * (3.0 - 2.0 * a);
}

// Dock the tool panels to the right edge as a vertical stack of floating glass
// cards. Each visible window animates a 0..1 "open" value; the open ones are
// right-aligned and stacked top-to-bottom, reflowing as neighbours open/close,
// and the spectrum is given the remaining width via l->plot_right.
void update_sidebars(AppState *s, Layout *l) {
    const int TOP          = l->exp_y;   // align panel tops with the plot
    const int RIGHT_MARGIN = 18;
    const int GAP          = 12;
    const float SPEED      = 0.22f;      // per-frame approach toward target

    DraggableWindow *panels[] = {
        &s->win_as, &s->win_pf, &s->win_avg,
        &s->win_br, &s->win_cut, &s->win_jump
    };
    int N = (int)(sizeof(panels) / sizeof(panels[0]));

    int vp_bottom = l->win_h - 34;        // viewport ends above the info bar
    if (vp_bottom < TOP + 60) vp_bottom = TOP + 60;
    int view_h = vp_bottom - TOP;

    // Pass 1: advance animations, measure the stacked height and widest panel.
    double e_arr[8];
    double total = 0.0, max_push = 0.0;
    int any = 0;
    for (int i = 0; i < N; i++) {
        DraggableWindow *p = panels[i];

        double target = p->visible ? 1.0 : 0.0;
        p->anim += (float)((target - p->anim) * SPEED);
        if (target > 0.5 && p->anim > 0.999f) p->anim = 1.0f;
        if (target < 0.5 && p->anim < 0.001f) p->anim = 0.0f;

        double e = ui_smoothstep(p->anim);
        e_arr[i] = e;
        if (e < 0.01) continue;
        any = 1;
        total += (p->rect.h + GAP) * e;
        double push = p->rect.w * e;
        if (push > max_push) max_push = push;
    }
    if (any && total >= GAP) total -= GAP;    // drop the trailing gap

    // Clamp the scroll offset to the available range.
    double max_scroll = total - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (s->sidebar_scroll < 0) s->sidebar_scroll = 0;
    if (s->sidebar_scroll > max_scroll) s->sidebar_scroll = max_scroll;

    // Pass 2: place the cards, clipped to the scrollable viewport.
    double y = (double)TOP - s->sidebar_scroll;
    for (int i = 0; i < N; i++) {
        DraggableWindow *p = panels[i];
        double e = e_arr[i];
        if (e < 0.01) {                       // fully closed: park off-screen
            p->rect.x = l->win_w + 80;
            p->rect.y = TOP;
            p->clip = (SDL_Rect){0, 0, 0, 0};
            continue;
        }

        int w = p->rect.w, h = p->rect.h;
        p->rect.x = l->win_w - RIGHT_MARGIN - w;   // right-aligned, full width
        p->rect.y = (int)y;

        int vis_w = (int)(w * e);             // animated slice (reveals from right)
        int cy = (int)y, ch = h;
        if (cy < TOP)        { ch -= (TOP - cy); cy = TOP; }   // clip above viewport
        if (cy + ch > vp_bottom) ch = vp_bottom - cy;          // clip below viewport
        if (ch < 0) ch = 0;
        p->clip = (SDL_Rect){ l->win_w - RIGHT_MARGIN - vis_w, cy, vis_w, ch };

        y += (h + GAP) * e;                   // stack downward, animated
    }

    l->plot_right = any ? (l->win_w - RIGHT_MARGIN - (int)max_push - GAP)
                        : (l->win_w - 25);
}

// IMPROVED: Floating "glass" panel — translucent body, rounded corners, soft
// shadow and a subtle top highlight. Opacity follows win->anim so it fades while
// it slides in. Geometry of the header / close button is unchanged so the
// existing hit-testing in controller.c still lines up.
void draw_draggable_window(SDL_Renderer *ren, TTF_Font *font, DraggableWindow *win) {
    double a = ui_smoothstep(win->anim);
    if (a <= 0.01) return;

    const int RAD = 14;
    Uint8 A = (Uint8)(a * 255.0);

    // Soft drop shadow.
    SDL_Rect shadow = {win->rect.x + 6, win->rect.y + 8, win->rect.w, win->rect.h};
    fill_rounded_rect(ren, shadow, RAD, (SDL_Color){0, 0, 0, (Uint8)(70 * a)});

    // Translucent glass body.
    fill_rounded_rect(ren, win->rect, RAD, (SDL_Color){26, 30, 38, (Uint8)(208 * a)});

    // Top highlight band (fake light catching the glass edge).
    SDL_Rect hi = {win->rect.x + RAD, win->rect.y + 1, win->rect.w - 2 * RAD, 1};
    SDL_SetRenderDrawColor(ren, 255, 255, 255, (Uint8)(28 * a));
    SDL_RenderFillRect(ren, &hi);

    // Outer border.
    SDL_SetRenderDrawColor(ren, BORDER_IDLE.r, BORDER_IDLE.g, BORDER_IDLE.b, (Uint8)(150 * a));
    SDL_RenderDrawRect(ren, &win->rect);

    // Header strip + title.
    SDL_Rect header = {win->rect.x, win->rect.y, win->rect.w, 34};
    fill_rounded_rect(ren, header, RAD, (SDL_Color){34, 39, 49, (Uint8)(220 * a)});

    SDL_Color title_c = TXT_BRIGHT; title_c.a = A;
    draw_text(ren, font, win->title, win->rect.x + 16, win->rect.y + 8, title_c);

    SDL_SetRenderDrawColor(ren, BORDER_GLOW.r, BORDER_GLOW.g, BORDER_GLOW.b, (Uint8)(180 * a));
    SDL_RenderDrawLine(ren, win->rect.x + 14, win->rect.y + 33, win->rect.x + win->rect.w - 14, win->rect.y + 33);

    // Close button (circular).
    int cx = win->rect.x + win->rect.w - 20;
    int cy = win->rect.y + 15;
    int r = 10;

    int mx, my; SDL_GetMouseState(&mx, &my);
    int distSq = (mx - cx) * (mx - cx) + (my - cy) * (my - cy);
    int hover = (distSq <= r * r);

    SDL_Rect close_r = {cx - r + 1, cy - r + 1, r * 2 - 2, r * 2 - 2};
    SDL_Color c_col = hover ? (SDL_Color){210, 72, 76, A} : (SDL_Color){52, 58, 68, A};
    fill_rounded_rect(ren, close_r, 8, c_col);

    SDL_Color x_c = TXT_BRIGHT; x_c.a = A;
    draw_text(ren, font, "x", cx - 3, cy - 8, x_c);
}

// Lazily-loaded icon font. The default UI font (Helvetica) lacks arrow/symbol
// glyphs, so toolbar icons are drawn with Arial Unicode, which provides them.
static TTF_Font *ui_icon_font(void) {
    static TTF_Font *f = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        f = TTF_OpenFont("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 13);
    }
    return f;
}

// Flat, crisp button matching spectravisual_redesign.html.
// Each visual state defines: a fill colour, a 1px border colour, and a text
// colour. BTN_NORMAL is transparent when idle (just dim text) and only grows a
// subtle filled background + border on hover. Active toggles and the
// danger/primary styles use coloured fills with matching borders and text.
void draw_button(SDL_Renderer *ren, TTF_Font *font, Button *btn, int mx, int my, int m_down, int active_state) {

    int hover = point_in_rect(mx, my, btn->rect);
    int pressed = (hover && m_down);

    SDL_Color bg, border, text_col;
    int has_bg = 1, has_border = 1;

    if (btn->is_toggle && active_state) {
        // Active toggle -> cyan accent (.btn-active)
        bg       = (SDL_Color){14, 58, 71, 255};
        border   = (SDL_Color){26, 85, 102, 255};
        text_col = (SDL_Color){64, 190, 215, 255};
        if (hover) bg = (SDL_Color){14, 64, 80, 255};
    } else if (btn->style == BTN_DANGER) {
        // Destructive action -> red accent (.btn-danger)
        bg       = (SDL_Color){61, 26, 26, 255};
        border   = (SDL_Color){90, 37, 37, 255};
        text_col = (SDL_Color){248, 113, 113, 255};
        if (hover) bg = (SDL_Color){74, 32, 32, 255};
    } else if (btn->style == BTN_PRIMARY) {
        // Primary action -> cyan accent (.btn-primary)
        bg       = (SDL_Color){26, 58, 74, 255};
        border   = (SDL_Color){37, 85, 101, 255};
        text_col = (SDL_Color){64, 190, 215, 255};
        if (hover) bg = (SDL_Color){30, 69, 85, 255};
    } else {
        // Normal: transparent idle, subtle filled hover (.btn / .btn:hover)
        if (hover) {
            bg       = (SDL_Color){30, 34, 48, 255};
            border   = (SDL_Color){42, 48, 64, 255};
            text_col = (SDL_Color){226, 232, 240, 255};
        } else {
            has_bg = 0;
            has_border = 0;
            text_col = (SDL_Color){156, 163, 175, 255};
        }
    }

    // Pressed: darken the fill slightly for tactile feedback.
    if (pressed && has_bg) {
        bg.r = (Uint8)(bg.r * 0.82);
        bg.g = (Uint8)(bg.g * 0.82);
        bg.b = (Uint8)(bg.b * 0.82);
    }

    // Draw the 1px rounded border by stacking the fill on a slightly larger
    // border-coloured rounded rect.
    if (has_border) {
        fill_rounded_rect(ren, btn->rect, 5, border);
        SDL_Rect inner = {btn->rect.x + 1, btn->rect.y + 1, btn->rect.w - 2, btn->rect.h - 2};
        if (has_bg) fill_rounded_rect(ren, inner, 4, bg);
    } else if (has_bg) {
        fill_rounded_rect(ren, btn->rect, 5, bg);
    }

    // Draw icon (icon font) + label (UI font) centered as one group.
    TTF_Font *ifont = (btn->icon[0]) ? ui_icon_font() : NULL;
    int iw = 0, ih = 0, lw = 0, lh = 0;
    if (ifont) TTF_SizeUTF8(ifont, btn->icon, &iw, &ih);
    if (font && btn->label[0]) TTF_SizeUTF8(font, btn->label, &lw, &lh);

    int gap = (iw && lw) ? 5 : 0;
    int total = iw + gap + lw;
    int sx = btn->rect.x + (btn->rect.w - total) / 2;
    int cy = btn->rect.y + btn->rect.h / 2;
    int dy = pressed ? 1 : 0;
    if (pressed) sx += 1;

    if (ifont && iw) {
        draw_text(ren, ifont, btn->icon, sx, cy - ih / 2 + dy, text_col);
        sx += iw + gap;
    }
    if (font && btn->label[0]) {
        draw_text(ren, font, btn->label, sx, cy - lh / 2 + dy, text_col);
    }
}
