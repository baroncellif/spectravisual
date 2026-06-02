#ifndef LAYOUT_H
#define LAYOUT_H

#include <SDL.h>
#include <SDL_ttf.h>
#include "types.h"

// --- UTILS ---
double nice_tick(double range);
int point_in_rect(int mx, int my, SDL_Rect r);
SDL_Color color_for_pred(char branch, char mu);

// --- DRAWING ---
// THIS LINE IS MISSING IN YOUR FILE:
void fill_rounded_rect(SDL_Renderer *ren, SDL_Rect dst, int radius, SDL_Color c);

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color);
void draw_text_vertical(SDL_Renderer *ren, TTF_Font *font, const char *txt, int x, int y, SDL_Color color);
void draw_draggable_window(SDL_Renderer *ren, TTF_Font *font, DraggableWindow *win);
void draw_button(SDL_Renderer *ren, TTF_Font *font, Button *btn, int mouse_x, int mouse_y, int mouse_down, int active_state);

// --- SIDEBAR DOCKING ---
// Animates the tool panels and docks the visible ones to the right edge as a
// stack of floating "glass" cards. Sets each window's rect/clip and fills in
// l->plot_right so the spectrum can be shrunk to make room. Call once per frame
// BEFORE event handling so hit-testing uses the up-to-date panel rects.
void update_sidebars(AppState *s, Layout *l);

#endif