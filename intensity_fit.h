#ifndef INTENSITY_FIT_H
#define INTENSITY_FIT_H

#include "types.h"

/* Fit the active experimental trace against the assigned transitions. */
int intensity_fit_run(AppState *state);

/* Area used by the fit for one measured transition.  Kept public so every
   caller that needs the measurement uses the same sorted-point invariant. */
int intensity_fit_integrate_area(const AppState *state, double center,
                                 double half_width, double *area);

/* Write the last fit, including each accepted/rejected observation. */
int intensity_fit_export(const AppState *state, const char *filename);

/* Multispecies intensity analysis.  These functions run in their own SDL
   windows and keep every fitted value in a preview-only state. */
void intensity_analysis_open(AppState *state);
void intensity_analysis_close(AppState *state);
void intensity_analysis_dispose(AppState *state);
int intensity_analysis_handle_event(AppState *state, const SDL_Event *event);
void intensity_analysis_poll(AppState *state);
void intensity_analysis_render(AppState *state);

/* The private state the fit preview window draws with render_app: a copy of
   the working state whose prediction carries the fitted intensities.  Built on
   first use and rebuilt when the working buffers or the result change; NULL
   until a fit has produced a result.  The working state is never modified. */
AppState *intensity_analysis_preview_state(AppState *state);

#endif
