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

// IMPROVED: Smooth Rounded Window
void draw_draggable_window(SDL_Renderer *ren, TTF_Font *font, DraggableWindow *win) {
    if(!win->visible) return;
    
    SDL_Rect shadow = {win->rect.x + 5, win->rect.y + 6, win->rect.w, win->rect.h};
    fill_rounded_rect(ren, shadow, 9, (SDL_Color){0, 0, 0, 80});

    fill_rounded_rect(ren, win->rect, 9, WIN_BG);
    SDL_SetRenderDrawColor(ren, BORDER_IDLE.r, BORDER_IDLE.g, BORDER_IDLE.b, 160);
    SDL_RenderDrawRect(ren, &win->rect);

    SDL_Rect header = {win->rect.x, win->rect.y, win->rect.w, 34};
    fill_rounded_rect(ren, header, 9, (SDL_Color){31, 35, 41, 245});

    draw_text(ren, font, win->title, win->rect.x + 15, win->rect.y + 8, TXT_BRIGHT);

    SDL_SetRenderDrawColor(ren, BORDER_GLOW.r, BORDER_GLOW.g, BORDER_GLOW.b, 190);
    SDL_RenderDrawLine(ren, win->rect.x + 12, win->rect.y + 33, win->rect.x + win->rect.w - 12, win->rect.y + 33);

    // 5. Close Button (Circular)
    int cx = win->rect.x + win->rect.w - 20;
    int cy = win->rect.y + 15;
    int r = 10;
    
    int mx, my; SDL_GetMouseState(&mx, &my);
    int distSq = (mx-cx)*(mx-cx) + (my-cy)*(my-cy);
    int hover = (distSq <= r*r);

    SDL_Rect close_r = {cx-r+1, cy-r+1, r*2-2, r*2-2};
    SDL_Color c_col = hover ? (SDL_Color){210, 72, 76, 255} : (SDL_Color){48, 54, 63, 255};
    fill_rounded_rect(ren, close_r, 8, c_col); 
    
    draw_text(ren, font, "x", cx - 3, cy - 8, TXT_BRIGHT);
}

// IMPROVED: Smooth Rounded Button
void draw_button(SDL_Renderer *ren, TTF_Font *font, Button *btn, int mx, int my, int m_down, int active_state) {
    
    int hover = point_in_rect(mx, my, btn->rect);
    int pressed = (hover && m_down);

    SDL_Color base = btn->color; // Use the color defined in main.c
    SDL_Color text_col = TXT_BRIGHT;

    // Interaction Colors
    if (btn->is_toggle && active_state) {
        base = (SDL_Color){0, 160, 180, 255}; // Active Cyan
    } else if (pressed) {
        base.r = (Uint8)(base.r * 0.7);
        base.g = (Uint8)(base.g * 0.7);
        base.b = (Uint8)(base.b * 0.7);
    } else if (hover) {
        // Brighten
        base.r = (Uint8)(base.r * 1.3 > 255 ? 255 : base.r * 1.3);
        base.g = (Uint8)(base.g * 1.3 > 255 ? 255 : base.g * 1.3);
        base.b = (Uint8)(base.b * 1.3 > 255 ? 255 : base.b * 1.3);
    }

    // Draw Smooth Rounded Body
    fill_rounded_rect(ren, btn->rect, 5, base);

    // Draw Text Centered
    if (font && btn->label[0]) {
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, btn->label, text_col);
        if (surf) {
            int tx = btn->rect.x + (btn->rect.w - surf->w) / 2;
            int ty = btn->rect.y + (btn->rect.h - surf->h) / 2;
            if (pressed) { tx += 1; ty += 1; }
            
            SDL_Rect dst = {tx, ty, surf->w, surf->h};
            SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
            SDL_RenderCopy(ren, tex, NULL, &dst);
            SDL_DestroyTexture(tex);
            SDL_FreeSurface(surf);
        }
    }
}
