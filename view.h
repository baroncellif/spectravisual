#ifndef VIEW_H
#define VIEW_H

#include <SDL.h>
#include <SDL_ttf.h>
#include "types.h"

// Main entry point for drawing the frame
void render_app(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *layout);

// The same frame, drawn but not presented. Pixels for an export are read here:
// reading them after SDL_RenderPresent returns whatever the driver leaves in
// the back buffer, which made the export save a stale frame.
void render_app_frame(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *layout);

#endif
