#include "controller.h"
#include "algorithms.h"
#include "loader.h"
#include "layout.h"
#include "ui_chrome.h"
#include "ui_panels.h"
#include <SDL.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define ABS(x) ((x)<0?-(x):(x))

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

// Scale the per-spectrum intensity gain. By default all visible spectra are
// scaled together; with "individual intensity" enabled only the active one is.
static void scale_intensity(AppState *s, double factor) {
    if (s->multi_indiv_int) {
        if (s->active_spec >= 0 && s->active_spec < s->n_spectra)
            s->spectra[s->active_spec].vscale *= factor;
    } else {
        for (int i = 0; i < s->n_spectra; i++)
            if (s->spectra[i].visible) s->spectra[i].vscale *= factor;
    }
}

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
                if (path_looks_like_cat(path)) snprintf(state->pending_pred_path, sizeof(state->pending_pred_path), "%s", path);
                else snprintf(state->pending_spec_path, sizeof(state->pending_spec_path), "%s", path);
                state->pending_load = 1;
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

    // 1. Inspector panels.
    //    The panels are docked in the right-hand column; every rectangle below
    //    comes from ui_panels.h, the same header the renderer draws from.
    {
        DraggableWindow *panels[UI_TOOL_COUNT] = {
            &s->win_as, &s->win_pf, &s->win_avg, &s->win_br,
            &s->win_cut, &s->win_filt, &s->win_jump, &s->win_spec
        };
        for (int t = 0; t < UI_TOOL_COUNT; t++) {
            DraggableWindow *p = panels[t];
            if (!p->visible || !point_in_rect(mx, my, p->rect)) continue;
            SDL_Rect w = p->rect;

            /* header: the only control is close */
            if (my < w.y + UI_SECTION_HEAD_H) {
                if (mx > w.x + w.w - 30) p->visible = 0;
                return;
            }
            if (b->button != SDL_BUTTON_LEFT) return;

            switch (t) {
            case UI_TOOL_ASSIGN: {
                clamp_assignment_scroll(s);
                int rows = ui_as_rows(s);
                int start_idx = s->assignments_scroll;
                if (start_idx > s->n_assignments - rows) start_idx = s->n_assignments - rows;
                if (start_idx < 0) start_idx = 0;
                for (int i = 0; i < rows; i++) {
                    int k = start_idx + i;
                    if (k >= s->n_assignments) break;
                    if (point_in_rect(mx, my, ui_as_row(w, i))) { s->selected_assignment = k; return; }
                }
                if (point_in_rect(mx, my, ui_as_save(s, w))) {
                    FILE *fp = fopen("assignments.txt", "w");
                    if (fp) {
                        fprintf(fp, "# PredFreq(MHz)  Ju Kau Kcu M1u M2u M3u  Jl Kal Kcl M1l M2l M3l  ExpFreq(MHz) ExpInt\n");
                        for (int k = 0; k < s->n_assignments; k++) {
                            PredLine p2 = s->assignments[k].pred;
                            fprintf(fp, "%12.4f  %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d   %12.4f %12.4e\n",
                                    p2.freq_mhz,
                                    p2.Ju, p2.Kau, p2.Kcu, p2.M1u, p2.M2u, p2.M3u,
                                    p2.Jl, p2.Kal, p2.Kcl, p2.M1l, p2.M2l, p2.M3l,
                                    s->assignments[k].exp_freq, s->assignments[k].exp_int);
                        }
                        fclose(fp);
                        printf("Saved assignments.txt\n");
                    }
                    return;
                }
                if (point_in_rect(mx, my, ui_as_delete(s, w))) { delete_assignment(s, s->selected_assignment); return; }
                return;
            }

            case UI_TOOL_PEAKS:
                if (point_in_rect(mx, my, ui_pf_sig(w))) {
                    s->input_state = INPUT_PF_SIG; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%d", s->pf_sig_pts);
                } else if (point_in_rect(mx, my, ui_pf_noise(w))) {
                    s->input_state = INPUT_PF_NOISE; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%d", s->pf_noise_pts);
                } else if (point_in_rect(mx, my, ui_pf_thresh(w))) {
                    s->input_state = INPUT_PF_THRESH; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.1f", s->pf_thresh);
                } else if (point_in_rect(mx, my, ui_pf_find(w))) {
                    run_peak_finder(s->current_pts, s->n_pts, s->vxmin, s->vxmax,
                                    s->pf_sig_pts, s->pf_noise_pts, s->pf_thresh, s->peaks, &s->n_peaks);
                } else if (point_in_rect(mx, my, ui_pf_export(w))) {
                    FILE *fp = fopen("linelist.csv", "w");
                    if (fp) {
                        fprintf(fp, "Freq,Int\n");
                        for (int k = 0; k < s->n_peaks; k++)
                            fprintf(fp, "%.6f,%.6e\n", s->peaks[k].x, s->peaks[k].y);
                        fclose(fp);
                        printf("Saved linelist.csv\n");
                    }
                }
                return;

            case UI_TOOL_AVG:
                if (point_in_rect(mx, my, ui_avg_field(w))) {
                    s->input_state = INPUT_AVG_PTS; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%d", s->rolling_avg_window);
                } else if (point_in_rect(mx, my, ui_avg_toggle(w))) {
                    s->rolling_avg_active = !s->rolling_avg_active;
                    if (s->rolling_avg_active) {
                        apply_rolling_average(s->raw_pts, s->smooth_pts, s->n_pts, s->rolling_avg_window);
                        s->current_pts = s->smooth_pts;
                    } else {
                        s->current_pts = s->raw_pts;
                    }
                }
                return;

            case UI_TOOL_BROAD: {
                SDL_Rect mode = ui_br_mode(w);
                if (point_in_rect(mx, my, mode)) {
                    s->broaden_mode = (mx < mode.x + mode.w / 2) ? 0 : 1;
                    s->input_state = INPUT_NONE;
                    return;
                }
                if (point_in_rect(mx, my, ui_br_f1(w))) {
                    if (s->broaden_mode == 0) {
                        s->input_state = INPUT_GAMMA; SDL_StartTextInput();
                        snprintf(s->text_input_buf, 32, "%.2f", s->lorentz_gamma);
                    } else {
                        s->input_state = INPUT_KBETA; SDL_StartTextInput();
                        snprintf(s->text_input_buf, 32, "%.2f", s->kaiser_beta);
                    }
                } else if (point_in_rect(mx, my, ui_br_f2(w))) {
                    if (s->broaden_mode == 0) {
                        s->input_state = INPUT_GAUSS; SDL_StartTextInput();
                        snprintf(s->text_input_buf, 32, "%.2f", s->gauss_gamma);
                    } else {
                        s->input_state = INPUT_KCEROS; SDL_StartTextInput();
                        snprintf(s->text_input_buf, 32, "%d", s->kaiser_ceros);
                    }
                } else if (s->broaden_mode == 1 && point_in_rect(mx, my, ui_br_f3(w))) {
                    s->input_state = INPUT_KINTR; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.3f", s->kaiser_intrinsic);
                } else if (point_in_rect(mx, my, ui_br_toggle(w, s->broaden_mode == 1))) {
                    s->broadening_active = !s->broadening_active;
                }
                return;
            }

            case UI_TOOL_CUT:
                if (point_in_rect(mx, my, ui_cut_min(w))) {
                    s->input_state = INPUT_PRED_MIN; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.1f", s->pred_min_log_int);
                } else if (point_in_rect(mx, my, ui_cut_max(w))) {
                    s->input_state = INPUT_PRED_MAX; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.1f", s->pred_max_log_int);
                }
                return;

            case UI_TOOL_JUMP:
                if (point_in_rect(mx, my, ui_jump_start(w))) {
                    s->input_state = INPUT_JUMP_MIN; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.1f", s->vxmin);
                } else if (point_in_rect(mx, my, ui_jump_end(w))) {
                    s->input_state = INPUT_JUMP_MAX; SDL_StartTextInput();
                    snprintf(s->text_input_buf, 32, "%.1f", s->vxmax);
                }
                return;

            case UI_TOOL_FILTER: {
                if (point_in_rect(mx, my, ui_filt_master(w))) { s->filter_active = !s->filter_active; return; }
                for (int i = 0; i < 3; i++)
                    if (point_in_rect(mx, my, ui_filt_mu(w, i))) { s->filt_mu[i] = !s->filt_mu[i]; return; }
                for (int i = 0; i < 3; i++)
                    if (point_in_rect(mx, my, ui_filt_br(w, i))) { s->filt_br[i] = !s->filt_br[i]; return; }
                if (point_in_rect(mx, my, ui_filt_range(w))) { s->filt_use_range = !s->filt_use_range; return; }

                int   qn_in[3][2] = {{INPUT_FILT_JMIN,  INPUT_FILT_JMAX},
                                     {INPUT_FILT_KAMIN, INPUT_FILT_KAMAX},
                                     {INPUT_FILT_KCMIN, INPUT_FILT_KCMAX}};
                int  *qn_val[3][2] = {{&s->filt_j_min,  &s->filt_j_max},
                                      {&s->filt_ka_min, &s->filt_ka_max},
                                      {&s->filt_kc_min, &s->filt_kc_max}};
                for (int i = 0; i < 3; i++) {
                    for (int c = 0; c < 2; c++) {
                        if (point_in_rect(mx, my, ui_filt_qn(w, i, c))) {
                            s->input_state = qn_in[i][c]; SDL_StartTextInput();
                            snprintf(s->text_input_buf, 32, "%d", *qn_val[i][c]);
                            return;
                        }
                    }
                }
                if (point_in_rect(mx, my, ui_filt_jump(w))) { s->filt_use_delta = !s->filt_use_delta; return; }
                int  d_in[3]  = {INPUT_FILT_DJ, INPUT_FILT_DKA, INPUT_FILT_DKC};
                int *d_val[3] = {&s->filt_dj, &s->filt_dka, &s->filt_dkc};
                for (int i = 0; i < 3; i++) {
                    if (point_in_rect(mx, my, ui_filt_delta(w, i))) {
                        s->input_state = d_in[i]; SDL_StartTextInput();
                        snprintf(s->text_input_buf, 32, "%d", *d_val[i]);
                        return;
                    }
                }
                return;
            }

            case UI_TOOL_SPECTRA: {
                SDL_Rect lay = ui_spec_layout(w), yn = ui_spec_ynorm(w);
                if (point_in_rect(mx, my, lay)) { s->multi_layout = (mx >= lay.x + lay.w / 2); return; }
                if (point_in_rect(mx, my, yn))  { s->multi_ynorm  = (mx >= yn.x + yn.w / 2);  return; }
                if (point_in_rect(mx, my, ui_spec_indiv(w))) { s->multi_indiv_int = !s->multi_indiv_int; return; }
                for (int i = 0; i < s->n_spectra; i++) {
                    if (point_in_rect(mx, my, ui_spec_minus(w, i))) { s->spectra[i].voffset -= 0.05; return; }
                    if (point_in_rect(mx, my, ui_spec_plus(w, i)))  { s->spectra[i].voffset += 0.05; return; }
                    if (point_in_rect(mx, my, ui_spec_vis(w, i)))   { s->spectra[i].visible = !s->spectra[i].visible; return; }
                    if (point_in_rect(mx, my, ui_spec_del(w, i)))   { s->pending_remove = i; return; }
                    if (point_in_rect(mx, my, ui_spec_name(w, i)))  { s->pending_select = i; return; }
                }
                return;
            }

            default:
                return;
            }
        }
    }

    // 2. Rail and command bar.
    //    Both rectangles come from ui_chrome.h, the same source the renderer
    //    draws from, so a visual change cannot move the click targets away.
    if (b->button == SDL_BUTTON_LEFT) {
        DraggableWindow *panels[UI_TOOL_COUNT] = {
            &s->win_as, &s->win_pf, &s->win_avg, &s->win_br,
            &s->win_cut, &s->win_filt, &s->win_jump, &s->win_spec
        };
        for (int t = 0; t < UI_TOOL_COUNT; t++) {
            if (point_in_rect(mx, my, ui_rail_rect(t))) {
                panels[t]->visible = !panels[t]->visible;
                return;
            }
        }
        if (ui_top_right_visible(l->win_w) &&
            point_in_rect(mx, my, ui_top_rect(UI_TOP_HELP, l->win_w))) {
            s->show_help = !s->show_help;
            return;
        }
        /* the rail itself swallows clicks so they never reach the plot */
        if (mx < UI_RAIL_W && my >= UI_CONTENT_Y) return;

        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_BAR, l->win_w))) {
            s->bar_active = !s->bar_active;
            if (s->bar_active) {
                s->bar_x  = (s->vxmin + s->vxmax) / 2.0;
                s->pbar_x = (s->pvxmin + s->pvxmax) / 2.0;
            }
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_MEASURE, l->win_w))) {
            s->measure_active = !s->measure_active;
            s->measure_phase = 0;
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_SYNC, l->win_w))) {
            s->sync_active = !s->sync_active;
            if (s->sync_active) { s->pvxmin = s->vxmin; s->pvxmax = s->vxmax; }
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_DELPEAK, l->win_w))) {
            if (s->n_peaks > 0) s->n_peaks--;
            return;
        }
        if (ui_top_right_visible(l->win_w)) {
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_EXPORT, l->win_w))) {
                if (s->data_loaded) s->export_requested = 1;
                return;
            }
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_OFFSET, l->win_w))) {
                s->input_state = INPUT_OFFSET;
                SDL_StartTextInput();
                snprintf(s->text_input_buf, 32, "%.4f", s->exp_offset);
                return;
            }
        }
        /* clicks on the chrome bands never fall through to the plot */
        if (my < UI_CONTENT_Y) return;
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
    if (s->input_state == INPUT_GAUSS) s->gauss_gamma = atof(s->text_input_buf);
    if (s->input_state == INPUT_KBETA) s->kaiser_beta = atof(s->text_input_buf);
    if (s->input_state == INPUT_KCEROS) { int c = atoi(s->text_input_buf); s->kaiser_ceros = c > 0 ? c : 1; }
    if (s->input_state == INPUT_KINTR) { double v = atof(s->text_input_buf); s->kaiser_intrinsic = v >= 0 ? v : 0; }
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
        // Y range: span all visible spectra (overlay/shared common axis).
        double miny = s->ymin, maxy = s->ymax;
        int seen = 0;
        for (int k = 0; k < s->n_spectra; k++) {
            Spectrum *S = &s->spectra[k];
            if (!S->visible) continue;
            if (!seen) { miny = S->ymin; maxy = S->ymax; seen = 1; }
            else { if (S->ymin < miny) miny = S->ymin; if (S->ymax > maxy) maxy = S->ymax; }
        }
        s->vymin = miny; s->vymax = maxy;
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
            // Y auto-scale, mode-aware:
            //  - stack OR overlay-normalized: each subplot uses its own per-spectrum
            //    gain (vscale), so fit the GAIN of each visible spectrum to its
            //    windowed max (top of band = windowed max).
            //  - overlay-shared: fit the common vymin/vymax to all visible traces.
            if (s->multi_ynorm) {
                for (int k = 0; k < s->n_spectra; k++) {
                    Spectrum *S = &s->spectra[k];
                    if (!S->visible || S->n_pts < 1) continue;
                    int istart = binary_search_lower(S->current_pts, S->n_pts, s->vxmin - S->exp_offset);
                    int iend   = binary_search_upper(S->current_pts, S->n_pts, s->vxmax - S->exp_offset);
                    if(istart < 0) istart = 0; if(iend >= S->n_pts) iend = S->n_pts - 1;
                    double wmax = -1e99;
                    for(int i=istart; i<=iend; i++)
                        if(S->current_pts[i].y > wmax) wmax = S->current_pts[i].y;
                    double denom = wmax - S->ymin;
                    if (denom > 0) S->vscale = (S->ymax - S->ymin) / denom;
                }
            } else {
                double miny=1e99, maxy=-1e99;
                for (int k = 0; k < s->n_spectra; k++) {
                    Spectrum *S = &s->spectra[k];
                    if (!S->visible || S->n_pts < 1) continue;
                    int istart = binary_search_lower(S->current_pts, S->n_pts, s->vxmin - S->exp_offset);
                    int iend   = binary_search_upper(S->current_pts, S->n_pts, s->vxmax - S->exp_offset);
                    if(istart < 0) istart = 0; if(iend >= S->n_pts) iend = S->n_pts - 1;
                    for(int i=istart; i<=iend; i++) {
                        if(S->current_pts[i].y < miny) miny = S->current_pts[i].y;
                        if(S->current_pts[i].y > maxy) maxy = S->current_pts[i].y;
                    }
                }
                if(miny < maxy) { s->vymin = miny; s->vymax = maxy; }
            }
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

        // Vertical Control / Pred Scale.
        // Intensity depends only on shared/norm:
        //  - norm   : change the per-spectrum GAIN (all together, or only the
        //             active one when "individual intensity" is enabled).
        //  - shared : zoom the common vymax (works in overlay and stacked).
        case SDLK_w:
            if(is_shift) s->pred_scale *= 1.1;
            else if(s->multi_ynorm) scale_intensity(s, 1.1);
            else s->vymax -= pan_y;
            break;
        case SDLK_z:
            if(is_shift) s->pred_scale *= 0.9;
            else if(s->multi_ynorm) scale_intensity(s, 0.9);
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
