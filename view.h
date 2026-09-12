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

// Returns the visible maximum of the rendered prediction: sticks when no
// usable broadening profile is active, otherwise the broadened profile.
double prediction_visible_max(const AppState *state, int samples);

/* Value of the broadened profile at `f`, exactly as the renderer evaluates
   it: the whole plot when hamiltonian_id is 0, or the trace of one state when
   a Hamiltonian is named.  Returns 0 while no broadening is configured. */
double broadened_value_at(const AppState *state, double f, int hamiltonian_id, int state_index);

#endif
