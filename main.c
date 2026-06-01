#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "loader.h"
#include "algorithms.h"
#include "layout.h"
#include "view.h"
#include "controller.h"

static void init_app_defaults(AppState *state) {
    state->pred_scale = 1.0;
    state->sync_active = 1;
    state->lorentz_gamma = 0.5;
    state->pf_sig_pts = 5;
    state->pf_noise_pts = 50;
    state->pf_thresh = 3.0;
    state->rolling_avg_window = 10;
    state->pred_min_log_int = -10.0;
    state->pred_max_log_int = 0.0;
    state->selected_assignment = -1;

    state->win_pf = (DraggableWindow){{100, 100, 300, 300}, 0, "PEAK FINDER"};
    state->win_br = (DraggableWindow){{150, 150, 300, 200}, 0, "BROADENING"};
    state->win_as = (DraggableWindow){{200, 200, 600, 400}, 0, "ASSIGNMENTS"};
    state->win_avg = (DraggableWindow){{250, 150, 300, 200}, 0, "ROLLING AVG"};
    state->win_cut = (DraggableWindow){{350, 250, 250, 160}, 0, "INTENSITY RANGE"};
    state->win_jump = (DraggableWindow){{400, 300, 250, 140}, 0, "FREQ JUMP"};
}

static void free_dataset(AppState *state) {
    free(state->raw_pts);
    free(state->smooth_pts);
    free(state->pred_lines);
    free(state->lin_data);

    state->raw_pts = NULL;
    state->smooth_pts = NULL;
    state->current_pts = NULL;
    state->pred_lines = NULL;
    state->lin_data = NULL;
    state->n_pts = 0;
    state->n_pred = 0;
    state->n_lin_data = 0;
    state->n_peaks = 0;
    state->n_selected = 0;
    state->n_assignments = 0;
    state->data_loaded = 0;
}

static int load_dataset(AppState *state, const char *exp_path, const char *pred_path) {
    Point *raw = NULL;
    Point *smooth = NULL;
    PredLine *pred = NULL;
    double *lin = NULL;
    double xmin, xmax, ymin, ymax;
    double pxmin, pxmax, pred_global_max;

    int n_pred = read_pred_cat_alloc(pred_path, &pred, &pxmin, &pxmax, &pred_global_max);
    if (n_pred <= 0 || !pred) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Could not load predictions: %s", pred_path);
        return 0;
    }

    int n_pts = read_data_alloc(exp_path, &raw, &xmin, &xmax, &ymin, &ymax);
    if (n_pts <= 0 || !raw) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Could not load spectrum: %s", exp_path);
        free(pred);
        return 0;
    }

    smooth = malloc(sizeof(Point) * n_pts);
    lin = malloc(sizeof(double) * MAX_LIN_POINTS);
    if (!smooth || !lin) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Not enough memory for loaded dataset.");
        free(raw);
        free(smooth);
        free(pred);
        free(lin);
        return 0;
    }

    free_dataset(state);
    state->raw_pts = raw;
    state->smooth_pts = smooth;
    state->current_pts = raw;
    state->pred_lines = pred;
    state->lin_data = lin;
    state->n_pred = n_pred;
    state->n_pts = n_pts;
    state->xmin = xmin; state->xmax = xmax; state->ymin = ymin; state->ymax = ymax;
    state->pxmin = pxmin; state->pxmax = pxmax;
    state->pred_global_max = pred_global_max;

    state->vxmin = state->xmin; state->vxmax = state->xmax;
    state->vymin = state->ymin; state->vymax = state->ymax;
    state->pvxmin = state->xmin; state->pvxmax = state->xmax;
    state->bar_x = (state->xmin + state->xmax) / 2.0;
    state->pbar_x = state->bar_x;
    state->data_loaded = 1;
    state->error_message[0] = '\0';
    snprintf(state->status_message, sizeof(state->status_message),
             "Loaded %d spectrum points and %d predicted lines.", n_pts, n_pred);
    snprintf(state->exp_path, sizeof(state->exp_path), "%s", exp_path);
    snprintf(state->pred_path, sizeof(state->pred_path), "%s", pred_path);

    load_existing_assignments("assignments.txt", state->assignments, &state->n_assignments);

    char assigned_freq_file[512];
    if (find_assigned_frequency_file(assigned_freq_file, sizeof(assigned_freq_file))) {
        state->n_lin_data = read_assigned_frequencies(assigned_freq_file, state->lin_data, MAX_LIN_POINTS);
    }

    if (state->verbose) {
        fprintf(stderr, "%s\n", state->status_message);
    }
    return 1;
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

    const char *exp_arg = NULL;
    const char *pred_arg = NULL;
    if (argc - argi == 2) {
        exp_arg = argv[argi];
        pred_arg = argv[argi + 1];
    } else if (argc - argi != 0) {
        fprintf(stderr, "Usage: %s [--verbose] [spectrum.txt pred.cat]\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    if (TTF_Init() != 0) return 1;

    SDL_Window *win = SDL_CreateWindow("SpectraVisual", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    
    TTF_Font *font = TTF_OpenFont("/Users/filippobaroncelli/Library/Fonts/Aptos-Mono.ttf",14);
    if (!font) {
        font = TTF_OpenFont("/System/Library/Fonts/Helvetica.ttc", 14); 
        if(!font) font = TTF_OpenFont("arial.ttf", 14);
    }
    if(!font) { fprintf(stderr, "No font found.\n"); return 1; }

    if (exp_arg && pred_arg) {
        load_dataset(&state, exp_arg, pred_arg);
    }
    
    int running = 1;
    Layout layout;

    while(running) {
        int w, h; SDL_GetWindowSize(win, &w, &h);
        layout.win_w = w; layout.win_h = h;
        layout.plot_x = (w < 560) ? 56 : ((w < 760) ? 64 : 78);
        layout.gap = (h < 520) ? 42 : 54;
        
        int usable_h = h - 132;
        if (usable_h < 220) usable_h = 220;
        layout.exp_h = usable_h * 0.6;
        layout.pred_h = usable_h * 0.4 - layout.gap;
        if (layout.pred_h < 70) layout.pred_h = 70;
        
        layout.exp_x = layout.plot_x; layout.exp_y = 72;
        layout.exp_w = w - layout.plot_x - 25;
        if (layout.exp_w < 240) layout.exp_w = 240;
        
        layout.pred_x = layout.plot_x; layout.pred_y = layout.exp_y + layout.exp_h + layout.gap;
        layout.pred_w = layout.exp_w;

        handle_app_events(&state, &layout, &running);
        if(state.pending_load) {
            state.pending_load = 0;
            if (state.exp_path[0] && state.pred_path[0]) {
                load_dataset(&state, state.exp_path, state.pred_path);
            }
        }
        if(state.data_loaded && state.sync_active) { state.pvxmin = state.vxmin; state.pvxmax = state.vxmax; }

        render_app(ren, font, &state, &layout);
        if (state.export_requested) {
            state.export_requested = 0;
            save_screenshot(ren, &state);
        }
    }

    free_dataset(&state);
    if(font) TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
