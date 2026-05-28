#include "view.h"
#include "layout.h" 
#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- INTERNAL CONSTANTS & PALETTE ---
static const SDL_Color COL_BG           = {20, 20, 20, 255};
static const SDL_Color COL_GRID         = {255, 255, 255, 255};
static const SDL_Color COL_TXT          = {255, 255, 255, 255};
static const SDL_Color COL_TXT_DIM      = {180, 180, 180, 255};
static const SDL_Color COL_ACCENT       = {0, 180, 220, 255}; // Cyan
static const SDL_Color COL_SEL_BOX      = {0, 255, 255, 50};
static const SDL_Color COL_INPUT_BG     = {10, 10, 15, 255};
static const SDL_Color COL_INPUT_BORDER = {60, 60, 70, 255};

// --- INTERNAL HELPERS PROTOTYPES ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);

// --- MAIN RENDER ENTRY POINT ---
void render_app(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    // 1. Clear Screen
    SDL_SetRenderDrawColor(ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(ren);

    // 2. Draw Graphs
    draw_spectrum_view(ren, font, state, l);
    draw_prediction_view(ren, font, state, l);

    // 3. Draw Selection Rect (if dragging)
    if(state->selecting_left || state->selecting_right) {
        SDL_SetRenderDrawColor(ren, COL_SEL_BOX.r, COL_SEL_BOX.g, COL_SEL_BOX.b, COL_SEL_BOX.a); 
        int rx = (state->sel_cur.x < state->sel_start.x) ? state->sel_cur.x : state->sel_start.x;
        int rw = abs(state->sel_cur.x - state->sel_start.x);
        SDL_Rect r = { rx, l->exp_y, rw, l->exp_h }; 
        SDL_RenderFillRect(ren, &r);
    }

    // 4. UI Elements (Windows, Buttons, Cursor Info)
    draw_ui_overlays(ren, font, state, l);
    draw_cursor_overlay(ren, font, state, l);

    // 5. Present
    SDL_RenderPresent(ren);
}

// --- SPECTRUM (EXPERIMENTAL) ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    // Clipping
    SDL_Rect clip = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_RenderSetClipRect(ren, &clip);
    
    // Draw Data Lines
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    
    // We search the raw arrays using the view limits minus the offset
    double effective_min = state->vxmin - state->exp_offset;
    double effective_max = state->vxmax - state->exp_offset;

    int start_idx = binary_search_lower(state->current_pts, state->n_pts, effective_min);
    int end_idx   = binary_search_upper(state->current_pts, state->n_pts, effective_max);
    
    // Safety clamp
    if (start_idx < 1) start_idx = 1; 
    if (end_idx >= state->n_pts) end_idx = state->n_pts - 1;

    for(int i = start_idx; i <= end_idx; i++) {
         double x1_val = state->current_pts[i-1].x + state->exp_offset;
         double x2_val = state->current_pts[i].x   + state->exp_offset;
         
         int px1 = l->exp_x + (x1_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
         int px2 = l->exp_x + (x2_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
         
         // Invert Y for graphics coordinates
         int py1 = l->exp_y + (1.0 - (state->current_pts[i-1].y - state->vymin)/(state->vymax - state->vymin)) * l->exp_h;
         int py2 = l->exp_y + (1.0 - (state->current_pts[i].y   - state->vymin)/(state->vymax - state->vymin)) * l->exp_h;
         
         SDL_RenderDrawLine(ren, px1, py1, px2, py2);
    }

    // Draw already-assigned experimental frequencies loaded from config/LIN.
    if (state->n_lin_data > 0) {
        SDL_SetRenderDrawColor(ren, 120, 255, 140, 210);
        int marker_top = l->exp_y + 4;
        int marker_bottom = l->exp_y + l->exp_h - 4;

        for (int i = 0; i < state->n_lin_data; i++) {
            double f = state->lin_data[i] + state->exp_offset;
            if (f < state->vxmin || f > state->vxmax) continue;

            int px = l->exp_x + (f - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_RenderDrawLine(ren, px, marker_top, px, marker_bottom);
            SDL_Rect cap = {px - 3, marker_top, 7, 7};
            SDL_RenderFillRect(ren, &cap);
        }
    }
    
    // Draw Found Peaks
    for (int ip = 0; ip < state->n_peaks; ip++) {
        double pkx = state->peaks[ip].x + state->exp_offset;
        if (pkx < state->vxmin || pkx > state->vxmax) continue;
        
        int px = l->exp_x + (pkx - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
        SDL_SetRenderDrawColor(ren, 255, 255, 0, 255); // Yellow
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);
        
        char label[64]; snprintf(label, sizeof(label), "%.3f", pkx);
        draw_text_vertical(ren, font, label, px + 4, l->exp_y + l->exp_h - 150, (SDL_Color){255,255,0,255});
    }

    // Draw Navigation Bar (Red)
    if(state->bar_active) {
        if(state->bar_x >= state->vxmin && state->bar_x <= state->vxmax) {
            int bx = l->exp_x + (state->bar_x - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 255, 0, 0, 255); 
            SDL_RenderDrawLine(ren, bx, l->exp_y, bx, l->exp_y + l->exp_h);
        }
    }

    // DISABLE CLIPPING so we can draw borders and ticks
    SDL_RenderSetClipRect(ren, NULL);

    // Draw Borders
    SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 255);
    SDL_RenderDrawRect(ren, &clip);
    
    // Draw X-Axis Ticks & Labels
    double xrange = state->vxmax - state->vxmin; 
    double xstep = nice_tick(xrange);
    double xstart = ceil(state->vxmin/xstep)*xstep;
    
    for(double x=xstart; x<=state->vxmax; x+=xstep) {
        int px = l->exp_x + (x - state->vxmin)/xrange * l->exp_w;
        SDL_RenderDrawLine(ren, px, l->exp_y + l->exp_h, px, l->exp_y + l->exp_h + 5);
        
        char buf[32]; snprintf(buf,sizeof(buf),"%.3f", x);
        draw_text(ren, font, buf, px - 20, l->exp_y + l->exp_h + 8, COL_TXT);
    }
    
    // X-Axis Title
    draw_text(ren, font, "Frequency / MHz", l->exp_x + l->exp_w/2 - 60, l->exp_y + l->exp_h + 28, COL_TXT);
    
    // --- Y-Axis Ticks & Labels (NEW) ---
    double yrange = state->vymax - state->vymin;
    if(yrange > 0) {
        double ystep = nice_tick(yrange);
        double ystart = ceil(state->vymin/ystep)*ystep;

        for(double y=ystart; y<=state->vymax; y+=ystep) {
            int py = l->exp_y + (1.0 - (y - state->vymin)/yrange) * l->exp_h;
            
            // Draw Tick on left axis
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x - 5, py);
            
            // Draw Label (Scientific notation)
            char buf[32]; 
            snprintf(buf, sizeof(buf), "%.1e", y);
            draw_text(ren, font, buf, l->exp_x - 65, py - 7, COL_TXT);
        }
    }
    // Y-Axis Title
    draw_text_vertical(ren, font, "Intensity", l->exp_x - 75, l->exp_y + l->exp_h/2 + 30, COL_TXT);
}

// --- PREDICTION (PICKETT) ---
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    SDL_Rect pred_rect = {l->pred_x, l->pred_y, l->pred_w, l->pred_h};
    
    // Draw Border
    SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 255);
    SDL_RenderDrawRect(ren, &pred_rect);

    // Grid Lines for Pred
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderDrawLine(ren, l->pred_x, l->pred_y + l->pred_h, l->pred_x + l->pred_w, l->pred_y + l->pred_h);

    if (state->n_pred > 0) {
        SDL_RenderSetClipRect(ren, &pred_rect);
        
        // Range filtering
        int p_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin);
        int p_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax);
        if(p_start < 0) p_start = 0; 
        if(p_end >= state->n_pred) p_end = state->n_pred - 1;

        // Optimization: Draw highest line per pixel column
        int last_x = -1;
        int max_y_at_x = -1; 
        int best_i_at_x = -1;

        for (int i=p_start; i<=p_end; i++) {
            // REMOVED INTENSITY CUT FILTER TO RESTORE VISIBILITY
            if (state->pred_lines[i].lgint < state->pred_min_log_int) continue;
            if (state->pred_lines[i].lgint > state->pred_max_log_int) continue;

            double h_ratio = (state->pred_lines[i].linear_int / state->pred_global_max) * state->pred_scale;
            if(h_ratio > 1.0) h_ratio = 1.0;
            
            // Calculate screen coordinates
            int sx = l->pred_x + (state->pred_lines[i].freq_mhz - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
            // sy1 is the Top Y of the line. Smaller Y = Higher on screen.
            int sy1 = l->pred_y + l->pred_h - (int)(h_ratio * (l->pred_h - 10));

            if (sx == last_x) {
                // If this line is taller (sy1 is smaller), keep it
                if (sy1 < max_y_at_x) { 
                    max_y_at_x = sy1; 
                    best_i_at_x = i; 
                }
            } else {
                // Draw previous column's best line
                if (best_i_at_x != -1) {
                    SDL_Color c = color_for_pred(state->pred_lines[best_i_at_x].branch, state->pred_lines[best_i_at_x].mu);
                    // Check selection highlight
                    int is_sel=0; 
                    for(int k=0; k<state->n_selected; k++) if(state->selected_indices[k]==best_i_at_x) is_sel=1;
                    
                    if(is_sel) SDL_SetRenderDrawColor(ren, 0, 255, 0, 255); 
                    else SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
                    
                    SDL_RenderDrawLine(ren, last_x, l->pred_y + l->pred_h, last_x, max_y_at_x);
                }
                // Start new column
                last_x = sx; 
                max_y_at_x = sy1; 
                best_i_at_x = i;
            }
        }
        
        // Draw tail (last pending line)
        if (best_i_at_x != -1) {
            SDL_Color c = color_for_pred(state->pred_lines[best_i_at_x].branch, state->pred_lines[best_i_at_x].mu);
            int is_sel=0; for(int k=0;k<state->n_selected;k++) if(state->selected_indices[k]==best_i_at_x) is_sel=1;
            if(is_sel) SDL_SetRenderDrawColor(ren, 0, 255, 0, 255); else SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
            SDL_RenderDrawLine(ren, last_x, l->pred_y + l->pred_h, last_x, max_y_at_x);
        }

        // Broadening Simulation (Lorentzian)
        if(state->broadening_active) {
            SDL_SetRenderDrawColor(ren, 0, 255, 255, 150); // Cyan transparent
            int steps = l->pred_w; 
            double prev_y = -1; 
            double cutoff = 50.0 * state->lorentz_gamma;
            
            // Optimization: Only calc lines near visible window + cutoff
            int p_calc_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin - cutoff);
            int p_calc_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax + cutoff);
            if(p_calc_start < 0) p_calc_start=0; 
            if(p_calc_end >= state->n_pred) p_calc_end=state->n_pred-1;

            for(int i=0; i<steps; i++) {
                double f = state->pvxmin + (double)i/l->pred_w * (state->pvxmax - state->pvxmin);
                double intensity_sum = 0;
                
                for(int k=p_calc_start; k<=p_calc_end; k++) {
                    double dist = f - state->pred_lines[k].freq_mhz;
                    if(fabs(dist) > cutoff) continue; 
                    if (state->pred_lines[k].lgint < state->pred_min_log_int) continue;
                    if (state->pred_lines[k].lgint > state->pred_max_log_int) continue;
                    double L = (state->lorentz_gamma * state->lorentz_gamma) / (dist*dist + state->lorentz_gamma*state->lorentz_gamma);
                    intensity_sum += state->pred_lines[k].linear_int * L;
                }
                
                double h_ratio = (intensity_sum / state->pred_global_max) * state->pred_scale; 
                int py = l->pred_y + l->pred_h - (int)(h_ratio * (l->pred_h - 10));
                if(py < l->pred_y) py = l->pred_y; // Clip top
                
                if(prev_y != -1) SDL_RenderDrawLine(ren, l->pred_x + i - 1, (int)prev_y, l->pred_x + i, py);
                prev_y = py;
            }
        }
        
        // Navigation Bar (Prediction)
        if(state->bar_active) {
            if(state->pbar_x >= state->pvxmin && state->pbar_x <= state->pvxmax) {
                int bx = l->pred_x + (state->pbar_x - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
                SDL_SetRenderDrawColor(ren, 255, 100, 0, 255); // Orange
                SDL_RenderDrawLine(ren, bx, l->pred_y, bx, l->pred_y + l->pred_h);
            }
        }

        // Draw assigned-frequency dots in the prediction pane as a compact locator.
        if (state->n_lin_data > 0) {
            SDL_SetRenderDrawColor(ren, 120, 255, 140, 220);
            int marker_y = l->pred_y + l->pred_h - 5; 
            
            for (int i=0; i<state->n_lin_data; i++) {
                double f = state->lin_data[i];
                if (f < state->pvxmin || f > state->pvxmax) continue;
                
                int px = l->pred_x + (f - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
                
                // Draw small 5x5 rect as "circle"
                SDL_Rect r = {px-2, marker_y-2, 5, 5};
                SDL_RenderFillRect(ren, &r);
            }
        }

        SDL_RenderSetClipRect(ren, NULL);
    }

    // Ticks
    double xrange = state->pvxmax - state->pvxmin;
    double xstep = nice_tick(xrange);
    double xstart = ceil(state->pvxmin/xstep)*xstep;
    for(double x=xstart; x<=state->pvxmax; x+=xstep) {
        int px = l->pred_x + (x - state->pvxmin)/xrange * l->pred_w;
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDrawLine(ren, px, l->pred_y + l->pred_h, px, l->pred_y + l->pred_h + 4);
        char buf[32]; snprintf(buf,sizeof(buf),"%.3f",x);
        draw_text(ren, font, buf, px - 20, l->pred_y + l->pred_h + 8, COL_TXT_DIM);
    }
}

// --- UI OVERLAYS (Buttons & Windows) ---
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    int mx, my; 
    int m_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

    // 1. TOOLBAR BUTTONS
    Button btn_bar  = {{10,  5, 60, 26}, "Bar",  {60, 60, 70, 255}, 1};
    Button btn_sync = {{80,  5, 60, 26}, "Sync", {60, 60, 70, 255}, 1};
    Button btn_del  = {{150, 5, 60, 26}, "Del",  {180, 60, 60, 255}, 0}; 
    Button btn_list = {{220, 5, 80, 26}, "List", {80, 60, 100, 255}, 0}; 
    Button btn_peak = {{310, 5, 80, 26}, "Peak", {80, 60, 100, 255}, 0};
    Button btn_roll = {{400, 5, 80, 26}, "Avg",  {80, 60, 100, 255}, 0};

    draw_button(ren, font, &btn_bar, mx, my, m_down, state->bar_active);
    draw_button(ren, font, &btn_sync, mx, my, m_down, state->sync_active);
    draw_button(ren, font, &btn_del, mx, my, m_down, 0);
    draw_button(ren, font, &btn_list, mx, my, m_down, state->win_as.visible);
    draw_button(ren, font, &btn_peak, mx, my, m_down, state->win_pf.visible);
    draw_button(ren, font, &btn_roll, mx, my, m_down, state->win_avg.visible);

    // 2. BROADENING WINDOW
    draw_draggable_window(ren, font, &state->win_br);
    if(state->win_br.visible) {
        int wx = state->win_br.rect.x, wy = state->win_br.rect.y;
        draw_text(ren, font, "Gamma (MHz):", wx+20, wy+65, COL_TXT_DIM);

        SDL_Rect r_gamma = {wx+120, wy+60, 80, 28};
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255);
        SDL_RenderFillRect(ren, &r_gamma);
        
        SDL_Color border = (state->input_state == INPUT_GAMMA) ? COL_ACCENT : COL_INPUT_BORDER;
        SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, 255);
        SDL_RenderDrawRect(ren, &r_gamma);

        char buf[64];
        if (state->input_state == INPUT_GAMMA) snprintf(buf, sizeof(buf), "%s_", state->text_input_buf);
        else snprintf(buf, sizeof(buf), "%.2f", state->lorentz_gamma);
        
        draw_text(ren, font, buf, r_gamma.x + 5, r_gamma.y + 5, COL_TXT);

        Button btn_br_tog = {{wx+50, wy+120, 200, 30}, "", {60, 60, 70, 255}, 1};
        snprintf(btn_br_tog.label, 32, state->broadening_active ? "ENABLED" : "DISABLED");
        draw_button(ren, font, &btn_br_tog, mx, my, m_down, state->broadening_active);
    }

    // 3. ROLLING AVG WINDOW
    draw_draggable_window(ren, font, &state->win_avg);
    if(state->win_avg.visible) {
        int wx = state->win_avg.rect.x, wy = state->win_avg.rect.y;
        draw_text(ren, font, "Window (pts):", wx+20, wy+65, COL_TXT_DIM);

        SDL_Rect r_win = {wx+130, wy+60, 80, 28};
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255);
        SDL_RenderFillRect(ren, &r_win);

        SDL_Color border = (state->input_state == INPUT_AVG_PTS) ? COL_ACCENT : COL_INPUT_BORDER;
        SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, 255);
        SDL_RenderDrawRect(ren, &r_win);

        char buf[64];
        if (state->input_state == INPUT_AVG_PTS) snprintf(buf, sizeof(buf), "%s_", state->text_input_buf);
        else snprintf(buf, sizeof(buf), "%d", state->rolling_avg_window);
        draw_text(ren, font, buf, r_win.x + 5, r_win.y + 5, COL_TXT);
        
        Button btn_avg_tog = {{wx+50, wy+120, 200, 30}, "", {60, 60, 70, 255}, 1};
        snprintf(btn_avg_tog.label, 32, state->rolling_avg_active ? "ENABLED" : "DISABLED");
        draw_button(ren, font, &btn_avg_tog, mx, my, m_down, state->rolling_avg_active);
    }

    // 4. PEAK FINDER WINDOW
    draw_draggable_window(ren, font, &state->win_pf);
    if(state->win_pf.visible) {
        int wx = state->win_pf.rect.x, wy = state->win_pf.rect.y;
        SDL_Rect r_sig = {wx+190, wy+50, 80, 26};
        SDL_Rect r_noi = {wx+190, wy+90, 80, 26};
        SDL_Rect r_thr = {wx+190, wy+130, 80, 26};

        draw_text(ren, font, "Search Width:", wx+20, wy+55, COL_TXT_DIM);
        draw_text(ren, font, "Noise Window:", wx+20, wy+95, COL_TXT_DIM);
        draw_text(ren, font, "Threshold (x):", wx+20, wy+135, COL_TXT_DIM);
        
        // Signal Input
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); SDL_RenderFillRect(ren, &r_sig);
        SDL_SetRenderDrawColor(ren, (state->input_state == INPUT_PF_SIG)?COL_ACCENT.r:COL_INPUT_BORDER.r, (state->input_state == INPUT_PF_SIG)?COL_ACCENT.g:COL_INPUT_BORDER.g, (state->input_state == INPUT_PF_SIG)?COL_ACCENT.b:COL_INPUT_BORDER.b, 255); SDL_RenderDrawRect(ren, &r_sig);
        char buf1[32]; if(state->input_state==INPUT_PF_SIG) snprintf(buf1,32,"%s_",state->text_input_buf); else snprintf(buf1,32,"%d",state->pf_sig_pts);
        draw_text(ren, font, buf1, r_sig.x+5, r_sig.y+4, COL_TXT);

        // Noise Input
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); SDL_RenderFillRect(ren, &r_noi);
        SDL_SetRenderDrawColor(ren, (state->input_state == INPUT_PF_NOISE)?COL_ACCENT.r:COL_INPUT_BORDER.r, (state->input_state == INPUT_PF_NOISE)?COL_ACCENT.g:COL_INPUT_BORDER.g, (state->input_state == INPUT_PF_NOISE)?COL_ACCENT.b:COL_INPUT_BORDER.b, 255); SDL_RenderDrawRect(ren, &r_noi);
        char buf2[32]; if(state->input_state==INPUT_PF_NOISE) snprintf(buf2,32,"%s_",state->text_input_buf); else snprintf(buf2,32,"%d",state->pf_noise_pts);
        draw_text(ren, font, buf2, r_noi.x+5, r_noi.y+4, COL_TXT);

        // Threshold Input
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); SDL_RenderFillRect(ren, &r_thr);
        SDL_SetRenderDrawColor(ren, (state->input_state == INPUT_PF_THRESH)?COL_ACCENT.r:COL_INPUT_BORDER.r, (state->input_state == INPUT_PF_THRESH)?COL_ACCENT.g:COL_INPUT_BORDER.g, (state->input_state == INPUT_PF_THRESH)?COL_ACCENT.b:COL_INPUT_BORDER.b, 255); SDL_RenderDrawRect(ren, &r_thr);
        char buf3[32]; if(state->input_state==INPUT_PF_THRESH) snprintf(buf3,32,"%s_",state->text_input_buf); else snprintf(buf3,32,"%.1f",state->pf_thresh);
        draw_text(ren, font, buf3, r_thr.x+5, r_thr.y+4, COL_TXT);

        // Buttons
        Button btn_run = {{wx+50, wy+200, 200, 30}, "FIND PEAKS", {0, 150, 0, 255}, 0};
        draw_button(ren, font, &btn_run, mx, my, m_down, 0);
        Button btn_exp = {{wx+50, wy+240, 200, 30}, "EXPORT LIST", {0, 100, 200, 255}, 0};
        draw_button(ren, font, &btn_exp, mx, my, m_down, 0);
    }

    // 5. ASSIGNMENT WINDOW
    draw_draggable_window(ren, font, &state->win_as);
    if(state->win_as.visible) {
        int wx = state->win_as.rect.x, wy = state->win_as.rect.y;
        int ty = wy + 60;
        
        draw_text(ren, font, "  Quantum numbers                          Exp Freq ", wx+20, ty, COL_TXT_DIM);
        SDL_SetRenderDrawColor(ren, 100,100,100,255); 
        SDL_RenderDrawLine(ren, wx+20, ty+25, wx+580, ty+25);
        ty += 30;
        
        // Simple scroll: show last 12
        int start_idx = (state->n_assignments > 12) ? state->n_assignments - 12 : 0;
        for(int k=start_idx; k<state->n_assignments; k++) {
            PredLine p = state->assignments[k].pred;
            char row[256];
            snprintf(row, sizeof(row), "  %2d %2d %2d %2d %2d %2d -> %2d %2d %2d %2d %2d %2d  %9.3f ",
                 p.Ju, p.Kau, p.Kcu,p.M1u,p.M2u,p.M3u, p.Jl, p.Kal, p.Kcl,p.M1l,p.M2l,p.M3l,
                 state->assignments[k].exp_freq);
            draw_text(ren, font, row, wx+20, ty, COL_TXT);
            ty += 20;
        }

        Button btn_save = {{wx+10, wy+360, 100, 30}, "Save All", {0, 100, 200, 255}, 0};
        draw_button(ren, font, &btn_save, mx, my, m_down, 0);
    }
    
    // NEW: 6. INTENSITY CUT WINDOW (Triggered by 'C')
    draw_draggable_window(ren, font, &state->win_cut);
    if(state->win_cut.visible) {
        int wx = state->win_cut.rect.x, wy = state->win_cut.rect.y;
        
        // Labels
        draw_text(ren, font, "Min Log(I):", wx+20, wy+55, COL_TXT_DIM);
        draw_text(ren, font, "Max Log(I):", wx+20, wy+95, COL_TXT_DIM);

        SDL_Rect r_min = {wx+120, wy+50, 80, 28};
        SDL_Rect r_max = {wx+120, wy+90, 80, 28};

        // Draw Min Input Field
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); 
        SDL_RenderFillRect(ren, &r_min);
        
        if(state->input_state == INPUT_PRED_MIN) {
            SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
            SDL_RenderDrawRect(ren, &r_min);
            char buf[32]; snprintf(buf, 32, "%s_", state->text_input_buf);
            draw_text(ren, font, buf, r_min.x+5, r_min.y+4, COL_TXT);
        } else {
            SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 255);
            SDL_RenderDrawRect(ren, &r_min);
            char buf[32]; snprintf(buf, 32, "%.1f", state->pred_min_log_int);
            draw_text(ren, font, buf, r_min.x+5, r_min.y+4, COL_TXT);
        }

        // Draw Max Input Field
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); 
        SDL_RenderFillRect(ren, &r_max);
        
        if(state->input_state == INPUT_PRED_MAX) {
            SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
            SDL_RenderDrawRect(ren, &r_max);
            char buf[32]; snprintf(buf, 32, "%s_", state->text_input_buf);
            draw_text(ren, font, buf, r_max.x+5, r_max.y+4, COL_TXT);
        } else {
            SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 255);
            SDL_RenderDrawRect(ren, &r_max);
            char buf[32]; snprintf(buf, 32, "%.1f", state->pred_max_log_int);
            draw_text(ren, font, buf, r_max.x+5, r_max.y+4, COL_TXT);
        }
    }
    // --- 7. FREQ JUMP WINDOW (Triggered by 'F') ---
    draw_draggable_window(ren, font, &state->win_jump);
    if(state->win_jump.visible) {
        int wx = state->win_jump.rect.x, wy = state->win_jump.rect.y;
        
        draw_text(ren, font, "Start Freq:", wx+20, wy+55, COL_TXT_DIM);
        draw_text(ren, font, "End Freq:",   wx+20, wy+95, COL_TXT_DIM);

        SDL_Rect r_start = {wx+120, wy+50, 80, 28};
        SDL_Rect r_end   = {wx+120, wy+90, 80, 28};

        // Draw Start Field
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); 
        SDL_RenderFillRect(ren, &r_start);
        
        if(state->input_state == INPUT_JUMP_MIN) {
            SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
            SDL_RenderDrawRect(ren, &r_start);
            char buf[32]; snprintf(buf, 32, "%s_", state->text_input_buf);
            draw_text(ren, font, buf, r_start.x+5, r_start.y+4, COL_TXT);
        } else {
            SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 255);
            SDL_RenderDrawRect(ren, &r_start);
            char buf[32]; snprintf(buf, 32, "%.1f", state->vxmin);
            draw_text(ren, font, buf, r_start.x+5, r_start.y+4, COL_TXT);
        }

        // Draw End Field
        SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); 
        SDL_RenderFillRect(ren, &r_end);
        
        if(state->input_state == INPUT_JUMP_MAX) {
            SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
            SDL_RenderDrawRect(ren, &r_end);
            char buf[32]; snprintf(buf, 32, "%s_", state->text_input_buf);
            draw_text(ren, font, buf, r_end.x+5, r_end.y+4, COL_TXT);
        } else {
            SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 255);
            SDL_RenderDrawRect(ren, &r_end);
            char buf[32]; snprintf(buf, 32, "%.1f", state->vxmax);
            draw_text(ren, font, buf, r_end.x+5, r_end.y+4, COL_TXT);
        }
    }

    // 8. STATIC OFFSET CONTROL (Bottom Right)
    int ox = l->pred_x + l->pred_w - 150;
    int oy = l->pred_y + l->pred_h + 10;
    
    draw_text(ren, font, "Offset (MHz):", ox - 100, oy + 5, COL_TXT_DIM);
    SDL_Rect r_off = {ox, oy, 140, 28};
    SDL_SetRenderDrawColor(ren, COL_INPUT_BG.r, COL_INPUT_BG.g, COL_INPUT_BG.b, 255); SDL_RenderFillRect(ren, &r_off);
    
    if(state->input_state == INPUT_OFFSET) {
        SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255); SDL_RenderDrawRect(ren, &r_off);
        char buf[32]; snprintf(buf, 32, "%s_", state->text_input_buf); draw_text(ren, font, buf, r_off.x+5, r_off.y+4, COL_TXT);
    } else {
        SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 255); SDL_RenderDrawRect(ren, &r_off);
        char buf[32]; snprintf(buf, 32, "%.4f", state->exp_offset); draw_text(ren, font, buf, r_off.x+5, r_off.y+4, COL_TXT);
    }
}

static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    int mx, my;
    SDL_GetMouseState(&mx, &my);

    // 1. Mouse Position Info (Top Right)
    if (point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
        double fx = (mx - l->exp_x) / (double)l->exp_w;
        double fy = 1.0 - (my - l->exp_y) / (double)l->exp_h;
        double cx = state->vxmin + fx * (state->vxmax - state->vxmin);
        double cy = state->vymin + fy * (state->vymax - state->vymin);
        
        char c1[64], c2[64];
        snprintf(c1, sizeof(c1), "f=%.3f MHz", cx); 
        snprintf(c2, sizeof(c2), "I=%.2e", cy);
        
        draw_text(ren, font, c1, l->win_w - 220, 10, COL_TXT);
        draw_text(ren, font, c2, l->win_w - 220, 30, COL_TXT);
    }

    // 2. Measure Tool Overlay
    if (state->measure_active) {
        draw_text(ren, font, "[MEASURE MODE]", l->exp_x + 10, l->exp_y + 10, (SDL_Color){255, 0, 255, 255});
        if (state->measure_phase == 1) {
            int px1 = l->exp_x + (state->measure_x1 - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 255, 0, 255, 200);
            SDL_RenderDrawLine(ren, px1, l->exp_y, px1, l->exp_y + l->exp_h);
            SDL_RenderDrawLine(ren, px1, my, mx, my);
            if (point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
                double curr_freq = state->vxmin + ((double)(mx - l->exp_x) / l->exp_w) * (state->vxmax - state->vxmin);
                double dist = fabs(curr_freq - state->measure_x1);
                char buf[64]; snprintf(buf, 64, "%.4f MHz", dist);
                draw_text(ren, font, buf, mx + 10, my - 20, (SDL_Color){255, 0, 255, 255});
            }
        }
    }

    // 3. Selection Info
    if(state->n_selected > 0) {
        PredLine *p = &state->pred_lines[state->selected_indices[state->n_selected-1]];
        char qn1[128], qn2[128];
        snprintf(qn1, sizeof(qn1), "Sel: %2d %2d %2d %2d %2d %2d", p->Ju, p->Kau, p->Kcu,p->M1u,p->M2u,p->M3u);
        snprintf(qn2, sizeof(qn2), " ->  %2d %2d %2d %2d %2d %2d (%.3f MHz)",  p->Jl, p->Kal, p->Kcl,p->M1l,p->M2l,p->M3l, p->freq_mhz);
        draw_text(ren, font, qn1, l->pred_x + 10, l->pred_y + 10, (SDL_Color){0,255,0,255});
        draw_text(ren, font, qn2, l->pred_x + 10, l->pred_y + 30, (SDL_Color){0,255,0,255});
        if(state->n_selected > 1) {
            char extra[64]; snprintf(extra, sizeof(extra), "... (+ %d more)", state->n_selected - 1);
            draw_text(ren, font, extra, l->pred_x + 10, l->pred_y + 50, (SDL_Color){50,255,100,255});
        }
    }
    
    // 4. Hover Info
    if(state->bar_active) {
        int p_hover_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pbar_x - 1.0);
        int p_hover_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pbar_x + 1.0);
        if(p_hover_start < 0) p_hover_start = 0; 
        if(p_hover_end >= state->n_pred) p_hover_end = state->n_pred - 1;

        double closest_dist = 1e99; int closest_idx = -1;
        double freq_tolerance = (state->pvxmax - state->pvxmin) * 0.01; 

        for(int i=p_hover_start; i<=p_hover_end; i++) {
            // Check cut
            if (state->pred_lines[i].lgint < state->pred_min_log_int || 
                state->pred_lines[i].lgint > state->pred_max_log_int) continue;

            double d = fabs(state->pred_lines[i].freq_mhz - state->pbar_x);
            if(d < closest_dist) { closest_dist = d; closest_idx = i; }
        }

        if(closest_idx >= 0 && closest_dist < freq_tolerance && state->n_selected == 0) {
            PredLine *p = &state->pred_lines[closest_idx];
            char h1[128];
            snprintf(h1, sizeof(h1), "Hov: %2d %2d %2d -> %2d %2d %2d (%.3f)", 
                p->Ju, p->Kau, p->Kcu, p->Jl, p->Kal, p->Kcl, p->freq_mhz);
            draw_text(ren, font, h1, l->pred_x + 10, l->pred_y + 10, (SDL_Color){255,165,0,255});
        }
    }
}
