#ifndef XIAM_H
#define XIAM_H

#include "types.h"

/* XIAM is intentionally a self-contained backend.  It reads and writes its
 * native .xi/.xo files and presents one XIAM Hamiltonian per input file. */
void xiam_init(AppState *state);
void xiam_dispose(AppState *state);
void xiam_open(AppState *state);
void xiam_close(AppState *state);
int  xiam_load_input_file(AppState *state, const char *path);
int  xiam_load_output_file(AppState *state, const char *path);
int  xiam_save_fit_file(AppState *state, const char *path);
int  xiam_calculate(AppState *state);
int  xiam_fit(AppState *state);
int  xiam_handle_event(AppState *state, const SDL_Event *event);
void xiam_render(AppState *state);

/* Shared identity helper: unlike a Pickett CAT record, XIAM's complete state
 * identity includes tunnelling and hyperfine labels outside CAT QNFMT. */
int xiam_same_transition(const PredLine *a, const PredLine *b);

#endif
