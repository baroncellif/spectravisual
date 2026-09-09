#include "view.h"
#include "layout.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Pseudo-Voigt line profile (Thompson-Cox-Hastings), peak-normalized to 1.
// gl = Lorentzian HWHM, gg = Gaussian HWHM (both MHz).
//   gg == 0 -> pure Lorentzian, gl == 0 -> pure Gaussian, both > 0 -> Voigt.
static double broaden_profile(double dist, double gl, double gg) {
    if (gl <= 0.0 && gg <= 0.0) return 0.0;
    double fL = 2.0 * gl;   // Lorentzian FWHM
    double fG = 2.0 * gg;   // Gaussian FWHM
    // Combined FWHM (TCH empirical 5th-order mix).
    double f = pow(pow(fG, 5.0)
                 + 2.69269 * pow(fG, 4.0) * fL
                 + 2.42843 * pow(fG, 3.0) * fL * fL
                 + 4.47163 * fG * fG * pow(fL, 3.0)
                 + 0.07842 * fG * pow(fL, 4.0)
                 + pow(fL, 5.0), 0.2);
    double r = fL / f;      // Lorentzian fraction of total width (0..1)
    double eta = 1.36603 * r - 0.47719 * r * r + 0.11116 * r * r * r;
    double hwhm = 0.5 * f;
    double L = (hwhm * hwhm) / (dist * dist + hwhm * hwhm);
    double G = exp(-0.69314718056 * (dist * dist) / (hwhm * hwhm)); // ln(2)
    return eta * L + (1.0 - eta) * G;
}

// ============================================================================
//  Kaiser-FFT line profile.
//  Reproduces the lineshape produced by the experimental pipeline
//  (multifft.py):  FID -> x kaiser(N, beta) -> zero-pad(x ceros) -> |FFT|.
//
//  Key fact: in units of the spectrum bin, the lineshape depends ONLY on
//  (beta, ceros), not on N.  The "fundamental resolution" is
//      dnu = ceros * df_bin   (MHz)            [boxcar first-null spacing]
//  and the offset coordinate is  r = dist / dnu.
//  The kernel R(r) is the (real, symmetric) DTFT of the Kaiser window,
//  normalized to R(0)=1.  It is signed so that close lines interfere
//  coherently (matching |FFT| of an in-phase FID).
// ============================================================================
#define KAISER_RMAX   28.0    // kernel support, in fundamental-resolution units
#define KAISER_NK     5601    // table samples over [-RMAX, RMAX]  (dr = 0.01)
#define KAISER_WINN   4096    // window length used to evaluate the DTFT shape

static double  g_kk_table[KAISER_NK];
static double  g_kk_beta  = -1.0;   // cached params; rebuild when changed
static double  g_kk_gg    = -1.0;   // intrinsic FWHM in resolution units
static int     g_kk_built = 0;

// Modified Bessel function I0(x) via power series (x stays small here).
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, hx = 0.5 * x;
    for (int k = 1; k < 40; k++) {
        term *= (hx / k) * (hx / k);
        sum  += term;
        if (term < 1e-14 * sum) break;
    }
    return sum;
}

// Build the normalized signed kernel table.
//   beta = Kaiser parameter.
//   gg   = intrinsic (molecular) line FWHM in fundamental-resolution units.
//          It tapers the FID with a Gaussian decay exp(-(pi*GB*t)^2/4ln2),
//          which both broadens the main lobe and fills the Kaiser sidelobes,
//          exactly as a real decaying FID does.
static void kaiser_build_kernel(double beta, double gg) {
    if (g_kk_built && beta == g_kk_beta && gg == g_kk_gg) return;
    int M = KAISER_WINN;
    double i0b = bessel_i0(beta);
    static double w[KAISER_WINN];
    double wsum = 0.0;
    double cen0 = 0.5 * (M - 1);
    for (int n = 0; n < M; n++) {
        double t = (2.0 * n - (M - 1)) / (double)(M - 1);     // -1..+1
        w[n] = bessel_i0(beta * sqrt(1.0 - t * t)) / i0b;     // kaiser(M, beta)
        if (gg > 0.0) {
            // time fraction from window centre: (n-cen)/(M-1) maps to t/T
            double tf = (n - cen0) / (double)(M - 1);
            double a  = M_PI * gg * tf;
            w[n] *= exp(-(a * a) / (4.0 * 0.69314718056)); // Gaussian taper, FWHM=gg
        }
        wsum += w[n];
    }
    double cen = 0.5 * (M - 1);
    for (int i = 0; i < KAISER_NK; i++) {
        double r = -KAISER_RMAX + (2.0 * KAISER_RMAX) * i / (double)(KAISER_NK - 1);
        // R(r) = sum_n w[n] cos(2*pi*r*(n-cen)/M) / wsum
        double acc = 0.0;
        double ang0 = 2.0 * M_PI * r / (double)M;
        for (int n = 0; n < M; n++) acc += w[n] * cos(ang0 * (n - cen));
        g_kk_table[i] = acc / wsum;
    }
    g_kk_beta = beta;
    g_kk_gg = gg;
    g_kk_built = 1;
}

// Evaluate the cached kernel at resolution-coordinate r (linear interp).
static double kaiser_kernel(double r) {
    double a = fabs(r);
    if (a >= KAISER_RMAX) return 0.0;
    double fidx = (r + KAISER_RMAX) / (2.0 * KAISER_RMAX) * (KAISER_NK - 1);
    int i = (int)fidx;
    if (i < 0) i = 0;
    if (i >= KAISER_NK - 1) return g_kk_table[KAISER_NK - 1];
    double frac = fidx - i;
    return g_kk_table[i] * (1.0 - frac) + g_kk_table[i + 1] * frac;
}

// Spectrum frequency-bin step (MHz), read straight from the loaded trace.
static double spectrum_df(const AppState *s) {
    if (s->n_pts < 2) return 0.0;
    double d = s->current_pts[1].x - s->current_pts[0].x;
    return (d > 0.0) ? d : 0.0;
}

// --- PALETTE ---
// The chrome palette lives in ui_theme.h so the renderer and the design stay in
// step; these aliases keep the plotting code below reading the way it did.
#define COL_BG           UI_GROUND
#define COL_PANEL        UI_PLOT
#define COL_GRID         UI_GRID
#define COL_AXIS         UI_AXIS
#define COL_TXT          UI_TEXT
#define COL_TXT_DIM      UI_DIM
#define COL_ACCENT       UI_ACCENT
#define COL_INPUT_BG     UI_INPUT
#define COL_INPUT_BORDER UI_LINE
static const SDL_Color COL_SEL_BOX = {76, 154, 255, 40};

// Formats a frequency with a group separator: 12 452.7104
static void fmt_mhz(char *buf, size_t n, double v, int dec) {
    char raw[64];
    snprintf(raw, sizeof(raw), "%.*f", dec, fabs(v));
    char *dot = strchr(raw, '.');
    int ilen = (int)(dot ? (size_t)(dot - raw) : strlen(raw));
    char out[80]; int o = 0;
    if (v < 0 && o < (int)sizeof(out) - 1) out[o++] = '-';
    for (int i = 0; i < ilen && o < (int)sizeof(out) - 2; i++) {
        if (i > 0 && (ilen - i) % 3 == 0) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = '\0';
    snprintf(buf, n, "%s%s", out, dot ? dot : "");
}

static void fmt_count(char *buf, size_t n, long v) {
    char raw[32]; snprintf(raw, sizeof(raw), "%ld", v);
    int len = (int)strlen(raw);
    char out[48]; int o = 0;
    for (int i = 0; i < len && o < (int)sizeof(out) - 2; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = '\0';
    snprintf(buf, n, "%s", out);
}

// --- INTERNAL HELPERS PROTOTYPES ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_onboarding(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_help_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_top_chrome(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_rail(SDL_Renderer *ren, AppState *state, Layout *l);
static void draw_panel_headers(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l);
static void draw_status_bar(SDL_Renderer *ren, AppState *state, Layout *l);
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

    // The plot ground covers the whole content area, so the axis gutters are
    // part of the pane instead of showing the window ground behind them.
    ui_fill(ren, (SDL_Rect){UI_RAIL_W, UI_CONTENT_Y,
                            l->plot_right - UI_RAIL_W,
                            l->win_h - UI_CONTENT_Y - UI_STATUS_H}, UI_PLOT);

    // 2. Draw Graphs / Start Screen. Only draw a pane if it has data, so an
    // empty spectrum/prediction window isn't shown when only one was loaded.
    if (state->data_loaded) {
        if (state->n_spectra > 0) draw_spectrum_view(ren, font, state, l);
        if (state->n_pred > 0)    draw_prediction_view(ren, font, state, l);
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
    draw_rail(ren, state, l);
    draw_status_bar(ren, state, l);
    draw_ui_overlays(ren, font, state, l);
    if (state->data_loaded) draw_cursor_overlay(ren, font, state, l);
    if (state->show_help) draw_help_overlay(ren, font, state, l);

    // 5. Present
    SDL_RenderPresent(ren);
}

// --- SPECTRUM (EXPERIMENTAL) ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    // Clipping
    SDL_Rect clip = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_SetRenderDrawColor(ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, COL_PANEL.a);
    SDL_RenderFillRect(ren, &clip);
    SDL_RenderSetClipRect(ren, &clip);
    
    // OFFSET MODE: each spectrum is *displayed* shifted by its exp_offset so it
    // slides onto the (fixed) prediction. The stored data keeps true frequencies,
    // so the visible window in true coords is [vxmin - offset, vxmax - offset].
    //
    // Multi-spectrum layout: overlay (all in the panel, distinct colours, optional
    // per-trace vertical offset) or vertical stack (one band per visible trace).
    // Y scale: shared (global) or normalized per trace.
    int nvis = 0;
    for (int s = 0; s < state->n_spectra; s++) if (state->spectra[s].visible) nvis++;

    int band_h = (state->multi_layout && nvis > 0) ? (l->exp_h / nvis) : l->exp_h;
    int vi = 0;
    for (int s = 0; s < state->n_spectra; s++) {
        Spectrum *sp = &state->spectra[s];
        if (!sp->visible || sp->n_pts < 2) continue;

        const int STACK_GAP = 8;   // pixels between stacked subplots
        int area_y = state->multi_layout ? (l->exp_y + vi * band_h) : l->exp_y;
        int area_h = state->multi_layout ? (band_h - STACK_GAP) : l->exp_h;
        if (area_h < 10) area_h = 10;
        // Vertical offset only makes sense in overlay; stack is true subplots.
        double voff_px = state->multi_layout ? 0.0 : sp->voffset * area_h;
        // Y scale depends ONLY on shared/norm (overlay vs stack only changes the
        // layout, not the scaling):
        //   - norm   : each trace normalized to its own ymin/ymax (per-spectrum gain)
        //   - shared : common zoomable vymin/vymax range for everyone
        int per_spec_y = state->multi_ynorm;
        double gain = (per_spec_y && sp->vscale > 0.0) ? sp->vscale : 1.0;

        double ymn, ymx;
        if (per_spec_y) { ymn = sp->ymin;     ymx = sp->ymax;     }
        else            { ymn = state->vymin; ymx = state->vymax; }
        if (ymx <= ymn) ymx = ymn + 1.0;

        SDL_SetRenderDrawColor(ren, sp->color.r, sp->color.g, sp->color.b, sp->color.a);

        int start_idx = binary_search_lower(sp->current_pts, sp->n_pts, state->vxmin - sp->exp_offset);
        int end_idx   = binary_search_upper(sp->current_pts, sp->n_pts, state->vxmax - sp->exp_offset);
        if (start_idx < 1) start_idx = 1;
        if (end_idx >= sp->n_pts) end_idx = sp->n_pts - 1;

        // In stack mode confine the trace to its own subplot band, so a peak that
        // exceeds the band is clipped instead of overflowing into the one above.
        if (state->multi_layout)
            SDL_RenderSetClipRect(ren, &(SDL_Rect){l->exp_x, area_y, l->exp_w, area_h});

        for (int i = start_idx; i <= end_idx; i++) {
            double x1_val = sp->current_pts[i-1].x + sp->exp_offset;
            double x2_val = sp->current_pts[i].x   + sp->exp_offset;
            int px1 = l->exp_x + (x1_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
            int px2 = l->exp_x + (x2_val - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
            double f1 = (sp->current_pts[i-1].y - ymn)/(ymx - ymn) * gain;
            double f2 = (sp->current_pts[i].y   - ymn)/(ymx - ymn) * gain;
            int py1 = area_y + (1.0 - f1) * area_h - voff_px;
            int py2 = area_y + (1.0 - f2) * area_h - voff_px;
            SDL_RenderDrawLine(ren, px1, py1, px2, py2);
        }

        if (state->multi_layout)
            SDL_RenderSetClipRect(ren, &clip);   // restore full-panel clip

        vi++;
    }

    // Legend (overlay only; in stack each band is labelled in place).
    if (state->n_spectra > 1 && !state->multi_layout) {
        int lx = l->exp_x + 10, ly = l->exp_y + 8;
        for (int s = 0; s < state->n_spectra; s++) {
            Spectrum *sp = &state->spectra[s];
            SDL_Rect sw = {lx, ly + 3, 12, 12};
            fill_rounded_rect(ren, sw, 3, sp->color);
            char nm[20];
            snprintf(nm, sizeof(nm), "%.18s", sp->name);
            ui_text(ren, UI_FONT_SANS_SM, nm, lx + 18, ly + 2,
                    (s == state->active_spec) ? UI_TEXT : UI_DIM);
            ly += 18;
        }
    }

    // Draw already-assigned experimental frequencies loaded from config/LIN.
    if (state->n_lin_data > 0) {
        SDL_SetRenderDrawColor(ren, 115, 225, 145, 130);
        int marker_top = l->exp_y + 4;
        int marker_bottom = l->exp_y + l->exp_h - 4;

        for (int i = 0; i < state->n_lin_data; i++) {
            double f = state->lin_data[i] + state->exp_offset;   // displayed shifted
            if (f < state->vxmin || f > state->vxmax) continue;

            int px = l->exp_x + (f - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_RenderDrawLine(ren, px, marker_top, px, marker_bottom);
            SDL_Rect cap = {px - 2, marker_top, 5, 5};
            SDL_RenderFillRect(ren, &cap);
        }
    }
    
    // Draw Found Peaks
    for (int ip = 0; ip < state->n_peaks; ip++) {
        double pkx = state->peaks[ip].x;            // true frequency
        double pkd = pkx + state->exp_offset;       // displayed position
        if (pkd < state->vxmin || pkd > state->vxmax) continue;

        int px = l->exp_x + (pkd - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
        SDL_SetRenderDrawColor(ren, 245, 210, 75, 220);
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);

        char label[64]; fmt_mhz(label, sizeof(label), pkx, 3);   // true frequency
        SDL_Rect tag = {px + 4, l->exp_y + 8, ui_text_w(UI_FONT_MONO_SM, label) + 14, 19};
        if (tag.x + tag.w < l->exp_x + l->exp_w) {
            fill_rounded_rect(ren, tag, 3, (SDL_Color){28, 24, 12, 225});
            ui_text_v(ren, UI_FONT_MONO_SM, label, tag.x + 7, tag, UI_WARN);
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

    // Stack mode: per-subplot frame + Y axis labels (drawn unclipped so the
    // value labels fit in the left margin).
    if (state->multi_layout && nvis > 0) {
        const int STACK_GAP = 8;
        int vj = 0;
        for (int s = 0; s < state->n_spectra; s++) {
            Spectrum *sp = &state->spectra[s];
            if (!sp->visible || sp->n_pts < 2) continue;
            int ay = l->exp_y + vj * band_h;
            int ah = band_h - STACK_GAP; if (ah < 10) ah = 10;
            // Range shown on this subplot's axis: norm = its own (gain-scaled),
            // shared = the common vymin/vymax (same for every subplot).
            double bmin, btop;
            if (state->multi_ynorm) {
                double gain = (sp->vscale > 0.0) ? sp->vscale : 1.0;
                bmin = sp->ymin;
                btop = sp->ymin + (sp->ymax - sp->ymin) / gain;
            } else {
                bmin = state->vymin;
                btop = state->vymax;
            }

            SDL_Rect box = {l->exp_x, ay, l->exp_w, ah};
            SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, 110);
            SDL_RenderDrawRect(ren, &box);

            // Exactly 3 Y ticks (max / mid / min), drawn INSIDE the band so the
            // labels of adjacent subplots never overlap.
            double mid_val = 0.5 * (btop + bmin);
            double tick_val[3] = { btop, mid_val, bmin };
            int    tick_y[3]   = { ay + 7, ay + ah/2, ay + ah - 7 };
            for (int t = 0; t < 3; t++) {
                SDL_RenderDrawLine(ren, l->exp_x - 4, tick_y[t], l->exp_x, tick_y[t]);
                char vb[24];
                snprintf(vb, sizeof(vb), "%.3g", tick_val[t]);
                ui_text_right(ren, UI_FONT_MONO_SM, vb, l->exp_x - 10,
                              tick_y[t] - ui_text_h(UI_FONT_MONO_SM) / 2, UI_FAINT);
            }
            ui_text(ren, UI_FONT_SANS_SM, sp->name, l->exp_x + 8, ay + 3,
                    (s == state->active_spec) ? sp->color : UI_FAINT);
            vj++;
        }
    }

    // Axis lines: left and bottom only, the way a plot is normally framed.
    ui_vline(ren, l->exp_x, l->exp_y, l->exp_y + l->exp_h, UI_AXIS);
    ui_hline(ren, l->exp_x, l->exp_x + l->exp_w, l->exp_y + l->exp_h, UI_AXIS);
    
    // Draw X-Axis Ticks & Labels
    double xrange = state->vxmax - state->vxmin;
    double xstep = tick_step_for_pixels(xrange, l->exp_w, 90);
    double xstart = ceil(state->vxmin/xstep)*xstep;

    if (xstep > 0 && xrange > 0)
    for(double x=xstart; x<=state->vxmax; x+=xstep) {
        int px = l->exp_x + (x - state->vxmin)/xrange * l->exp_w;
        SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 60);
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);
        SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
        SDL_RenderDrawLine(ren, px, l->exp_y + l->exp_h, px, l->exp_y + l->exp_h + 4);

        // With no prediction pane below, this is the only frequency axis.
        if (state->n_pred == 0) {
            char buf[40]; fmt_mhz(buf, sizeof(buf), x, 3);
            ui_text(ren, UI_FONT_MONO_SM, buf,
                    px - ui_text_w(UI_FONT_MONO_SM, buf) / 2, l->exp_y + l->exp_h + 7, UI_FAINT);
        }
    }
    
    // --- Y-Axis Ticks & Labels (NEW) ---
    // In stack mode each subplot draws its own Y axis above, so skip the global one.
    double yrange = state->vymax - state->vymin;
    if(yrange > 0 && !state->multi_layout) {
        double ystep = tick_step_for_pixels(yrange, l->exp_h, 34);
        double ystart = ceil(state->vymin/ystep)*ystep;

        for(double y=ystart; y<=state->vymax; y+=ystep) {
            int py = l->exp_y + (1.0 - (y - state->vymin)/yrange) * l->exp_h;
            
            // Draw Tick on left axis
            SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
            SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x - 5, py);
            SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 45);
            SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x + l->exp_w, py);
            
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1e", y);
            if (py > l->exp_y + 6 && py < l->exp_y + l->exp_h - 6) {
                ui_text_right(ren, UI_FONT_MONO_SM, buf, l->exp_x - 10,
                              py - ui_text_h(UI_FONT_MONO_SM) / 2, UI_FAINT);
            }
        }
    }
}

// --- PREDICTION (PICKETT) ---
static void draw_prediction_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    SDL_Rect pred_rect = {l->pred_x, l->pred_y, l->pred_w, l->pred_h};
    SDL_SetRenderDrawColor(ren, COL_PANEL.r, COL_PANEL.g, COL_PANEL.b, COL_PANEL.a);
    SDL_RenderFillRect(ren, &pred_rect);

    ui_vline(ren, l->pred_x, l->pred_y, l->pred_y + l->pred_h, UI_AXIS);
    ui_hline(ren, l->pred_x, l->pred_x + l->pred_w, l->pred_y + l->pred_h, UI_AXIS);

    if (state->n_pred > 0) {
        SDL_RenderSetClipRect(ren, &pred_rect);
        
        // Range filtering in raw prediction coordinates. exp_offset is displayed
        // as a visual shift applied to predictions.
        int p_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin);
        int p_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax);
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
            if (!pred_passes_filter(state, i)) continue;

            double h_ratio = (state->pred_lines[i].linear_int / state->pred_global_max) * state->pred_scale;
            if(h_ratio > 1.0) h_ratio = 1.0;

            int sx = l->pred_x + (state->pred_lines[i].freq_mhz - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
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
                    SDL_Rect cluster_mark = {group_sx - 2, l->pred_y + 4, 5, 2};
                    fill_rounded_rect(ren, cluster_mark, 1, (SDL_Color){147, 149, 156, 120});
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
                    SDL_Rect cluster_mark = {group_sx - 2, l->pred_y + 4, 5, 2};
                    fill_rounded_rect(ren, cluster_mark, 1, (SDL_Color){147, 149, 156, 120});
            }
        }

        // Broadening Simulation: mode 0 = analytic (L/G/V), mode 1 = Kaiser-FFT
        int   kmode = (state->broaden_mode == 1);
        double bw   = fmax(state->lorentz_gamma, state->gauss_gamma);
        double dnu  = 0.0, cutoff = 0.0;
        int draw_broad = 0;
        if (state->broadening_active) {
            if (kmode) {
                double df = spectrum_df(state);
                int cer = state->kaiser_ceros > 0 ? state->kaiser_ceros : 1;
                dnu = cer * df;                       // fundamental resolution (MHz)
                if (dnu > 0.0) {
                    double gg = state->kaiser_intrinsic / dnu; // intrinsic FWHM in res units
                    kaiser_build_kernel(state->kaiser_beta, gg);
                    cutoff = KAISER_RMAX * dnu;
                    draw_broad = 1;
                }
            } else if (bw > 0.0) {
                cutoff = 50.0 * bw;
                draw_broad = 1;
            }
        }
        if(draw_broad) {
            SDL_SetRenderDrawColor(ren, 0, 255, 255, 150); // Cyan transparent
            int steps = l->pred_w;
            double prev_y = -1;

            // Optimization: Only calc lines near visible window + cutoff
            int p_calc_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin - cutoff);
            int p_calc_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax + cutoff);
            if(p_calc_start < 0) p_calc_start=0;
            if(p_calc_end >= state->n_pred) p_calc_end=state->n_pred-1;

            for(int i=0; i<steps; i++) {
                double f = state->pvxmin + (double)i/l->pred_w * (state->pvxmax - state->pvxmin);
                double raw_f = f;   // predictions are drawn at their true frequency now
                double intensity_sum = 0;  // analytic: incoherent; kaiser: coherent (signed)

                for(int k=p_calc_start; k<=p_calc_end; k++) {
                    double dist = raw_f - state->pred_lines[k].freq_mhz;
                    if(fabs(dist) > cutoff) continue;
                    if (!pred_passes_filter(state, k)) continue;
                    if (kmode)
                        intensity_sum += state->pred_lines[k].linear_int * kaiser_kernel(dist / dnu);
                    else
                        intensity_sum += state->pred_lines[k].linear_int
                                         * broaden_profile(dist, state->lorentz_gamma, state->gauss_gamma);
                }
                if (kmode) intensity_sum = fabs(intensity_sum); // |FFT| of in-phase FID

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
    if (xstep > 0 && xrange > 0)
    for(double x=xstart; x<=state->pvxmax; x+=xstep) {
        int px = l->pred_x + (x - state->pvxmin)/xrange * l->pred_w;
        SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, COL_GRID.a);
        SDL_RenderDrawLine(ren, px, l->pred_y, px, l->pred_y + l->pred_h);
        SDL_SetRenderDrawColor(ren, COL_AXIS.r, COL_AXIS.g, COL_AXIS.b, COL_AXIS.a);
        SDL_RenderDrawLine(ren, px, l->pred_y + l->pred_h, px, l->pred_y + l->pred_h + 4);
        char buf[40]; fmt_mhz(buf, sizeof(buf), x, 3);
        ui_text(ren, UI_FONT_MONO_SM, buf,
                px - ui_text_w(UI_FONT_MONO_SM, buf) / 2, l->pred_y + l->pred_h + 7, UI_FAINT);
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

// ============================================================================
//  WINDOW CHROME
//  Two bands at the top (document + commands), an icon rail on the left, one
//  status line at the bottom. Everything else on screen is the data.
// ============================================================================
static void draw_top_chrome(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    int mx, my;
    int mdown = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    char buf[512];

    /* --- document bar: which files this window is showing --- */
    SDL_Rect doc = {0, 0, l->win_w, UI_TITLEBAR_H};
    ui_fill(ren, doc, UI_TITLEBAR);
    ui_hline(ren, 0, l->win_w, UI_TITLEBAR_H - 1, (SDL_Color){10, 11, 12, 255});

    if (state->exp_path[0] || state->pred_path[0]) {
        const char *sp = state->exp_path[0] ? short_path(state->exp_path) : NULL;
        const char *pr = state->pred_path[0] ? short_path(state->pred_path) : NULL;
        if (sp && pr)      snprintf(buf, sizeof(buf), "%s   \xC2\xB7   %s", sp, pr);
        else if (sp)       snprintf(buf, sizeof(buf), "%s", sp);
        else               snprintf(buf, sizeof(buf), "%s", pr);
    } else {
        snprintf(buf, sizeof(buf), "No file loaded");
    }
    int tw = ui_text_w(UI_FONT_SANS, buf);
    ui_text_v(ren, UI_FONT_SANS, buf, (l->win_w - tw) / 2, doc, UI_DIM);

    /* --- command bar --- */
    SDL_Rect bar = {0, UI_TITLEBAR_H, l->win_w, UI_TOPBAR_H};
    ui_fill(ren, bar, UI_CHROME);
    ui_hline(ren, 0, l->win_w, UI_CONTENT_Y - 1, UI_LINE);

    ui_button(ren, ui_top_rect(UI_TOP_BAR, l->win_w), "Bar", UI_ICON_BAR,
              UI_BTN_QUIET, state->bar_active, mx, my, mdown);
    ui_button(ren, ui_top_rect(UI_TOP_MEASURE, l->win_w), "Measure", UI_ICON_MEASURE,
              UI_BTN_QUIET, state->measure_active, mx, my, mdown);
    ui_button(ren, ui_top_rect(UI_TOP_SYNC, l->win_w), "Sync", UI_ICON_SYNC,
              UI_BTN_QUIET, state->sync_active, mx, my, mdown);

    ui_vline(ren, UI_TOP_SEP_X, UI_TOP_BTN_Y + 3, UI_TOP_BTN_Y + UI_TOP_BTN_H - 3, UI_LINE);

    ui_button(ren, ui_top_rect(UI_TOP_DELPEAK, l->win_w), "Delete peak", UI_ICON_TRASH,
              UI_BTN_DANGER, 0, mx, my, mdown);

    if (ui_top_right_visible(l->win_w)) {
        SDL_Rect off = ui_top_rect(UI_TOP_OFFSET, l->win_w);
        char val[40];
        if (state->input_state == INPUT_OFFSET) snprintf(val, sizeof(val), "%s_", state->text_input_buf);
        else                                    snprintf(val, sizeof(val), "%.4f", state->exp_offset);
        ui_field(ren, off, "Offset", val, state->input_state == INPUT_OFFSET);

        ui_button(ren, ui_top_rect(UI_TOP_EXPORT, l->win_w), "Export view", UI_ICON_EXPORT,
                  UI_BTN_QUIET, 0, mx, my, mdown);
        ui_button(ren, ui_top_rect(UI_TOP_HELP, l->win_w), "Shortcuts", UI_ICON_HELP,
                  UI_BTN_QUIET, state->show_help, mx, my, mdown);
    }

    /* the error / hint line sits at the left of the document bar */
    if (state->error_message[0]) {
        ui_text_v(ren, UI_FONT_SANS_SM, state->error_message, 12, doc, UI_DANGER_TEXT);
    } else if (!state->data_loaded && state->status_message[0]) {
        ui_text_v(ren, UI_FONT_SANS_SM, state->status_message, 12, doc, UI_FAINT);
    }
}

// Left rail: one icon per tool panel, in the order the panels stack.
static void draw_rail(SDL_Renderer *ren, AppState *state, Layout *l) {
    int mx, my;
    int mdown = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

    SDL_Rect rail = {0, UI_CONTENT_Y, UI_RAIL_W, l->win_h - UI_CONTENT_Y - UI_STATUS_H};
    ui_fill(ren, rail, UI_CHROME);
    ui_vline(ren, UI_RAIL_W - 1, rail.y, rail.y + rail.h, UI_LINE);

    const DraggableWindow *panels[UI_TOOL_COUNT] = {
        &state->win_as, &state->win_pf, &state->win_avg, &state->win_br,
        &state->win_cut, &state->win_filt, &state->win_jump, &state->win_spec
    };

    int tip_tool = -1;
    SDL_Rect tip_rect = {0, 0, 0, 0};

    for (int t = 0; t < UI_TOOL_COUNT; t++) {
        SDL_Rect r = ui_rail_rect(t);
        int on = panels[t]->visible;
        int hover = ui_button(ren, r, "", ui_tool_icon(t), UI_BTN_QUIET, on, mx, my, mdown);
        if (on) {
            SDL_Rect mark = {0, r.y + 6, 2, r.h - 12};
            ui_fill(ren, mark, UI_ACCENT);
        }
        if (ui_rail_separator_after(t))
            ui_hline(ren, r.x + 6, r.x + r.w - 6, r.y + r.h + UI_RAIL_SEP / 2, UI_LINE);
        if (hover) { tip_tool = t; tip_rect = r; }
    }

    /* tooltip last, so it sits above the neighbouring buttons */
    if (tip_tool >= 0) {
        const char *title = ui_tool_title(tip_tool);
        const char *key   = ui_tool_key(tip_tool);
        int tw = ui_text_w(UI_FONT_SANS, title);
        int kw = (key && *key) ? ui_text_w(UI_FONT_MONO_SM, key) + 10 : 0;
        SDL_Rect tip = {UI_RAIL_W + 6, tip_rect.y + (tip_rect.h - 22) / 2, tw + kw + 20, 22};
        fill_rounded_rect(ren, tip, 4, (SDL_Color){8, 9, 10, 245});
        ui_frame(ren, tip, UI_LINE);
        ui_text_v(ren, UI_FONT_SANS, title, tip.x + 10, tip, UI_TEXT);
        if (kw) ui_text_v(ren, UI_FONT_MONO_SM, key, tip.x + 10 + tw + 10, tip, UI_FAINT);
    }
}

// A small state chip, e.g. "filter off". Returns its width.
static int draw_chip(SDL_Renderer *ren, int right_x, int cy, const char *text, SDL_Color c, int dotted) {
    int tw = ui_text_w(UI_FONT_SANS_SM, text);
    int w = tw + (dotted ? 24 : 16);
    SDL_Rect r = {right_x - w, cy - 9, w, 18};
    fill_rounded_rect(ren, r, 3, UI_RAISED);
    if (dotted) {
        SDL_Rect dot = {r.x + 8, cy - 3, 6, 6};
        fill_rounded_rect(ren, dot, 3, c);
        ui_text_v(ren, UI_FONT_SANS_SM, text, r.x + 18, r, c);
    } else {
        ui_text_v(ren, UI_FONT_SANS_SM, text, r.x + 8, r, c);
    }
    return w;
}

static void draw_panel_headers(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    char buf[128];
    int pane_x = UI_RAIL_W;
    int pane_w = l->plot_right - pane_x;
    if (pane_w < 80) return;

    if (state->n_spectra > 0) {
        SDL_Rect head = {pane_x, l->exp_y - UI_PANEL_HEADER_H, pane_w, UI_PANEL_HEADER_H};
        ui_fill(ren, head, UI_PANEL);
        ui_hline(ren, head.x, head.x + head.w, head.y + head.h - 1, UI_LINE_SOFT);
        ui_text_v(ren, UI_FONT_TITLE, "Experimental", head.x + 12, head, UI_TEXT);

        int nvis = 0;
        for (int i = 0; i < state->n_spectra; i++) if (state->spectra[i].visible) nvis++;
        char pts[32]; fmt_count(pts, sizeof(pts), (long)state->n_pts);
        snprintf(buf, sizeof(buf), "%d trace%s   %s pts", nvis, nvis == 1 ? "" : "s", pts);
        ui_text_v(ren, UI_FONT_MONO_SM, buf,
                  head.x + 12 + ui_text_w(UI_FONT_TITLE, "Experimental") + 16, head, UI_FAINT);

        int rx = head.x + head.w - 12, cy = head.y + head.h / 2;
        if (state->n_spectra > 1) {
            snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s",
                     state->multi_layout ? "stack" : "overlay",
                     state->multi_ynorm ? "normalised Y" : "shared Y");
            rx -= draw_chip(ren, rx, cy, buf, UI_DIM, 0) + 6;
        }
        if (state->rolling_avg_active) {
            snprintf(buf, sizeof(buf), "rolling avg %d pt", state->rolling_avg_window);
            rx -= draw_chip(ren, rx, cy, buf, UI_OK, 1) + 6;
        }
    }

    if (state->n_pred > 0) {
        SDL_Rect head = {pane_x, l->pred_y - UI_PANEL_HEADER_H, pane_w, UI_PANEL_HEADER_H};
        ui_fill(ren, head, UI_PANEL);
        ui_hline(ren, head.x, head.x + head.w, head.y + head.h - 1, UI_LINE_SOFT);
        ui_text_v(ren, UI_FONT_TITLE, "Prediction", head.x + 12, head, UI_TEXT);

        int shown = 0;
        for (int i = 0; i < state->n_pred; i++) {
            double f = state->pred_lines[i].freq_mhz;
            if (f < state->pvxmin || f > state->pvxmax) continue;
            if (pred_passes_filter(state, i)) shown++;
        }
        char cnt[32]; fmt_count(cnt, sizeof(cnt), (long)shown);
        snprintf(buf, sizeof(buf), "%s lines in view", cnt);
        ui_text_v(ren, UI_FONT_MONO_SM, buf,
                  head.x + 12 + ui_text_w(UI_FONT_TITLE, "Prediction") + 16, head, UI_FAINT);

        int rx = head.x + head.w - 12, cy = head.y + head.h / 2;
        if (state->broadening_active) {
            if (state->broaden_mode == 1) {
                double df = spectrum_df(state);
                int cer = state->kaiser_ceros > 0 ? state->kaiser_ceros : 1;
                snprintf(buf, sizeof(buf), "Kaiser FFT \xC2\xB7 res %.4g MHz", cer * df);
            } else {
                snprintf(buf, sizeof(buf), "%s profile",
                         (state->lorentz_gamma > 0 && state->gauss_gamma > 0) ? "Voigt" :
                         (state->lorentz_gamma > 0) ? "Lorentz" : "Gauss");
            }
            rx -= draw_chip(ren, rx, cy, buf, UI_DIM, 0) + 6;
        }
        rx -= draw_chip(ren, rx, cy, state->filter_active ? "filter on" : "filter off",
                        state->filter_active ? UI_WARN : UI_FAINT, 1) + 6;
        (void)rx;
    }
}

// One status line: what the pointer reads, what is marked, what is loaded.
static void draw_status_bar(SDL_Renderer *ren, AppState *state, Layout *l) {
    char buf[96];
    SDL_Rect bar = {0, l->win_h - UI_STATUS_H, l->win_w, UI_STATUS_H};
    ui_fill(ren, bar, UI_CHROME);
    ui_hline(ren, 0, l->win_w, bar.y, UI_LINE);

    int mx, my; SDL_GetMouseState(&mx, &my);
    int in_exp = state->data_loaded &&
                 point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h});
    int ty_mono = bar.y + (bar.h - ui_text_h(UI_FONT_MONO)) / 2;
    int ty_sans = bar.y + (bar.h - ui_text_h(UI_FONT_SANS_SM)) / 2;

    int x = 12;
    ui_text(ren, UI_FONT_SANS_SM, "f", x, ty_sans, UI_FAINT); x += 14;
    if (in_exp) {
        double fx = (mx - l->exp_x) / (double)l->exp_w;
        fmt_mhz(buf, sizeof(buf), state->vxmin + fx * (state->vxmax - state->vxmin) - state->exp_offset, 4);
    } else snprintf(buf, sizeof(buf), "\xE2\x80\x94");
    ui_text(ren, UI_FONT_MONO, buf, x, ty_mono, in_exp ? UI_ACCENT : UI_FAINT);
    x += 104;
    ui_text(ren, UI_FONT_SANS_SM, "MHz", x, ty_sans, UI_FAINT); x += 42;

    ui_text(ren, UI_FONT_SANS_SM, "I", x, ty_sans, UI_FAINT); x += 14;
    if (in_exp) {
        double fy = 1.0 - (my - l->exp_y) / (double)l->exp_h;
        snprintf(buf, sizeof(buf), "%.2e", state->vymin + fy * (state->vymax - state->vymin));
    } else snprintf(buf, sizeof(buf), "\xE2\x80\x94");
    ui_text(ren, UI_FONT_MONO, buf, x, ty_mono, in_exp ? UI_TEXT : UI_FAINT);
    x += 92;

    ui_text(ren, UI_FONT_SANS_SM, "bar", x, ty_sans, UI_FAINT); x += 30;
    if (state->bar_active && state->data_loaded) fmt_mhz(buf, sizeof(buf), state->bar_x, 4);
    else snprintf(buf, sizeof(buf), "off");
    ui_text(ren, UI_FONT_MONO, buf, x, ty_mono, state->bar_active ? UI_WARN : UI_FAINT);
    x += 104;

    if (state->data_loaded && l->win_w > 760) {
        ui_text(ren, UI_FONT_SANS_SM, "span", x, ty_sans, UI_FAINT); x += 38;
        snprintf(buf, sizeof(buf), "%.2f MHz", state->vxmax - state->vxmin);
        ui_text(ren, UI_FONT_MONO, buf, x, ty_mono, UI_TEXT);
    }

    int rx = l->win_w - 12;
    snprintf(buf, sizeof(buf), "%s", state->sync_active ? "on" : "off");
    ui_text_right(ren, UI_FONT_MONO, buf, rx, ty_mono, state->sync_active ? UI_TEXT : UI_FAINT);
    rx -= ui_text_w(UI_FONT_MONO, buf) + 6;
    ui_text_right(ren, UI_FONT_SANS_SM, "sync", rx, ty_sans, UI_FAINT);
    rx -= ui_text_w(UI_FONT_SANS_SM, "sync") + 22;

    if (l->win_w > 640) {
        snprintf(buf, sizeof(buf), "%d", state->n_assignments);
        ui_text_right(ren, UI_FONT_MONO, buf, rx, ty_mono, UI_TEXT);
        rx -= ui_text_w(UI_FONT_MONO, buf) + 6;
        ui_text_right(ren, UI_FONT_SANS_SM, "assigned", rx, ty_sans, UI_FAINT);
        rx -= ui_text_w(UI_FONT_SANS_SM, "assigned") + 22;

        snprintf(buf, sizeof(buf), "%d", state->n_peaks);
        ui_text_right(ren, UI_FONT_MONO, buf, rx, ty_mono, state->n_peaks ? UI_WARN : UI_FAINT);
        rx -= ui_text_w(UI_FONT_MONO, buf) + 6;
        ui_text_right(ren, UI_FONT_SANS_SM, "peaks", rx, ty_sans, UI_FAINT);
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

// Value of an input field: the live edit buffer with a caret while focused,
// otherwise the stored value.
static void field_val(AppState *st, int which, char *out, size_t n, const char *fmt, double v) {
    if ((int)st->input_state == which) snprintf(out, n, "%s_", st->text_input_buf);
    else                          snprintf(out, n, fmt, v);
}
static void field_val_i(AppState *st, int which, char *out, size_t n, int v) {
    if ((int)st->input_state == which) snprintf(out, n, "%s_", st->text_input_buf);
    else                          snprintf(out, n, "%d", v);
}

// A labelled numeric row inside a panel: caption on the left, field on the right.
static void panel_row(SDL_Renderer *ren, AppState *st, const char *label,
                      SDL_Rect field, int which, const char *value) {
    SDL_Rect lab = {field.x - 200, field.y, 190, field.h};
    ui_text_v(ren, UI_FONT_SANS, label, field.x - 135, lab, UI_DIM);
    ui_field(ren, field, NULL, value, (int)st->input_state == which);
}

// --- INSPECTOR PANELS ---
// Every rectangle below is also written in controller.c's hit-testing; the two
// must stay identical.
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font; (void)l;
    int mx, my;
    int m_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    char buf[128];

    // 1. BROADENING
    if (state->win_br.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_br.clip);
        draw_inspector_section(ren, &state->win_br, UI_ICON_BELL, mx, my);
        int wx = state->win_br.rect.x, wy = state->win_br.rect.y;

        static const char *modes[2] = {"Analytic", "Kaiser FFT"};
        ui_segmented(ren, (SDL_Rect){wx + 15, wy + 40, 260, 26}, modes, 2, state->broaden_mode == 1);

        if (state->broaden_mode == 0) {
            field_val(state, INPUT_GAMMA, buf, sizeof(buf), "%.2f", state->lorentz_gamma);
            panel_row(ren, state, "Lorentz HWHM", (SDL_Rect){wx + 155, wy + 82, 120, 28}, INPUT_GAMMA, buf);
            field_val(state, INPUT_GAUSS, buf, sizeof(buf), "%.2f", state->gauss_gamma);
            panel_row(ren, state, "Gauss HWHM", (SDL_Rect){wx + 155, wy + 122, 120, 28}, INPUT_GAUSS, buf);

            const char *shape;
            if (state->lorentz_gamma > 0.0 && state->gauss_gamma > 0.0) shape = "Profile: Voigt";
            else if (state->lorentz_gamma > 0.0)                        shape = "Profile: Lorentzian";
            else if (state->gauss_gamma  > 0.0)                         shape = "Profile: Gaussian";
            else                                                        shape = "Set a width to see a profile";
            ui_text(ren, UI_FONT_SANS_SM, shape, wx + 20, wy + 168, UI_FAINT);
        } else {
            field_val(state, INPUT_KBETA, buf, sizeof(buf), "%.2f", state->kaiser_beta);
            panel_row(ren, state, "Kaiser beta", (SDL_Rect){wx + 155, wy + 82, 120, 28}, INPUT_KBETA, buf);
            field_val_i(state, INPUT_KCEROS, buf, sizeof(buf), state->kaiser_ceros);
            panel_row(ren, state, "Zero-pad", (SDL_Rect){wx + 155, wy + 122, 120, 28}, INPUT_KCEROS, buf);
            field_val(state, INPUT_KINTR, buf, sizeof(buf), "%.3f", state->kaiser_intrinsic);
            panel_row(ren, state, "Intrinsic FWHM", (SDL_Rect){wx + 155, wy + 162, 120, 28}, INPUT_KINTR, buf);

            double df = spectrum_df(state);
            int cer = state->kaiser_ceros > 0 ? state->kaiser_ceros : 1;
            if (df > 0.0) {
                snprintf(buf, sizeof(buf), "bin df   %.5g MHz", df);
                ui_text(ren, UI_FONT_MONO_SM, buf, wx + 20, wy + 202, UI_FAINT);
                snprintf(buf, sizeof(buf), "res = ceros x df   %.4g MHz", cer * df);
                ui_text(ren, UI_FONT_MONO_SM, buf, wx + 20, wy + 220, UI_ACCENT_TEXT);
            } else {
                ui_text(ren, UI_FONT_SANS_SM, "Load a spectrum for the bin step", wx + 20, wy + 205, UI_FAINT);
            }
        }
        ui_toggle_row(ren, (SDL_Rect){wx + 50, wy + 255, 200, 30}, "Simulated profile",
                      state->broadening_active, mx, my);
    }

    // 2. ROLLING AVERAGE
    if (state->win_avg.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_avg.clip);
        draw_inspector_section(ren, &state->win_avg, UI_ICON_WAVE, mx, my);
        int wx = state->win_avg.rect.x, wy = state->win_avg.rect.y;

        field_val_i(state, INPUT_AVG_PTS, buf, sizeof(buf), state->rolling_avg_window);
        panel_row(ren, state, "Window (pts)", (SDL_Rect){wx + 130, wy + 60, 145, 28}, INPUT_AVG_PTS, buf);
        ui_toggle_row(ren, (SDL_Rect){wx + 50, wy + 120, 200, 30}, "Smoothing",
                      state->rolling_avg_active, mx, my);
        ui_text(ren, UI_FONT_SANS_SM, "Active trace only. The raw data is kept.",
                wx + 20, wy + 160, UI_FAINT);
    }

    // 3. PEAK FINDER
    if (state->win_pf.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_pf.clip);
        draw_inspector_section(ren, &state->win_pf, UI_ICON_PEAK, mx, my);
        int wx = state->win_pf.rect.x, wy = state->win_pf.rect.y;

        field_val_i(state, INPUT_PF_SIG, buf, sizeof(buf), state->pf_sig_pts);
        panel_row(ren, state, "Search width", (SDL_Rect){wx + 190, wy + 50, 85, 26}, INPUT_PF_SIG, buf);
        field_val_i(state, INPUT_PF_NOISE, buf, sizeof(buf), state->pf_noise_pts);
        panel_row(ren, state, "Noise window", (SDL_Rect){wx + 190, wy + 90, 85, 26}, INPUT_PF_NOISE, buf);
        field_val(state, INPUT_PF_THRESH, buf, sizeof(buf), "%.1f", state->pf_thresh);
        panel_row(ren, state, "Threshold", (SDL_Rect){wx + 190, wy + 130, 85, 26}, INPUT_PF_THRESH, buf);

        ui_button(ren, (SDL_Rect){wx + 50, wy + 200, 200, 30}, "Find peaks", UI_ICON_PEAK,
                  UI_BTN_PRIMARY, 0, mx, my, m_down);
        ui_button(ren, (SDL_Rect){wx + 50, wy + 240, 200, 30}, "Export list", UI_ICON_EXPORT,
                  UI_BTN_QUIET, 0, mx, my, m_down);
        snprintf(buf, sizeof(buf), "%d peak%s found", state->n_peaks, state->n_peaks == 1 ? "" : "s");
        ui_text(ren, UI_FONT_SANS_SM, buf, wx + 20, wy + 278, UI_FAINT);
    }

    // 4. ASSIGNMENTS
    if (state->win_as.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_as.clip);
        draw_inspector_section(ren, &state->win_as, UI_ICON_LIST, mx, my);
        int wx = state->win_as.rect.x, wy = state->win_as.rect.y;
        int tw = state->win_as.rect.w;

        SDL_Rect head = {wx + 15, wy + 64, tw - 30, 20};
        ui_fill(ren, head, UI_INPUT);
        ui_text_v(ren, UI_FONT_SANS_SM, "J Ka Kc", head.x + 8, head, UI_FAINT);
        ui_text_right(ren, UI_FONT_SANS_SM, "exp freq / MHz", head.x + head.w - 8,
                      head.y + (head.h - ui_text_h(UI_FONT_SANS_SM)) / 2, UI_FAINT);

        int visible_rows = 13;
        int start_idx = state->assignments_scroll;
        if (start_idx > state->n_assignments - visible_rows) start_idx = state->n_assignments - visible_rows;
        if (start_idx < 0) start_idx = 0;
        int end_idx = start_idx + visible_rows;
        if (end_idx > state->n_assignments) end_idx = state->n_assignments;

        for (int k = start_idx; k < end_idx; k++) {
            PredLine p = state->assignments[k].pred;
            SDL_Rect row = {wx + 15, wy + 88 + (k - start_idx) * 20, tw - 30, 18};
            int sel = (k == state->selected_assignment);
            if (sel) fill_rounded_rect(ren, row, 3, UI_ACCENT_SOFT);
            else if (point_in_rect(mx, my, row)) fill_rounded_rect(ren, row, 3, UI_RAISED);

            snprintf(buf, sizeof(buf), "%d %d %d <- %d %d %d",
                     p.Ju, p.Kau, p.Kcu, p.Jl, p.Kal, p.Kcl);
            ui_text_v(ren, UI_FONT_MONO_SM, buf, row.x + 8, row, sel ? UI_ACCENT_TEXT : UI_DIM);
            fmt_mhz(buf, sizeof(buf), state->assignments[k].exp_freq, 4);
            ui_text_right(ren, UI_FONT_MONO_SM, buf, row.x + row.w - 8,
                          row.y + (row.h - ui_text_h(UI_FONT_MONO_SM)) / 2, UI_TEXT);
        }

        if (state->n_assignments == 0) {
            ui_text(ren, UI_FONT_SANS_SM, "Left-drag a peak while a predicted", wx + 20, wy + 96, UI_FAINT);
            ui_text(ren, UI_FONT_SANS_SM, "line is selected to assign it.", wx + 20, wy + 112, UI_FAINT);
        }

        /* hyperfine detail of the selected row, which no longer fits in the table */
        if (state->selected_assignment >= 0 && state->selected_assignment < state->n_assignments) {
            PredLine p = state->assignments[state->selected_assignment].pred;
            snprintf(buf, sizeof(buf), "F  %d %d %d <- %d %d %d",
                     p.M1u, p.M2u, p.M3u, p.M1l, p.M2l, p.M3l);
            ui_text(ren, UI_FONT_MONO_SM, buf, wx + 15, wy + 334, UI_FAINT);
            fmt_mhz(buf, sizeof(buf), p.freq_mhz, 4);
            ui_text(ren, UI_FONT_MONO_SM, buf, wx + 15, wy + 350, UI_FAINT);
            ui_text(ren, UI_FONT_SANS_SM, "predicted", wx + 125, wy + 350, UI_FAINT);
        }
        if (state->n_assignments > visible_rows) {
            snprintf(buf, sizeof(buf), "%d-%d / %d", start_idx + 1, end_idx, state->n_assignments);
            ui_text_right(ren, UI_FONT_MONO_SM, buf, wx + tw - 15, wy + 334, UI_FAINT);
        }

        ui_button(ren, (SDL_Rect){wx + 10, wy + 360, 100, 30}, "Save all", -1,
                  UI_BTN_PRIMARY, 0, mx, my, m_down);
        ui_button(ren, (SDL_Rect){wx + 120, wy + 360, 110, 30}, "Delete", UI_ICON_TRASH,
                  UI_BTN_DANGER, 0, mx, my, m_down);
    }

    // 5. INTENSITY RANGE
    if (state->win_cut.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_cut.clip);
        draw_inspector_section(ren, &state->win_cut, UI_ICON_RANGE, mx, my);
        int wx = state->win_cut.rect.x, wy = state->win_cut.rect.y;

        field_val(state, INPUT_PRED_MIN, buf, sizeof(buf), "%.1f", state->pred_min_log_int);
        panel_row(ren, state, "Min log I", (SDL_Rect){wx + 120, wy + 50, 155, 28}, INPUT_PRED_MIN, buf);
        field_val(state, INPUT_PRED_MAX, buf, sizeof(buf), "%.1f", state->pred_max_log_int);
        panel_row(ren, state, "Max log I", (SDL_Rect){wx + 120, wy + 90, 155, 28}, INPUT_PRED_MAX, buf);
        ui_text(ren, UI_FONT_SANS_SM, "Weak lines are hidden, not unloaded.", wx + 20, wy + 128, UI_FAINT);
    }

    // 6. FREQUENCY JUMP
    if (state->win_jump.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_jump.clip);
        draw_inspector_section(ren, &state->win_jump, UI_ICON_JUMP, mx, my);
        int wx = state->win_jump.rect.x, wy = state->win_jump.rect.y;

        field_val(state, INPUT_JUMP_MIN, buf, sizeof(buf), "%.1f", state->vxmin);
        panel_row(ren, state, "Start", (SDL_Rect){wx + 120, wy + 50, 155, 28}, INPUT_JUMP_MIN, buf);
        field_val(state, INPUT_JUMP_MAX, buf, sizeof(buf), "%.1f", state->vxmax);
        panel_row(ren, state, "End", (SDL_Rect){wx + 120, wy + 90, 155, 28}, INPUT_JUMP_MAX, buf);
    }

    // 7. TRANSITION FILTER
    if (state->win_filt.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_filt.clip);
        draw_inspector_section(ren, &state->win_filt, UI_ICON_FILTER, mx, my);
        int wx = state->win_filt.rect.x, wy = state->win_filt.rect.y;

        ui_toggle_row(ren, (SDL_Rect){wx + 15, wy + 44, 230, 26}, "Filter", state->filter_active, mx, my);

        ui_text(ren, UI_FONT_SANS_SM, "Dipole", wx + 20, wy + 80, UI_FAINT);
        static const char *mu_lbl[3] = {"mu a", "mu b", "mu c"};
        for (int i = 0; i < 3; i++)
            ui_button(ren, (SDL_Rect){wx + 15 + i * 80, wy + 98, 70, 26}, mu_lbl[i], -1,
                      UI_BTN_QUIET, state->filt_mu[i], mx, my, m_down);

        ui_text(ren, UI_FONT_SANS_SM, "Branch", wx + 20, wy + 134, UI_FAINT);
        static const char *br_lbl[3] = {"P", "Q", "R"};
        for (int i = 0; i < 3; i++)
            ui_button(ren, (SDL_Rect){wx + 15 + i * 80, wy + 152, 70, 26}, br_lbl[i], -1,
                      UI_BTN_QUIET, state->filt_br[i], mx, my, m_down);

        ui_toggle_row(ren, (SDL_Rect){wx + 15, wy + 190, 230, 26}, "Quantum number range",
                      state->filt_use_range, mx, my);
        ui_text(ren, UI_FONT_SANS_SM, "min", wx + 126, wy + 220, UI_FAINT);
        ui_text(ren, UI_FONT_SANS_SM, "max", wx + 196, wy + 220, UI_FAINT);
        const char *qn_lbl[3] = {"J", "Ka", "Kc"};
        int qn_lo[3] = {state->filt_j_min, state->filt_ka_min, state->filt_kc_min};
        int qn_hi[3] = {state->filt_j_max, state->filt_ka_max, state->filt_kc_max};
        int qn_in_lo[3] = {INPUT_FILT_JMIN, INPUT_FILT_KAMIN, INPUT_FILT_KCMIN};
        int qn_in_hi[3] = {INPUT_FILT_JMAX, INPUT_FILT_KAMAX, INPUT_FILT_KCMAX};
        for (int i = 0; i < 3; i++) {
            int ry = wy + 236 + i * 30;
            ui_text(ren, UI_FONT_MONO_SM, qn_lbl[i], wx + 25, ry + 5, UI_DIM);
            field_val_i(state, qn_in_lo[i], buf, sizeof(buf), qn_lo[i]);
            ui_field(ren, (SDL_Rect){wx + 110, ry, 55, 24}, NULL, buf, (int)state->input_state == qn_in_lo[i]);
            field_val_i(state, qn_in_hi[i], buf, sizeof(buf), qn_hi[i]);
            ui_field(ren, (SDL_Rect){wx + 180, ry, 55, 24}, NULL, buf, (int)state->input_state == qn_in_hi[i]);
        }

        ui_toggle_row(ren, (SDL_Rect){wx + 15, wy + 332, 230, 26}, "Quantum number jump",
                      state->filt_use_delta, mx, my);
        const char *d_lbl[3] = {"dJ", "dKa", "dKc"};
        int d_val[3] = {state->filt_dj, state->filt_dka, state->filt_dkc};
        int d_in[3]  = {INPUT_FILT_DJ, INPUT_FILT_DKA, INPUT_FILT_DKC};
        int d_lx[3]  = {18, 92, 172};
        int d_fx[3]  = {44, 124, 204};
        for (int i = 0; i < 3; i++) {
            ui_text(ren, UI_FONT_MONO_SM, d_lbl[i], wx + d_lx[i], wy + 369, UI_DIM);
            field_val_i(state, d_in[i], buf, sizeof(buf), d_val[i]);
            ui_field(ren, (SDL_Rect){wx + d_fx[i], wy + 364, 36, 24}, NULL, buf, (int)state->input_state == d_in[i]);
        }
    }

    // 8. SPECTRA
    if (state->win_spec.anim > 0.01f) {
        SDL_RenderSetClipRect(ren, &state->win_spec.clip);
        draw_inspector_section(ren, &state->win_spec, UI_ICON_LAYERS, mx, my);
        int wx = state->win_spec.rect.x, wy = state->win_spec.rect.y;

        ui_button(ren, (SDL_Rect){wx + 15, wy + 44, 130, 26},
                  state->multi_layout ? "Stack" : "Overlay", -1, UI_BTN_QUIET,
                  state->multi_layout, mx, my, m_down);
        ui_button(ren, (SDL_Rect){wx + 155, wy + 44, 130, 26},
                  state->multi_ynorm ? "Normalised Y" : "Shared Y", -1, UI_BTN_QUIET,
                  state->multi_ynorm, mx, my, m_down);
        ui_button(ren, (SDL_Rect){wx + 15, wy + 76, 270, 26},
                  state->multi_indiv_int ? "Intensity: active trace" : "Intensity: all traces",
                  -1, UI_BTN_QUIET, state->multi_indiv_int, mx, my, m_down);

        ui_text(ren, UI_FONT_SANS_SM, "Click a name to make it active", wx + 18, wy + 108, UI_FAINT);

        for (int i = 0; i < state->n_spectra; i++) {
            Spectrum *sp = &state->spectra[i];
            int rowy = wy + 124 + i * 30;
            SDL_Rect row = {wx + 10, rowy - 2, 278, 26};
            if (i == state->active_spec) fill_rounded_rect(ren, row, 3, UI_ACCENT_SOFT);
            else if (point_in_rect(mx, my, row)) fill_rounded_rect(ren, row, 3, UI_RAISED);

            fill_rounded_rect(ren, (SDL_Rect){wx + 14, rowy + 6, 9, 9}, 2, sp->color);
            char nm[20]; snprintf(nm, sizeof(nm), "%.14s", sp->name);
            ui_text(ren, UI_FONT_SANS, nm, wx + 30, rowy + 4, sp->visible ? UI_TEXT : UI_FAINT);

            ui_button(ren, (SDL_Rect){wx + 150, rowy, 24, 22}, "-", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            ui_button(ren, (SDL_Rect){wx + 176, rowy, 24, 22}, "+", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            ui_button(ren, (SDL_Rect){wx + 204, rowy, 34, 22}, "", UI_ICON_EYE, UI_BTN_QUIET,
                      sp->visible, mx, my, m_down);
            ui_button(ren, (SDL_Rect){wx + 244, rowy, 26, 22}, "", UI_ICON_CLOSE, UI_BTN_DANGER,
                      0, mx, my, m_down);
        }

        if (state->n_spectra == 0) {
            ui_text(ren, UI_FONT_SANS_SM, "Drop a spectrum file to add one.", wx + 18, wy + 128, UI_FAINT);
        } else {
            int ty = wy + 128 + state->n_spectra * 30;
            ui_text(ren, UI_FONT_SANS_SM, "- / +  shift a trace vertically", wx + 18, ty, UI_FAINT);
            ui_text(ren, UI_FONT_SANS_SM, "W / Z  intensity", wx + 18, ty + 16, UI_FAINT);
        }
    }

    SDL_RenderSetClipRect(ren, NULL);
}

static void draw_onboarding(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    SDL_Rect card = {l->win_w / 2 - 280, l->win_h / 2 - 150, 560, 268};
    if (card.x < 24) card.x = 24;
    if (card.w > l->win_w - 48) card.w = l->win_w - 48;

    fill_rounded_rect(ren, card, 8, (SDL_Color){16, 17, 19, 255});
    ui_frame(ren, card, UI_LINE);

    int x = card.x + 34, y = card.y + 30;
    ui_text(ren, UI_FONT_TITLE, "Drop a spectrum and a prediction", x, y, UI_TEXT);
    y += 28;
    ui_text(ren, UI_FONT_SANS, "SpectraVisual opens a .txt or .csv trace together with a Pickett", x, y, UI_DIM);
    y += 18;
    ui_text(ren, UI_FONT_SANS, ".cat prediction. Either one on its own works too.", x, y, UI_DIM);
    y += 30;

    SDL_Rect cmd = {x, y, card.w - 68, 30};
    fill_rounded_rect(ren, cmd, 4, UI_INPUT);
    ui_frame(ren, cmd, UI_LINE);
    ui_text_v(ren, UI_FONT_MONO, "spectravisual spectrum.txt pred.cat", cmd.x + 10, cmd, UI_ACCENT_TEXT);
    y += 48;

    const char *hints[3] = {
        "Drop more spectra any time to overlay or stack them",
        "Press H for the shortcut list",
        "Add --verbose to keep terminal logs"
    };
    for (int i = 0; i < 3; i++) {
        SDL_Rect dot = {x + 1, y + 6, 4, 4};
        fill_rounded_rect(ren, dot, 2, UI_FAINT);
        ui_text(ren, UI_FONT_SANS_SM, hints[i], x + 14, y, UI_FAINT);
        y += 20;
    }

    if (state->exp_path[0] || state->pred_path[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "spectrum: %s", state->exp_path[0] ? short_path(state->exp_path) : "waiting");
        ui_text(ren, UI_FONT_MONO_SM, buf, x, card.y + card.h - 40, state->exp_path[0] ? UI_DIM : UI_ACCENT_TEXT);
        snprintf(buf, sizeof(buf), "prediction: %s", state->pred_path[0] ? short_path(state->pred_path) : "waiting");
        ui_text(ren, UI_FONT_MONO_SM, buf, x, card.y + card.h - 24, state->pred_path[0] ? UI_DIM : UI_ACCENT_TEXT);
    }
}

// Keyboard and mouse reference, grouped the way the work is done.
static void draw_help_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font; (void)state;
    SDL_SetRenderDrawColor(ren, 6, 7, 8, 160);
    SDL_RenderFillRect(ren, &(SDL_Rect){0, 0, l->win_w, l->win_h});

    SDL_Rect card = {l->win_w / 2 - 340, 86, 680, 440};
    if (card.x < 24) card.x = 24;
    if (card.w > l->win_w - 48) card.w = l->win_w - 48;
    fill_rounded_rect(ren, card, 8, (SDL_Color){19, 20, 22, 250});
    ui_frame(ren, card, UI_LINE);

    SDL_Rect head = {card.x, card.y, card.w, 38};
    ui_text_v(ren, UI_FONT_TITLE, "Keyboard and mouse", head.x + 20, head, UI_TEXT);
    ui_hline(ren, card.x, card.x + card.w, card.y + 38, UI_LINE);

    struct { const char *group; const char *key; const char *what; } rows[] = {
        {"Navigate",  "Q  E",      "Zoom out / in on frequency"},
        {NULL,        "A  S",      "Pan left / right"},
        {NULL,        "W  Z",      "Scale experimental intensity"},
        {NULL,        "Tab",       "Autoscale intensity to the view"},
        {NULL,        "R",         "Reset the view"},
        {NULL,        "Up Down",   "Shift the spectrum vertically"},
        {"Measure",   "K  L",      "Move the bar"},
        {NULL,        "G",         "Distance between two points"},
        {NULL,        "Delete",    "Remove the latest peak"},
        {NULL,        "X",         "Export the view as BMP"},
        {NULL,        "H  ?",      "This list"},
        {"Tools",     "N",         "Assignments"},
        {NULL,        "P",         "Peak finder"},
        {NULL,        "T",         "Rolling average"},
        {NULL,        "M",         "Broadening"},
        {NULL,        "C",         "Intensity range"},
        {NULL,        "F",         "Frequency jump"},
        {NULL,        "B",         "Transition filter"},
        {"Mouse",     "left drag", "Zoom into a frequency range"},
        {NULL,        "right drag","Pick the peak in the range"},
        {NULL,        "alt drag",  "Slide the spectrum onto the prediction"},
        {NULL,        "click",     "Select a predicted transition"},
        {NULL,        "cmd click", "Add to the selection"},
        {"Fields",    "Enter",     "Confirm the value"},
        {NULL,        "Esc",       "Cancel the edit"},
        {NULL,        "shift+key", "Without Sync, acts on the prediction only"}
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0]));
    int col_w = (card.w - 60) / 2;
    int x = card.x + 24, y = card.y + 58;
    for (int i = 0; i < n; i++) {
        if (i == 11) { x = card.x + 30 + col_w; y = card.y + 58; }   /* second column starts at Tools */
        if (rows[i].group) {
            if (i != 0 && i != 11) y += 10;
            ui_text(ren, UI_FONT_SANS_SM, rows[i].group, x, y, UI_FAINT);
            y += 20;
        }
        SDL_Rect kb = {x, y - 1, ui_text_w(UI_FONT_MONO_SM, rows[i].key) + 12, 17};
        fill_rounded_rect(ren, kb, 3, UI_RAISED);
        ui_text_v(ren, UI_FONT_MONO_SM, rows[i].key, kb.x + 6, kb, UI_TEXT);
        ui_text(ren, UI_FONT_SANS_SM, rows[i].what, x + 96, y, UI_DIM);
        y += 22;
    }
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
        if (last->x + state->exp_offset >= state->vxmin && last->x + state->exp_offset <= state->vxmax) {
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
        double cx = state->vxmin + fx * (state->vxmax - state->vxmin) - state->exp_offset;  // true freq
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
            int px1 = l->exp_x + (state->measure_x1 + state->exp_offset - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 225, 100, 245, 210);
            SDL_RenderDrawLine(ren, px1, l->exp_y, px1, l->exp_y + l->exp_h);
            SDL_RenderDrawLine(ren, px1, my, mx, my);
            if (point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h})) {
                double curr_freq = state->vxmin + ((double)(mx - l->exp_x) / l->exp_w) * (state->vxmax - state->vxmin) - state->exp_offset;
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
        int p_hover_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pbar_x - 1.0);
        int p_hover_end   = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pbar_x + 1.0);
        if(p_hover_start < 0) p_hover_start = 0; 
        if(p_hover_end >= state->n_pred) p_hover_end = state->n_pred - 1;

        double closest_dist = 1e99; int closest_idx = -1;
        int hover_idx[4];
        int hover_n = 0;
        double freq_tolerance = (state->pvxmax - state->pvxmin) * 0.01; 

        for(int i=p_hover_start; i<=p_hover_end; i++) {
            // Check cut + active filters
            if (!pred_passes_filter(state, i)) continue;

            double d = fabs(state->pred_lines[i].freq_mhz - state->pbar_x);
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
