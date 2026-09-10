#include "view.h"
#include "layout.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include "ui_panels.h"
#include "settings.h"
#include "plotgpu.h"
#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

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


/* ---------------------------------------------------------------------------
 *  The broadened profile, evaluated in one place
 *
 *  Both the drawing and Shift+Tab used to sample the profile on their own grid:
 *  the renderer once per screen column, the normaliser on a uniform grid over
 *  the view.  A predicted line is far narrower than either step as soon as the
 *  view is a few MHz wide, so both missed the top of the line by a different
 *  amount and the peak changed height with zoom - and after Shift+Tab it could
 *  grow past the top of the pane when zooming in, because the drawing had found
 *  a taller sample than the normaliser had.
 *
 *  The extremum of a line profile is at the line centre, so any interval is
 *  evaluated at the centres it contains, plus a few samples for the shape in
 *  between.  The answer no longer depends on the zoom.
 * ------------------------------------------------------------------------- */
typedef struct {
    int    active;      /* 0 when no usable profile is configured */
    int    kmode;       /* 1 = Kaiser FFT lineshape               */
    double dnu;         /* Kaiser fundamental resolution, MHz     */
    double cutoff;      /* how far a line still contributes, MHz  */
    double lorentz, gauss;
} BroadCfg;

static BroadCfg broad_config(const AppState *s) {
    BroadCfg c;
    memset(&c, 0, sizeof(c));
    c.kmode = (s->broaden_mode == 1);
    c.lorentz = s->lorentz_gamma;
    c.gauss = s->gauss_gamma;
    if (!s->broadening_active) return c;
    if (c.kmode) {
        double df = spectrum_df(s);
        int cer = s->kaiser_ceros > 0 ? s->kaiser_ceros : 1;
        c.dnu = cer * df;
        if (c.dnu <= 0.0) return c;
        kaiser_build_kernel(s->kaiser_beta, s->kaiser_intrinsic / c.dnu);
        c.cutoff = KAISER_RMAX * c.dnu;
        c.active = 1;
    } else {
        double bw = fmax(c.lorentz, c.gauss);
        if (bw <= 0.0) return c;
        c.cutoff = 50.0 * bw;
        c.active = 1;
    }
    return c;
}

/* Sum of every line that reaches `f`. */
static double broad_value_at(const AppState *s, const BroadCfg *c, double f) {
    int lo = binary_search_pred_lower(s->pred_lines, s->n_pred, f - c->cutoff);
    int hi = binary_search_pred_upper(s->pred_lines, s->n_pred, f + c->cutoff);
    if (lo < 0) lo = 0;
    if (hi >= s->n_pred) hi = s->n_pred - 1;
    double sum = 0.0;
    for (int k = lo; k <= hi; k++) {
        double dist = f - s->pred_lines[k].freq_mhz;
        if (fabs(dist) > c->cutoff || !pred_passes_filter(s, k)) continue;
        if (c->kmode) sum += s->pred_lines[k].linear_int * kaiser_kernel(dist / c->dnu);
        else          sum += s->pred_lines[k].linear_int * broaden_profile(dist, c->lorentz, c->gauss);
    }
    return c->kmode ? fabs(sum) : sum;   /* |FFT| of an in-phase FID */
}

/* How finely an interval has to be sampled for the shape between the centres. */
static int broad_subsamples(const BroadCfg *c, double interval) {
    double feature = c->kmode ? c->dnu : fmax(c->lorentz, c->gauss);
    if (feature <= 0.0) return 1;
    double want = interval / (0.25 * feature);
    int sub = (want > 1.0) ? (int)ceil(want) : 1;
    return sub > 8 ? 8 : sub;
}

/* Highest value the profile reaches in [f0, f1): the line centres inside the
   interval, plus `sub` samples across it for the shape between them. */
static double broad_max_between(const AppState *s, const BroadCfg *c, double f0, double f1, int sub) {
    double best = 0.0;
    if (sub < 1) sub = 1;
    for (int i = 0; i < sub; i++) {
        double v = broad_value_at(s, c, f0 + (i + 0.5) / sub * (f1 - f0));
        if (v > best) best = v;
    }
    int lo = binary_search_pred_lower(s->pred_lines, s->n_pred, f0);
    int hi = binary_search_pred_upper(s->pred_lines, s->n_pred, f1);
    if (lo < 0) lo = 0;
    if (hi >= s->n_pred) hi = s->n_pred - 1;
    for (int k = lo; k <= hi; k++) {
        if (s->pred_lines[k].freq_mhz < f0 || s->pred_lines[k].freq_mhz >= f1) continue;
        if (!pred_passes_filter(s, k)) continue;
        double v = broad_value_at(s, c, s->pred_lines[k].freq_mhz);
        if (v > best) best = v;
    }
    return best;
}

double prediction_visible_max(const AppState *state, int samples) {
    if (!state || state->n_pred <= 0 || state->pvxmax <= state->pvxmin) return 0.0;

    int p_start = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pvxmin);
    int p_end = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pvxmax);
    if (p_start < 0) p_start = 0;
    if (p_end >= state->n_pred) p_end = state->n_pred - 1;
    if (p_start > p_end) return 0.0;

    double stick_max = 0.0;
    for (int k = p_start; k <= p_end; k++)
        if (pred_passes_filter(state, k) && state->pred_lines[k].linear_int > stick_max)
            stick_max = state->pred_lines[k].linear_int;

    BroadCfg cfg = broad_config(state);
    if (!cfg.active) return stick_max;

    /* Walked exactly the way the renderer walks it - the same intervals, the
       same sub-sampling rule - so "normalise to the visible maximum" puts the
       peak on the top of the pane instead of just below or just above it. */
    if (samples < 2) samples = 1024;
    if (samples > 4096) samples = 4096;
    double col = (state->pvxmax - state->pvxmin) / samples;
    int sub = broad_subsamples(&cfg, col);
    double best = 0.0;
    for (int i = 0; i < samples; i++) {
        double f0 = state->pvxmin + i * col;
        double v = broad_max_between(state, &cfg, f0, f0 + col, sub);
        if (v > best) best = v;
    }
    return best;
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

// Frequencies are written plainly, the way they are typed and pasted into a
// line list: 12345.6540, with no group separator.
static void fmt_mhz(char *buf, size_t n, double v, int dec) {
    snprintf(buf, n, "%.*f", dec, v);
}

static void fmt_count(char *buf, size_t n, long v) {
    snprintf(buf, n, "%ld", v);
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
static int  field_focus(AppState *st, int which);
static void field_val(AppState *st, int which, char *out, size_t n, const char *fmt, double v);
static void draw_field(SDL_Renderer *ren, AppState *st, SDL_Rect r, const char *label,
                       const char *value, const char *unit, int which);
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

/* Scratch geometry for the plotted lines, grown once and reused every frame. */
static SDL_FPoint *g_plot_buf = NULL;
static int         g_plot_cap = 0;

static SDL_FPoint *plot_buf(int n) {
    if (n <= g_plot_cap) return g_plot_buf;
    int cap = g_plot_cap ? g_plot_cap : 4096;
    while (cap < n) cap *= 2;
    SDL_FPoint *grown = (SDL_FPoint *)realloc(g_plot_buf, (size_t)cap * sizeof(SDL_FPoint));
    if (!grown) return NULL;
    g_plot_buf = grown;
    g_plot_cap = cap;
    return g_plot_buf;
}

// --- MAIN RENDER ENTRY POINT ---
// Draws one frame without presenting it. Screenshots read the pixels here,
// because reading them after SDL_RenderPresent returns whatever the driver
// leaves in the back buffer - which is how the export ended up saving a stale
// frame.
void render_app_frame(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    plotgpu_begin_frame(ren, ui_scale());
    // 1. Clear Screen
    SDL_SetRenderDrawColor(ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(ren);

    draw_top_chrome(ren, font, state, l);

    // The plot ground covers the whole content area, so the axis gutters are
    // part of the pane instead of showing the window ground behind them.
    ui_fill(ren, (SDL_Rect){UI_RAIL_W, UI_CONTENT_Y,
                            l->plot_right - UI_RAIL_W,
                            l->win_h - UI_CONTENT_Y - UI_STATUS_H}, settings_plot_bg(state));

    // 2. Draw Graphs / Start Screen. Only draw a pane if it has data, so an
    // empty spectrum/prediction window isn't shown when only one was loaded.
    if (state->data_loaded) {
        if (state->n_spectra > 0) draw_spectrum_view(ren, font, state, l);
        if (state->n_pred > 0)    draw_prediction_view(ren, font, state, l);
    } else {
        draw_onboarding(ren, font, state, l);
    }

    /* The queued spectrum geometry is drawn here: after the panes, so it sits on
       their grid, and before the overlays and the panels, which must stay on
       top of it. */
    plotgpu_flush(ren);

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
}

void render_app(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    render_app_frame(ren, font, state, l);
    SDL_RenderPresent(ren);
}

// --- SPECTRUM (EXPERIMENTAL) ---
static void draw_spectrum_view(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    // Clipping
    SDL_Rect clip = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_Color plot_bg = settings_plot_bg(state);
    SDL_SetRenderDrawColor(ren, plot_bg.r, plot_bg.g, plot_bg.b, 255);
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

        /* Geometry is built in float and handed over in one piece.  Where the
           trace carries more samples than the pane has pixels, each column is
           reduced to its own min-max bar: that is faster than drawing tens of
           thousands of invisible segments, and it cannot drop a narrow line
           that falls between two screen columns. */
        /* Per-trace opacity: overlaid spectra stay readable through each other. */
        SDL_Color trace_col = sp->color;
        int op = sp->opacity > 0 ? sp->opacity : 100;
        trace_col.a = (Uint8)(255 * (op / 100.0));

        double sx = l->exp_w / (state->vxmax - state->vxmin);
        int n_samples = end_idx - start_idx + 1;
        float trace_w = (float)state->settings.trace_width;

        /* Reduced on the DEVICE pixel grid, not the logical one: a Retina pane
           is 2000 columns wide, and reducing to its 1000 logical columns threw
           away half the horizontal resolution before drawing anything. */
        double dev = ui_scale();
        int dev_cols = (int)(l->exp_w * dev);
        if (dev_cols < 1) dev_cols = 1;

        if (n_samples > dev_cols) {
            /* One entry and one exit point per column, chained into a single
               polyline: drawing each column as its own bar left gaps wherever
               two neighbours did not overlap in y. */
            int cap = 2 * (dev_cols + 2);
            SDL_FPoint *buf = plot_buf(cap);
            int n = 0, col = INT_MIN;
            double col_min = 0, col_max = 0, prev_min = 0, prev_max = 0;
            for (int i = start_idx; i <= end_idx + 1; i++) {
                int last = (i > end_idx);
                double px = 0, py = 0;
                int c = col;
                if (!last) {
                    px = l->exp_x + (sp->current_pts[i].x + sp->exp_offset - state->vxmin) * sx;
                    double f = (sp->current_pts[i].y - ymn)/(ymx - ymn) * gain;
                    py = area_y + (1.0 - f) * area_h - voff_px;
                    c = (int)(px * dev);
                }
                if ((last || c != col) && col != INT_MIN && buf && n + 2 <= cap) {
                    /* Touch the previous column, so the envelope never opens a
                       gap where two neighbours do not overlap in y. */
                    if (n > 0) {
                        if (col_min > prev_max) col_min = prev_max;
                        else if (col_max < prev_min) col_max = prev_min;
                    }
                    float cx = (float)((col + 0.5) / dev);
                    buf[n++] = (SDL_FPoint){cx, (float)col_min};
                    buf[n++] = (SDL_FPoint){cx, (float)col_max};
                    prev_min = col_min; prev_max = col_max;
                }
                if (last) break;
                if (c != col) { col = c; col_min = col_max = py; }
                else {
                    if (py < col_min) col_min = py;
                    if (py > col_max) col_max = py;
                }
            }
            if (buf) ui_plot_columns(ren, buf, n, trace_w, trace_col);
        } else {
            SDL_FPoint *buf = plot_buf(n_samples);
            int n = 0;
            for (int i = start_idx; i <= end_idx && buf; i++) {
                double px = l->exp_x + (sp->current_pts[i].x + sp->exp_offset - state->vxmin) * sx;
                double f  = (sp->current_pts[i].y - ymn)/(ymx - ymn) * gain;
                buf[n++] = (SDL_FPoint){(float)px, (float)(area_y + (1.0 - f) * area_h - voff_px)};
            }
            if (buf) ui_plot_polyline(ren, buf, n, trace_w, trace_col);
        }

        if (state->multi_layout)
            SDL_RenderSetClipRect(ren, &clip);   // restore full-panel clip

        vi++;
    }

    // Legend (overlay only; in stack each band is labelled in place).
    if (state->n_spectra > 1 && !state->multi_layout && state->settings.show_legend) {
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
        SDL_Color ac = state->settings.assigned_color;
        SDL_SetRenderDrawColor(ren, ac.r, ac.g, ac.b, 130);
        int marker_top = l->exp_y + 4;
        int marker_bottom = l->exp_y + l->exp_h - 4;

        for (int i = 0; i < state->n_lin_data; i++) {
            double f = state->lin_data[i] + state->exp_offset;   // displayed shifted
            if (f < state->vxmin || f > state->vxmax) continue;

            int px = l->exp_x + (f - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
            SDL_RenderDrawLine(ren, px, marker_top, px, marker_bottom);
            SDL_Rect cap = {px - 2, marker_top, 5, 5};
            SDL_SetRenderDrawColor(ren, ac.r, ac.g, ac.b, 255);
            SDL_RenderFillRect(ren, &cap);
            SDL_SetRenderDrawColor(ren, ac.r, ac.g, ac.b, 130);
        }
    }
    
    // Draw Found Peaks
    for (int ip = 0; ip < state->n_peaks; ip++) {
        double pkx = state->peaks[ip].x;            // true frequency
        double pkd = pkx + state->exp_offset;       // displayed position
        if (pkd < state->vxmin || pkd > state->vxmax) continue;

        int px = l->exp_x + (pkd - state->vxmin) / (state->vxmax - state->vxmin) * l->exp_w;
        SDL_Color pc = state->settings.peak_color;
        SDL_SetRenderDrawColor(ren, pc.r, pc.g, pc.b, 220);
        SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);

        if (state->settings.show_peak_labels) {
            char label[64]; fmt_mhz(label, sizeof(label), pkx, 3);   // true frequency
            SDL_Rect tag = {px + 4, l->exp_y + 8, ui_text_w(UI_FONT_MONO_SM, label) + 14, 19};
            if (tag.x + tag.w < l->exp_x + l->exp_w) {
                fill_rounded_rect(ren, tag, 3, (SDL_Color){28, 24, 12, 225});
                ui_text_v(ren, UI_FONT_MONO_SM, label, tag.x + 7, tag, pc);
            }
        }
    }

    // Draw Navigation Bar (Red)
    if(state->bar_active) {
        if(state->bar_x >= state->vxmin && state->bar_x <= state->vxmax) {
            int bx = l->exp_x + (state->bar_x - state->vxmin)/(state->vxmax - state->vxmin) * l->exp_w;
            SDL_Color bc = state->settings.bar_color;
            SDL_SetRenderDrawColor(ren, bc.r, bc.g, bc.b, 230);
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
        if (state->settings.show_grid) {
            SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 60);
            SDL_RenderDrawLine(ren, px, l->exp_y, px, l->exp_y + l->exp_h);
        }
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
            if (state->settings.show_grid) {
                SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, 45);
                SDL_RenderDrawLine(ren, l->exp_x, py, l->exp_x + l->exp_w, py);
            }
            
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
    SDL_Color pred_bg = settings_plot_bg(state);
    SDL_SetRenderDrawColor(ren, pred_bg.r, pred_bg.g, pred_bg.b, 255);
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

        // Broadening simulation: mode 0 = analytic (L/G/V), mode 1 = Kaiser-FFT.
        BroadCfg cfg = broad_config(state);
        if (cfg.active) {
            /* Drawn under the sticks and with its own opacity: the sticks
               carry the branch and dipole colour code, and a solid profile on
               top of them hides exactly the information being assigned. */
            SDL_Color prc = state->settings.profile_color;
            prc.a = (Uint8)(255 * (state->settings.profile_opacity / 100.0));
            int steps = l->pred_w;
            SDL_FPoint *pbuf = plot_buf(steps + 1);
            int pn = 0;

            double span     = state->pvxmax - state->pvxmin;
            double col_span = span / (double)steps;
            int sub = broad_subsamples(&cfg, col_span);

            for (int i = 0; i < steps; i++) {
                double f0 = state->pvxmin + i * col_span;
                double best = broad_max_between(state, &cfg, f0, f0 + col_span, sub);
                double h_ratio = (best / state->pred_global_max) * state->pred_scale;
                double py = l->pred_y + l->pred_h - h_ratio * (l->pred_h - 10);
                if (py < l->pred_y) py = l->pred_y;          /* clip at the top */
                if (pbuf && pn <= steps) pbuf[pn++] = (SDL_FPoint){(float)(l->pred_x + i), (float)py};
            }
            if (pbuf) ui_plot_polyline(ren, pbuf, pn, (float)state->settings.profile_width, prc);
        }

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
                if (group_n + overflow_n > 1 && state->settings.show_blend_marks) {
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
            if (group_n + overflow_n > 1 && state->settings.show_blend_marks) {
                    SDL_Rect cluster_mark = {group_sx - 2, l->pred_y + 4, 5, 2};
                    fill_rounded_rect(ren, cluster_mark, 1, (SDL_Color){147, 149, 156, 120});
            }
        }

        // Navigation Bar (Prediction)
        if(state->bar_active) {
            if(state->pbar_x >= state->pvxmin && state->pbar_x <= state->pvxmax) {
                int bx = l->pred_x + (state->pbar_x - state->pvxmin)/(state->pvxmax - state->pvxmin) * l->pred_w;
                SDL_Color bc2 = state->settings.bar_color;
                SDL_SetRenderDrawColor(ren, bc2.r, bc2.g, bc2.b, 230);
                SDL_RenderDrawLine(ren, bx, l->pred_y, bx, l->pred_y + l->pred_h);
            }
        }

        // Draw assigned-frequency dots in the prediction pane as a compact locator.
        if (state->n_lin_data > 0) {
            SDL_Color ac2 = state->settings.assigned_color;
            SDL_SetRenderDrawColor(ren, ac2.r, ac2.g, ac2.b, 200);
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
        if (state->settings.show_grid) {
            SDL_SetRenderDrawColor(ren, COL_GRID.r, COL_GRID.g, COL_GRID.b, COL_GRID.a);
            SDL_RenderDrawLine(ren, px, l->pred_y, px, l->pred_y + l->pred_h);
        }
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
              UI_BTN_DANGER_QUIET, 0, mx, my, mdown);

    if (ui_top_right_visible(l->win_w)) {
        SDL_Rect cat_temp = ui_top_rect(UI_TOP_CAT_TEMP, l->win_w);
        char cat_temp_val[40];
        if (field_focus(state, INPUT_CAT_TEMP)) snprintf(cat_temp_val, sizeof(cat_temp_val), "%s", state->text_input_buf);
        else if (state->cat_temp_k > 0.0)       snprintf(cat_temp_val, sizeof(cat_temp_val), "%.2f", state->cat_temp_k);
        else                                     snprintf(cat_temp_val, sizeof(cat_temp_val), "set");
        draw_field(ren, state, cat_temp, "T cat", cat_temp_val, "K", INPUT_CAT_TEMP);

        SDL_Rect temp = ui_top_rect(UI_TOP_ROT_TEMP, l->win_w);
        char temp_val[40];
        if (field_focus(state, INPUT_ROT_TEMP)) snprintf(temp_val, sizeof(temp_val), "%s", state->text_input_buf);
        else if (state->rot_temp_k > 0.0)       snprintf(temp_val, sizeof(temp_val), "%.2f", state->rot_temp_k);
        else                                     snprintf(temp_val, sizeof(temp_val), "set");
        draw_field(ren, state, temp, "T rot", temp_val, "K", INPUT_ROT_TEMP);

        SDL_Rect off = ui_top_rect(UI_TOP_OFFSET, l->win_w);
        char val[40];
        field_val(state, INPUT_OFFSET, val, sizeof(val), "%.4f", state->exp_offset);
        draw_field(ren, state, off, "Offset", val, NULL, INPUT_OFFSET);

        ui_button(ren, ui_top_rect(UI_TOP_EXPORT, l->win_w), "Export view", UI_ICON_EXPORT,
                  UI_BTN_QUIET, 0, mx, my, mdown);
        ui_button(ren, ui_top_rect(UI_TOP_HELP, l->win_w), "Shortcuts", UI_ICON_HELP,
                  UI_BTN_QUIET, state->show_help, mx, my, mdown);
        ui_button(ren, ui_top_rect(UI_TOP_SETTINGS, l->win_w), "", UI_ICON_GEAR,
                  UI_BTN_QUIET, state->settings.open, mx, my, mdown);
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
        &state->win_dip, &state->win_cut, &state->win_filt, &state->win_jump, &state->win_spec, &state->win_predfit
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
static int draw_chip(SDL_Renderer *ren, int right_x, int cy, const char *text, SDL_Color c,
                     int dotted, int role) {
    int tw = ui_text_w(role, text);
    int w = tw + (dotted ? 24 : 16);
    SDL_Rect r = {right_x - w, cy - 9, w, 18};
    fill_rounded_rect(ren, r, 3, UI_RAISED);
    if (dotted) {
        SDL_Rect dot = {r.x + 8, cy - 3, 6, 6};
        fill_rounded_rect(ren, dot, 3, c);
        ui_text_v(ren, role, text, r.x + 18, r, c);
    } else {
        ui_text_v(ren, role, text, r.x + 8, r, c);
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
            rx -= draw_chip(ren, rx, cy, buf, UI_DIM, 0, UI_FONT_SANS_SM) + 6;
        }
        if (state->rolling_avg_active) {
            snprintf(buf, sizeof(buf), "rolling avg %d pt", state->rolling_avg_window);
            rx -= draw_chip(ren, rx, cy, buf, UI_OK, 1, UI_FONT_MONO_SM) + 6;
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
            rx -= draw_chip(ren, rx, cy, buf, UI_DIM, 0, UI_FONT_MONO_SM) + 6;
        }
        rx -= draw_chip(ren, rx, cy, state->filter_active ? "filter on" : "filter off",
                        state->filter_active ? UI_WARN : UI_FAINT, 1, UI_FONT_SANS_SM) + 6;
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
        int stick_op = state->settings.stick_opacity > 0 ? state->settings.stick_opacity : 100;
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, (Uint8)(255 * (stick_op / 100.0)));
    }
    ui_thick_line(ren, sx, l->pred_y + l->pred_h, sx, sy1, state->settings.pred_width);
}

static int field_focus(AppState *st, int which) { return (int)st->input_state == which; }

// Value shown in a field: the live edit buffer while it is focused (the caret
// and any selection are drawn by ui_field_ex), otherwise the stored value.
static void field_val(AppState *st, int which, char *out, size_t n, const char *fmt, double v) {
    if (field_focus(st, which)) snprintf(out, n, "%s", st->text_input_buf);
    else                        snprintf(out, n, fmt, v);
}
static void field_val_i(AppState *st, int which, char *out, size_t n, int v) {
    if (field_focus(st, which)) snprintf(out, n, "%s", st->text_input_buf);
    else                        snprintf(out, n, "%d", v);
}

// Draws a field and, while it is focused, records where it was drawn. The
// controller reads that rectangle back to turn a click or a drag into a caret
// position, so the text cursor lands exactly between the drawn characters.
static void draw_field(SDL_Renderer *ren, AppState *st, SDL_Rect r, const char *label,
                       const char *value, const char *unit, int which) {
    int focused = field_focus(st, which);
    if (focused) {
        st->input_rect = r;
        snprintf(st->input_unit, sizeof(st->input_unit), "%s", unit ? unit : "");
    }
    ui_field_ex(ren, r, label, value, unit, focused,
                focused ? st->input_caret : -1, focused ? st->input_anchor : -1);
}

// One row of a panel: caption on the left, value field on the right edge.
static void panel_field(SDL_Renderer *ren, AppState *st, SDL_Rect panel, const char *label,
                        SDL_Rect field, int which, const char *value, const char *unit) {
    ui_text_v(ren, UI_FONT_SANS, label, panel.x + UI_P_PAD, field, UI_DIM);
    draw_field(ren, st, field, NULL, value, unit, which);
}

// A line of explanatory text under a group of controls, and its continuation.
static void panel_hint(SDL_Renderer *ren, SDL_Rect row, const char *text) {
    ui_text(ren, UI_FONT_SANS_SM, text, row.x, row.y + 4, UI_FAINT);
}
static void panel_hint2(SDL_Renderer *ren, SDL_Rect row, const char *text) {
    ui_text(ren, UI_FONT_SANS_SM, text, row.x, row.y + 20, UI_FAINT);
}

// --- INSPECTOR PANELS ---
// Every rectangle comes from ui_panels.h, which controller.c also reads, so a
// control and its click target cannot drift apart.
static void draw_ui_overlays(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font; (void)l;
    int mx, my;
    int m_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    char buf[128];

    // 1. ASSIGNMENTS
    if (state->win_as.visible) {
        SDL_RenderSetClipRect(ren, &state->win_as.clip);
        draw_inspector_section(ren, &state->win_as, UI_ICON_LIST, mx, my);
        SDL_Rect w = state->win_as.rect;

        SDL_Rect head = ui_as_head(w);
        ui_text_v(ren, UI_FONT_SANS_SM, "J Ka Kc", head.x + 6, head, UI_FAINT);
        ui_text_right(ren, UI_FONT_SANS_SM, "exp freq / MHz", head.x + head.w - 6,
                      head.y + (head.h - ui_text_h(UI_FONT_SANS_SM)) / 2, UI_FAINT);
        ui_hline(ren, head.x, head.x + head.w, head.y + head.h - 1, UI_LINE);

        int rows = ui_as_rows(state);
        int start_idx = state->assignments_scroll;
        if (start_idx > state->n_assignments - rows) start_idx = state->n_assignments - rows;
        if (start_idx < 0) start_idx = 0;

        for (int i = 0; i < rows; i++) {
            int k = start_idx + i;
            SDL_Rect row = ui_as_row(w, i);
            if (k >= state->n_assignments) continue;
            PredLine p = state->assignments[k].pred;
            int sel = (k == state->selected_assignment);
            if (sel) fill_rounded_rect(ren, row, 3, UI_ACCENT_SOFT);
            else if (point_in_rect(mx, my, row)) fill_rounded_rect(ren, row, 3, UI_RAISED);

            snprintf(buf, sizeof(buf), "%d %d %d <- %d %d %d",
                     p.Ju, p.Kau, p.Kcu, p.Jl, p.Kal, p.Kcl);
            ui_text_v(ren, UI_FONT_MONO_SM, buf, row.x + 6, row, sel ? UI_ACCENT_TEXT : UI_DIM);
            fmt_mhz(buf, sizeof(buf), state->assignments[k].exp_freq, 4);
            ui_text_right(ren, UI_FONT_MONO_SM, buf, row.x + row.w - 6,
                          row.y + (row.h - ui_text_h(UI_FONT_MONO_SM)) / 2, UI_TEXT);
        }
        if (state->n_assignments == 0) {
            SDL_Rect row = ui_as_row(w, 0);
            panel_hint(ren, row, "Drag a peak while a predicted line is");
            row = ui_as_row(w, 1);
            panel_hint(ren, row, "selected to record an assignment.");
        }

        ui_button(ren, ui_as_save(state, w), "Save all", -1, UI_BTN_PRIMARY, 0, mx, my, m_down);
        ui_button(ren, ui_as_delete(state, w), "Delete selected", -1, UI_BTN_DANGER, 0, mx, my, m_down);

        SDL_Rect save = ui_as_save(state, w);
        ui_text(ren, UI_FONT_SANS_SM, "Written to assignments.txt in the working directory.",
                save.x, save.y + save.h + 10, UI_FAINT);
    }

    // 2. PEAK FINDER
    if (state->win_pf.visible) {
        SDL_RenderSetClipRect(ren, &state->win_pf.clip);
        draw_inspector_section(ren, &state->win_pf, UI_ICON_PEAK, mx, my);
        SDL_Rect w = state->win_pf.rect;

        field_val_i(state, INPUT_PF_SIG, buf, sizeof(buf), state->pf_sig_pts);
        panel_field(ren, state, w, "Search width", ui_pf_sig(w), INPUT_PF_SIG, buf, "pts");
        field_val_i(state, INPUT_PF_NOISE, buf, sizeof(buf), state->pf_noise_pts);
        panel_field(ren, state, w, "Noise window", ui_pf_noise(w), INPUT_PF_NOISE, buf, "pts");
        field_val(state, INPUT_PF_THRESH, buf, sizeof(buf), "%.1f", state->pf_thresh);
        panel_field(ren, state, w, "Threshold", ui_pf_thresh(w), INPUT_PF_THRESH, buf, "x sigma");

        ui_button(ren, ui_pf_find(w), "Find peaks", -1, UI_BTN_PRIMARY, 0, mx, my, m_down);
        ui_button(ren, ui_pf_export(w), "Export list", -1, UI_BTN_QUIET, 0, mx, my, m_down);

        snprintf(buf, sizeof(buf), "%d peak%s in the current view. Right-drag on",
                 state->n_peaks, state->n_peaks == 1 ? "" : "s");
        panel_hint(ren, ui_p_row(w, 4), buf);
        panel_hint2(ren, ui_p_row(w, 4), "the spectrum picks one by hand.");
    }

    // 3. ROLLING AVERAGE
    if (state->win_avg.visible) {
        SDL_RenderSetClipRect(ren, &state->win_avg.clip);
        draw_inspector_section(ren, &state->win_avg, UI_ICON_WAVE, mx, my);
        SDL_Rect w = state->win_avg.rect;

        field_val_i(state, INPUT_AVG_PTS, buf, sizeof(buf), state->rolling_avg_window);
        panel_field(ren, state, w, "Window", ui_avg_field(w), INPUT_AVG_PTS, buf, "pts");
        ui_toggle_row(ren, ui_avg_toggle(w), "Smoothing", state->rolling_avg_active, mx, my);
        panel_hint(ren, ui_p_row(w, 2), "Active trace only. The raw data is kept.");
    }

    // 4. BROADENING
    if (state->win_br.visible) {
        SDL_RenderSetClipRect(ren, &state->win_br.clip);
        draw_inspector_section(ren, &state->win_br, UI_ICON_BELL, mx, my);
        SDL_Rect w = state->win_br.rect;
        int kaiser = (state->broaden_mode == 1);

        static const char *modes[2] = {"Analytic", "Kaiser FFT"};
        ui_segmented(ren, ui_br_mode(w), modes, 2, kaiser);

        if (!kaiser) {
            field_val(state, INPUT_GAMMA, buf, sizeof(buf), "%.2f", state->lorentz_gamma);
            panel_field(ren, state, w, "Lorentz HWHM", ui_br_f1(w), INPUT_GAMMA, buf, "MHz");
            field_val(state, INPUT_GAUSS, buf, sizeof(buf), "%.2f", state->gauss_gamma);
            panel_field(ren, state, w, "Gauss HWHM", ui_br_f2(w), INPUT_GAUSS, buf, "MHz");

            const char *shape;
            if (state->lorentz_gamma > 0.0 && state->gauss_gamma > 0.0) shape = "Profile: Voigt (both widths set)";
            else if (state->lorentz_gamma > 0.0)                        shape = "Profile: Lorentzian";
            else if (state->gauss_gamma  > 0.0)                         shape = "Profile: Gaussian";
            else                                                        shape = "Set a width to see a profile";
            panel_hint(ren, ui_p_row(w, 3), shape);
        } else {
            field_val(state, INPUT_KBETA, buf, sizeof(buf), "%.2f", state->kaiser_beta);
            panel_field(ren, state, w, "Kaiser beta", ui_br_f1(w), INPUT_KBETA, buf, "");
            field_val_i(state, INPUT_KCEROS, buf, sizeof(buf), state->kaiser_ceros);
            panel_field(ren, state, w, "Zero-pad", ui_br_f2(w), INPUT_KCEROS, buf, "x");
            field_val(state, INPUT_KINTR, buf, sizeof(buf), "%.3f", state->kaiser_intrinsic);
            panel_field(ren, state, w, "Intrinsic FWHM", ui_br_f3(w), INPUT_KINTR, buf, "MHz");

            double df = spectrum_df(state);
            int cer = state->kaiser_ceros > 0 ? state->kaiser_ceros : 1;
            if (df > 0.0) {
                snprintf(buf, sizeof(buf), "bin df %.5g MHz", df);
                panel_hint(ren, ui_p_row(w, 4), buf);
                snprintf(buf, sizeof(buf), "resolution = ceros x df = %.4g MHz", cer * df);
                ui_text(ren, UI_FONT_SANS_SM, buf, ui_p_row(w, 5).x, ui_p_row(w, 5).y + 4, UI_ACCENT_TEXT);
            } else {
                panel_hint(ren, ui_p_row(w, 4), "Load a spectrum for the bin step.");
            }
        }
        ui_toggle_row(ren, ui_br_toggle(w, kaiser), "Simulated profile", state->broadening_active, mx, my);
    }

    // 5. INTENSITY ANALYSIS
    if (state->win_dip.visible) {
        SDL_RenderSetClipRect(ren, &state->win_dip.clip);
        draw_inspector_section(ren, &state->win_dip, UI_ICON_DIPOLE, mx, my);
        SDL_Rect w = state->win_dip.rect;

        ui_text(ren, UI_FONT_SANS_SM, "TEMPERATURES", ui_p_row(w, 0).x, ui_p_row(w, 0).y + 8, UI_ACCENT_TEXT);
        field_val(state, INPUT_CAT_TEMP, buf, sizeof(buf), "%.2f", state->cat_temp_k);
        panel_field(ren, state, w, "T cat", ui_int_cat_temp(w), INPUT_CAT_TEMP, buf, "K");
        field_val(state, INPUT_ROT_TEMP, buf, sizeof(buf), "%.2f", state->rot_temp_k);
        panel_field(ren, state, w, "T rot", ui_int_rot_temp(w), INPUT_ROT_TEMP, buf, "K");

        ui_text(ren, UI_FONT_SANS_SM, "DIPOLE MOMENTS", ui_p_row(w, 3).x, ui_p_row(w, 3).y + 8, UI_ACCENT_TEXT);
        SDL_Rect cat = ui_dip_field(w, 0, 0), red = ui_dip_field(w, 0, 1);
        ui_text(ren, UI_FONT_SANS_SM, "mu cat", cat.x + (cat.w - ui_text_w(UI_FONT_SANS_SM, "mu cat")) / 2,
                ui_p_row(w, 4).y + 8, UI_FAINT);
        ui_text(ren, UI_FONT_SANS_SM, "mu red", red.x + (red.w - ui_text_w(UI_FONT_SANS_SM, "mu red")) / 2,
                ui_p_row(w, 4).y + 8, UI_ACCENT_TEXT);

        static const char *component[3] = {"a", "b", "c"};
        static const int cat_input[3] = {INPUT_MUCAT_A, INPUT_MUCAT_B, INPUT_MUCAT_C};
        static const int red_input[3] = {INPUT_MURED_A, INPUT_MURED_B, INPUT_MURED_C};
        for (int i = 0; i < 3; i++) {
            SDL_Rect cat_f = ui_dip_field(w, i, 0), red_f = ui_dip_field(w, i, 1);
            ui_text_v(ren, UI_FONT_SANS, component[i], ui_p_row(w, 5 + i).x + 4, cat_f, UI_DIM);
            field_val(state, cat_input[i], buf, sizeof(buf), "%.4f", state->dipole_cat[i]);
            draw_field(ren, state, cat_f, NULL, buf, "D", cat_input[i]);
            field_val(state, red_input[i], buf, sizeof(buf), "%.4f", state->dipole_red[i]);
            draw_field(ren, state, red_f, NULL, buf, "D", red_input[i]);
        }
        panel_hint(ren, ui_p_row(w, 8), "Set T cat/T rot, then mu cat and mu red.");

        ui_text(ren, UI_FONT_SANS_SM, "RELATIVE INTENSITY FIT", ui_p_row(w, 9).x,
                ui_p_row(w, 9).y + 8, UI_ACCENT_TEXT);
        field_val(state, INPUT_FIT_WINDOW, buf, sizeof(buf), "%.4f", state->intfit_half_window_mhz);
        panel_field(ren, state, w, "Area half-window", ui_fit_window(w), INPUT_FIT_WINDOW, buf, "MHz");
        ui_toggle_row(ren, ui_fit_temperature(w), "Fit T rot", state->intfit_fit_temperature, mx, my);
        ui_text_v(ren, UI_FONT_SANS, "Fit dipoles", ui_p_row(w, 12).x, ui_p_row(w, 12), UI_DIM);
        static const char *dipole_label[3] = {"mu a", "mu b", "mu c"};
        for (int i = 0; i < 3; i++)
            ui_button(ren, ui_fit_dipole(w, i), dipole_label[i], -1, UI_BTN_QUIET,
                      state->intfit_fit_dipole[i], mx, my, m_down);
        ui_button(ren, ui_fit_run(w), "Run fit", -1, UI_BTN_PRIMARY, 0, mx, my, m_down);
        ui_button(ren, ui_fit_export(w), "Export fit", UI_ICON_EXPORT, UI_BTN_QUIET, 0, mx, my, m_down);
        if (state->intfit_has_result) {
            ui_text(ren, UI_FONT_SANS_SM, "FIT RESULT", ui_p_row(w, 14).x,
                    ui_p_row(w, 14).y + 8, UI_ACCENT_TEXT);
            snprintf(buf, sizeof(buf), "T rot  %.2f K (%s)    scale  %.4g", state->rot_temp_k,
                     state->intfit_fit_temperature ? "fitted" : "fixed", state->intfit_scale);
            panel_hint(ren, ui_p_row(w, 15), buf);
            snprintf(buf, sizeof(buf), "Lines  %d/%d used;  %d rejected;  RMS  %.3g",
                     state->intfit_n_used, state->intfit_n_candidate,
                     state->intfit_n_rejected, state->intfit_log_rms);
            panel_hint(ren, ui_p_row(w, 16), buf);
            snprintf(buf, sizeof(buf), "mu red / D:  a %.4g   b %.4g   c %.4g",
                     state->dipole_red[0] != 0.0 ? state->dipole_red[0] : state->dipole_cat[0],
                     state->dipole_red[1] != 0.0 ? state->dipole_red[1] : state->dipole_cat[1],
                     state->dipole_red[2] != 0.0 ? state->dipole_red[2] : state->dipole_cat[2]);
            panel_hint(ren, ui_p_row(w, 17), buf);
            if (state->intfit_reference_component >= 0) {
                static const char comp[] = {'a', 'b', 'c'};
                snprintf(buf, sizeof(buf), "mu_%c reference; factors a %.3g  b %.3g  c %.3g",
                         comp[state->intfit_reference_component], state->intfit_component_scale[0],
                         state->intfit_component_scale[1], state->intfit_component_scale[2]);
                panel_hint(ren, ui_p_row(w, 18), buf);
            } else panel_hint(ren, ui_p_row(w, 18), "No relative dipole correction was determined.");
        } else {
            panel_hint(ren, ui_p_row(w, 15), "Uses areas around assigned experimental peaks.");
            panel_hint(ren, ui_p_row(w, 16), "Use isolated lines; blends are rejected automatically.");
        }
        panel_hint(ren, ui_p_row(w, 19), state->intfit_message[0] ? state->intfit_message :
                   "The global scale is always fitted (experimental units are arbitrary).");
    }

    // 6. INTENSITY RANGE
    if (state->win_cut.visible) {
        SDL_RenderSetClipRect(ren, &state->win_cut.clip);
        draw_inspector_section(ren, &state->win_cut, UI_ICON_RANGE, mx, my);
        SDL_Rect w = state->win_cut.rect;

        field_val(state, INPUT_PRED_MIN, buf, sizeof(buf), "%.1f", state->pred_min_log_int);
        panel_field(ren, state, w, "Min log I", ui_cut_min(w), INPUT_PRED_MIN, buf, "");
        field_val(state, INPUT_PRED_MAX, buf, sizeof(buf), "%.1f", state->pred_max_log_int);
        panel_field(ren, state, w, "Max log I", ui_cut_max(w), INPUT_PRED_MAX, buf, "");

        int shown = 0;
        for (int i = 0; i < state->n_pred; i++) if (pred_passes_filter(state, i)) shown++;
        panel_hint(ren, ui_p_row(w, 2), "Hides predicted lines outside the range.");
        snprintf(buf, sizeof(buf), "%d of %d lines shown.", shown, state->n_pred);
        panel_hint2(ren, ui_p_row(w, 2), buf);
    }

    // 7. FREQUENCY JUMP
    if (state->win_jump.visible) {
        SDL_RenderSetClipRect(ren, &state->win_jump.clip);
        draw_inspector_section(ren, &state->win_jump, UI_ICON_JUMP, mx, my);
        SDL_Rect w = state->win_jump.rect;

        field_val(state, INPUT_JUMP_MIN, buf, sizeof(buf), "%.1f", state->vxmin);
        panel_field(ren, state, w, "Start", ui_jump_start(w), INPUT_JUMP_MIN, buf, "MHz");
        field_val(state, INPUT_JUMP_MAX, buf, sizeof(buf), "%.1f", state->vxmax);
        panel_field(ren, state, w, "End", ui_jump_end(w), INPUT_JUMP_MAX, buf, "MHz");
    }

    // 8. TRANSITION FILTER
    if (state->win_filt.visible) {
        SDL_RenderSetClipRect(ren, &state->win_filt.clip);
        draw_inspector_section(ren, &state->win_filt, UI_ICON_FILTER, mx, my);
        SDL_Rect w = state->win_filt.rect;

        ui_toggle_row(ren, ui_filt_master(w), "Filter", state->filter_active, mx, my);

        SDL_Rect row = ui_p_row(w, 1);
        ui_text_v(ren, UI_FONT_SANS, "Dipole", row.x + 4, row, UI_DIM);
        static const char *mu_lbl[3] = {"mu a", "mu b", "mu c"};
        for (int i = 0; i < 3; i++)
            ui_button(ren, ui_filt_mu(w, i), mu_lbl[i], -1, UI_BTN_QUIET, state->filt_mu[i], mx, my, m_down);

        row = ui_p_row(w, 2);
        ui_text_v(ren, UI_FONT_SANS, "Branch", row.x + 4, row, UI_DIM);
        static const char *br_lbl[3] = {"P", "Q", "R"};
        for (int i = 0; i < 3; i++)
            ui_button(ren, ui_filt_br(w, i), br_lbl[i], -1, UI_BTN_QUIET, state->filt_br[i], mx, my, m_down);

        ui_toggle_row(ren, ui_filt_range(w), "Quantum number range", state->filt_use_range, mx, my);

        SDL_Rect hdr = ui_filt_delta_hdr(w);
        SDL_Rect c0 = ui_filt_qn(w, 0, 0), c1 = ui_filt_qn(w, 0, 1);
        ui_text(ren, UI_FONT_SANS_SM, "min", c0.x + (c0.w - ui_text_w(UI_FONT_SANS_SM, "min")) / 2, hdr.y + 10, UI_FAINT);
        ui_text(ren, UI_FONT_SANS_SM, "max", c1.x + (c1.w - ui_text_w(UI_FONT_SANS_SM, "max")) / 2, hdr.y + 10, UI_FAINT);

        const char *qn_lbl[3] = {"J", "Ka", "Kc"};
        int qn_lo[3] = {state->filt_j_min, state->filt_ka_min, state->filt_kc_min};
        int qn_hi[3] = {state->filt_j_max, state->filt_ka_max, state->filt_kc_max};
        int qn_in_lo[3] = {INPUT_FILT_JMIN, INPUT_FILT_KAMIN, INPUT_FILT_KCMIN};
        int qn_in_hi[3] = {INPUT_FILT_JMAX, INPUT_FILT_KAMAX, INPUT_FILT_KCMAX};
        for (int i = 0; i < 3; i++) {
            SDL_Rect lo = ui_filt_qn(w, i, 0), hi = ui_filt_qn(w, i, 1);
            ui_text_v(ren, UI_FONT_SANS, qn_lbl[i], ui_p_row(w, 5 + i).x + 4, lo, UI_DIM);
            field_val_i(state, qn_in_lo[i], buf, sizeof(buf), qn_lo[i]);
            draw_field(ren, state, lo, NULL, buf, NULL, qn_in_lo[i]);
            field_val_i(state, qn_in_hi[i], buf, sizeof(buf), qn_hi[i]);
            draw_field(ren, state, hi, NULL, buf, NULL, qn_in_hi[i]);
        }

        ui_toggle_row(ren, ui_filt_jump(w), "Quantum number jump", state->filt_use_delta, mx, my);
        const char *d_lbl[3] = {"dJ", "dKa", "dKc"};
        int d_val[3] = {state->filt_dj, state->filt_dka, state->filt_dkc};
        int d_in[3]  = {INPUT_FILT_DJ, INPUT_FILT_DKA, INPUT_FILT_DKC};
        for (int i = 0; i < 3; i++) {
            SDL_Rect f = ui_filt_delta(w, i);
            ui_text_v(ren, UI_FONT_SANS, d_lbl[i], f.x - ui_text_w(UI_FONT_SANS, d_lbl[i]) - 6, f, UI_DIM);
            field_val_i(state, d_in[i], buf, sizeof(buf), d_val[i]);
            draw_field(ren, state, f, NULL, buf, NULL, d_in[i]);
        }
    }

    // 8. SPECTRA
    if (state->win_spec.visible) {
        SDL_RenderSetClipRect(ren, &state->win_spec.clip);
        draw_inspector_section(ren, &state->win_spec, UI_ICON_LAYERS, mx, my);
        SDL_Rect w = state->win_spec.rect;

        static const char *lay[2] = {"Overlay", "Stack"};
        static const char *yy[2]  = {"Shared Y", "Normalised"};
        ui_segmented(ren, ui_spec_layout(w), lay, 2, state->multi_layout);
        ui_segmented(ren, ui_spec_ynorm(w), yy, 2, state->multi_ynorm);
        ui_toggle_row(ren, ui_spec_indiv(w), "Intensity keys act on the active trace only",
                      state->multi_indiv_int, mx, my);

        for (int i = 0; i < state->n_spectra; i++) {
            Spectrum *sp = &state->spectra[i];
            SDL_Rect row = ui_spec_row(w, i);
            SDL_Rect l1 = ui_spec_line1(w, i), l2 = ui_spec_line2(w, i);
            if (i == state->active_spec) fill_rounded_rect(ren, row, 3, UI_ACCENT_SOFT);
            else if (point_in_rect(mx, my, row)) fill_rounded_rect(ren, row, 3, UI_RAISED);

            fill_rounded_rect(ren, (SDL_Rect){l1.x + 6, l1.y + 8, 9, 9}, 2, sp->color);
            char nm[24]; snprintf(nm, sizeof(nm), "%.16s", sp->name);
            ui_text_v(ren, UI_FONT_SANS, nm, l1.x + 22, l1, sp->visible ? UI_TEXT : UI_FAINT);
            ui_button(ren, ui_spec_vis(w, i), "", UI_ICON_EYE, UI_BTN_QUIET, sp->visible, mx, my, m_down);
            ui_button(ren, ui_spec_del(w, i), "", UI_ICON_CLOSE, UI_BTN_DANGER, 0, mx, my, m_down);

            /* second line: the numbers that belong to this trace alone */
            ui_text_v(ren, UI_FONT_SANS_SM, "shift", l2.x + 6, l2, UI_FAINT);
            ui_button(ren, ui_spec_minus(w, i), "-", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            ui_button(ren, ui_spec_plus(w, i),  "+", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            snprintf(nm, sizeof(nm), "%+.2f", sp->voffset);
            ui_text_v(ren, UI_FONT_MONO_SM, nm, ui_spec_plus(w, i).x + 26, l2, UI_DIM);

            int op = sp->opacity > 0 ? sp->opacity : 100;
            ui_button(ren, ui_spec_op_minus(w, i), "-", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            ui_button(ren, ui_spec_op_plus(w, i),  "+", -1, UI_BTN_QUIET, 0, mx, my, m_down);
            snprintf(nm, sizeof(nm), "%d%%", op);
            ui_text_v(ren, UI_FONT_MONO_SM, nm,
                      ui_spec_op_minus(w, i).x + 26, l2, op < 100 ? UI_ACCENT_TEXT : UI_DIM);
        }

        SDL_Rect last = ui_spec_row(w, state->n_spectra > 0 ? state->n_spectra : 0);
        if (state->n_spectra == 0)
            ui_text(ren, UI_FONT_SANS_SM, "Drop a spectrum file to add one.", last.x + 6, last.y + 4, UI_FAINT);
        else
            ui_text(ren, UI_FONT_SANS_SM, "Drop a file to add a trace.", last.x + 6, last.y + 6, UI_FAINT);
    }

    if (state->win_predfit.visible) {
        SDL_RenderSetClipRect(ren, &state->win_predfit.clip);
        draw_inspector_section(ren, &state->win_predfit, UI_ICON_LIST, mx, my);
        SDL_Rect w=state->win_predfit.rect; PredFitState *p=&state->predfit;
        ui_text(ren,UI_FONT_SANS_SM,"QUICK PREDICTION",ui_p_row(w,0).x,ui_p_row(w,0).y+8,UI_ACCENT_TEXT);
        const char *lab[]={"A","B","C","mu a","mu b","mu c","T rot","Start","End"};
        double val[]={p->a,p->b,p->c,p->mu[0],p->mu[1],p->mu[2],p->temp_k,p->fmin_ghz,p->fmax_ghz};
        int inp[]={INPUT_PF_A,INPUT_PF_B,INPUT_PF_C,INPUT_PF_MUA,INPUT_PF_MUB,INPUT_PF_MUC,INPUT_PF_TEMP,INPUT_PF_FMIN,INPUT_PF_FMAX};
        const char *unit[]={"MHz","MHz","MHz","D","D","D","K","GHz","GHz"};
        for(int i=0;i<9;i++){field_val(state,inp[i],buf,sizeof(buf),"%.6g",val[i]);panel_field(ren,state,w,lab[i],ui_pf_model(w,i+1),inp[i],buf,unit[i]);}
        ui_button(ren,ui_pf_calculate(w),"Calculate",-1,UI_BTN_PRIMARY,0,mx,my,m_down);
        ui_button(ren,ui_pf_fit(w),"Fit",-1,UI_BTN_QUIET,0,mx,my,m_down);
        ui_button(ren,ui_pf_undo(w),"Undo",-1,UI_BTN_QUIET,0,mx,my,m_down);
        ui_button(ren,ui_pf_advanced(w),"Advanced...",UI_ICON_LIST,UI_BTN_QUIET,0,mx,my,m_down);
        double qrot = (p->a > 0.0 && p->b > 0.0 && p->c > 0.0 && p->temp_k > 0.0)
                    ? 5.3311e6 * sqrt((p->temp_k*p->temp_k*p->temp_k)/(p->a*p->b*p->c)) : 0.0;
        snprintf(buf,sizeof(buf),"Qrot(T) = %.6g   S-reduction, prolate, sigma = 1",qrot);
        panel_hint(ren,ui_p_row(w,12),buf);
        panel_hint(ren,ui_p_row(w,13),p->status[0]?p->status:"Calculate runs SPCAT; Fit runs SPFIT then SPCAT.");
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

    struct { const char *group; const char *key; const char *what; } rows[] = {
        {"Navigate",  "Q  E",      "Zoom out / in on frequency"},
        {NULL,        "A  S",      "Pan left / right"},
        {NULL,        "W  Z",      "Scale experimental intensity"},
        {NULL,        "Tab",       "Autoscale intensity to the view"},
        {NULL,        "R",         "Reset the view"},
        {NULL,        "Shift+Tab", "Normalize prediction"},
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
        {NULL,        "D",         "Dipole moments"},
        {NULL,        "C",         "Intensity range"},
        {NULL,        "F",         "Frequency jump"},
        {NULL,        "B",         "Transition filter"},
        {NULL,        "cmd F",     "Run SPFIT (Pred&Fit)"},
        {NULL,        "cmd B",     "Undo the last fit"},
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

    /* Split at a group boundary past the middle, and size the card to the
       taller of the two columns: the list grows as the program does, and a
       fixed height quietly cut the last rows off. */
    int split = n;
    for (int i = 0; i < n; i++) if (rows[i].group && i >= n / 2) { split = i; break; }
    int h1 = 0, h2 = 0;
    for (int i = 0; i < n; i++) {
        int add = 22 + (rows[i].group ? 30 : 0);
        if (i < split) h1 += add; else h2 += add;
    }
    card.h = 58 + (h1 > h2 ? h1 : h2) + 20;
    if (card.y + card.h > l->win_h - 30) card.h = l->win_h - 30 - card.y;
    fill_rounded_rect(ren, card, 8, (SDL_Color){19, 20, 22, 250});
    ui_frame(ren, card, UI_LINE);
    SDL_Rect head = {card.x, card.y, card.w, 38};
    ui_text_v(ren, UI_FONT_TITLE, "Keyboard and mouse", head.x + 20, head, UI_TEXT);
    ui_hline(ren, card.x, card.x + card.w, card.y + 38, UI_LINE);

    int col_w = (card.w - 60) / 2;
    int x = card.x + 24, y = card.y + 58;
    for (int i = 0; i < n; i++) {
        if (i == split) { x = card.x + 30 + col_w; y = card.y + 58; }
        if (rows[i].group) {
            if (i != 0 && i != split) y += 10;
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

// A floating card: dark ground, hairline, header strip.
static SDL_Rect ui_card(SDL_Renderer *ren, SDL_Rect r, const char *title, SDL_Color title_c,
                        const char *note) {
    fill_rounded_rect(ren, r, 5, (SDL_Color){12, 13, 15, 240});
    ui_frame(ren, r, UI_LINE);
    SDL_Rect head = {r.x, r.y, r.w, 22};
    ui_text_v(ren, UI_FONT_SANS, title, head.x + 10, head, title_c);
    if (note) {
        int nw = ui_text_w(UI_FONT_SANS_SM, note);
        ui_text_v(ren, UI_FONT_SANS_SM, note, head.x + head.w - 10 - nw, head, UI_FAINT);
    }
    ui_hline(ren, r.x + 1, r.x + r.w - 1, r.y + 22, UI_LINE_SOFT);
    return head;
}

// One transition row inside a card: branch tag, quantum numbers, frequency.
static void ui_card_row(SDL_Renderer *ren, SDL_Rect r, int i, const PredLine *p) {
    char buf[128];
    int y = r.y + 22 + i * 19;
    SDL_Rect row = {r.x, y, r.w, 19};
    snprintf(buf, sizeof(buf), "%c%c", p->branch, p->mu);
    ui_text_v(ren, UI_FONT_MONO_SM, buf, r.x + 10, row, color_for_pred(p->branch, p->mu));
    snprintf(buf, sizeof(buf), "%d %d %d <- %d %d %d",
             p->Ju, p->Kau, p->Kcu, p->Jl, p->Kal, p->Kcl);
    ui_text_v(ren, UI_FONT_MONO_SM, buf, r.x + 38, row, UI_TEXT);
    fmt_mhz(buf, sizeof(buf), p->freq_mhz, 4);
    ui_text_right(ren, UI_FONT_MONO_SM, buf, r.x + r.w - 10,
                  y + (19 - ui_text_h(UI_FONT_MONO_SM)) / 2, UI_ACCENT);
}

// A compact readout that follows the pointer.
static void ui_tag(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color c) {
    int tw = ui_text_w(UI_FONT_MONO_SM, text);
    SDL_Rect r = {x, y, tw + 16, 20};
    fill_rounded_rect(ren, r, 4, (SDL_Color){8, 9, 10, 235});
    ui_frame(ren, r, UI_LINE);
    ui_text_v(ren, UI_FONT_MONO_SM, text, r.x + 8, r, c);
}

static void draw_cursor_overlay(SDL_Renderer *ren, TTF_Font *font, AppState *state, Layout *l) {
    (void)font;
    int mx, my;
    char buf[160];
    SDL_GetMouseState(&mx, &my);
    int in_exp = point_in_rect(mx, my, (SDL_Rect){l->exp_x, l->exp_y, l->exp_w, l->exp_h});

    if (state->dragging_offset) {
        snprintf(buf, sizeof(buf), "offset %+.4f MHz", state->exp_offset);
        ui_tag(ren, l->exp_x + l->exp_w - 190, l->exp_y + 10, buf, UI_ACCENT);
    }

    if (state->selecting_left || state->selecting_right) {
        double x0 = state->vxmin + (double)(state->sel_start.x - l->exp_x) / l->exp_w * (state->vxmax - state->vxmin);
        double x1 = state->vxmin + (double)(state->sel_cur.x - l->exp_x) / l->exp_w * (state->vxmax - state->vxmin);
        if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
        snprintf(buf, sizeof(buf), "%s  %.4f MHz", state->selecting_right ? "peak search" : "zoom", x1 - x0);
        int bx = state->sel_cur.x + 12, by = state->sel_cur.y - 30;
        if (bx + 190 > l->exp_x + l->exp_w) bx = state->sel_cur.x - 190;
        if (by < l->exp_y + 6) by = l->exp_y + 6;
        ui_tag(ren, bx, by, buf, state->selecting_right ? UI_WARN : UI_ACCENT);
    }

    // Pointer readout, next to the cursor as in the plot's own coordinates.
    if (in_exp && !state->selecting_left && !state->selecting_right) {
        double fx = (mx - l->exp_x) / (double)l->exp_w;
        double fy = 1.0 - (my - l->exp_y) / (double)l->exp_h;
        double cx = state->vxmin + fx * (state->vxmax - state->vxmin) - state->exp_offset;
        double cy = state->vymin + fy * (state->vymax - state->vymin);
        char freq[48]; fmt_mhz(freq, sizeof(freq), cx, 4);
        snprintf(buf, sizeof(buf), "f %s MHz   I %.1e", freq, cy);
        int bx = mx + 14, by = my - 30;
        if (bx + 230 > l->exp_x + l->exp_w) bx = mx - 230;
        if (by < l->exp_y + 6) by = l->exp_y + 6;
        ui_tag(ren, bx, by, buf, state->settings.cursor_color);

        SDL_Color cc = state->settings.cursor_color;
        SDL_SetRenderDrawColor(ren, cc.r, cc.g, cc.b, 150);
        SDL_RenderDrawLine(ren, mx, l->exp_y, mx, l->exp_y + l->exp_h);
    }

    if (state->n_peaks > 0 && !state->selecting_right) {
        Peak *last = &state->peaks[state->n_peaks - 1];
        if (last->x + state->exp_offset >= state->vxmin && last->x + state->exp_offset <= state->vxmax) {
            char freq[48]; fmt_mhz(freq, sizeof(freq), last->x, 4);
            snprintf(buf, sizeof(buf), "last peak %s", freq);
            ui_tag(ren, l->exp_x + 10, l->exp_y + l->exp_h - 30, buf, UI_WARN);
        }
    }

    // Measure tool
    if (state->measure_active) {
        ui_tag(ren, l->exp_x + l->exp_w - 120, l->exp_y + 10, "measure", (SDL_Color){225, 160, 245, 255});
        if (state->measure_phase == 1) {
            int px1 = l->exp_x + (state->measure_x1 + state->exp_offset - state->vxmin) /
                                 (state->vxmax - state->vxmin) * l->exp_w;
            SDL_SetRenderDrawColor(ren, 225, 130, 245, 210);
            SDL_RenderDrawLine(ren, px1, l->exp_y, px1, l->exp_y + l->exp_h);
            SDL_RenderDrawLine(ren, px1, my, mx, my);
            if (in_exp) {
                double curr = state->vxmin + ((double)(mx - l->exp_x) / l->exp_w) *
                                             (state->vxmax - state->vxmin) - state->exp_offset;
                snprintf(buf, sizeof(buf), "%.4f MHz", fabs(curr - state->measure_x1));
                ui_tag(ren, mx + 12, my - 26, buf, (SDL_Color){235, 175, 250, 255});
            }
        }
    }

    // Selected predicted transitions
    if (state->n_selected > 0) {
        int show_n = state->n_selected < 4 ? state->n_selected : 4;
        SDL_Rect card = {l->pred_x + l->pred_w - 372, l->pred_y + 10, 362, 22 + show_n * 19 + 6};
        if (card.x < l->pred_x + 10) card.x = l->pred_x + 10;
        snprintf(buf, sizeof(buf), "%d line%s selected", state->n_selected, state->n_selected == 1 ? "" : "s");
        ui_card(ren, card, buf, UI_TEXT, "ctrl-click to add");
        for (int i = 0; i < show_n; i++)
            ui_card_row(ren, card, i, &state->pred_lines[state->selected_indices[i]]);
        return;
    }

    // Transitions near the bar
    if (state->bar_active && state->n_pred > 0) {
        int p0 = binary_search_pred_lower(state->pred_lines, state->n_pred, state->pbar_x - 1.0);
        int p1 = binary_search_pred_upper(state->pred_lines, state->n_pred, state->pbar_x + 1.0);
        if (p0 < 0) p0 = 0;
        if (p1 >= state->n_pred) p1 = state->n_pred - 1;

        double tol = (state->pvxmax - state->pvxmin) * 0.01;
        double closest = 1e99;
        int hover_idx[4], hover_n = 0;
        for (int i = p0; i <= p1; i++) {
            if (!pred_passes_filter(state, i)) continue;
            double d = fabs(state->pred_lines[i].freq_mhz - state->pbar_x);
            if (d < closest) closest = d;
            if (d < tol && hover_n < 4) hover_idx[hover_n++] = i;
        }
        if (hover_n > 0 && closest < tol) {
            SDL_Rect card = {l->pred_x + l->pred_w - 372, l->pred_y + 10, 362, 22 + hover_n * 19 + 6};
            if (card.x < l->pred_x + 10) card.x = l->pred_x + 10;
            snprintf(buf, sizeof(buf), "%d line%s near the bar", hover_n, hover_n == 1 ? "" : "s");
            ui_card(ren, card, buf, UI_WARN, NULL);
            for (int i = 0; i < hover_n; i++)
                ui_card_row(ren, card, i, &state->pred_lines[hover_idx[i]]);
        }
    }
}
