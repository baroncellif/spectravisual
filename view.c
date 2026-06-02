#include "view.h"
#include "layout.h" 
#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- INTERNAL CONSTANTS & PALETTE ---
static const SDL_Color COL_BG           = {15, 17, 20, 255};
static const SDL_Color COL_PANEL        = {22, 25, 29, 255};
static const SDL_Color COL_GRID         = {78, 84, 92, 120};
static const SDL_Color COL_AXIS         = {190, 196, 204, 210};
static const SDL_Color COL_TXT          = {232, 238, 245, 255};
static const SDL_Color COL_TXT_DIM      = {145, 153, 164, 255};
static const SDL_Color COL_ACCENT       = {64, 190, 215, 255};
static const SDL_Color COL_SEL_BOX      = {64, 190, 215, 42};
static const SDL_Color COL_INPUT_BG     = {10, 12, 15, 255};
static const SDL_Color COL_INPUT_BORDER = {70, 78, 88, 255};

#define UI_TITLE_H 22
#define UI_TOOLBAR_Y 22
#define UI_TOOLBAR_H 34
#define UI_STATUS_Y 56
#define UI_STATUS_H 20
#define UI_PANEL_HEADER_H 20
#define UI_INFO_H 24

// --- INTERNAL HELPERS PROTOTYPES ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_onboarding(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_help_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_top_chrome(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_panel_headers(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_info_bar(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static const char *short_path(const char *path);
static int pred_line_is_selected(AppState *state, int idx);
static void draw_pred_line(SDL_Renderer *ren, AppState *state, Layout *l, int idx, int sx, int sy1);
static double tick_step_for_pixels(double range, int pixels, int min_px);

static double tick_step_for_pixels(double range, int pixels, int min_px) {
    if (range <= 0 || pixels <= 0) return 1.0;
    double max_ticks = pixels / (double)min_px;
    if (max_ticks < 2.0) max_ticks = 2.0;

    double target = range / max_ticks;
    double base = pow(10.0, floor(log10(target)));
    double frac = target / base;

    if (frac <= 1.0) return base;
    if (frac <= 2.0) return 2.0 * base;
    if (frac <= 5.0) return 5.0 * base;
    return 10.0 * base;
}

// --- MAIN RENDER ENTRY POINT ---
void render_app(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    // 1. Clear Screen
    SDL_SetRenderDrawColor(ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(ren);

    draw_top_chrome(ren, font, state, l);

    // 2. Draw Graphs / Start Screen
    if (state->data_loaded) {
        draw_spectrum_view(ren, font, state, l);
        draw_prediction_view(ren, font, state, l);
    } else {
        draw_onboarding(ren, font, state, l);
    }

    // 3. Draw Selection Rect (if dragging)
    if(state->data_loaded && (state->selecting_left || state->selecting_right)) {
        SDL_SetRenderDrawColor(ren, COL_SEL_BOX.r, COL_SEL_BOX.g, COL_SEL_BOX.b, COL_SEL_BOX.a); 
        int rx = (state->sel_cur.x < state->sel_start.x) ? state->sel_cur.x : state->sel_start.x;
        int rw = abs(state->sel_cur.x - state->sel_start.x);
        SDL_Rect r = { rx, l->exp_y, rw, l->exp_h }; 
        SDL_RenderFillRect(ren, &r);
    }

    // 4. UI Elements (Windows, Buttons, Cursor Info)
    draw_panel_headers(ren, font, state, l);
    draw_info_bar(ren, font, state, l);
    draw_ui_overlays(ren, font, state, l);
    if (state->data_loaded) draw_cursor_overlay(ren, font, state, l);
    if (state->show_help) draw_help_overlay(ren, font, state, l);

    // 5. Present
    SDL_RenderPresent(ren);
}

// --- SPECTRUM (EXPERIMENTAL) ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    // Clipping
    SDL_Rect clip = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_SetRenderDrawColor(ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, COL_PANEL.a);
    SDL_RenderFillRect(ren, &clip);
    SDL_RenderSetClipRect(ren, &clip);
    
    // Draw Data Lines
    SDL_SetRenderDrawColor(ren, 205, 214, 225, 235);
    
    int start_idx = binary_search_lower(state->current_pts, state->n_pts, state->vxmin);
    int end_idx   = binary_search_upper(state->current_pts, state->n_pts, state->vxmax);
    
    // Safety clamp
    if (start_idx < 1) start_idx = 1; 
    if (end_idx >= state->n_pts) end_idx = state->n_pts - 1;

    for(int i = start_idx; i <= end_idx; i++) {
         double x1_val = state->current_pts[i-1].x;
         double x2_val = state->current_pts[i].x;
         
         int px1 = l->exp_x + (x1_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
         int px2 = l->exp_x + (x2_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
         
         // Invert Y for graphics coordinates
         int py1 = l->exp_y + (1.0 - (state->current_pts[i-1].y - state->vymin)/(state->vymax - state->vymin)) * l->exp_h;
         int py2 = l->exp_y + (1.0 - (state->current_pts[i].y   - state->vymin)/(state->vymax - state->vymin)) * l->exp_h;
         
         SDL_RenderDrawLine(ren, px1, py1, px2, py2);
    }

    // Draw already-assigned experimental frequencies loaded from config/LIN.
    if (state->n_lin_data > 0) {
        SDL_SetRenderDrawColor(ren, 115, 225, 145, 130);
        int marker_top = l->exp_y + 4;
        int marker_bottom = l->exp_y + l->exp_h - 4;

        for (int i = 0; i < state->n_lin_data; i++) {
            double f = state->lin_data[i];
            if (f < state->vxmin || f > state->vxmax) continue;

            int px = l->exp_x + (f - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_RenderDrawLine(ren, px, marker_top, px, marker_bottom);
            SDL_Rect cap = {px - 2, marker_top, 5, 5};
            SDL_RenderFillRect(ren, &cap);
        }
    }
    
    // Draw Found Peaks
    for (int ip = 0; ip < state->n_peaks; ip++) {
        double pkx = state->peaks[ip].x;
        if (pkx < state->vxmin || pkx > state->vxmax) continue;
        
        int px = l->exp_x + (pkx - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
        SDL_SetRenderDrawColor(ren, 245, 210, 75, 220);
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);
        
        char label[64]; snprintf(label, sizeof(label), "%.3f", pkx);
        SDL_Rect tag = {px + 4, l->exp_y + 8, 74, 22};
        if (tag.x + tag.w < l->exp_x + l->exp_w) {
            fill_rounded_rect(ren, tag, 4, (SDL_Color){34, 30, 12, 210});
            draw_text(ren, font, label, tag.x + 6, tag.y + 4, (SDL_Color){245,210,75,255});
        }
    }

    // Draw Navigation Bar (Red)
    if(state->bar_active) {
        if(state->bar_x >= state->vxmin && state->bar_x <= state->vxmax) {
            int bx = l->exp_x + (state->bar_x - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 255, 95, 95, 230);
            SDL_RenderDrawLine(ren, bx, l->exp_y, bx, l->exp_y + l->exp_h);
        }
    }

    // DISABLE CLIPPING so we can draw borders and ticks
    SDL_RenderSetClipRect(ren, NULL);

    // Draw Borders
    SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
    SDL_RenderDrawRect(ren, &clip);
    
    // Draw X-Axis Ticks & Labels
    double xrange = state->vxmax - state->vxmin; 
    double xstep = tick_step_for_pixels(xrange, l->exp_w, 90);
    double xstart = ceil(state->vxmin/xstep)*xstep;
    
    for(double x=xstart; x<=state->vxmax; x+=xstep) {
        int px = l->exp_x + (x - state->vxmin)/xrange * l->exp_w;
        SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 60);
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);
        SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
        SDL_RenderDrawLine(ren, px, l->exp_y + l->exp_h, px, l->exp_y + l->exp_h + 5);
        
        char buf[32]; snprintf(buf,sizeof(buf),"%.3f", x);
        draw_text(ren, font, buf, px - 24, l->exp_y + l->exp_h + 8, COL_TXT);
    }
    
    // X-Axis Title
    if (l->gap >= 45 && l->exp_w > 360) {
        draw_text(ren, font, "Frequency / MHz", l->exp_x + l->exp_w/2 - 60, l->exp_y + l->exp_h + 28, COL_TXT_DIM);
    }
    
    // --- Y-Axis Ticks & Labels (NEW) ---
    double yrange = state->vymax - state->vymin;
    if(yrange > 0) {
        double ystep = tick_step_for_pixels(yrange, l->exp_h, 34);
        double ystart = ceil(state->vymin/ystep)*ystep;

        for(double y=ystart; y<=state->vymax; y+=ystep) {
            int py = l->exp_y + (1.0 - (y - state->vymin)/yrange) * l->exp_h;
            
            // Draw Tick on left axis
            SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
            SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x - 5, py);
            SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 45);
            SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x + l->exp_w, py);
            
            // Draw Label (Scientific notation)
            char buf[32]; 
            snprintf(buf, sizeof(buf), "%.1e", y);
            if (py > l->exp_y + 6 && py < l->exp_y + l->exp_h - 6) {
                draw_text(ren, font, buf, l->exp_x - 65, py - 7, COL_TXT_DIM);
            }
        }
    }
    // Y-Axis Title
    if (l->exp_h > 160) {
        draw_text_vertical(ren, font, "Intensity", l->exp_x - 75, l->exp_y + l->exp_h/2 + 30, COL_TXT_DIM);
    }
}

// --- PREDICTION (PICKETT) ---
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    SDL_Rect pred_rect = {l->pred_x, l->pred_y, l->pred_w, l->pred_h};
    SDL_SetRenderDrawColor(ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, COL_PANEL.a);
    SDL_RenderFillRect(ren, &pred_rect);
    
    // Draw Border
    SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
    SDL_RenderDrawRect(ren, &pred_rect);

    // Grid Lines for Pred
    SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
    SDL_RenderDrawLine(ren, l->pred_x, l->pred_y + l->pred_h, l->pred_x + l->pred_w, l->pred_y + l->pred_h);

    if (state->n_pred > 0) {
        SDL_RenderSetClipRect(ren, &pred_rect);
        
        // Range filtering in raw prediction coordinates. exp_offset is displayed
        // as a visual shift applied to predictions.
        int p_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin - state->exp_offset);
        int p_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax - state->exp_offset);
        if(p_start < 0) p_start = 0; 
        if(p_end >= state->n_pred) p_end = state->n_pred - 1;

        // Draw all predicted lines. Lines that collapse into the same pixel column
        // are spread by a few pixels so blended transitions remain visible.
        int group_idx[256];
        int group_sy[256];
        int group_sx = -1000000;
        int group_n = 0;
        int overflow_n = 0;

        for (int i=p_start; i<=p_end; i++) {
            if (state->pred_lines[i].lgint < state->pred_min_log_int) continue;
            if (state->pred_lines[i].lgint > state->pred_max_log_int) continue;

            double h_ratio = (state->pred_lines[i].linear_int / state->pred_global_max) * state->pred_scale;
            if(h_ratio > 1.0) h_ratio = 1.0;

            double shifted_freq = state->pred_lines[i].freq_mhz + state->exp_offset;
            int sx = l->pred_x + (shifted_freq - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
            int sy1 = l->pred_y + l->pred_h - (int)(h_ratio * (l->pred_h - 10));

            if (group_n > 0 && sx != group_sx) {
                int draw_n = group_n;
                if (draw_n > 15) draw_n = 15;
                for (int k = 0; k < draw_n; k++) {
                    int offset = (draw_n == 1) ? 0 : (k - (draw_n - 1) / 2);
                    if (draw_n > 7) offset = (offset * 8) / draw_n;
                    draw_pred_line(ren, state, l, group_idx[k], group_sx + offset, group_sy[k]);
                }
                if (group_n + overflow_n > 1) {
                    SDL_Rect cluster_mark = {group_sx - 4, l->pred_y + 5, 9, 5};
                    fill_rounded_rect(ren, cluster_mark, 2, (SDL_Color){235, 240, 245, 145});
                }
                group_n = 0;
                overflow_n = 0;
            }

            group_sx = sx;
            if (group_n < 256) {
                group_idx[group_n] = i;
                group_sy[group_n] = sy1;
                group_n++;
            } else {
                overflow_n++;
            }
        }

        if (group_n > 0) {
            int draw_n = group_n;
            if (draw_n > 15) draw_n = 15;
            for (int k = 0; k < draw_n; k++) {
                int offset = (draw_n == 1) ? 0 : (k - (draw_n - 1) / 2);
                if (draw_n > 7) offset = (offset * 8) / draw_n;
                draw_pred_line(ren, state, l, group_idx[k], group_sx + offset, group_sy[k]);
            }
            if (group_n + overflow_n > 1) {
                    SDL_Rect cluster_mark = {group_sx - 4, l->pred_y + 5, 9, 5};
                    fill_rounded_rect(ren, cluster_mark, 2, (SDL_Color){235, 240, 245, 145});
            }
        }

        // Broadening Simulation (Lorentzian)
        if(state->broadening_active) {
            SDL_SetRenderDrawColor(ren, 0, 255, 255, 150); // Cyan transparent
            int steps = l->pred_w; 
            double prev_y = -1; 
            double cutoff = 50.0 * state->lorentz_gamma;
            
            // Optimization: Only calc lines near visible window + cutoff
            int p_calc_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin - state->exp_offset - cutoff);
            int p_calc_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax - state->exp_offset + cutoff);
            if(p_calc_start < 0) p_calc_start=0; 
            if(p_calc_end >= state->n_pred) p_calc_end=state->n_pred-1;

            for(int i=0; i<steps; i++) {
                double f = state->pvxmin + (double)i/l->pred_w * (state->pvxmax - state->pvxmin);
                double raw_f = f - state->exp_offset;
                double intensity_sum = 0;
                
                for(int k=p_calc_start; k<=p_calc_end; k++) {
                    double dist = raw_f - state->pred_lines[k].freq_mhz;
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
                SDL_SetRenderDrawColor(ren, 255, 150, 70, 230);
                SDL_RenderDrawLine(ren, bx, l->pred_y, bx, l->pred_y + l->pred_h);
            }
        }

        // Draw assigned-frequency dots in the prediction pane as a compact locator.
        if (state->n_lin_data > 0) {
            SDL_SetRenderDrawColor(ren, 115, 225, 145, 200);
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
    double xstep = tick_step_for_pixels(xrange, l->pred_w, 90);
    double xstart = ceil(state->pvxmin/xstep)*xstep;
    for(double x=xstart; x<=state->pvxmax; x+=xstep) {
        int px = l->pred_x + (x - state->pvxmin)/xrange * l->pred_w;
        SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, COL_GRID.a);
        SDL_RenderDrawLine(ren, px, l->pred_y, px, l->pred_y + l->pred_h);
        SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
        SDL_RenderDrawLine(ren, px, l->pred_y + l->pred_h, px, l->pred_y + l->pred_h + 4);
        char buf[32]; snprintf(buf,sizeof(buf),"%.3f",x);
        draw_text(ren, font, buf, px - 24, l->pred_y + l->pred_h + 8, COL_TXT_DIM);
    }
}

static int pred_line_is_selected(AppState *state, int idx) {
    for (int k = 0; k < state->n_selected; k++) {
        if (state->selected_indices[k] == idx) return 1;
    }
    return 0;
}

static const char *short_path(const char *path) {
    const char *slash;
    if (!path || !*path) return "-";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void draw_top_chrome(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    SDL_Rect title = {0, 0, l->win_w, UI_TITLE_H};
    SDL_Rect toolbar = {0, UI_TOOLBAR_Y, l->win_w, UI_TOOLBAR_H};
    SDL_Rect status = {0, UI_STATUS_Y, l->win_w, UI_STATUS_H};
    char buf[256];

    SDL_SetRenderDrawColor(ren, 15, 17, 21, 255);
    SDL_RenderFillRect(ren, &title);
    SDL_SetRenderDrawColor(ren, 30, 34, 48, 255);
    SDL_RenderDrawLine(ren, 0, UI_TITLE_H - 1, l->win_w, UI_TITLE_H - 1);

    draw_text(ren, font, "SpectraVisual", 14, 6, COL_TXT);
    snprintf(buf, sizeof(buf), "%s - %s", short_path(state->exp_path), short_path(state->pred_path));
    if (l->win_w > 720) draw_text(ren, font, buf, 154, 6, COL_TXT_DIM);

    SDL_SetRenderDrawColor(ren, 19, 22, 28, 255);
    SDL_RenderFillRect(ren, &toolbar);
    SDL_SetRenderDrawColor(ren, 30, 34, 48, 255);
    SDL_RenderDrawLine(ren, 0, UI_TOOLBAR_Y + UI_TOOLBAR_H - 1, l->win_w, UI_TOOLBAR_Y + UI_TOOLBAR_H - 1);

    SDL_SetRenderDrawColor(ren, 13, 15, 19, 255);
    SDL_RenderFillRect(ren, &status);
    SDL_SetRenderDrawColor(ren, 30, 34, 48, 255);
    SDL_RenderDrawLine(ren, 0, UI_STATUS_Y + UI_STATUS_H - 1, l->win_w, UI_STATUS_Y + UI_STATUS_H - 1);

    fill_rounded_rect(ren, (SDL_Rect){14, UI_STATUS_Y + 7, 6, 6}, 3,
                      state->data_loaded ? (SDL_Color){34, 197, 94, 255} : (SDL_Color){248, 187, 68, 255});
    // Status line: only show errors / transient messages / the drop hint.
    // (Point counts, frequency and intensity ranges are intentionally omitted;
    //  the axes already convey ranges and the readouts live in the info bar.)
    if (state->error_message[0]) {
        draw_text(ren, font, state->error_message, 30, UI_STATUS_Y + 3, (SDL_Color){255, 170, 175, 255});
    } else if (!state->data_loaded && state->status_message[0]) {
        draw_text(ren, font, state->status_message, 30, UI_STATUS_Y + 3, COL_TXT_DIM);
    } else if (!state->data_loaded) {
        draw_text(ren, font, "Drop a spectrum file and a .cat file, or launch with both paths from Terminal.", 30, UI_STATUS_Y + 3, COL_TXT_DIM);
    }
}

static void draw_panel_headers(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    char buf[128];
    SDL_Rect exp_head = {l->exp_x, l->exp_y - UI_PANEL_HEADER_H, l->exp_w, UI_PANEL_HEADER_H};
    SDL_Rect pred_head = {l->pred_x, l->pred_y - UI_PANEL_HEADER_H, l->pred_w, UI_PANEL_HEADER_H};

    fill_rounded_rect(ren, exp_head, 0, (SDL_Color){15, 18, 24, 255});
    fill_rounded_rect(ren, pred_head, 0, (SDL_Color){15, 18, 24, 255});
    SDL_SetRenderDrawColor(ren, 30, 34, 48, 255);
    SDL_RenderDrawLine(ren, exp_head.x, exp_head.y + exp_head.h - 1, exp_head.x + exp_head.w, exp_head.y + exp_head.h - 1);
    SDL_RenderDrawLine(ren, pred_head.x, pred_head.y + pred_head.h - 1, pred_head.x + pred_head.w, pred_head.y + pred_head.h - 1);

    draw_text(ren, font, "EXPERIMENTAL SPECTRUM", exp_head.x + 14, exp_head.y + 3, (SDL_Color){100, 110, 126, 255});

    draw_text(ren, font, "PREDICTION", pred_head.x + 14, pred_head.y + 3, (SDL_Color){100, 110, 126, 255});
    if (state->data_loaded && pred_head.w > 760) {
        snprintf(buf, sizeof(buf), "%d lines   %s", state->n_pred, state->sync_active ? "synced" : "free");
        draw_text(ren, font, buf, pred_head.x + 120, pred_head.y + 3, COL_TXT_DIM);
    }
}

// Draws "label" (dim) followed by "value" (coloured), e.g. "Cursor: 4231.872 MHz".
static void draw_kv(SDL_Renderer *ren, TTF_Font *font, const char *label,
                    const char *value, int x, int y, SDL_Color vcol) {
    draw_text(ren, font, label, x, y, COL_TXT_DIM);
    int w = 0, h = 0;
    TTF_SizeUTF8(font, label, &w, &h);
    draw_text(ren, font, value, x + w + 5, y, vcol);
}

static void draw_info_bar(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    SDL_Rect info = {0, l->win_h - UI_INFO_H, l->win_w, UI_INFO_H};
    char buf[128];
    int mx, my;

    SDL_SetRenderDrawColor(ren, 11, 13, 16, 255);
    SDL_RenderFillRect(ren, &info);
    SDL_SetRenderDrawColor(ren, 26, 30, 40, 255);
    SDL_RenderDrawLine(ren, 0, info.y, l->win_w, info.y);

    int ty = info.y + 5;
    SDL_GetMouseState(&mx, &my);
    if (state->data_loaded && point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
        double fx = (mx - l->exp_x) / (double)l->exp_w;
        double fy = 1.0 - (my - l->exp_y) / (double)l->exp_h;
        double cx = state->vxmin + fx * (state->vxmax - state->vxmin);
        double cy = state->vymin + fy * (state->vymax - state->vymin);
        snprintf(buf, sizeof(buf), "%.4f MHz", cx);
        draw_kv(ren, font, "Cursor:", buf, 14, ty, COL_ACCENT);
        snprintf(buf, sizeof(buf), "%.2e", cy);
        draw_kv(ren, font, "Int:", buf, 190, ty, COL_TXT);
    } else {
        draw_kv(ren, font, "Cursor:", "-", 14, ty, COL_TXT_DIM);
        draw_kv(ren, font, "Int:", "-", 190, ty, COL_TXT_DIM);
    }

    if (state->bar_active && state->data_loaded) {
        snprintf(buf, sizeof(buf), "%.4f MHz", state->bar_x);
        draw_kv(ren, font, "Bar:", buf, 320, ty, (SDL_Color){251, 191, 36, 255});
    } else {
        draw_kv(ren, font, "Bar:", "off", 320, ty, COL_TXT_DIM);
    }

    if (l->win_w > 760) {
        snprintf(buf, sizeof(buf), "%d", state->n_peaks);
        draw_kv(ren, font, "Peaks:", buf, l->win_w - 350, ty, COL_TXT_DIM);
    }
    if (l->win_w > 900) {
        snprintf(buf, sizeof(buf), "%d", state->n_assignments);
        draw_kv(ren, font, "Assignments:", buf, l->win_w - 230, ty, COL_ACCENT);
    }
    if (state->data_loaded && l->win_w > 1080) {
        snprintf(buf, sizeof(buf), "%.1f MHz", state->vxmax - state->vxmin);
        draw_kv(ren, font, "Zoom:", buf, l->win_w - 105, ty, COL_TXT_DIM);
    }
}

static void draw_pred_line(SDL_Renderer *ren, AppState *state, Layout *l, int idx, int sx, int sy1) {
    SDL_Color c = color_for_pred(state->pred_lines[idx].branch, state->pred_lines[idx].mu);
    if (pred_line_is_selected(state, idx)) {
        SDL_SetRenderDrawColor(ren, 105, 255, 150, 120);
        SDL_RenderDrawLine(ren, sx - 2, l->pred_y + l->pred_h, sx - 2, sy1);
        SDL_RenderDrawLine(ren, sx + 2, l->pred_y + l->pred_h, sx + 2, sy1);
        SDL_SetRenderDrawColor(ren, 105, 255, 150, 255);
        SDL_RenderDrawLine(ren, sx - 1, l->pred_y + l->pred_h, sx - 1, sy1);
        SDL_RenderDrawLine(ren, sx + 1, l->pred_y + l->pred_h, sx + 1, sy1);
    } else {
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 195);
    }
    SDL_RenderDrawLine(ren, sx, l->pred_y + l->pred_h, sx, sy1);
}

// --- UI OVERLAYS (Buttons & Windows) ---
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    int mx, my; 
    int m_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    int by = UI_TOOLBAR_Y + 5;

    // 1. TOOLBAR BUTTONS  (icons come from the Arial Unicode icon font)
    int right_x = l->win_w - 18;
    Button btn_bar  = {{12,  by, 58, 24}, "Bar",   {14, 58, 71, 255}, 1, BTN_NORMAL,  "\xE2\x86\x91"}; // up
    Button btn_sync = {{76,  by, 70, 24}, "Sync",  {14, 58, 71, 255}, 1, BTN_NORMAL,  "\xE2\x86\xBB"}; // clockwise
    Button btn_del  = {{156, by, 58, 24}, "Del",   {70, 28, 30, 255}, 0, BTN_DANGER,  "\xE2\x9C\x95"}; // x
    Button btn_list = {{236, by, 64, 24}, "List",  {30, 34, 48, 255}, 0, BTN_NORMAL,  "\xE2\x98\xB0"}; // trigram
    Button btn_peak = {{306, by, 68, 24}, "Peak",  {30, 34, 48, 255}, 0, BTN_NORMAL,  "\xCE\x9B"};     // lambda
    Button btn_roll = {{380, by, 62, 24}, "Avg",   {30, 34, 48, 255}, 0, BTN_NORMAL,  "\xE2\x88\xBF"}; // sine
    Button btn_broad = {{448, by, 82, 24}, "Broad", {30, 34, 48, 255}, 0, BTN_NORMAL, "\xE2\x88\xA7"}; // wedge
    Button btn_cut = {{552, by, 58, 24}, "Cut",   {30, 34, 48, 255}, 0, BTN_NORMAL,  "\xE2\x86\x94"}; // leftright
    Button btn_jump = {{616, by, 78, 24}, "Jump",  {30, 34, 48, 255}, 0, BTN_NORMAL, "\xE2\x86\x92"}; // right

    SDL_SetRenderDrawColor(ren, 42, 47, 61, 255);
    SDL_RenderDrawLine(ren, 224, by + 4, 224, by + 20);
    SDL_RenderDrawLine(ren, 540, by + 4, 540, by + 20);

    draw_button(ren, font, &btn_bar, mx, my, m_down, state->bar_active);
    draw_button(ren, font, &btn_sync, mx, my, m_down, state->sync_active);
    draw_button(ren, font, &btn_del, mx, my, m_down, 0);
    draw_button(ren, font, &btn_list, mx, my, m_down, state->win_as.visible);
    draw_button(ren, font, &btn_peak, mx, my, m_down, state->win_pf.visible);
    if (l->win_w > 450) draw_button(ren, font, &btn_roll, mx, my, m_down, state->win_avg.visible);
    if (l->win_w > 540) draw_button(ren, font, &btn_broad, mx, my, m_down, state->win_br.visible || state->broadening_active);
    if (l->win_w > 620) draw_button(ren, font, &btn_cut, mx, my, m_down, state->win_cut.visible);
    if (l->win_w > 700) draw_button(ren, font, &btn_jump, mx, my, m_down, state->win_jump.visible);

    if (l->win_w > 900) {
        int input_w = 122;
        SDL_Rect r_off = {right_x - input_w, by, input_w, 24};
        right_x = r_off.x - 64;
        draw_text(ren, font, "Offset", r_off.x - 56, r_off.y + 4, COL_TXT_DIM);

        fill_rounded_rect(ren, r_off, 5, COL_INPUT_BG);
        SDL_Color border = (state->input_state == INPUT_OFFSET) ? COL_ACCENT : COL_INPUT_BORDER;
        SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, border.a);
        SDL_RenderDrawRect(ren, &r_off);

        char buf[32];
        if(state->input_state == INPUT_OFFSET) snprintf(buf, sizeof(buf), "%s_", state->text_input_buf);
        else snprintf(buf, sizeof(buf), "%.4f", state->exp_offset);
        draw_text(ren, font, buf, r_off.x + 7, r_off.y + 4, COL_TXT);
    }

    if (l->win_w > 980) {
        Button btn_export = {{right_x - 88, by, 82, 24}, "Export", {26, 58, 70, 255}, 0, BTN_PRIMARY, "\xE2\x86\x93"}; // down
        Button btn_help = {{right_x - 154, by, 60, 24}, "Help", {30, 34, 48, 255}, 0, BTN_NORMAL, "?"};
        SDL_SetRenderDrawColor(ren, 42, 47, 61, 255);
        SDL_RenderDrawLine(ren, right_x - 166, by + 4, right_x - 166, by + 20);
        draw_button(ren, font, &btn_help, mx, my, m_down, state->show_help);
        draw_button(ren, font, &btn_export, mx, my, m_down, 0);
    }

    // 2. BROADENING WINDOW
    if(state->win_br.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_br.clip);
        draw_draggable_window(ren, font, &state->win_br);
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
    if(state->win_avg.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_avg.clip);
        draw_draggable_window(ren, font, &state->win_avg);
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
    if(state->win_pf.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_pf.clip);
        draw_draggable_window(ren, font, &state->win_pf);
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
        Button btn_run = {{wx+50, wy+200, 200, 30}, "FIND PEAKS", {0, 150, 0, 255}, 0, BTN_PRIMARY};
        draw_button(ren, font, &btn_run, mx, my, m_down, 0);
        Button btn_exp = {{wx+50, wy+240, 200, 30}, "EXPORT LIST", {0, 100, 200, 255}, 0, BTN_PRIMARY};
        draw_button(ren, font, &btn_exp, mx, my, m_down, 0);
    }

    // 5. ASSIGNMENT WINDOW
    if(state->win_as.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_as.clip);
        draw_draggable_window(ren, font, &state->win_as);
        int wx = state->win_as.rect.x, wy = state->win_as.rect.y;
        int ty = wy + 60;
        
        SDL_Rect table_head = {wx + 14, ty - 6, state->win_as.rect.w - 28, 28};
        fill_rounded_rect(ren, table_head, 5, (SDL_Color){18, 21, 25, 220});
        draw_text(ren, font, "Quantum numbers", wx+24, ty, COL_TXT_DIM);
        draw_text(ren, font, "Exp Freq", wx+300, ty, COL_TXT_DIM);
        ty += 30;
        
        int visible_rows = 13;
        int start_idx = state->assignments_scroll;
        if (start_idx < 0) start_idx = 0;
        if (start_idx > state->n_assignments - visible_rows) start_idx = state->n_assignments - visible_rows;
        if (start_idx < 0) start_idx = 0;
        int end_idx = start_idx + visible_rows;
        if (end_idx > state->n_assignments) end_idx = state->n_assignments;

        for(int k=start_idx; k<end_idx; k++) {
            PredLine p = state->assignments[k].pred;
            char row[256];
            SDL_Rect row_bg = {wx + 14, ty - 2, state->win_as.rect.w - 28, 19};
            if (k == state->selected_assignment) {
                fill_rounded_rect(ren, row_bg, 4, (SDL_Color){0, 120, 135, 190});
            } else if ((k - start_idx) % 2 == 0) {
                SDL_SetRenderDrawColor(ren, 30, 34, 40, 130);
                SDL_RenderFillRect(ren, &row_bg);
            }
            snprintf(row, sizeof(row), "  %2d %2d %2d %2d %2d %2d -> %2d %2d %2d %2d %2d %2d  %9.3f ",
                 p.Ju, p.Kau, p.Kcu,p.M1u,p.M2u,p.M3u, p.Jl, p.Kal, p.Kcl,p.M1l,p.M2l,p.M3l,
                 state->assignments[k].exp_freq);
            draw_text(ren, font, row, wx+20, ty, COL_TXT);
            ty += 20;
        }

        if (state->n_assignments > visible_rows) {
            char count[64];
            snprintf(count, sizeof(count), "%d-%d / %d", start_idx + 1, end_idx, state->n_assignments);
            draw_text(ren, font, count, wx + state->win_as.rect.w - 115, wy + 360, COL_TXT_DIM);
        }

        Button btn_save = {{wx+10, wy+360, 100, 30}, "Save All", {0, 100, 200, 255}, 0, BTN_PRIMARY};
        draw_button(ren, font, &btn_save, mx, my, m_down, 0);
        Button btn_del_as = {{wx+120, wy+360, 110, 30}, "Delete", {170, 60, 60, 255}, 0, BTN_DANGER};
        draw_button(ren, font, &btn_del_as, mx, my, m_down, state->selected_assignment >= 0);
    }
    
    // NEW: 6. INTENSITY CUT WINDOW (Triggered by 'C')
    if(state->win_cut.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_cut.clip);
        draw_draggable_window(ren, font, &state->win_cut);
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
    if(state->win_jump.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_jump.clip);
        draw_draggable_window(ren, font, &state->win_jump);
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

    SDL_RenderSetClipRect(ren, NULL);
}

static void draw_onboarding(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    SDL_Rect panel = {l->win_w / 2 - 330, l->win_h / 2 - 150, 660, 300};
    if (panel.x < 24) panel.x = 24;
    if (panel.w > l->win_w - 48) panel.w = l->win_w - 48;

    fill_rounded_rect(ren, panel, 10, (SDL_Color){22, 25, 29, 245});
    SDL_SetRenderDrawColor(ren, COL_INPUT_BORDER.r, COL_INPUT_BORDER.g, COL_INPUT_BORDER.b, 180);
    SDL_RenderDrawRect(ren, &panel);

    draw_text(ren, font, "SpectraVisual", panel.x + 28, panel.y + 24, COL_TXT);
    draw_text(ren, font, "Drop a spectrum file and a pred.cat file anywhere in this window.", panel.x + 28, panel.y + 62, COL_TXT_DIM);
    draw_text(ren, font, "Or launch from Terminal:", panel.x + 28, panel.y + 100, COL_TXT_DIM);
    draw_text(ren, font, "spectravisual spectrum.txt pred.cat", panel.x + 48, panel.y + 126, COL_ACCENT);
    draw_text(ren, font, "Add --verbose only when you want terminal logs.", panel.x + 28, panel.y + 164, COL_TXT_DIM);
    draw_text(ren, font, "Press H for shortcuts. Press X after loading to export the current view.", panel.x + 28, panel.y + 202, COL_TXT_DIM);

    if (state->exp_path[0] || state->pred_path[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Spectrum: %s", state->exp_path[0] ? state->exp_path : "waiting...");
        draw_text(ren, font, buf, panel.x + 28, panel.y + 238, state->exp_path[0] ? COL_TXT_DIM : COL_ACCENT);
        snprintf(buf, sizeof(buf), "Prediction: %s", state->pred_path[0] ? state->pred_path : "waiting...");
        draw_text(ren, font, buf, panel.x + 28, panel.y + 260, state->pred_path[0] ? COL_TXT_DIM : COL_ACCENT);
    }
}

static void draw_help_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)state;
    SDL_Rect panel = {l->win_w / 2 - 330, 82, 660, 420};
    if (panel.x < 24) panel.x = 24;
    if (panel.w > l->win_w - 48) panel.w = l->win_w - 48;

    fill_rounded_rect(ren, panel, 10, (SDL_Color){12, 14, 17, 238});
    SDL_SetRenderDrawColor(ren, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 190);
    SDL_RenderDrawRect(ren, &panel);

    int x = panel.x + 28;
    int y = panel.y + 24;
    draw_text(ren, font, "Shortcuts", x, y, COL_TXT);
    y += 36;
    draw_text(ren, font, "Mouse: left-drag zoom, right-drag peak pick, Option-left-drag prediction offset", x, y, COL_TXT_DIM); y += 28;
    draw_text(ren, font, "A/S pan, Q/E zoom, Tab autoscale intensity, R reset view", x, y, COL_TXT_DIM); y += 28;
    draw_text(ren, font, "Shift + W/Z or arrows scales predicted intensity", x, y, COL_TXT_DIM); y += 28;
    draw_text(ren, font, "K/L move bar, G measure, H or ? help, X export BMP", x, y, COL_TXT_DIM); y += 28;
    draw_text(ren, font, "N assignments, P peak finder, T rolling average, M broadening", x, y, COL_TXT_DIM); y += 28;
    draw_text(ren, font, "C intensity cut, F frequency jump, Delete removes latest peak", x, y, COL_TXT_DIM); y += 42;
    draw_text(ren, font, "Input fields", x, y, COL_TXT); y += 30;
    draw_text(ren, font, "Enter confirms, Esc cancels, clicking elsewhere confirms and continues.", x, y, COL_TXT_DIM); y += 42;
    draw_text(ren, font, "Export", x, y, COL_TXT); y += 30;
    draw_text(ren, font, "X or Export saves spectravisual_export.bmp in the current directory.", x, y, COL_TXT_DIM);
}

static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    int mx, my;
    SDL_GetMouseState(&mx, &my);

    if (state->dragging_offset) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Offset %.4f MHz", state->exp_offset);
        SDL_Rect badge = {l->exp_x + l->exp_w - 190, l->exp_y + 42, 180, 26};
        fill_rounded_rect(ren, badge, 5, (SDL_Color){8, 24, 30, 225});
        draw_text(ren, font, buf, badge.x + 10, badge.y + 5, COL_ACCENT);
    }

    if (state->selecting_left || state->selecting_right) {
        double x0 = state->vxmin + (double)(state->sel_start.x - l->exp_x)/l->exp_w * (state->vxmax - state->vxmin);
        double x1 = state->vxmin + (double)(state->sel_cur.x - l->exp_x)/l->exp_w * (state->vxmax - state->vxmin);
        if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }

        char buf[96];
        snprintf(buf, sizeof(buf), "%s %.4f MHz", state->selecting_right ? "Peak search" : "Zoom", x1 - x0);
        SDL_Rect badge = {state->sel_cur.x + 10, state->sel_cur.y - 34, 170, 26};
        if (badge.x + badge.w > l->exp_x + l->exp_w) badge.x = state->sel_cur.x - badge.w - 10;
        if (badge.y < l->exp_y) badge.y = l->exp_y + 8;
        fill_rounded_rect(ren, badge, 5, (SDL_Color){8, 10, 12, 225});
        draw_text(ren, font, buf, badge.x + 10, badge.y + 5, state->selecting_right ? (SDL_Color){245,210,75,255} : COL_ACCENT);
    }

    if (state->n_peaks > 0 && !state->selecting_right) {
        Peak *last = &state->peaks[state->n_peaks - 1];
        if (last->x >= state->vxmin && last->x <= state->vxmax) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Last peak %.4f", last->x);
            SDL_Rect badge = {l->exp_x + 10, l->exp_y + l->exp_h - 34, 155, 24};
            fill_rounded_rect(ren, badge, 5, (SDL_Color){34, 30, 12, 190});
            draw_text(ren, font, buf, badge.x + 9, badge.y + 4, (SDL_Color){245,210,75,255});
        }
    }

    // 1. Mouse Position Info
    if (point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
        double fx = (mx - l->exp_x) / (double)l->exp_w;
        double fy = 1.0 - (my - l->exp_y) / (double)l->exp_h;
        double cx = state->vxmin + fx * (state->vxmax - state->vxmin);
        double cy = state->vymin + fy * (state->vymax - state->vymin);
        
        char c[128];
        snprintf(c, sizeof(c), "f %.3f MHz   I %.2e", cx, cy);

        SDL_Rect badge = {l->exp_x + 10, l->exp_y + 10, 230, 26};
        fill_rounded_rect(ren, badge, 5, (SDL_Color){8, 10, 12, 205});
        draw_text(ren, font, c, badge.x + 8, badge.y + 5, COL_TXT_DIM);
    }

    // 2. Measure Tool Overlay
    if (state->measure_active) {
        SDL_Rect badge = {l->exp_x + l->exp_w - 155, l->exp_y + 10, 145, 26};
        fill_rounded_rect(ren, badge, 5, (SDL_Color){55, 20, 75, 220});
        draw_text(ren, font, "MEASURE", badge.x + 12, badge.y + 5, (SDL_Color){245, 185, 255, 255});
        if (state->measure_phase == 1) {
            int px1 = l->exp_x + (state->measure_x1 - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 225, 100, 245, 210);
            SDL_RenderDrawLine(ren, px1, l->exp_y, px1, l->exp_y + l->exp_h);
            SDL_RenderDrawLine(ren, px1, my, mx, my);
            if (point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
                double curr_freq = state->vxmin + ((double)(mx - l->exp_x) / l->exp_w) * (state->vxmax - state->vxmin);
                double dist = fabs(curr_freq - state->measure_x1);
                char buf[64]; snprintf(buf, 64, "%.4f MHz", dist);
                draw_text(ren, font, buf, mx + 10, my - 20, (SDL_Color){245, 185, 255, 255});
            }
        }
    }

    // 3. Selection Info
    if(state->n_selected > 0) {
        char title[64];
        snprintf(title, sizeof(title), "Selected lines: %d", state->n_selected);
        SDL_Rect panel = {l->pred_x + 10, l->pred_y + 10, 395, 34 + (state->n_selected < 4 ? state->n_selected : 4) * 18};
        fill_rounded_rect(ren, panel, 5, (SDL_Color){8, 18, 12, 210});
        draw_text(ren, font, title, panel.x + 10, panel.y + 7, (SDL_Color){105,235,145,255});

        int show_n = state->n_selected < 4 ? state->n_selected : 4;
        for (int row_i = 0; row_i < show_n; row_i++) {
            PredLine *p = &state->pred_lines[state->selected_indices[row_i]];
            char row[192];
            snprintf(row, sizeof(row), "%2d %2d %2d %2d %2d %2d -> %2d %2d %2d %2d %2d %2d  %.4f",
                     p->Ju, p->Kau, p->Kcu, p->M1u, p->M2u, p->M3u,
                     p->Jl, p->Kal, p->Kcl, p->M1l, p->M2l, p->M3l,
                     p->freq_mhz);
            draw_text(ren, font, row, panel.x + 10, panel.y + 28 + row_i * 18, (SDL_Color){115,235,150,255});
        }
    }
    
    // 4. Hover Info
    if(state->bar_active) {
        int p_hover_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pbar_x - state->exp_offset - 1.0);
        int p_hover_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pbar_x - state->exp_offset + 1.0);
        if(p_hover_start < 0) p_hover_start = 0; 
        if(p_hover_end >= state->n_pred) p_hover_end = state->n_pred - 1;

        double closest_dist = 1e99; int closest_idx = -1;
        int hover_idx[4];
        int hover_n = 0;
        double freq_tolerance = (state->pvxmax - state->pvxmin) * 0.01; 

        for(int i=p_hover_start; i<=p_hover_end; i++) {
            // Check cut
            if (state->pred_lines[i].lgint < state->pred_min_log_int || 
                state->pred_lines[i].lgint > state->pred_max_log_int) continue;

            double d = fabs(state->pred_lines[i].freq_mhz + state->exp_offset - state->pbar_x);
            if(d < closest_dist) { closest_dist = d; closest_idx = i; }
            if (d < freq_tolerance && hover_n < 4) hover_idx[hover_n++] = i;
        }

        if(closest_idx >= 0 && closest_dist < freq_tolerance && state->n_selected == 0) {
            char htitle[64];
            snprintf(htitle, sizeof(htitle), "Near bar: %d line%s", hover_n, hover_n == 1 ? "" : "s");
            SDL_Rect panel = {l->pred_x + 10, l->pred_y + 10, 395, 34 + hover_n * 18};
            fill_rounded_rect(ren, panel, 5, (SDL_Color){28, 17, 8, 210});
            draw_text(ren, font, htitle, panel.x + 10, panel.y + 7, (SDL_Color){245,175,80,255});
            for (int row_i = 0; row_i < hover_n; row_i++) {
                PredLine *p = &state->pred_lines[hover_idx[row_i]];
                char h1[160];
                snprintf(h1, sizeof(h1), "%2d %2d %2d %2d %2d %2d -> %2d %2d %2d %2d %2d %2d  %.4f",
                    p->Ju, p->Kau, p->Kcu, p->M1u, p->M2u, p->M3u,
                    p->Jl, p->Kal, p->Kcl, p->M1l, p->M2l, p->M3l,
                    p->freq_mhz);
                draw_text(ren, font, h1, panel.x + 10, panel.y + 28 + row_i * 18, (SDL_Color){245,190,105,255});
            }
        }
    }
}
