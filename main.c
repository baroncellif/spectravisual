#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#include "types.h"
#include "loader.h"
#include "algorithms.h"
#include "layout.h"
#include "ui_theme.h"
#include "view.h"
#include "controller.h"
#include "intensity_fit.h"
#include "predfit.h"
#include "settings.h"
#include "xiam.h"
#include "plotgpu.h"

static int add_spectrum(AppState *state, const char *path);

static void init_app_defaults(AppState *state) {
    state->pred_scale = 1.0;
    state->cat_temp_k = 0.0;
    state->rot_temp_k = 0.0;
    state->intfit_half_window_mhz = 0.25;
    state->intfit_fit_temperature = 1;
    state->intfit_fit_dipole[0] = 1;
    state->intfit_fit_dipole[1] = 1;
    state->intfit_fit_dipole[2] = 1;
    state->intfit_reference_component = -1;
    state->intfit_component_scale[0] = 1.0;
    state->intfit_component_scale[1] = 1.0;
    state->intfit_component_scale[2] = 1.0;
    state->sync_active = 1;
    state->broaden_mode = 0;
    state->lorentz_gamma = 0.0;
    state->gauss_gamma = 0.0;
    state->kaiser_beta = 1.0;
    state->kaiser_ceros = 6;
    state->kaiser_intrinsic = 0.0;
    state->pf_sig_pts = 5;
    state->pf_noise_pts = 50;
    state->pf_thresh = 3.0;
    state->rolling_avg_window = 10;
    state->pred_min_log_int = -10.0;
    state->pred_max_log_int = 0.0;
    state->selected_assignment = -1;

    // Quantum-number / branch filter defaults: inactive, everything allowed.
    state->filter_active = 0;
    state->filt_mu[0] = state->filt_mu[1] = state->filt_mu[2] = 1;
    state->filt_br[0] = state->filt_br[1] = state->filt_br[2] = 1;
    state->filt_use_range = 0;
    state->filt_j_min = 0;   state->filt_j_max = 200;
    state->filt_ka_min = 0;  state->filt_ka_max = 200;
    state->filt_kc_min = 0;  state->filt_kc_max = 200;
    state->filt_use_delta = 0;
    state->filt_dj = 1;  state->filt_dka = 0;  state->filt_dkc = 1;

    // Titles and sizes of the inspector sections. The x/y are placeholders:
    // update_sidebars docks every open panel in the right-hand column.
    state->win_pf   = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 300}, 0, "Peak finder"};
    state->win_br   = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 300}, 0, "Broadening"};
    state->win_as   = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 400}, 0, "Assignments"};
    state->win_avg  = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 190}, 0, "Rolling average"};
    state->win_cut  = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 152}, 0, "Intensity range"};
    state->win_jump = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 132}, 0, "Frequency jump"};
    state->win_filt = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 402}, 0, "Transition filter"};
    state->win_dip  = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 620}, 0, "Intensity analysis"};
    state->win_spec = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 360}, 0, "Spectra"};
    state->win_predfit = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 430}, 0, "Pred&Fit"};
    state->win_xiam = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 100}, 0, "XIAM"};
    predfit_init(state);
    xiam_init(state);

    state->n_spectra = 0;
    state->active_spec = -1;
    state->multi_layout = 0;   // overlay
    state->multi_ynorm = 0;    // shared Y
    state->multi_indiv_int = 0; // intensity controls affect all spectra
    state->pending_select = -1;
    state->pending_remove = -1;
}

// Copy the active spectrum's fields into the legacy AppState fields the tools read.
static void mirror_active(AppState *state) {
    if (state->active_spec < 0 || state->active_spec >= state->n_spectra) {
        state->raw_pts = NULL; state->smooth_pts = NULL; state->current_pts = NULL;
        state->n_pts = 0; state->exp_offset = 0.0; state->rolling_avg_active = 0;
        state->exp_path[0] = '\0';
        return;
    }
    Spectrum *sp = &state->spectra[state->active_spec];
    state->raw_pts = sp->raw_pts;
    state->smooth_pts = sp->smooth_pts;
    state->current_pts = sp->current_pts;
    state->n_pts = sp->n_pts;
    state->xmin = sp->xmin; state->xmax = sp->xmax;
    state->ymin = sp->ymin; state->ymax = sp->ymax;
    state->exp_offset = sp->exp_offset;
    state->rolling_avg_active = sp->rolling_avg_active;
    snprintf(state->exp_path, sizeof(state->exp_path), "%s", sp->path);
}

// Write back the mutable mirror fields into the active spectrum.
static void commit_active(AppState *state) {
    if (state->active_spec < 0 || state->active_spec >= state->n_spectra) return;
    Spectrum *sp = &state->spectra[state->active_spec];
    sp->current_pts = state->current_pts;
    sp->exp_offset = state->exp_offset;
    sp->rolling_avg_active = state->rolling_avg_active;
}

void app_sync_view_state(AppState *state) {
    commit_active(state);
    if (state->data_loaded && state->sync_active) { state->pvxmin = state->vxmin; state->pvxmax = state->vxmax; }
}

static void select_spectrum(AppState *state, int idx) {
    if (idx < 0 || idx >= state->n_spectra) return;
    commit_active(state);
    state->active_spec = idx;
    state->n_peaks = 0;        // peaks/selection belong to a single spectrum
    state->n_selected = 0;
    mirror_active(state);
}

static void restore_session_spectrum(AppState *state, int idx, const SessionSpectrum *saved) {
    if (idx < 0 || idx >= state->n_spectra || !saved) return;
    Spectrum *sp = &state->spectra[idx];
    sp->visible = saved->visible != 0;
    sp->opacity = saved->opacity >= 10 && saved->opacity <= 100 ? saved->opacity : state->settings.trace_opacity;
    sp->vscale = saved->vscale > 0.0 && isfinite(saved->vscale) ? saved->vscale : 1.0;
    sp->exp_offset = isfinite(saved->exp_offset) ? saved->exp_offset : 0.0;
    sp->voffset = isfinite(saved->voffset) ? saved->voffset : 0.0;
    sp->rolling_avg_active = saved->rolling_avg_active != 0;
    if (sp->rolling_avg_active) {
        apply_rolling_average(sp->raw_pts, sp->smooth_pts, sp->n_pts, state->rolling_avg_window);
        sp->current_pts = sp->smooth_pts;
    }
    /* add_spectrum made this the active trace; refresh its live mirror before
       loading the next one, otherwise commit_active would overwrite it. */
    if (state->active_spec == idx) mirror_active(state);
}

/* A saved zoom belongs to the document, but it cannot be allowed to make a
 * restored multi-Hamiltonian project look incomplete.  This happened when a
 * session was saved on a narrow line: several catalogues were correctly
 * merged, yet none of their transitions fell inside that old viewport. */
static int session_view_hides_simulated_model(const AppState *state,
                                              double fmin, double fmax) {
    const PredFitState *p = &state->predfit;
    if (!state->pred_lines || p->n_simulated <= 1 || !(fmax > fmin)) return 0;
    int seen[MAX_PICKETT_HAMILTONIANS] = {0};
    for (int i = 0; i < state->n_pred; i++) {
        const PredLine *line = &state->pred_lines[i];
        if (line->freq_mhz < fmin || line->freq_mhz > fmax) continue;
        for (int h = 0; h < p->n_simulated; h++)
            if (p->simulated[h].hamiltonian_id == line->hamiltonian_id)
                seen[h] = 1;
    }
    for (int h = 0; h < p->n_simulated; h++) if (!seen[h]) return 1;
    return 0;
}

static void show_full_simulation_range(AppState *state) {
    state->pvxmin = state->pxmin;
    state->pvxmax = state->pxmax;
    if (state->sync_active) {
        state->vxmin = state->pxmin;
        state->vxmax = state->pxmax;
    }
    state->bar_x = (state->pxmin + state->pxmax) * 0.5;
    state->pbar_x = state->bar_x;
}

static void restore_session_view(AppState *state) {
    if (!state->session_has_view) return;
    if (isfinite(state->session_vxmin) && isfinite(state->session_vxmax) && state->session_vxmax > state->session_vxmin) {
        state->vxmin = state->session_vxmin; state->vxmax = state->session_vxmax;
    }
    if (isfinite(state->session_vymin) && isfinite(state->session_vymax) && state->session_vymax > state->session_vymin) {
        state->vymin = state->session_vymin; state->vymax = state->session_vymax;
    }
    state->sync_active = state->session_sync_active;
    if (!state->sync_active && isfinite(state->session_pvxmin) && isfinite(state->session_pvxmax) &&
        state->session_pvxmax > state->session_pvxmin) {
        state->pvxmin = state->session_pvxmin; state->pvxmax = state->session_pvxmax;
    }
    double pmin = state->sync_active ? state->vxmin : state->pvxmin;
    double pmax = state->sync_active ? state->vxmax : state->pvxmax;
    if (session_view_hides_simulated_model(state, pmin, pmax)) {
        /* pxmin/pxmax are the envelope of every catalogue just merged by
           set_predictions_simulated().  With sync on, expand both panes. */
        show_full_simulation_range(state);
        snprintf(state->status_message, sizeof(state->status_message),
                 "Restored all %d simulated Hamiltonians; expanded a saved zoom that hid part of the project.",
                 state->predfit.n_simulated);
    }
    state->session_has_view = 0;  /* restore once; navigation is live afterwards */
}

/* Adds a note to the title-bar message without hiding what is already there. */
static void append_error_message(AppState *state, const char *note) {
    size_t used = strlen(state->error_message);
    if (used == 0) snprintf(state->error_message, sizeof(state->error_message), "%s", note);
    else snprintf(state->error_message + used, sizeof(state->error_message) - used, " %s", note);
}

static void ensure_aux_loaded(AppState *state) {
    if (state->n_assignments != 0 || state->aux_loaded) return;
    state->aux_loaded = 1;
    char path[600], note[512];
    AssignmentFileReport report;
    settings_data_file(state, "assignments.txt", path, sizeof(path));
    load_assignments_file(path, state->assignments, &state->n_assignments, &report);
    assignment_file_message(&report, path, note, sizeof(note));
    if (note[0]) append_error_message(state, note);
    /* Pred&Fit's temporary inclusion choices are separate from the
       complete assignment list, and must apply even when a user opens a
       catalogue directly instead of restoring model.cat. */
    predfit_load_exclusions(state);
}

static void spec_basename(const char *path, char *out, int n) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, n, "%s", b);
}

static void free_dataset(AppState *state) {
    for (int i = 0; i < state->n_spectra; i++) {
        free(state->spectra[i].raw_pts);
        free(state->spectra[i].smooth_pts);
    }
    state->n_spectra = 0;
    state->active_spec = -1;
    free(state->pred_lines);
    state->pred_lines = NULL;
    state->raw_pts = NULL;
    state->smooth_pts = NULL;
    state->current_pts = NULL;
    state->n_pts = 0;
    state->n_pred = 0;
    state->n_peaks = 0;
    state->n_selected = 0;
    state->n_assignments = 0;
    state->aux_loaded = 0;
    state->data_loaded = 0;
}

/* Point the workspace at the session the user named.  The session file lives
   in the .fit of its own project, and that folder is where the catalogues and
   the Pickett files it refers to are: opening one from elsewhere has to move
   the data directory with it, or the restore would read another project. */
static void adopt_session_data_dir(AppState *state, const char *session_path) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", session_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;              /* "spectravisual.state": already in .fit */
    *slash = '\0';                   /* .../<data dir>/.fit                    */
    slash = strrchr(dir, '/');
    const char *leaf = slash ? slash + 1 : dir;
    if (strcmp(leaf, FIT_DIR_NAME) != 0) return;   /* not a .fit: leave settings alone */
    if (slash) *slash = '\0';
    else dir[0] = '\0';              /* ".fit/spectravisual.state" = here      */
    snprintf(state->settings.data_dir, sizeof(state->settings.data_dir), "%s", dir);
}

/* Open the state file as a session, not as a two-column trace.  A dropped
   spectravisual.state intentionally replaces the active working set, just as
   opening a project file would in a conventional program. */
static void reopen_predfit_session(AppState *state) {
    free_dataset(state);

    predfit_load_session(state);
    predfit_restore_latest(state);
    if (state->session_has_view && state->session_rolling_avg_window > 0)
        state->rolling_avg_window = state->session_rolling_avg_window;
    for (int k = 0; k < state->n_session_spec; k++) {
        int before = state->n_spectra;
        if (add_spectrum(state, state->session_spectrum[k].path))
            restore_session_spectrum(state, before, &state->session_spectrum[k]);
    }
    if (state->session_active_spec >= 0 && state->session_active_spec < state->n_spectra)
        select_spectrum(state, state->session_active_spec);
    snprintf(state->status_message, sizeof(state->status_message),
             "Restored %d experimental spectrum%s from the Pred&Fit session.",
             state->n_spectra, state->n_spectra == 1 ? "" : "s");
}

/* Pickett catalogues/simulations replace one another, as do successive XIAM
   outputs for the active native model.  They are deliberately two different
   queues, however: a user can load XIAM and SPCAT in the same event batch and
   expects one mixed prediction, not whichever happens to be processed last. */
static int pending_load_is_pickett_plot(PendingLoadKind kind) {
    return kind == PENDING_LOAD_CATALOG || kind == PENDING_LOAD_SIMULATION;
}

static int pending_load_is_xiam_plot(PendingLoadKind kind) {
    return kind == PENDING_LOAD_XIAM_OUTPUT;
}

static void drop_pending_plot_loads(AppState *state, PendingLoadKind incoming) {
    int drop_pickett = pending_load_is_pickett_plot(incoming);
    int drop_xiam = pending_load_is_xiam_plot(incoming);
    int kept = 0;
    for (int i = 0; i < state->pending_load_count; i++) {
        int from = (state->pending_load_head + i) % MAX_PENDING_LOADS;
        PendingLoadKind kind = state->pending_loads[from].kind;
        if ((drop_pickett && pending_load_is_pickett_plot(kind)) ||
            (drop_xiam && pending_load_is_xiam_plot(kind))) continue;
        int to = (state->pending_load_head + kept) % MAX_PENDING_LOADS;
        if (to != from) state->pending_loads[to] = state->pending_loads[from];
        kept++;
    }
    state->pending_load_count = kept;
}

int app_enqueue_pending_load(AppState *state, PendingLoadKind kind,
                             const char *path, int generated_catalog) {
    if (!state || !path || !path[0]) return 0;
    if (pending_load_is_pickett_plot(kind) || pending_load_is_xiam_plot(kind))
        drop_pending_plot_loads(state, kind);
    if (state->pending_load_count >= MAX_PENDING_LOADS) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Too many files waiting to load (maximum %d).", MAX_PENDING_LOADS);
        return 0;
    }
    int slot = (state->pending_load_head + state->pending_load_count) % MAX_PENDING_LOADS;
    PendingLoadRequest *request = &state->pending_loads[slot];
    request->kind = kind;
    request->generated_catalog = generated_catalog != 0;
    snprintf(request->path, sizeof(request->path), "%s", path);
    state->pending_load_count++;
    return 1;
}

// Load an experimental spectrum and append it to the store (becomes active).
static int add_spectrum(AppState *state, const char *path) {
    if (predfit_is_session_file(state, path)) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "The session state is not an experimental spectrum.");
        return 0;
    }
    if (state->n_spectra >= MAX_SPECTRA) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Maximum of %d spectra already loaded.", MAX_SPECTRA);
        return 0;
    }
    Point *raw = NULL;
    double xmin, xmax, ymin, ymax;
    int was_descending = 0;
    int n = read_data_alloc(path, &raw, &xmin, &xmax, &ymin, &ymax, &was_descending);
    if (n <= 0 || !raw) {
        if (n == -2)
            snprintf(state->error_message, sizeof(state->error_message),
                     "Could not load spectrum: frequencies must be strictly monotonic (no duplicates): %s", path);
        else
            snprintf(state->error_message, sizeof(state->error_message),
                     "Could not load spectrum: %s", path);
        return 0;
    }
    Point *smooth = malloc(sizeof(Point) * n);
    if (!smooth) {
        free(raw);
        snprintf(state->error_message, sizeof(state->error_message),
                 "Not enough memory for loaded spectrum.");
        return 0;
    }

    commit_active(state);   // preserve the previously active spectrum's edits

    int idx = state->n_spectra++;
    Spectrum *sp = &state->spectra[idx];
    memset(sp, 0, sizeof(*sp));
    sp->raw_pts = raw; sp->smooth_pts = smooth; sp->current_pts = raw; sp->n_pts = n;
    sp->xmin = xmin; sp->xmax = xmax; sp->ymin = ymin; sp->ymax = ymax;
    sp->visible = 1;
    sp->vscale = 1.0;
    sp->color = state->settings.trace_color[idx % MAX_SPECTRA];
    sp->opacity = state->settings.trace_opacity;
    snprintf(sp->path, sizeof(sp->path), "%s", path);
    spec_basename(path, sp->name, sizeof(sp->name));

    int first = !state->data_loaded;
    state->active_spec = idx;
    state->n_peaks = 0; state->n_selected = 0;
    mirror_active(state);
    state->data_loaded = 1;

    if (first) {
        state->vxmin = xmin; state->vxmax = xmax;
        state->vymin = ymin; state->vymax = ymax;
        state->pvxmin = xmin; state->pvxmax = xmax;
        state->bar_x = (xmin + xmax) / 2.0; state->pbar_x = state->bar_x;
        if (state->n_pred == 0) { state->pxmin = xmin; state->pxmax = xmax; }
    }
    snprintf(state->status_message, sizeof(state->status_message),
             was_descending ? "Loaded spectrum '%s' (%d pts; reordered from descending frequency). %d spectra loaded."
                            : "Loaded spectrum '%s' (%d pts). %d spectra loaded.",
             sp->name, n, state->n_spectra);
    state->error_message[0] = '\0';
    ensure_aux_loaded(state);   /* after the messages above: it can add its own */
    if (state->verbose) fprintf(stderr, "%s\n", state->status_message);
    return 1;
}

static int pred_line_by_frequency(const void *a, const void *b) {
    double fa = ((const PredLine *)a)->freq_mhz, fb = ((const PredLine *)b)->freq_mhz;
    return fa < fb ? -1 : fa > fb ? 1 : 0;
}

/* Load one Pickett/external catalogue.  Native XIAM output is a separately
 * owned prediction source, so replacing the Pickett side must retain it. */
static int set_predictions(AppState *state, const char *path) {
    PredLine *pred = NULL;
    double pxmin, pxmax, pgmax;
    int unsupported = 0;
    int n = read_pred_cat_alloc_counted(path, &pred, &pxmin, &pxmax, &pgmax, &unsupported);
    if (n <= 0 || !pred) {
        if (unsupported > 0)
            snprintf(state->error_message, sizeof(state->error_message),
                     "Could not load predictions: %s (%d lines skipped: NQN 0 or above 6 is not supported).",
                     path, unsupported);
        else
            snprintf(state->error_message, sizeof(state->error_message),
                     "Could not load predictions: %s", path);
        return 0;
    }
    int native_kept = 0;
    for (int i = 0; i < state->n_pred; i++)
        if (state->pred_lines[i].backend == PRED_BACKEND_XIAM) native_kept++;
    if (native_kept) {
        PredLine *merged = malloc((size_t)(n + native_kept) * sizeof(*merged));
        if (!merged) {
            free(pred);
            snprintf(state->error_message, sizeof(state->error_message),
                     "Not enough memory to keep the existing XIAM prediction with %s.", path);
            return 0;
        }
        int at = 0;
        for (int i = 0; i < state->n_pred; i++)
            if (state->pred_lines[i].backend == PRED_BACKEND_XIAM)
                merged[at++] = state->pred_lines[i];
        memcpy(merged + at, pred, (size_t)n * sizeof(*pred));
        at += n;
        free(pred);
        pred = merged;
        n = at;
        qsort(pred, (size_t)n, sizeof(*pred), pred_line_by_frequency);
        pxmin = pred[0].freq_mhz; pxmax = pred[n - 1].freq_mhz; pgmax = 0.0;
        for (int i = 0; i < n; i++) if (pred[i].linear_int > pgmax) pgmax = pred[i].linear_int;
    }
    free(state->pred_lines);
    state->pred_lines = pred;
    state->n_pred = n;
    /* The selection is a list of indices into pred_lines: in another catalogue
       they point at other transitions, so a new one starts with none selected. */
    state->n_selected = 0;
    state->pxmin = pxmin; state->pxmax = pxmax;
    /* Only model.cat created in the Pred&Fit workspace has a known Tcat and
       per-species settings.  An arbitrary .cat remains a standalone file. */
    if (!predfit_is_generated_catalog(state, path)) {
        state->predfit.generated_catalog_active = 0;
        state->predfit.generated_catalog_pending = 0;
    }
    /* Whatever this catalogue is, it is now the whole plot: the merged rows
       of a simulation are gone and their models must not rescale it. */
    state->predfit.n_simulated = 0;
    state->cat_temp_k = 0.0;
    state->dipole_cat[0] = state->dipole_cat[1] = state->dipole_cat[2] = 0.0;
    predfit_recompute_display_intensities(state);
    snprintf(state->pred_path, sizeof(state->pred_path), "%s", path);

    int first = !state->data_loaded;
    state->data_loaded = 1;
    if (first) {
        state->xmin = pxmin; state->xmax = pxmax; state->ymin = 0.0; state->ymax = 1.0;
        state->vxmin = pxmin; state->vxmax = pxmax; state->vymin = 0.0; state->vymax = 1.0;
        state->pvxmin = pxmin; state->pvxmax = pxmax;
        state->bar_x = (pxmin + pxmax) / 2.0; state->pbar_x = state->bar_x;
    }
    if (unsupported > 0) {
        /* Also in the title bar: status_message is only shown while nothing
           is loaded, and skipped catalogue lines must not go unnoticed. */
        snprintf(state->status_message, sizeof(state->status_message),
                 "Loaded %d catalogue lines%s; %d skipped: NQN 0 or above 6 is not supported.",
                 n - native_kept, native_kept ? " alongside native XIAM" : "", unsupported);
        snprintf(state->error_message, sizeof(state->error_message), "%s", state->status_message);
    } else {
        snprintf(state->status_message, sizeof(state->status_message),
                 "Loaded %d catalogue lines%s.", n - native_kept,
                 native_kept ? " alongside native XIAM" : "");
        state->error_message[0] = '\0';
    }
    ensure_aux_loaded(state);   /* after the messages above: it can add its own */
    if (state->verbose) fprintf(stderr, "%s\n", state->status_message);
    return 1;
}

/* Merge the catalogues of one simulation into a single prediction set.  Every
   row keeps the Hamiltonian that produced it, so intensities, assignment
   ownership and the Fitting tab stay attached to the right model even though
   several models share one plot. */
static int set_predictions_simulated(AppState *state) {
    PredFitState *p = &state->predfit;
    PredLine *merged = NULL;
    int n_merged = 0, unsupported = 0, sources = 0;
    char failed[200] = "";
    for (int i = 0; i < p->n_simulated; i++) {
        PredLine *rows = NULL;
        double rmin = 0.0, rmax = 0.0, rmax_int = 0.0;
        int skipped = 0;
        int n = read_pred_cat_alloc_counted(p->simulated[i].cat_path, &rows,
                                            &rmin, &rmax, &rmax_int, &skipped);
        unsupported += skipped;
        if (n <= 0 || !rows) {
            free(rows);
            if (!failed[0])
                snprintf(failed, sizeof(failed), "%s produced no readable catalogue.",
                         p->simulated[i].cat_path);
            continue;
        }
        PredLine *grown = realloc(merged, (size_t)(n_merged + n) * sizeof(*merged));
        if (!grown) { free(rows); break; }
        merged = grown;
        for (int k = 0; k < n; k++) {
            rows[k].hamiltonian_id = p->simulated[i].hamiltonian_id;
            rows[k].backend = PRED_BACKEND_PICKETT;
            merged[n_merged + k] = rows[k];
        }
        free(rows);
        (void)rmin; (void)rmax;
        n_merged += n;
        sources++;
    }
    if (n_merged <= 0 || !merged) {
        free(merged);
        snprintf(state->error_message, sizeof(state->error_message),
                 "Simulation produced no predicted line. %s", failed);
        return 0;
    }
    /* Keep native outputs already in the viewer.  XIAM and SPCAT do not share
       a file format or an owner namespace, but their sticks are comparable on
       the same MHz axis and must reach the same broadening pass. */
    int native_kept = 0;
    for (int i = 0; i < state->n_pred; i++)
        if (state->pred_lines[i].backend == PRED_BACKEND_XIAM) native_kept++;
    if (native_kept) {
        PredLine *grown = realloc(merged, (size_t)(n_merged + native_kept) * sizeof(*merged));
        if (!grown) {
            free(merged);
            snprintf(state->error_message, sizeof(state->error_message),
                     "Not enough memory to keep the existing XIAM prediction with the SPCAT simulation.");
            return 0;
        }
        merged = grown;
        for (int i = 0; i < state->n_pred; i++)
            if (state->pred_lines[i].backend == PRED_BACKEND_XIAM)
                merged[n_merged++] = state->pred_lines[i];
    }
    /* One plot, one frequency order: the merged rows come from several files. */
    qsort(merged, (size_t)n_merged, sizeof(*merged), pred_line_by_frequency);
    free(state->pred_lines);
    state->pred_lines = merged;
    state->n_pred = n_merged;
    state->n_selected = 0;
    state->pxmin = merged[0].freq_mhz;
    state->pxmax = merged[n_merged - 1].freq_mhz;
    /* Inspect the just-merged rows, not the prediction from the previous
       frame.  Otherwise a pre-existing XIAM-only plot can make Simulate
       decide that its newly generated Pickett models are already visible. */
    if (!state->session_has_view &&
        session_view_hides_simulated_model(state, state->sync_active ? state->vxmin : state->pvxmin,
                                            state->sync_active ? state->vxmax : state->pvxmax))
        show_full_simulation_range(state);
    /* The visible path is the active Hamiltonian's catalogue when it took
       part, so "is this the generated catalogue?" keeps its usual answer. */
    const char *shown = p->simulated[0].cat_path;
    int active_id = predfit_active_hamiltonian_id(state);
    for (int i = 0; i < p->n_simulated; i++)
        if (p->simulated[i].hamiltonian_id == active_id) shown = p->simulated[i].cat_path;
    snprintf(state->pred_path, sizeof(state->pred_path), "%s", shown);
    predfit_adopt_simulation(state);

    int first = !state->data_loaded;
    state->data_loaded = 1;
    if (first) {
        state->xmin = state->pxmin; state->xmax = state->pxmax; state->ymin = 0.0; state->ymax = 1.0;
        state->vxmin = state->pxmin; state->vxmax = state->pxmax; state->vymin = 0.0; state->vymax = 1.0;
        state->pvxmin = state->pxmin; state->pvxmax = state->pxmax;
        state->bar_x = (state->pxmin + state->pxmax) / 2.0; state->pbar_x = state->bar_x;
    }
    snprintf(state->status_message, sizeof(state->status_message),
             "Simulated %d Hamiltonian%s: %d SPCAT lines%s%s.",
             sources, sources == 1 ? "" : "s", n_merged - native_kept,
             native_kept ? " plus native XIAM" : "",
             unsupported > 0 ? " (some skipped: NQN 0 or above 6)" : "");
    if (unsupported > 0 || failed[0])
        snprintf(state->error_message, sizeof(state->error_message), "%s%s%s",
                 state->status_message, failed[0] ? " " : "", failed);
    else
        state->error_message[0] = '\0';
    ensure_aux_loaded(state);
    if (state->verbose) fprintf(stderr, "%s\n", state->status_message);
    return 1;
}

/* Consume all requests collected in this frame.  In particular, a session
   request must not discard later drops in the same queue. */
static void process_pending_loads(AppState *state) {
    while (state->pending_load_count > 0) {
        PendingLoadRequest request = state->pending_loads[state->pending_load_head];
        state->pending_load_head = (state->pending_load_head + 1) % MAX_PENDING_LOADS;
        state->pending_load_count--;
        switch (request.kind) {
            case PENDING_LOAD_CATALOG:
                set_predictions(state, request.path);
                if (request.generated_catalog) predfit_adopt_generated_catalog(state);
                break;
            case PENDING_LOAD_XIAM_INPUT:
                xiam_load_input_file(state, request.path);
                break;
            case PENDING_LOAD_XIAM_OUTPUT:
                xiam_load_output_file(state, request.path);
                break;
            case PENDING_LOAD_SIMULATION:
                set_predictions_simulated(state);
                break;
            case PENDING_LOAD_SESSION:
                reopen_predfit_session(state);
                break;
            case PENDING_LOAD_SPECTRUM:
                /* The session is written only when the user asks for it in
                    Pred&Fit Advanced; loading a spectrum just makes it stale. */
                if (add_spectrum(state, request.path)) state->predfit.session_dirty = 1;
                break;
        }
    }
}

static void remove_spectrum(AppState *state, int idx) {
    if (idx < 0 || idx >= state->n_spectra) return;
    int removed_before_active = idx < state->active_spec;
    free(state->spectra[idx].raw_pts);
    free(state->spectra[idx].smooth_pts);
    for (int i = idx; i < state->n_spectra - 1; i++)
        state->spectra[i] = state->spectra[i + 1];
    state->n_spectra--;
    if (removed_before_active) state->active_spec--;
    if (state->active_spec >= state->n_spectra) state->active_spec = state->n_spectra - 1;
    state->n_peaks = 0; state->n_selected = 0;
    if (state->n_spectra == 0 && state->n_pred == 0) state->data_loaded = 0;
    mirror_active(state);
}


static void save_screenshot(SDL_Renderer *ren, AppState *state) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) {
        snprintf(state->error_message, sizeof(state->error_message), "Could not create export surface.");
        return;
    }
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) != 0) {
        snprintf(state->error_message, sizeof(state->error_message), "Could not read renderer pixels.");
        SDL_FreeSurface(surf);
        return;
    }
    char path[600];
    settings_data_file(state, "spectravisual_export.bmp", path, sizeof(path));
    if (SDL_SaveBMP(surf, path) != 0) {
        snprintf(state->error_message, sizeof(state->error_message), "Could not save %s.", path);
    } else {
        snprintf(state->status_message, sizeof(state->status_message), "Exported %s", path);
    }
    SDL_FreeSurface(surf);
}

int main(int argc, char *argv[])
{
    /* AppState contains the fixed-capacity assignment/project buffers.  It is
       intentionally static: multi-Hamiltonian snapshots make it several MB,
       which can overflow macOS's process stack before SDL has even opened a
       window. Static storage is zero-initialized just like the old `{0}`. */
    static AppState state;
    init_app_defaults(&state);

    int argi = 1;
    if (argc > 1 && (strcmp(argv[1], "--verbose") == 0 || strcmp(argv[1], "-v") == 0)) {
        state.verbose = 1;
        argi++;
    } else {
        freopen("/dev/null", "w", stdout);
    }

    // Accept any number of spectrum files and an optional .cat prediction file,
    // in any order. Files are classified by their .cat extension.
    const char *spec_args[MAX_SPECTRA];
    int n_spec_args = 0;
    const char *pred_arg = NULL;
    const char *xiam_input_arg = NULL;
    const char *xiam_output_arg = NULL;
    const char *session_arg = NULL;
    for (int k = argi; k < argc; k++) {
        int len = (int)strlen(argv[k]);
        const char *base = strrchr(argv[k], '/');
        base = base ? base + 1 : argv[k];
        /* A session file is a project, not a two-column trace: it is opened
           below, the way dropping it on the window opens it. */
        if (strcmp(base, "spectravisual.state") == 0) { session_arg = argv[k]; continue; }
        if (len >= 4 && strcasecmp(argv[k] + len - 4, ".cat") == 0) pred_arg = argv[k];
        else if (len >= 3 && strcasecmp(argv[k] + len - 3, ".xi") == 0) xiam_input_arg = argv[k];
        else if (len >= 3 && strcasecmp(argv[k] + len - 3, ".xo") == 0) xiam_output_arg = argv[k];
        else if (n_spec_args < MAX_SPECTRA) spec_args[n_spec_args++] = argv[k];
    }

    /* Startup is deliberately a blank workspace. A prior session is restored
       only when the user names one, never merely because a .fit directory
       happens to be beside the executable. */
    settings_init(&state, argv[0]);
    settings_apply_defaults(&state);
    /* A session names the workspace it belongs to: everything Pred&Fit reads
       back - the catalogues, the Pickett files - lives in the .fit beside it,
       which is not necessarily the configured data directory. */
    if (session_arg) adopt_session_data_dir(&state, session_arg);
    predfit_refresh_work_dir(&state);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    if (TTF_Init() != 0) return 1;

    SDL_Window *win = SDL_CreateWindow("SpectraVisual", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1400, 884, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_SetWindowMinimumSize(win, 900, 560);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // On a Retina display the drawable is larger than the window. Draw in window
    // (logical) coordinates but let the renderer scale to the real pixel grid,
    // and open the fonts at the same factor: text and hairlines are then
    // rendered at native density instead of being blown up from a 1x bitmap.
    // main_dpi tracks the main window's own renderer scale; ui_dpi is the one
    // shared font cache's scale, which follows whichever window (main,
    // Pred&Fit Advanced or XIAM) currently has focus - see the loop below.
    float main_dpi = 1.0f, ui_dpi = 1.0f;
    {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &dw, &dh);
        if (ww > 0 && dw > 0) main_dpi = (float)dw / (float)ww;
        ui_dpi = main_dpi;
        SDL_RenderSetScale(ren, main_dpi, main_dpi);
    }
    if (!ui_fonts_init(ui_dpi)) { fprintf(stderr, "No font found.\n"); return 1; }
    if (!plotgpu_init(ren)) fprintf(stderr, "Plot renderer unavailable; spectra will not be drawn.\n");
    TTF_Font *font = ui_font(UI_FONT_SANS);

    /* The session comes first: it replaces the working set, so anything else
       named on the command line is opened on top of what it restored.  Its own
       catalogue is queued, so a .cat given here supersedes it - naming both
       means "this session, but that prediction". */
    if (session_arg) reopen_predfit_session(&state);
    if (pred_arg) app_enqueue_pending_load(&state, PENDING_LOAD_CATALOG, pred_arg, 0);
    if (xiam_input_arg) app_enqueue_pending_load(&state, PENDING_LOAD_XIAM_INPUT, xiam_input_arg, 0);
    if (xiam_output_arg) app_enqueue_pending_load(&state, PENDING_LOAD_XIAM_OUTPUT, xiam_output_arg, 0);
    for (int k = 0; k < n_spec_args; k++) add_spectrum(&state, spec_args[k]);
    /* Command-line files other than a session are a fresh task: they never
       cause a hidden restore or overwrite an existing Pred&Fit session. */
    
    int running = 1;
    Layout layout;

    while(running) {
        int w, h; SDL_GetWindowSize(win, &w, &h);
        {   // the main window can be moved to a display with a different
            // density; its own renderer always tracks its own window.
            int dw = 0, dh = 0;
            SDL_GetRendererOutputSize(ren, &dw, &dh);
            float now = (w > 0 && dw > 0) ? (float)dw / (float)w : 1.0f;
            if (fabs(now - main_dpi) > 0.01f) {
                main_dpi = now;
                SDL_RenderSetScale(ren, main_dpi, main_dpi);
            }
        }
        {   // Pred&Fit Advanced and XIAM are separate windows that can each
            // sit on their own display too, but every window draws text
            // through the one shared font cache below. Follow whichever
            // window currently has focus so its own proportions are the
            // ones that come out right, instead of always the main
            // window's - that was the bug: moving Advanced/XIAM to a
            // second monitor left their text sized for the main window's
            // density, out of step with their own (correctly adaptive)
            // render scale.
            SDL_Window *focused = win; SDL_Renderer *focused_ren = ren;
            PredFitState *pf = &state.predfit; XiamState *xi = &state.xiam;
            if (pf->advanced_window && (SDL_GetWindowFlags(pf->advanced_window) & SDL_WINDOW_INPUT_FOCUS)) {
                focused = pf->advanced_window; focused_ren = pf->advanced_renderer;
            } else if (xi->window && (SDL_GetWindowFlags(xi->window) & SDL_WINDOW_INPUT_FOCUS)) {
                focused = xi->window; focused_ren = xi->renderer;
            }
            int fw = 0, fh = 0, dw = 0, dh = 0;
            SDL_GetWindowSize(focused, &fw, &fh);
            SDL_GetRendererOutputSize(focused_ren, &dw, &dh);
            float now = (fw > 0 && dw > 0) ? (float)dw / (float)fw : 1.0f;
            if (fabs(now - ui_dpi) > 0.01f) {
                ui_dpi = now;
                ui_fonts_close();
                ui_fonts_init(ui_dpi);
                font = ui_font(UI_FONT_SANS);
            }
        }
        app_compute_layout(&state, &layout, w, h);

        handle_app_events(&state, &layout, &running);
        process_pending_loads(&state);
        restore_session_view(&state);
        predfit_render_advanced(&state);
        xiam_render(&state);
        intensity_analysis_poll(&state);
        intensity_analysis_render(&state);
        settings_render(&state);
        if(state.pending_select >= 0) { select_spectrum(&state, state.pending_select); state.pending_select = -1; state.predfit.session_dirty = 1; }
        if(state.pending_remove >= 0) { remove_spectrum(&state, state.pending_remove); state.pending_remove = -1; state.predfit.session_dirty = 1; }

        // Keep the active spectrum in sync with the mirror fields the tools edit.
        app_sync_view_state(&state);

        render_app(ren, font, &state, &layout);
        if (state.export_requested) {
            state.export_requested = 0;
            // Redraw without presenting: the back buffer then holds exactly the
            // frame the user is looking at.
            render_app_frame(ren, font, &state, &layout);
            save_screenshot(ren, &state);
            SDL_RenderPresent(ren);
        }
    }

    /* Quitting no longer writes the session: Save session in Pred&Fit
       Advanced is the only route that overwrites spectravisual.state. */
    free_dataset(&state);
    plotgpu_shutdown();
    settings_dispose(&state);
    intensity_analysis_dispose(&state);
    predfit_dispose(&state);
    xiam_dispose(&state);
    ui_fonts_close();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
