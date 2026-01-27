#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "types.h"
#include "loader.h"
#include "algorithms.h"
#include "layout.h"
#include "view.h"
#include "controller.h"

int main(int argc, char *argv[])
{
    if (argc!=3) {
        fprintf(stderr,"Usage: %s exp.csv pred.cat\n", argv[0]);
        return 1;
    }

    // --- 1. INITIALIZATION ---
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    if (TTF_Init() != 0) return 1;

    SDL_Window *win = SDL_CreateWindow("Spectral Analysis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 700, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    
    // Attempt Font Load
    TTF_Font *font = TTF_OpenFont("/Users/filippobaroncelli/Library/Fonts/Aptos-Mono.ttf",14);
    if (!font) {
        printf("Custom font not found, trying system fonts...\n");
        font = TTF_OpenFont("/System/Library/Fonts/Helvetica.ttc", 14); 
        if(!font) font = TTF_OpenFont("arial.ttf", 14);
    }
    if(!font) { fprintf(stderr, "No font found! Exiting.\n"); return 1; }

    // --- 2. STATE SETUP ---
    AppState state = {0};
    state.raw_pts = malloc(sizeof(Point)*MAXPTS);
    state.smooth_pts = malloc(sizeof(Point)*MAXPTS);
    state.current_pts = state.raw_pts;
    state.pred_lines = malloc(sizeof(PredLine)*MAXPTS);
    
    // Load Data
    printf("Loading data...\n");
    state.n_pred = read_pred_cat(argv[2], state.pred_lines, MAXPTS, &state.pxmin, &state.pxmax, &state.pred_global_max);
    state.n_pts = read_data(argv[1], state.raw_pts, MAXPTS, &state.xmin, &state.xmax, &state.ymin, &state.ymax);
    load_existing_assignments("assignments.txt", state.assignments, &state.n_assignments);

    // Defaults
    state.vxmin = state.xmin; state.vxmax = state.xmax;
    state.vymin = state.ymin; state.vymax = state.ymax;
    state.pvxmin = state.xmin; state.pvxmax = state.xmax;
    state.pred_scale = 1.0;
    state.sync_active = 1;
    state.bar_x = (state.xmin+state.xmax)/2;
    state.pbar_x = state.bar_x;
    state.lorentz_gamma = 0.5;
    state.pf_sig_pts = 5; state.pf_noise_pts = 50; state.pf_thresh = 3.0;
    state.rolling_avg_window = 10;
    state.measure_active = 0;  // Set to 0 for OFF, 1 for ON
    // NEW: Initialize Intensity Cut defaults
    state.pred_min_log_int = -10.0; // Default min
    state.pred_max_log_int = 0.0;   // Default max
    // Window Inits
    state.win_pf = (DraggableWindow){{100, 100, 300, 300}, 0, "PEAK FINDER"};
    state.win_br = (DraggableWindow){{150, 150, 300, 200}, 0, "BROADENING"};
    state.win_as = (DraggableWindow){{200, 200, 600, 400}, 0, "ASSIGNMENTS"};
    state.win_avg = (DraggableWindow){{250, 150, 300, 200}, 0, "ROLLING AVG"};
    state.win_cut = (DraggableWindow){{350, 250, 250, 160}, 0, "INTENSITY RANGE"};

    // NEW: Initialize the Cut Window Position and Size!
    state.win_cut = (DraggableWindow){{350, 250, 250, 160}, 0, "INTENSITY RANGE"};
    // NEW: Initialize Frequency Jump Window
    state.win_jump = (DraggableWindow){{400, 300, 250, 140}, 0, "FREQ JUMP"};
    // --- 3. MAIN LOOP ---
    int running = 1;
    Layout layout;

    while(running) {
        // Calculate Layout based on current window size
        int w, h; SDL_GetWindowSize(win, &w, &h);
        layout.win_w = w; layout.win_h = h;
        layout.plot_x = 80; layout.gap = 25;
        
        // Split screen: 2/3 Exp, 1/3 Pred (adjustable later)
        int usable_h = h - 100;
        layout.exp_h = usable_h * 0.6;
        layout.pred_h = usable_h * 0.4 - layout.gap;
        
        layout.exp_x = layout.plot_x; layout.exp_y = 60;
        layout.exp_w = w - 100;
        
        layout.pred_x = layout.plot_x; layout.pred_y = layout.exp_y + layout.exp_h + layout.gap;
        layout.pred_w = layout.exp_w;

        // Controller
        handle_app_events(&state, &layout, &running);
        if(state.sync_active) { state.pvxmin = state.vxmin; state.pvxmax = state.vxmax; }

        // View
        render_app(ren, font, &state, &layout);
    }

    // --- 4. CLEANUP ---
    free(state.raw_pts); free(state.smooth_pts); free(state.pred_lines);
    if(font) TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}