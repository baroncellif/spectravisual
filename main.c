#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "loader.h"
#include "algorithms.h"
#include "layout.h"
#include "ui_theme.h"
#include "view.h"
#include "controller.h"

static void init_app_defaults(AppState *state) {
    state->pred_scale = 1.0;
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
    state->win_spec = (DraggableWindow){{0, 0, UI_INSPECTOR_W, 360}, 0, "Spectra"};

    state->n_spectra = 0;
    state->active_spec = -1;
    state->multi_layout = 0;   // overlay
    state->multi_ynorm = 0;    // shared Y
    state->multi_indiv_int = 0; // intensity controls affect all spectra
    state->pending_select = -1;
    state->pending_remove = -1;
}

// Distinct colors auto-assigned to spectra as they are loaded.
static const SDL_Color SPEC_PALETTE[MAX_SPECTRA] = {
    {205, 214, 225, 235},  // light grey-blue (matches the original single-trace)
    { 90, 200, 250, 235},  // cyan
    {255, 170,  80, 235},  // orange
    {130, 220, 130, 235},  // green
    {235, 130, 200, 235},  // pink
    {245, 220,  90, 235},  // yellow
    {170, 150, 245, 235},  // violet
    {240, 110, 110, 235},  // red
};

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

static void select_spectrum(AppState *state, int idx) {
    if (idx < 0 || idx >= state->n_spectra) return;
    commit_active(state);
    state->active_spec = idx;
    state->n_peaks = 0;        // peaks/selection belong to a single spectrum
    state->n_selected = 0;
    mirror_active(state);
}

static void ensure_aux_loaded(AppState *state) {
    if (state->lin_data) return;
    state->lin_data = malloc(sizeof(double) * MAX_LIN_POINTS);
    if (!state->lin_data) return;
    load_existing_assignments("assignments.txt", state->assignments, &state->n_assignments);
    char f[512];
    if (find_assigned_frequency_file(f, sizeof(f)))
        state->n_lin_data = read_assigned_frequencies(f, state->lin_data, MAX_LIN_POINTS);
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
    free(state->lin_data);
    state->pred_lines = NULL;
    state->lin_data = NULL;
    state->raw_pts = NULL;
    state->smooth_pts = NULL;
    state->current_pts = NULL;
    state->n_pts = 0;
    state->n_pred = 0;
    state->n_lin_data = 0;
    state->n_peaks = 0;
    state->n_selected = 0;
    state->n_assignments = 0;
    state->data_loaded = 0;
}

// Load an experimental spectrum and append it to the store (becomes active).
static int add_spectrum(AppState *state, const char *path) {
    if (state->n_spectra >= MAX_SPECTRA) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Maximum of %d spectra already loaded.", MAX_SPECTRA);
        return 0;
    }
    Point *raw = NULL;
    double xmin, xmax, ymin, ymax;
    int n = read_data_alloc(path, &raw, &xmin, &xmax, &ymin, &ymax);
    if (n <= 0 || !raw) {
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
    sp->color = SPEC_PALETTE[idx % MAX_SPECTRA];
    snprintf(sp->path, sizeof(sp->path), "%s", path);
    spec_basename(path, sp->name, sizeof(sp->name));

    int first = !state->data_loaded;
    state->active_spec = idx;
    state->n_peaks = 0; state->n_selected = 0;
    mirror_active(state);
    state->data_loaded = 1;
    ensure_aux_loaded(state);

    if (first) {
        state->vxmin = xmin; state->vxmax = xmax;
        state->vymin = ymin; state->vymax = ymax;
        state->pvxmin = xmin; state->pvxmax = xmax;
        state->bar_x = (xmin + xmax) / 2.0; state->pbar_x = state->bar_x;
        if (state->n_pred == 0) { state->pxmin = xmin; state->pxmax = xmax; }
    }
    snprintf(state->status_message, sizeof(state->status_message),
             "Loaded spectrum '%s' (%d pts). %d spectra loaded.", sp->name, n, state->n_spectra);
    state->error_message[0] = '\0';
    if (state->verbose) fprintf(stderr, "%s\n", state->status_message);
    return 1;
}

// Load (replace) the prediction set.
static int set_predictions(AppState *state, const char *path) {
    PredLine *pred = NULL;
    double pxmin, pxmax, pgmax;
    int n = read_pred_cat_alloc(path, &pred, &pxmin, &pxmax, &pgmax);
    if (n <= 0 || !pred) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Could not load predictions: %s", path);
        return 0;
    }
    free(state->pred_lines);
    state->pred_lines = pred;
    state->n_pred = n;
    state->pxmin = pxmin; state->pxmax = pxmax;
    state->pred_global_max = pgmax;
    snprintf(state->pred_path, sizeof(state->pred_path), "%s", path);

    int first = !state->data_loaded;
    state->data_loaded = 1;
    ensure_aux_loaded(state);
    if (first) {
        state->xmin = pxmin; state->xmax = pxmax; state->ymin = 0.0; state->ymax = 1.0;
        state->vxmin = pxmin; state->vxmax = pxmax; state->vymin = 0.0; state->vymax = 1.0;
        state->pvxmin = pxmin; state->pvxmax = pxmax;
        state->bar_x = (pxmin + pxmax) / 2.0; state->pbar_x = state->bar_x;
    }
    snprintf(state->status_message, sizeof(state->status_message),
             "Loaded %d predicted lines.", n);
    state->error_message[0] = '\0';
    if (state->verbose) fprintf(stderr, "%s\n", state->status_message);
    return 1;
}

static void remove_spectrum(AppState *state, int idx) {
    if (idx < 0 || idx >= state->n_spectra) return;
    free(state->spectra[idx].raw_pts);
    free(state->spectra[idx].smooth_pts);
    for (int i = idx; i < state->n_spectra - 1; i++)
        state->spectra[i] = state->spectra[i + 1];
    state->n_spectra--;
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
    if (SDL_SaveBMP(surf, "spectravisual_export.bmp") != 0) {
        snprintf(state->error_message, sizeof(state->error_message), "Could not save spectravisual_export.bmp.");
    } else {
        snprintf(state->status_message, sizeof(state->status_message), "Exported spectravisual_export.bmp");
    }
    SDL_FreeSurface(surf);
}

int main(int argc, char *argv[])
{
    AppState state = {0};
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
    for (int k = argi; k < argc; k++) {
        int len = (int)strlen(argv[k]);
        if (len >= 4 && strcmp(argv[k] + len - 4, ".cat") == 0) pred_arg = argv[k];
        else if (n_spec_args < MAX_SPECTRA) spec_args[n_spec_args++] = argv[k];
    }

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
    float ui_dpi = 1.0f;
    {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &dw, &dh);
        if (ww > 0 && dw > 0) ui_dpi = (float)dw / (float)ww;
        SDL_RenderSetScale(ren, ui_dpi, ui_dpi);
    }
    if (!ui_fonts_init(ui_dpi)) { fprintf(stderr, "No font found.\n"); return 1; }
    TTF_Font *font = ui_font(UI_FONT_SANS);

    if (pred_arg) set_predictions(&state, pred_arg);
    for (int k = 0; k < n_spec_args; k++) add_spectrum(&state, spec_args[k]);
    
    int running = 1;
    Layout layout;

    while(running) {
        int w, h; SDL_GetWindowSize(win, &w, &h);
        layout.win_w = w; layout.win_h = h;
        layout.plot_x = UI_RAIL_W + UI_PLOT_GUTTER;
        layout.gap = UI_PANEL_HEADER_H;

        int content_top    = UI_CONTENT_Y;
        int content_bottom = h - UI_STATUS_H;

        // Dock the inspector first: it decides how much width is left.
        update_sidebars(&state, &layout);

        int has_exp  = (state.n_spectra > 0);
        int has_pred = (state.n_pred > 0);
        int avail = content_bottom - content_top;
        if (avail < 200) avail = 200;

        // Each pane carries a header; the shared frequency axis is drawn once,
        // under the bottom pane.
        if (has_exp && has_pred) {
            int usable = avail - 2 * UI_PANEL_HEADER_H - UI_PRED_AXIS_H;
            if (usable < 120) usable = 120;
            layout.exp_h  = (int)(usable * 0.62);
            layout.pred_h = usable - layout.exp_h;
            layout.exp_y  = content_top + UI_PANEL_HEADER_H;
            layout.pred_y = layout.exp_y + layout.exp_h + UI_PANEL_HEADER_H;
        } else if (has_pred) {
            layout.exp_h  = 0;
            layout.pred_h = avail - UI_PANEL_HEADER_H - UI_PRED_AXIS_H;
            layout.exp_y  = content_top + UI_PANEL_HEADER_H;
            layout.pred_y = content_top + UI_PANEL_HEADER_H;
        } else {
            layout.exp_h  = avail - UI_PANEL_HEADER_H - UI_PRED_AXIS_H;
            layout.pred_h = 0;
            layout.exp_y  = content_top + UI_PANEL_HEADER_H;
            layout.pred_y = layout.exp_y;
        }
        if (layout.exp_h  < 0) layout.exp_h  = 0;
        if (layout.pred_h < 0) layout.pred_h = 0;

        layout.exp_x = layout.plot_x;
        layout.exp_w = layout.plot_right - layout.exp_x - 16;
        if (layout.exp_w < 240) layout.exp_w = 240;
        layout.pred_x = layout.plot_x;
        layout.pred_w = layout.exp_w;

        handle_app_events(&state, &layout, &running);
        if(state.pending_load) {
            state.pending_load = 0;
            if (state.pending_pred_path[0]) { set_predictions(&state, state.pending_pred_path); state.pending_pred_path[0] = '\0'; }
            if (state.pending_spec_path[0]) { add_spectrum(&state, state.pending_spec_path); state.pending_spec_path[0] = '\0'; }
        }
        if(state.pending_select >= 0) { select_spectrum(&state, state.pending_select); state.pending_select = -1; }
        if(state.pending_remove >= 0) { remove_spectrum(&state, state.pending_remove); state.pending_remove = -1; }

        // Keep the active spectrum in sync with the mirror fields the tools edit.
        commit_active(&state);

        if(state.data_loaded && state.sync_active) { state.pvxmin = state.vxmin; state.pvxmax = state.vxmax; }

        render_app(ren, font, &state, &layout);
        if (state.export_requested) {
            state.export_requested = 0;
            save_screenshot(ren, &state);
        }
    }

    free_dataset(&state);
    ui_fonts_close();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
