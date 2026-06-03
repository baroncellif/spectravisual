#include "controller.h"
#include "algorithms.h"
#include "loader.h"
#include "layout.h"
#include <SDL.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define ABS(x) ((x)<0?-(x):(x))
#define UI_TOOLBAR_Y 28

// --- INTERNAL HELPERS ---
static void handle_keydown(AppState *s, Layout *l, SDL_KeyboardEvent *key);
static void handle_text_input_event(AppState *s, char *text);
static void handle_mouse_down(AppState *s, Layout *l, SDL_MouseButtonEvent *b);
static void handle_mouse_up(AppState *s, Layout *l, SDL_MouseButtonEvent *b);
static void handle_mouse_motion(AppState *s, Layout *l, SDL_MouseMotionEvent *m);
static void handle_mouse_wheel(AppState *s, Layout *l, SDL_MouseWheelEvent *w);
static void run_right_click_peak_find(AppState *s, double x0, double x1);
static void assign_selected_predictions(AppState *s, double exp_freq, double exp_int);
static void delete_assignment(AppState *s, int idx);
static void clamp_assignment_scroll(AppState *s);
static void commit_text_input(AppState *s);
static int path_looks_like_cat(const char *path);

// --- MAIN EVENT LOOP ---
void handle_app_events(AppState *state, Layout *l, int *running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { *running = 0; return; }

        if (state->input_state != INPUT_NONE) {
            if (e.type == SDL_TEXTINPUT) {
                handle_text_input_event(state, e.text.text);
                continue;
            } 
            else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    size_t len = strlen(state->text_input_buf);
                    if (len > 0) state->text_input_buf[len-1] = '\0';
                    continue;
                }
                else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    state->input_state = INPUT_NONE;
                    SDL_StopTextInput();
                    continue;
                }
                else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    commit_text_input(state);
                    continue;
                }
                continue;
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) {
                commit_text_input(state);
            }
        }

        switch (e.type) {
            case SDL_DROPFILE: {
                char *path = e.drop.file;
                if (path_looks_like_cat(path)) snprintf(state->pred_path, sizeof(state->pred_path), "%s", path);
                else snprintf(state->exp_path, sizeof(state->exp_path), "%s", path);
                if (state->exp_path[0] && state->pred_path[0]) state->pending_load = 1;
                else snprintf(state->status_message, sizeof(state->status_message),
                              "Drop both a spectrum file and a .cat file.");
                SDL_free(path);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: handle_mouse_down(state, l, &e.button); break;
            case SDL_MOUSEBUTTONUP:   handle_mouse_up(state, l, &e.button); break;
            case SDL_MOUSEMOTION:     handle_mouse_motion(state, l, &e.motion); break;
            case SDL_MOUSEWHEEL:      handle_mouse_wheel(state, l, &e.wheel); break;
            case SDL_KEYDOWN:         handle_keydown(state, l, &e.key); break;
        }
    }
}

// --- MOUSE DOWN (Hit Testing) ---
static void handle_mouse_down(AppState *s, Layout *l, SDL_MouseButtonEvent *b) {
    int mx = b->x; 
    int my = b->y;
    int handled = 0;
    (void)handled;

    // 1. Check Floating Windows
    // Helper macro to reduce boilerplate for window drag checks
    #define CHECK_WIN_DRAG(win) \
        if ((win).visible && point_in_rect(mx, my, (win).rect)) { \
            handled = 1; \
            if (my < (win).rect.y + 30) { \
                if (mx > (win).rect.x + (win).rect.w - 30) { (win).visible = 0; } \
                else { s->drag_target = &(win); s->drag_offset.x = mx - (win).rect.x; s->drag_offset.y = my - (win).rect.y; } \
                return; \
            } \
        }

    // NEW: Intensity Cut Window
        if (s->win_cut.visible && point_in_rect(mx, my, s->win_cut.rect)) {
            handled = 1;
            if (my < s->win_cut.rect.y + 30) {
                if (mx > s->win_cut.rect.x + s->win_cut.rect.w - 30) s->win_cut.visible = 0;
                else { s->drag_target = &s->win_cut; s->drag_offset.x = mx - s->win_cut.rect.x; s->drag_offset.y = my - s->win_cut.rect.y; }
                return;
            }
            SDL_Rect r_min = {s->win_cut.rect.x + 120, s->win_cut.rect.y + 50, 80, 28};
            SDL_Rect r_max = {s->win_cut.rect.x + 120, s->win_cut.rect.y + 90, 80, 28};
            if (point_in_rect(mx, my, r_min)) { s->input_state = INPUT_PRED_MIN; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%.1f", s->pred_min_log_int); } 
            else if (point_in_rect(mx, my, r_max)) { s->input_state = INPUT_PRED_MAX; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%.1f", s->pred_max_log_int); }
            return;
        }
    
    // NEW: Frequency Jump Window
    if (s->win_jump.visible && point_in_rect(mx, my, s->win_jump.rect)) {
        handled = 1;
        if (my < s->win_jump.rect.y + 30) {
            if (mx > s->win_jump.rect.x + s->win_jump.rect.w - 30) s->win_jump.visible = 0;
            else { s->drag_target = &s->win_jump; s->drag_offset.x = mx - s->win_jump.rect.x; s->drag_offset.y = my - s->win_jump.rect.y; }
            return;
        }
        SDL_Rect r_start = {s->win_jump.rect.x + 120, s->win_jump.rect.y + 50, 80, 28};
        SDL_Rect r_end   = {s->win_jump.rect.x + 120, s->win_jump.rect.y + 90, 80, 28};
        if (point_in_rect(mx, my, r_start)) { s->input_state = INPUT_JUMP_MIN; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%.1f", s->vxmin); } 
        else if (point_in_rect(mx, my, r_end)) { s->input_state = INPUT_JUMP_MAX; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%.1f", s->vxmax); }
        return;
    }

    // FILTER Window (quantum-number / branch gating)
    if (s->win_filt.visible && point_in_rect(mx, my, s->win_filt.rect)) {
        handled = 1;
        int wx = s->win_filt.rect.x, wy = s->win_filt.rect.y;
        if (my < wy + 30) {
            if (mx > wx + s->win_filt.rect.w - 30) s->win_filt.visible = 0;
            else { s->drag_target = &s->win_filt; s->drag_offset.x = mx - wx; s->drag_offset.y = my - wy; }
            return;
        }
        // Master enable
        if (point_in_rect(mx, my, (SDL_Rect){wx+15, wy+44, 230, 26})) { s->filter_active = !s->filter_active; return; }
        // Dipole (mu) toggles
        for (int i = 0; i < 3; i++)
            if (point_in_rect(mx, my, (SDL_Rect){wx+15+i*80, wy+98, 70, 26})) { s->filt_mu[i] = !s->filt_mu[i]; return; }
        // Branch toggles
        for (int i = 0; i < 3; i++)
            if (point_in_rect(mx, my, (SDL_Rect){wx+15+i*80, wy+152, 70, 26})) { s->filt_br[i] = !s->filt_br[i]; return; }
        // Range gate
        if (point_in_rect(mx, my, (SDL_Rect){wx+15, wy+190, 230, 26})) { s->filt_use_range = !s->filt_use_range; return; }
        // Range input fields (J / Ka / Kc, min / max)
        if (point_in_rect(mx, my, (SDL_Rect){wx+110, wy+236, 55, 24})) { s->input_state=INPUT_FILT_JMIN;  SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_j_min);  return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+180, wy+236, 55, 24})) { s->input_state=INPUT_FILT_JMAX;  SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_j_max);  return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+110, wy+266, 55, 24})) { s->input_state=INPUT_FILT_KAMIN; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_ka_min); return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+180, wy+266, 55, 24})) { s->input_state=INPUT_FILT_KAMAX; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_ka_max); return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+110, wy+296, 55, 24})) { s->input_state=INPUT_FILT_KCMIN; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_kc_min); return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+180, wy+296, 55, 24})) { s->input_state=INPUT_FILT_KCMAX; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_kc_max); return; }
        // Delta gate
        if (point_in_rect(mx, my, (SDL_Rect){wx+15, wy+332, 230, 26})) { s->filt_use_delta = !s->filt_use_delta; return; }
        // Delta input fields (dJ / dKa / dKc)
        if (point_in_rect(mx, my, (SDL_Rect){wx+44,  wy+364, 36, 24})) { s->input_state=INPUT_FILT_DJ;  SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_dj);  return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+124, wy+364, 36, 24})) { s->input_state=INPUT_FILT_DKA; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_dka); return; }
        if (point_in_rect(mx, my, (SDL_Rect){wx+204, wy+364, 36, 24})) { s->input_state=INPUT_FILT_DKC; SDL_StartTextInput(); snprintf(s->text_input_buf,32,"%d",s->filt_dkc); return; }
        return;
    }

    // A. Broadening Window
    if (s->win_br.visible && point_in_rect(mx, my, s->win_br.rect)) {
        handled = 1;
        // Header / Close logic
        if (my < s->win_br.rect.y + 30) {
            if (mx > s->win_br.rect.x + s->win_br.rect.w - 30) s->win_br.visible = 0;
            else { s->drag_target = &s->win_br; s->drag_offset.x = mx - s->win_br.rect.x; s->drag_offset.y = my - s->win_br.rect.y; }
            return;
        }
        // Content logic
        SDL_Rect r_in = {s->win_br.rect.x + 120, s->win_br.rect.y + 60, 80, 28};
        SDL_Rect r_tog = {s->win_br.rect.x + 50, s->win_br.rect.y + 120, 200, 30};
        
        if (point_in_rect(mx, my, r_in)) {
            s->input_state = INPUT_GAMMA; SDL_StartTextInput(); 
            snprintf(s->text_input_buf, 64, "%.2f", s->lorentz_gamma);
        } 
        else if (point_in_rect(mx, my, r_tog)) {
            s->broadening_active = !s->broadening_active;
        }
        return;
    }

    // B. Rolling Avg Window
    if (s->win_avg.visible && point_in_rect(mx, my, s->win_avg.rect)) {
        handled = 1;
        if (my < s->win_avg.rect.y + 30) {
            if (mx > s->win_avg.rect.x + s->win_avg.rect.w - 30) s->win_avg.visible = 0;
            else { s->drag_target = &s->win_avg; s->drag_offset.x = mx - s->win_avg.rect.x; s->drag_offset.y = my - s->win_avg.rect.y; }
            return;
        }
        SDL_Rect r_in = {s->win_avg.rect.x + 130, s->win_avg.rect.y + 60, 80, 28};
        SDL_Rect r_tog = {s->win_avg.rect.x + 50, s->win_avg.rect.y + 120, 200, 30};
        
        if (point_in_rect(mx, my, r_in)) {
            s->input_state = INPUT_AVG_PTS; SDL_StartTextInput();
            snprintf(s->text_input_buf, 64, "%d", s->rolling_avg_window);
        }
        else if (point_in_rect(mx, my, r_tog)) {
            s->rolling_avg_active = !s->rolling_avg_active;
            if(s->rolling_avg_active) {
                apply_rolling_average(s->raw_pts, s->smooth_pts, s->n_pts, s->rolling_avg_window);
                s->current_pts = s->smooth_pts;
            } else {
                s->current_pts = s->raw_pts;
            }
        }
        return;
    }

    // C. Peak Finder Window
    if (s->win_pf.visible && point_in_rect(mx, my, s->win_pf.rect)) {
        handled = 1;
        if (my < s->win_pf.rect.y + 30) {
            if (mx > s->win_pf.rect.x + s->win_pf.rect.w - 30) s->win_pf.visible = 0;
            else { s->drag_target = &s->win_pf; s->drag_offset.x = mx - s->win_pf.rect.x; s->drag_offset.y = my - s->win_pf.rect.y; }
            return;
        }
        
        int wx = s->win_pf.rect.x, wy = s->win_pf.rect.y;
        if(point_in_rect(mx, my, (SDL_Rect){wx+190, wy+50, 80, 26})) {
            s->input_state = INPUT_PF_SIG; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%d", s->pf_sig_pts);
        } else if(point_in_rect(mx, my, (SDL_Rect){wx+190, wy+90, 80, 26})) {
            s->input_state = INPUT_PF_NOISE; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%d", s->pf_noise_pts);
        } else if(point_in_rect(mx, my, (SDL_Rect){wx+190, wy+130, 80, 26})) {
            s->input_state = INPUT_PF_THRESH; SDL_StartTextInput(); snprintf(s->text_input_buf, 32, "%.1f", s->pf_thresh);
        } else if(point_in_rect(mx, my, (SDL_Rect){wx+50, wy+200, 200, 30})) {
            run_peak_finder(s->current_pts, s->n_pts, s->vxmin, s->vxmax, s->pf_sig_pts, s->pf_noise_pts, s->pf_thresh, s->peaks, &s->n_peaks);
        } else if(point_in_rect(mx, my, (SDL_Rect){wx+50, wy+240, 200, 30})) {
            FILE *fp = fopen("linelist.csv", "w");
            if(fp) {
                fprintf(fp, "Freq,Int\n");
                for(int k=0; k<s->n_peaks; k++) fprintf(fp, "%.6f,%.6e\n", s->peaks[k].x, s->peaks[k].y);
                fclose(fp);
                printf("Saved linelist.csv\n");
            }
        }
        return;
    }

    // D. Assignments Window
    if (s->win_as.visible && point_in_rect(mx, my, s->win_as.rect)) {
        handled = 1;
        if (my < s->win_as.rect.y + 30) {
            if (mx > s->win_as.rect.x + s->win_as.rect.w - 30) s->win_as.visible = 0;
            else { s->drag_target = &s->win_as; s->drag_offset.x = mx - s->win_as.rect.x; s->drag_offset.y = my - s->win_as.rect.y; }
            return;
        }
        clamp_assignment_scroll(s);
        int start_idx = s->assignments_scroll;
        int end_idx = start_idx + 13;
        if (end_idx > s->n_assignments) end_idx = s->n_assignments;
        for (int k = start_idx; k < end_idx; k++) {
            int row_y = s->win_as.rect.y + 90 + (k - start_idx) * 20;
            SDL_Rect row_rect = {s->win_as.rect.x + 15, row_y - 2, s->win_as.rect.w - 30, 18};
            if (point_in_rect(mx, my, row_rect)) {
                s->selected_assignment = k;
                return;
            }
        }
        if(point_in_rect(mx, my, (SDL_Rect){s->win_as.rect.x+10, s->win_as.rect.y+360, 100, 30})) {
             FILE *fp = fopen("assignments.txt", "w"); 
             if(fp) {
                 fprintf(fp, "# PredFreq(MHz)  Ju Kau Kcu M1u M2u M3u  Jl Kal Kcl M1l M2l M3l  ExpFreq(MHz) ExpInt\n");
                 for(int k=0; k<s->n_assignments; k++) {
                     PredLine p = s->assignments[k].pred;
                     fprintf(fp, "%12.4f  %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d   %12.4f %12.4e\n",
                         p.freq_mhz,
                         p.Ju, p.Kau, p.Kcu,p.M1u,p.M2u,p.M3u, p.Jl, p.Kal, p.Kcl,p.M1l,p.M2l,p.M3l,
                         s->assignments[k].exp_freq, s->assignments[k].exp_int);
                 }
                 fclose(fp);
                 printf("Saved assignments.txt\n");
             }
        }
        if(point_in_rect(mx, my, (SDL_Rect){s->win_as.rect.x+120, s->win_as.rect.y+360, 110, 30})) {
            delete_assignment(s, s->selected_assignment);
        }
        return;
    }
    // --- NEW: Intensity Cut Window ---
    if (s->win_cut.visible && point_in_rect(mx, my, s->win_cut.rect)) {
        handled = 1;
        // Header (Drag/Close)
        if (my < s->win_cut.rect.y + 30) {
            if (mx > s->win_cut.rect.x + s->win_cut.rect.w - 30) s->win_cut.visible = 0;
            else { s->drag_target = &s->win_cut; s->drag_offset.x = mx - s->win_cut.rect.x; s->drag_offset.y = my - s->win_cut.rect.y; }
            return;
        }

        // Content (Input Fields) - Coordinates match view.c
        SDL_Rect r_min = {s->win_cut.rect.x + 120, s->win_cut.rect.y + 50, 80, 28};
        SDL_Rect r_max = {s->win_cut.rect.x + 120, s->win_cut.rect.y + 90, 80, 28};

        if (point_in_rect(mx, my, r_min)) {
            s->input_state = INPUT_PRED_MIN; 
            SDL_StartTextInput(); 
            snprintf(s->text_input_buf, 32, "%.1f", s->pred_min_log_int);
        } 
        else if (point_in_rect(mx, my, r_max)) {
            s->input_state = INPUT_PRED_MAX; 
            SDL_StartTextInput(); 
            snprintf(s->text_input_buf, 32, "%.1f", s->pred_max_log_int);
        }
        return;
    }
    // 2. Toolbar Buttons
    // (We reconstruct rects to match view.c)
    int by = UI_TOOLBAR_Y + 5;
    int right_x = l->win_w - 18;
    Button btn_bar  = {{12,  by, 58, 24}, "", {0,0,0,0}, 0};
    Button btn_sync = {{76,  by, 70, 24}, "", {0,0,0,0}, 0};
    Button btn_del  = {{156, by, 58, 24}, "", {0,0,0,0}, 0};
    Button btn_list = {{236, by, 64, 24}, "", {0,0,0,0}, 0};
    Button btn_peak = {{306, by, 68, 24}, "", {0,0,0,0}, 0};
    Button btn_roll = {{380, by, 62, 24}, "", {0,0,0,0}, 0};
    Button btn_broad = {{448, by, 82, 24}, "", {0,0,0,0}, 0};
    Button btn_cut = {{552, by, 58, 24}, "", {0,0,0,0}, 0};
    Button btn_jump = {{616, by, 78, 24}, "", {0,0,0,0}, 0};
    Button btn_filt = {{700, by, 72, 24}, "", {0,0,0,0}, 0};
    SDL_Rect r_off = {right_x - 122, by, 122, 24};
    right_x = r_off.x - 64;
    Button btn_export = {{right_x - 88, by, 82, 24}, "", {0,0,0,0}, 0};
    Button btn_help = {{right_x - 154, by, 60, 24}, "", {0,0,0,0}, 0};
    int aux_controls_visible = (l->win_w > 980);
    int offset_control_visible = (l->win_w > 900);

    if (!s->data_loaded && b->button == SDL_BUTTON_LEFT) {
        if (aux_controls_visible && point_in_rect(mx, my, btn_help.rect)) s->show_help = !s->show_help;
        return;
    }

    if (b->button == SDL_BUTTON_LEFT) {
        if (point_in_rect(mx, my, btn_bar.rect)) {
            s->bar_active = !s->bar_active;
            if(s->bar_active) { s->bar_x = (s->vxmin + s->vxmax)/2.0; s->pbar_x = (s->pvxmin + s->pvxmax)/2.0; }
            return;
        }
        if (point_in_rect(mx, my, btn_sync.rect)) {
            s->sync_active = !s->sync_active;
            if(s->sync_active) { s->pvxmin = s->vxmin; s->pvxmax = s->vxmax; }
            return;
        }
        if (point_in_rect(mx, my, btn_del.rect)) {
            if(s->n_peaks > 0) s->n_peaks--;
            return;
        }
        if (point_in_rect(mx, my, btn_list.rect)) { s->win_as.visible = !s->win_as.visible; return; }
        if (point_in_rect(mx, my, btn_peak.rect)) { s->win_pf.visible = !s->win_pf.visible; return; }
        if (l->win_w > 450 && point_in_rect(mx, my, btn_roll.rect)) { s->win_avg.visible = !s->win_avg.visible; return; }
        if (l->win_w > 540 && point_in_rect(mx, my, btn_broad.rect)) { s->win_br.visible = !s->win_br.visible; return; }
        if (l->win_w > 620 && point_in_rect(mx, my, btn_cut.rect)) { s->win_cut.visible = !s->win_cut.visible; return; }
        if (l->win_w > 700 && point_in_rect(mx, my, btn_jump.rect)) { s->win_jump.visible = !s->win_jump.visible; return; }
        if (l->win_w > 790 && point_in_rect(mx, my, btn_filt.rect)) { s->win_filt.visible = !s->win_filt.visible; return; }
        if (aux_controls_visible && point_in_rect(mx, my, btn_help.rect)) { s->show_help = !s->show_help; return; }
        if (aux_controls_visible && point_in_rect(mx, my, btn_export.rect)) { if (s->data_loaded) s->export_requested = 1; return; }
        if (offset_control_visible && point_in_rect(mx, my, r_off)) {
            s->input_state = INPUT_OFFSET;
            SDL_StartTextInput();
            snprintf(s->text_input_buf, 32, "%.4f", s->exp_offset);
            return;
        }
    }

    if (!s->data_loaded) return;

    // 3. Canvas Interactions
    SDL_Rect r_exp = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_Rect r_pred = {l->pred_x, l->pred_y, l->pred_w, l->pred_h};

    // --- NEW: Measure Tool Logic ---
    if (s->measure_active && !(SDL_GetModState() & KMOD_ALT) && point_in_rect(mx, my, r_exp) && b->button == SDL_BUTTON_LEFT) {
        // Use the TRUE (un-offset) frequency: the spectrum is only shifted visually.
        double freq = s->vxmin + ((double)(mx - l->exp_x) / l->exp_w) * (s->vxmax - s->vxmin) - s->exp_offset;

        if (s->measure_phase == 0) {
            s->measure_x1 = freq;
            s->measure_phase = 1; // Waiting for second click
        } else {
            double dist = fabs(freq - s->measure_x1);
            printf("Measured Distance: %.4f MHz\n", dist);
            s->measure_phase = 0; // Reset for next measurement
        }
        return; // Consume click so we don't zoom
    }    

    if (point_in_rect(mx, my, r_exp)) {
        if (b->button == SDL_BUTTON_LEFT) {
            if (SDL_GetModState() & KMOD_ALT) {
                s->dragging_offset = 1;
                s->drag_last_x = mx;
                return;
            }
            s->selecting_left = 1;
            s->sel_start = (SDL_Point){mx, my};
            s->sel_cur = s->sel_start;
        } else if (b->button == SDL_BUTTON_RIGHT) {
            s->selecting_right = 1;
            s->sel_start = (SDL_Point){mx, my};
            s->sel_cur = s->sel_start;
        }
    } 
    else if (point_in_rect(mx, my, r_pred)) {
        if (b->button == SDL_BUTTON_LEFT) {
            // Prediction Selection Logic
            int mod = SDL_GetModState();
            int append = (mod & KMOD_GUI) || (mod & KMOD_CTRL);
            if (!append) s->n_selected = 0;

            double freq_per_pixel = (s->pvxmax - s->pvxmin) / l->pred_w;
            double tolerance = 5.0 * freq_per_pixel; 
            double click_freq = s->pvxmin + ((double)(mx - l->pred_x) / l->pred_w) * (s->pvxmax - s->pvxmin);
            double raw_click_freq = click_freq;   // predictions live at their true freq now

            for(int i=0; i < s->n_pred; i++) {
            // Ignore lines hidden by the intensity cut or the active filters
                if (!pred_passes_filter(s, i)) continue;

                if(fabs(s->pred_lines[i].freq_mhz - raw_click_freq) < tolerance) {
                    if(s->n_selected < MAX_SELECTED) {
                        // Avoid duplicates
                        int dup = 0;
                        for(int k=0; k < s->n_selected; k++) if(s->selected_indices[k] == i) dup=1;
                        if(!dup) s->selected_indices[s->n_selected++] = i;
                    }
                }
            }
        }
    }
}

// --- MOUSE MOTION (Dragging) ---
static void handle_mouse_motion(AppState *s, Layout *l, SDL_MouseMotionEvent *m) {
    if (s->drag_target) {
        s->drag_target->rect.x = m->x - s->drag_offset.x;
        s->drag_target->rect.y = m->y - s->drag_offset.y;
    } 
    else if (s->dragging_offset) {
        if (l->exp_w <= 0) return;
        double freq_per_pixel = (s->vxmax - s->vxmin) / (double)l->exp_w;
        s->exp_offset += (m->x - s->drag_last_x) * freq_per_pixel;
        s->drag_last_x = m->x;
    }
    else if (s->selecting_left || s->selecting_right) {
        s->sel_cur.x = m->x;
        s->sel_cur.y = m->y;
    }
}

static void handle_mouse_wheel(AppState *s, Layout *l, SDL_MouseWheelEvent *w) {
    if (w->y == 0) return;

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    // Over the assignments table -> scroll that list.
    if (s->win_as.visible && point_in_rect(mx, my, s->win_as.rect)) {
        s->assignments_scroll += (w->y > 0) ? -3 : 3;
        clamp_assignment_scroll(s);
        return;
    }

    // Anywhere over the docked panel column -> scroll the sidebar stack.
    // (clamping happens in update_sidebars, which knows the content height)
    if (mx > l->plot_right) {
        s->sidebar_scroll += (w->y > 0) ? -45.0 : 45.0;
    }
}

// --- MOUSE UP (Action Completion) ---
static void handle_mouse_up(AppState *s, Layout *l, SDL_MouseButtonEvent *b) {
    s->drag_target = NULL;
    if (b->button == SDL_BUTTON_LEFT && s->dragging_offset) {
        s->dragging_offset = 0;
        printf("Prediction offset: %.4f MHz\n", s->exp_offset);
        return;
    }

    // A. Left Click Drag (Zoom)
    if (b->button == SDL_BUTTON_LEFT && s->selecting_left) {
        s->selecting_left = 0;
        if (ABS(s->sel_cur.x - s->sel_start.x) > 5) {
            double x0 = s->vxmin + (double)(s->sel_start.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin);
            double x1 = s->vxmin + (double)(s->sel_cur.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin);
            
            if (x1 < x0) { double t=x0; x0=x1; x1=t; }
            s->vxmin = x0; 
            s->vxmax = x1;
            
            // Sync Prediction if needed
            if(s->sync_active) {
                s->pvxmin = x0; 
                s->pvxmax = x1;
            }
        }
    }

    // B. Right Click Drag (Peak Find)
    if (b->button == SDL_BUTTON_RIGHT && s->selecting_right) {
        s->selecting_right = 0;
        // Convert the screen selection to TRUE data coords (subtract the visual
        // offset) so the peak is found and stored at its original frequency.
        double x0 = s->vxmin + (double)(s->sel_start.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin) - s->exp_offset;
        double x1 = s->vxmin + (double)(s->sel_cur.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin) - s->exp_offset;
        if (x1 < x0) { double t=x0; x0=x1; x1=t; }

        run_right_click_peak_find(s, x0, x1);
    }
}

// --- HELPER: Right Click Peak Calculation ---
static void run_right_click_peak_find(AppState *s, double raw_x0, double raw_x1) {
    if (!s->data_loaded) return;
    int start = binary_search_lower(s->current_pts, s->n_pts, raw_x0);
    int end   = binary_search_upper(s->current_pts, s->n_pts, raw_x1);
    if(start < 0) start = 0; 
    if(end >= s->n_pts) end = s->n_pts - 1;

    // Find max Y in range
    double best_y = -1e99;
    int bi = -1;
    for(int i=start; i<=end; i++) {
        if(s->current_pts[i].y > best_y) { 
            best_y = s->current_pts[i].y; 
            bi = i; 
        }
    }

    if (bi >= 1 && bi < s->n_pts - 1 && s->n_peaks < MAX_PEAKS) {
        // Parabolic Interpolation
        double y1 = s->current_pts[bi-1].y;
        double y2 = s->current_pts[bi].y;
        double y3 = s->current_pts[bi+1].y;
        double x2 = s->current_pts[bi].x;
        
        double px, py;
        double denom = (y1 - 2*y2 + y3);
        
        if (fabs(denom) < 1e-18) { 
            px = x2; 
            py = y2; 
        } else {
            double dx = 0.5 * ((y1 - y3) / denom);
            // Convert index delta to freq delta
            double freq_step = s->current_pts[bi+1].x - x2; 
            px = x2 + dx * freq_step;
            py = y2 - 0.25 * (y1 - y3) * dx;
        }
        
        s->peaks[s->n_peaks].x = px; 
        s->peaks[s->n_peaks].y = py;
        s->n_peaks++;
        
        // Clipboard
        char clip[64]; snprintf(clip, 64, "%.4f", px);
        SDL_SetClipboardText(clip);
        printf("Peak found: %.4f (Copied to clipboard)\n", px);

        // Auto-assign if prediction is selected
        assign_selected_predictions(s, px, py);
    }
}

static void assign_selected_predictions(AppState *s, double exp_freq, double exp_int) {
    if (s->n_selected <= 0) return;

    int requested = s->n_selected;
    for(int k = 0; k < requested; k++) {
        int idx = s->selected_indices[k];
        if (idx < 0 || idx >= s->n_pred) continue;
        add_or_update_assignment(s->assignments, &s->n_assignments,
                                 s->pred_lines[idx], exp_freq, exp_int);
    }

    printf("Assigned %d selected predicted line%s to %.4f MHz\n",
           requested, requested == 1 ? "" : "s", exp_freq);
    s->n_selected = 0;
    s->assignments_scroll = s->n_assignments - 13;
    clamp_assignment_scroll(s);
}

static void delete_assignment(AppState *s, int idx) {
    if (idx < 0 || idx >= s->n_assignments) return;

    double pred_freq = s->assignments[idx].pred.freq_mhz;
    for (int i = idx; i < s->n_assignments - 1; i++) {
        s->assignments[i] = s->assignments[i + 1];
    }
    s->n_assignments--;

    if (s->n_assignments <= 0) {
        s->selected_assignment = -1;
    } else if (idx >= s->n_assignments) {
        s->selected_assignment = s->n_assignments - 1;
    } else {
        s->selected_assignment = idx;
    }

    clamp_assignment_scroll(s);
    printf("Deleted assignment for %.4f MHz\n", pred_freq);
}

static void clamp_assignment_scroll(AppState *s) {
    int max_scroll = s->n_assignments - 13;
    if (max_scroll < 0) max_scroll = 0;
    if (s->assignments_scroll < 0) s->assignments_scroll = 0;
    if (s->assignments_scroll > max_scroll) s->assignments_scroll = max_scroll;
}

static void commit_text_input(AppState *s) {
    if (s->input_state == INPUT_NONE) return;

    if (s->input_state == INPUT_GAMMA) s->lorentz_gamma = atof(s->text_input_buf);
    else if (s->input_state == INPUT_PF_SIG) s->pf_sig_pts = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_PF_NOISE) s->pf_noise_pts = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_PF_THRESH) s->pf_thresh = atof(s->text_input_buf);
    else if (s->input_state == INPUT_AVG_PTS) {
        s->rolling_avg_window = atoi(s->text_input_buf);
        if(s->rolling_avg_active) {
            apply_rolling_average(s->raw_pts, s->smooth_pts, s->n_pts, s->rolling_avg_window);
            s->current_pts = s->smooth_pts;
        }
    }
    else if (s->input_state == INPUT_PRED_MIN) s->pred_min_log_int = atof(s->text_input_buf);
    else if (s->input_state == INPUT_PRED_MAX) s->pred_max_log_int = atof(s->text_input_buf);
    else if (s->input_state == INPUT_JUMP_MIN) {
        s->vxmin = atof(s->text_input_buf);
        if(s->sync_active) s->pvxmin = s->vxmin;
    }
    else if (s->input_state == INPUT_JUMP_MAX) {
        s->vxmax = atof(s->text_input_buf);
        if(s->sync_active) s->pvxmax = s->vxmax;
    }
    else if (s->input_state == INPUT_OFFSET) {
        s->exp_offset = atof(s->text_input_buf);
    }
    else if (s->input_state == INPUT_FILT_JMIN)  s->filt_j_min  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_JMAX)  s->filt_j_max  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KAMIN) s->filt_ka_min = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KAMAX) s->filt_ka_max = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KCMIN) s->filt_kc_min = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KCMAX) s->filt_kc_max = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DJ)    s->filt_dj  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DKA)   s->filt_dka = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DKC)   s->filt_dkc = atoi(s->text_input_buf);

    s->input_state = INPUT_NONE;
    SDL_StopTextInput();
}

// --- TEXT INPUT BUFFER ---
static void handle_text_input_event(AppState *s, char *text) {
    if (strlen(s->text_input_buf) < 63) {
        strcat(s->text_input_buf, text);
    }
}

static int path_looks_like_cat(const char *path) {
    size_t len = strlen(path);
    return len >= 4 && strcmp(path + len - 4, ".cat") == 0;
}

// --- KEYBOARD ---
static void handle_keydown(AppState *s, Layout *l, SDL_KeyboardEvent *key) {
    if (s->input_state != INPUT_NONE) return; // Should be handled by main loop, but safety check

    int sym = key->keysym.sym;
    int mod = key->keysym.mod;
    int is_shift = (mod & KMOD_SHIFT);
    double speed_mult = (mod & KMOD_CAPS) ? 3.0 : 1.0;

    if (sym == SDLK_h || sym == SDLK_SLASH) {
        s->show_help = !s->show_help;
        return;
    }
    if (sym == SDLK_x) {
        if (s->data_loaded) s->export_requested = 1;
        return;
    }
    if (!s->data_loaded) return;

    // Delete Peak
    if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
        if (is_shift) s->n_peaks = 0; 
        else if (s->n_peaks > 0) s->n_peaks--;
        return;
    }

    if (s->win_as.visible && sym == SDLK_PAGEUP) {
        s->assignments_scroll -= 13;
        clamp_assignment_scroll(s);
        return;
    }
    if (s->win_as.visible && sym == SDLK_PAGEDOWN) {
        s->assignments_scroll += 13;
        clamp_assignment_scroll(s);
        return;
    }

    // Toggle Windows
    if (sym == SDLK_n) s->win_as.visible = !s->win_as.visible;
    if (sym == SDLK_m) s->win_br.visible = !s->win_br.visible;
    if (sym == SDLK_p) s->win_pf.visible = !s->win_pf.visible;
    if (sym == SDLK_t) s->win_avg.visible = !s->win_avg.visible;
    if (sym == SDLK_r) { // Reset View
        s->vxmin = s->xmin; s->vxmax = s->xmax;
        s->vymin = s->ymin; s->vymax = s->ymax;
        s->pvxmin = s->xmin; s->pvxmax = s->xmax;
        s->pred_scale = 1.0;
        return;
    }
    if (sym == SDLK_c) s->win_cut.visible = !s->win_cut.visible;
    if (sym == SDLK_f) s->win_jump.visible = !s->win_jump.visible;
    if (sym == SDLK_b) s->win_filt.visible = !s->win_filt.visible;
    
    // Navigation Calc
    double width_px = (double)l->exp_w;
    if(width_px < 1) width_px = 1000; // Safety

    double pan_px = 100.0 * speed_mult;
    double pan_exp = pan_px * (s->vxmax - s->vxmin) / width_px;
    double pan_pred = pan_px * (s->pvxmax - s->pvxmin) / width_px;
    double pan_y = pan_px * (s->vymax - s->vymin) / (double)l->exp_h;

    double bar_step_exp = 2.0 * (s->vxmax - s->vxmin) / width_px * speed_mult * 2.0;
    double bar_step_pred = 2.0 * (s->pvxmax - s->pvxmin) / width_px * speed_mult * 2.0;

    switch(sym) {
        // Y-Axis Auto Scale
        case SDLK_TAB: {
            // The spectrum is displayed shifted by exp_offset, so the visible
            // window in TRUE data coords is [vxmin - offset, vxmax - offset].
            int istart = binary_search_lower(s->current_pts, s->n_pts, s->vxmin - s->exp_offset);
            int iend = binary_search_upper(s->current_pts, s->n_pts, s->vxmax - s->exp_offset);
            if(istart < 0) istart = 0; if(iend >= s->n_pts) iend = s->n_pts - 1;
            
            double miny=1e99, maxy=-1e99;
            for(int i=istart; i<=iend; i++) {
                if(s->current_pts[i].y < miny) miny = s->current_pts[i].y;
                if(s->current_pts[i].y > maxy) maxy = s->current_pts[i].y;
            }
            if(miny < maxy) { s->vymin = miny; s->vymax = maxy; }
            break;
        }

        // Panning X
        case SDLK_a:
            if(s->sync_active) { s->vxmin -= pan_exp; s->vxmax -= pan_exp; }
            else { 
                if(is_shift) { s->pvxmin -= pan_pred; s->pvxmax -= pan_pred; } 
                else { s->vxmin -= pan_exp; s->vxmax -= pan_exp; } 
            }
            break;
        case SDLK_s:
            if(s->sync_active) { s->vxmin += pan_exp; s->vxmax += pan_exp; }
            else { 
                if(is_shift) { s->pvxmin += pan_pred; s->pvxmax += pan_pred; } 
                else { s->vxmin += pan_exp; s->vxmax += pan_exp; } 
            }
            break;

        // Zoom In (E) / Out (Q)
        case SDLK_e: { // In
            double f = 0.9; 
            // Logic: shrink range around center (or bar)
            if(s->sync_active) {
                double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                double w = (s->vxmax - s->vxmin) * f;
                s->vxmin = c - w/2; s->vxmax = c + w/2;
            } else {
                if(is_shift) {
                    double c = s->bar_active ? s->pbar_x : (s->pvxmin + s->pvxmax)/2.0;
                    double w = (s->pvxmax - s->pvxmin) * f;
                    s->pvxmin = c - w/2; s->pvxmax = c + w/2;
                } else {
                    double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                    double w = (s->vxmax - s->vxmin) * f;
                    s->vxmin = c - w/2; s->vxmax = c + w/2;
                }
            }
            break;
        }
        case SDLK_q: { // Out
            double f = 1.1; 
            if(s->sync_active) {
                double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                double w = (s->vxmax - s->vxmin) * f;
                s->vxmin = c - w/2; s->vxmax = c + w/2;
            } else {
                if(is_shift) {
                    double c = s->bar_active ? s->pbar_x : (s->pvxmin + s->pvxmax)/2.0;
                    double w = (s->pvxmax - s->pvxmin) * f;
                    s->pvxmin = c - w/2; s->pvxmax = c + w/2;
                } else {
                    double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                    double w = (s->vxmax - s->vxmin) * f;
                    s->vxmin = c - w/2; s->vxmax = c + w/2;
                }
            }
            break;
        }

        // Vertical Control / Pred Scale
        case SDLK_w:
            if(is_shift) s->pred_scale *= 1.1; 
            else s->vymax -= pan_y; 
            break;
        case SDLK_z:
            if(is_shift) s->pred_scale *= 0.9; 
            else s->vymax += pan_y; 
            break;
        case SDLK_UP:
            if(is_shift) s->pred_scale *= 1.1; 
            else { s->vymin -= pan_y/10; s->vymax -= pan_y/10; } 
            break;
        case SDLK_DOWN:
            if(is_shift) s->pred_scale *= 0.9; 
            else { s->vymin += pan_y/10; s->vymax += pan_y/10; } 
            break;

        // Move Bar (K / L)
        case SDLK_k: 
            if(s->bar_active) {
                if(s->sync_active) { s->bar_x -= bar_step_exp; s->pbar_x -= bar_step_exp; }
                else { if(is_shift) s->pbar_x -= bar_step_pred; else s->bar_x -= bar_step_exp; }
            } break;
        case SDLK_l: 
            if(s->bar_active) {
                if(s->sync_active) { s->bar_x += bar_step_exp; s->pbar_x += bar_step_exp; }
                else { if(is_shift) s->pbar_x += bar_step_pred; else s->bar_x += bar_step_exp; }
            } break;
        
        // Future Features (Placeholders for features requested)
 //       case SDLK_c: 
 //           // s->input_state = INPUT_PRED_INT_MIN; ...
 //           break;
        // --- NEW: Toggle Measure Mode ---
        case SDLK_g:
            s->measure_active = !s->measure_active;
            s->measure_phase = 0; // Reset phase
            printf("Measure Mode: %s\n", s->measure_active ? "ON" : "OFF");
            break;
    }
}
