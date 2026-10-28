#ifndef XIAM_H
#define XIAM_H

#include "types.h"
#include "predfit.h"

/* XIAM is intentionally a self-contained backend.  It reads and writes its
 * native .xi/.xo files and presents one XIAM Hamiltonian per input file. */
void xiam_init(AppState *state);
void xiam_dispose(AppState *state);
void xiam_open(AppState *state);
void xiam_close(AppState *state);
int  xiam_load_input_file(AppState *state, const char *path);
int  xiam_load_output_file(AppState *state, const char *path);
/* Import every native XIAM pair in a Pred&Fit-style drop directory.  A .xi
 * is the source of truth; a same-stem .xo is optional and restores its
 * catalogue/report after that input has selected its own Hamiltonian. */
int  xiam_import_load_dir(AppState *state, const char *directory);
int  xiam_save_fit_file(AppState *state, const char *path);
int  xiam_calculate(AppState *state);
int  xiam_fit(AppState *state);
int  xiam_handle_event(AppState *state, const SDL_Event *event);
void xiam_render(AppState *state);

/* Native XIAM intensities have no Pickett state population model.  Their
 * per-Hamiltonian concentration is therefore a direct multiplier of the
 * SPCAT intensity printed in the .xo catalogue. */
double xiam_hamiltonian_concentration(const AppState *state, int hamiltonian_id);
/* One broadened trace per simulated XIAM Hamiltonian, in its own colour.  A
 * XIAM model resolves its torsional and hyperfine states internally, so the
 * trace covers the whole model rather than one state. */
int xiam_plot_traces(const AppState *state, PredfitSpeciesTrace *out, int max);
SDL_Color xiam_trace_color(const AppState *state, int hamiltonian_id);
int xiam_set_hamiltonian_concentration(AppState *state, int hamiltonian_id,
                                       double concentration);
void xiam_apply_concentrations(AppState *state);

/* The XIAM workspace is part of spectravisual.state.  Source text is saved
 * with the session, while a readable .xo is reopened on restore to repopulate
 * the native prediction overlay. */
void xiam_write_session(AppState *state, FILE *fp);
void xiam_session_begin(AppState *state);
int xiam_read_session_line(AppState *state, const char *line);
void xiam_session_finalize(AppState *state);
int xiam_restore_session_predictions(AppState *state);

/* Shared identity helper: unlike a Pickett CAT record, XIAM's complete state
 * identity includes tunnelling and hyperfine labels outside CAT QNFMT. */
int xiam_same_transition(const PredLine *a, const PredLine *b);

/* Closes any inline edit of an assignment's observed frequency/uncertainty
 * and drops the hovered-row cache, without touching the assignment itself.
 * For callers outside xiam.c (e.g. a global reorder of the assignment list)
 * that just made those row indices stale. */
void xiam_cancel_assignment_edit(AppState *state);

#endif
