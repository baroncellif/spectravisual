#ifndef VIEW_H
#define VIEW_H

#include <SDL.h>
#include <SDL_ttf.h>
#include "types.h"

// Main entry point for drawing the frame
void render_app(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *layout);

#endif