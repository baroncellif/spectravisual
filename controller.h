#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "types.h"
#include <SDL.h>

void handle_app_events(AppState *state, Layout *layout, int *running);
/* Navigation-only handling of one event of a viewer window whose state has
   viewer_readonly set: the same handlers, without edits or file writes. */
void handle_viewer_event(AppState *state, Layout *layout, const SDL_Event *event);
/* Put both panes of the intensity-fit preview on its shared relative axis:
   0 at the bottom, intensity_axis_top at the top of the experimental and of
   the prediction pane alike.  No effect outside that preview. */
void viewer_apply_intensity_axis(AppState *state);
/* The one deletion route shared by the main assignment panel and Pred&Fit
   Advanced.  It updates assignments.txt and removes any stale fit exclusion. */
void delete_assignment(AppState *state, int idx);
/* The authoritative assignments.txt writer, shared with project-level edits
   that can remove a whole Hamiltonian and its owned assignments. */
int save_assignments(AppState *state);

#endif
