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

/* Write the spectrum, per-species LIN/CAT/INT and run.json the Python
   intensity_fit package reads, into the existing directory dir.  config and
   output receive the paths to hand to it.  Returns 0 with the reason in the
   window message when nothing can be fitted. */
int intensity_analysis_write_python_inputs(AppState *state, const char *dir,
                                           char *config, size_t config_size,
                                           char *output, size_t output_size);

/* The private state the fit preview window draws with render_app: a copy of
   the working state whose prediction carries the fitted intensities.  Built on
   first use and rebuilt when the working buffers or the result change; NULL
   until a fit has produced a result.  The working state is never modified. */
AppState *intensity_analysis_preview_state(AppState *state);

/* Keep the window's choices and result while Pred&Fit offers the same
   species, refreshing their catalogue values; start over otherwise. */
void intensity_analysis_sync_species(AppState *state);

/* The intensity fit in spectravisual.state: its options, species choices,
   result, report and fit log.  session_begin clears the analysis before a
   session is read; read_session_line returns 1 for an intfit_ record. */
void intensity_analysis_write_session(const AppState *state, FILE *fp);
void intensity_analysis_session_begin(AppState *state);
int intensity_analysis_read_session_line(AppState *state, const char *line);

#endif
