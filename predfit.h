#ifndef PREDFIT_H
#define PREDFIT_H

#include "types.h"

void predfit_init(AppState *state);
int predfit_calculate(AppState *state);
int predfit_fit(AppState *state);
int predfit_undo_last_fit(AppState *state);
void predfit_publish_shared_state(AppState *state);
void predfit_adopt_shared_state(AppState *state);
void predfit_adopt_generated_catalog(AppState *state);
int predfit_restore_latest(AppState *state);

/* The session file: which spectra were open, which one was active, and the
   fit uncertainties the user chose. Save it whenever the working set changes. */
void predfit_load_session(AppState *state);
void predfit_save_session(const AppState *state);
void predfit_open_advanced(AppState *state);
void predfit_close_advanced(AppState *state);
void predfit_dispose(AppState *state);
int predfit_handle_advanced_event(AppState *state, const SDL_Event *event);
void predfit_render_advanced(AppState *state);

#endif
