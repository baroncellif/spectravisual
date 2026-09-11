#include "intensity_fit.h"
#include "predfit.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C2_K_CM 1.438776877
#define MHZ_PER_CM 29979.2458

typedef struct {
    int assignment_index;
    const PredLine *pred;
    double exp_area;
    double model;
    double log_ratio;
    double residual;
    int used;
} FitWork;

static int dipole_component(char mu) {
    if (mu == 'a' || mu == 'A') return 0;
    if (mu == 'b' || mu == 'B') return 1;
    if (mu == 'c' || mu == 'C') return 2;
    return -1;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_of(const double *values, int n) {
    if (n <= 0) return NAN;
    double *copy = malloc((size_t)n * sizeof(*copy));
    if (!copy) return NAN;
    memcpy(copy, values, (size_t)n * sizeof(*copy));
    qsort(copy, (size_t)n, sizeof(*copy), cmp_double);
    double answer = (n & 1) ? copy[n / 2] : 0.5 * (copy[n / 2 - 1] + copy[n / 2]);
    free(copy);
    return answer;
}

static int same_qn(const PredLine *a, const PredLine *b) {
    return a->n_qn == b->n_qn &&
           a->Ju == b->Ju && a->Kau == b->Kau && a->Kcu == b->Kcu &&
           a->M1u == b->M1u && a->M2u == b->M2u && a->M3u == b->M3u &&
           a->Jl == b->Jl && a->Kal == b->Kal && a->Kcl == b->Kcl &&
           a->M1l == b->M1l && a->M2l == b->M2l && a->M3l == b->M3l;
}

/* Catalogues are frequency-sorted.  Match the saved assignment back to the
 * current line so its CAT intensity is never stale after a T/mu edit. */
static const PredLine *current_pred_line(const AppState *s, const Assignment *a) {
    int lo = 0, hi = s->n_pred;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->pred_lines[mid].freq_mhz < a->pred.freq_mhz) lo = mid + 1;
        else hi = mid;
    }
    int first = lo;
    for (int i = first - 3; i <= first + 3; i++) {
        if (i < 0 || i >= s->n_pred) continue;
        if (same_qn(&s->pred_lines[i], &a->pred)) return &s->pred_lines[i];
    }
    const PredLine *best = NULL;
    double best_d = DBL_MAX;
    for (int i = first - 2; i <= first + 2; i++) {
        if (i < 0 || i >= s->n_pred) continue;
        double d = fabs(s->pred_lines[i].freq_mhz - a->pred.freq_mhz);
        if (d < best_d) { best_d = d; best = &s->pred_lines[i]; }
    }
    return best_d < 1e-5 ? best : NULL;
}

static int lower_point(const Point *p, int n, double x) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p[mid].x < x) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Integral of the displayed active trace.  The edge points are linearly
 * interpolated, so a changed half-width does not jump at a data-bin edge. */
int intensity_fit_integrate_area(const AppState *s, double center, double half_width, double *area) {
    if (!s->current_pts || s->n_pts < 2 || !(half_width > 0.0)) return 0;
    double left = center - half_width, right = center + half_width;
    if (left < s->current_pts[0].x || right > s->current_pts[s->n_pts - 1].x) return 0;
    int i = lower_point(s->current_pts, s->n_pts, left);
    if (i <= 0 || i >= s->n_pts) return 0;
    double x0 = s->current_pts[i - 1].x, x1 = s->current_pts[i].x;
    double yprev = s->current_pts[i - 1].y + (left - x0) *
                   (s->current_pts[i].y - s->current_pts[i - 1].y) / (x1 - x0);
    double xprev = left, sum = 0.0;
    for (; i < s->n_pts && s->current_pts[i].x < right; i++) {
        double x = s->current_pts[i].x, y = s->current_pts[i].y;
        sum += 0.5 * (yprev + y) * (x - xprev);
        xprev = x; yprev = y;
    }
    if (i >= s->n_pts) return 0;
    x0 = s->current_pts[i - 1].x; x1 = s->current_pts[i].x;
    double yright = s->current_pts[i - 1].y + (right - x0) *
                    (s->current_pts[i].y - s->current_pts[i - 1].y) / (x1 - x0);
    sum += 0.5 * (yprev + yright) * (right - xprev);
    *area = sum;
    return isfinite(sum);
}

static double line_model(const AppState *s, const PredLine *p, double temp_k) {
    double out = pow(10.0, p->cat_lgint);
    if (!(out > 0.0) || !isfinite(out)) return 0.0;
    int c = dipole_component(p->mu);
    if (c >= 0 && s->dipole_cat[c] != 0.0 && s->dipole_red[c] != 0.0)
        out *= (s->dipole_red[c] * s->dipole_red[c]) /
               (s->dipole_cat[c] * s->dipole_cat[c]);

    if (!(s->cat_temp_k > 0.0) || !(temp_k > 0.0)) return out;
    double nu_cm = p->freq_mhz / MHZ_PER_CM;
    double stim_t = -expm1(-C2_K_CM * nu_cm / temp_k);
    double stim_cat = -expm1(-C2_K_CM * nu_cm / s->cat_temp_k);
    double pop_t = exp(-C2_K_CM * p->elo_cm / temp_k) * stim_t /
                   pow(temp_k, 0.5 * p->rot_dof);
    double pop_cat = exp(-C2_K_CM * p->elo_cm / s->cat_temp_k) * stim_cat /
                     pow(s->cat_temp_k, 0.5 * p->rot_dof);
    return (pop_cat > 0.0 && isfinite(pop_t)) ? out * pop_t / pop_cat : 0.0;
}

static int collect_work(const AppState *s, FitWork *work) {
    int n = 0;
    for (int i = 0; i < s->n_assignments; i++) {
        const PredLine *p = current_pred_line(s, &s->assignments[i]);
        double area = 0.0;
        if (!p || !intensity_fit_integrate_area(s, s->assignments[i].exp_freq,
                                                s->intfit_half_window_mhz, &area) || !(area > 0.0))
            continue;
        work[n++] = (FitWork){i, p, area, 0.0, 0.0, 0.0, 0};
    }
    return n;
}

/* Estimate a global scale in log intensity.  A MAD pass removes blends and
 * failed assignments before the final mean and RMS are reported. */
static double evaluate_temperature(const AppState *s, FitWork *work, int n,
                                   double temp_k, double *scale_out, int *used_out) {
    double *v = malloc((size_t)n * sizeof(*v));
    double *dev = malloc((size_t)n * sizeof(*dev));
    if (!v || !dev) { free(v); free(dev); return DBL_MAX; }
    int m = 0;
    for (int i = 0; i < n; i++) {
        work[i].model = line_model(s, work[i].pred, temp_k);
        work[i].used = 0;
        if (work[i].model > 0.0 && isfinite(work[i].model)) {
            work[i].log_ratio = log(work[i].exp_area / work[i].model);
            if (isfinite(work[i].log_ratio)) v[m++] = work[i].log_ratio;
        }
    }
    if (m < 2) { free(v); free(dev); return DBL_MAX; }
    double med = median_of(v, m);
    for (int i = 0; i < m; i++) dev[i] = fabs(v[i] - med);
    double mad = median_of(dev, m);
    double cut = fmax(0.12, 3.0 * 1.4826 * (isfinite(mad) ? mad : 0.0));
    int used = 0;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        if (!(work[i].model > 0.0) || !isfinite(work[i].log_ratio)) continue;
        if (fabs(work[i].log_ratio - med) <= cut) {
            work[i].used = 1;
            sum += work[i].log_ratio;
            used++;
        }
    }
    if (used < 2) {
        used = 0; sum = 0.0;
        for (int i = 0; i < n; i++) if (work[i].model > 0.0 && isfinite(work[i].log_ratio)) {
            work[i].used = 1; sum += work[i].log_ratio; used++;
        }
    }
    double scale_log = sum / (double)used;
    double ss = 0.0;
    for (int i = 0; i < n; i++) {
        work[i].residual = work[i].log_ratio - scale_log;
        if (work[i].used) ss += work[i].residual * work[i].residual;
    }
    if (scale_out) *scale_out = exp(scale_log);
    if (used_out) *used_out = used;
    free(v); free(dev);
    return sqrt(ss / (double)used);
}

static double fitted_temperature(const AppState *s, FitWork *work, int n) {
    if (!s->intfit_fit_temperature || !(s->cat_temp_k > 0.0))
        return s->rot_temp_k > 0.0 ? s->rot_temp_k : s->cat_temp_k;
    const double lo = log(0.05), hi = log(fmax(1000.0, 3.0 * s->cat_temp_k));
    double best_x = lo, best = DBL_MAX, step = (hi - lo) / 120.0;
    for (int i = 0; i <= 120; i++) {
        double x = lo + i * step;
        double score = evaluate_temperature(s, work, n, exp(x), NULL, NULL);
        if (score < best) { best = score; best_x = x; }
    }
    double a = fmax(lo, best_x - step), b = fmin(hi, best_x + step);
    const double phi = 0.6180339887498949;
    double c = b - phi * (b - a), d = a + phi * (b - a);
    double fc = evaluate_temperature(s, work, n, exp(c), NULL, NULL);
    double fd = evaluate_temperature(s, work, n, exp(d), NULL, NULL);
    for (int i = 0; i < 32; i++) {
        if (fc < fd) { b = d; d = c; fd = fc; c = b - phi * (b - a); fc = evaluate_temperature(s, work, n, exp(c), NULL, NULL); }
        else         { a = c; c = d; fc = fd; d = a + phi * (b - a); fd = evaluate_temperature(s, work, n, exp(d), NULL, NULL); }
    }
    return exp(0.5 * (a + b));
}

static int fit_relative_dipoles(AppState *s, FitWork *work, int n) {
    double vals[3][MAX_ASSIGNMENTS];
    int count[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) if (work[i].used) {
        int c = dipole_component(work[i].pred->mu);
        if (c >= 0 && s->dipole_cat[c] != 0.0 &&
            (s->dipole_red[c] != 0.0 || !s->intfit_fit_dipole[c]))
            vals[c][count[c]++] = work[i].residual;
    }
    /* Prefer an eligible component which the user deliberately did not ask
       to alter as the reference.  That makes “fit mu_b only” meaningful:
       mu_a stays fixed and supplies the otherwise missing absolute anchor. */
    int ref = -1;
    for (int c = 0; c < 3; c++)
        if (!s->intfit_fit_dipole[c] && count[c] >= 2) { ref = c; break; }
    if (ref < 0)
        ref = count[0] >= 2 ? 0 : (count[1] >= 2 ? 1 : (count[2] >= 2 ? 2 : -1));
    s->intfit_reference_component = ref;
    for (int c = 0; c < 3; c++) {
        s->intfit_component_n[c] = count[c];
        s->intfit_component_scale[c] = 1.0;
    }
    if (ref < 0) return 0;
    double ref_med = median_of(vals[ref], count[ref]);
    int changed = 0;
    for (int c = 0; c < 3; c++) if (c != ref && s->intfit_fit_dipole[c] && count[c] >= 2) {
        double delta = median_of(vals[c], count[c]) - ref_med;
        double factor = exp(0.5 * delta);
        if (isfinite(factor) && factor > 0.0) {
            s->dipole_red[c] *= factor;
            s->intfit_component_scale[c] = factor;
            changed = 1;
        }
    }
    return changed;
}

int intensity_fit_run(AppState *s) {
    if (!s || !s->current_pts || s->n_pts < 2 || s->n_pred <= 0 || s->n_assignments < 2) {
        if (s) snprintf(s->intfit_message, sizeof(s->intfit_message), "Need spectrum, catalog, and at least 2 assignments.");
        return 0;
    }
    if (!(s->intfit_half_window_mhz > 0.0)) {
        snprintf(s->intfit_message, sizeof(s->intfit_message), "Area half-width must be positive.");
        return 0;
    }
    if (s->intfit_fit_temperature && !(s->cat_temp_k > 0.0)) {
        snprintf(s->intfit_message, sizeof(s->intfit_message), "Set T cat before fitting T rot.");
        return 0;
    }

    /* A blank mu red means “same as catalogue” elsewhere in the app.  Make
       that explicit only for components selected for a correction. */
    for (int c = 0; c < 3; c++)
        if (s->intfit_fit_dipole[c] && s->dipole_cat[c] != 0.0 && s->dipole_red[c] == 0.0)
            s->dipole_red[c] = s->dipole_cat[c];
    double starting_red[3];
    memcpy(starting_red, s->dipole_red, sizeof(starting_red));

    FitWork *work = malloc((size_t)s->n_assignments * sizeof(*work));
    if (!work) { snprintf(s->intfit_message, sizeof(s->intfit_message), "Not enough memory for fit."); return 0; }
    int n = collect_work(s, work);
    if (n < 2) {
        snprintf(s->intfit_message, sizeof(s->intfit_message), "No 2 positive assigned areas in the active trace.");
        free(work); return 0;
    }
    double temp = fitted_temperature(s, work, n);
    double scale = 1.0;
    int used = 0;
    double rms = evaluate_temperature(s, work, n, temp, &scale, &used);
    if (!isfinite(rms) || used < 2) {
        snprintf(s->intfit_message, sizeof(s->intfit_message), "Fit could not form two valid observations.");
        free(work); return 0;
    }
    /* Temperature and relative component ratios are coupled.  Alternate the
       two small one-dimensional fits a few times, which is stable here and
       avoids pretending that the first temperature is independent of mu. */
    for (int pass = 0; pass < 5; pass++) {
        if (!fit_relative_dipoles(s, work, n)) break;
        temp = fitted_temperature(s, work, n);
        rms = evaluate_temperature(s, work, n, temp, &scale, &used);
    }
    /* Report the total correction relative to the values that were present
       when Run fit was pressed, not merely the last iterative increment. */
    for (int c = 0; c < 3; c++)
        if (starting_red[c] != 0.0) s->intfit_component_scale[c] = s->dipole_red[c] / starting_red[c];

    s->intfit_has_result = 1;
    s->intfit_n_candidate = n;
    s->intfit_n_used = used;
    s->intfit_n_rejected = n - used;
    s->intfit_scale = scale;
    s->intfit_log_rms = rms;
    if (s->intfit_fit_temperature) s->rot_temp_k = temp;
    predfit_adopt_shared_state(s);
    for (int i = 0; i < s->n_assignments; i++) s->intfit_lines[i].valid = 0;
    for (int i = 0; i < n; i++) {
        IntFitLine *r = &s->intfit_lines[work[i].assignment_index];
        r->assignment_index = work[i].assignment_index;
        r->valid = 1;
        r->used = work[i].used;
        r->component = work[i].pred->mu;
        r->exp_area = work[i].exp_area;
        r->model_int = scale * work[i].model;
        r->ratio = r->model_int > 0.0 ? r->exp_area / r->model_int : NAN;
        r->residual_log = work[i].residual;
    }
    snprintf(s->intfit_message, sizeof(s->intfit_message), "%d/%d lines used; log RMS %.3g", used, n, rms);
    free(work);
    return 1;
}

int intensity_fit_export(const AppState *s, const char *filename) {
    if (!s || !s->intfit_has_result || !filename) return 0;
    FILE *fp = fopen(filename, "w");
    if (!fp) return 0;
    static const char comp[] = {'a', 'b', 'c'};
    fprintf(fp, "INTENSITY FIT (single species, relative intensities)\n");
    fprintf(fp, "Spectrum: %s\nCatalog: %s\n\n", s->exp_path, s->pred_path);
    fprintf(fp, "T cat / K: %.8g\nT rot / K: %.8g%s\n", s->cat_temp_k, s->rot_temp_k,
            s->intfit_fit_temperature ? "  (fitted)" : "  (fixed)");
    fprintf(fp, "Area half-window / MHz: %.8g\n", s->intfit_half_window_mhz);
    fprintf(fp, "Global scale: %.12g\nLog-intensity RMS: %.8g\n", s->intfit_scale, s->intfit_log_rms);
    fprintf(fp, "Lines: candidate=%d used=%d rejected=%d\n", s->intfit_n_candidate,
            s->intfit_n_used, s->intfit_n_rejected);
    if (s->intfit_reference_component >= 0)
        fprintf(fp, "Dipole reference: mu_%c (scale fixed to 1)\n", comp[s->intfit_reference_component]);
    fprintf(fp, "\ncomponent  selected  n_lines  fitted_mu_factor  mu_cat/D  mu_red/D\n");
    for (int c = 0; c < 3; c++)
        fprintf(fp, "mu_%c       %8s  %7d  %16.8g  %8.5g  %8.5g\n", comp[c],
                s->intfit_fit_dipole[c] ? "yes" : "no", s->intfit_component_n[c],
                s->intfit_component_scale[c], s->dipole_cat[c], s->dipole_red[c]);
    fprintf(fp, "\nuse comp PredFreq_MHz ExpFreq_MHz ExpArea CalcArea Exp/Calc logResidual QN(up;lower)\n");
    for (int i = 0; i < s->n_assignments; i++) {
        const IntFitLine *r = &s->intfit_lines[i];
        if (!r->valid) continue;
        const Assignment *a = &s->assignments[i];
        const PredLine *p = &a->pred;
        fprintf(fp, "%3s %4c %12.5f %12.5f %12.6e %12.6e %9.5f %11.6f  "
                    "%d %d %d %d %d %d; %d %d %d %d %d %d\n",
                r->used ? "yes" : "no", r->component, p->freq_mhz, a->exp_freq,
                r->exp_area, r->model_int, r->ratio, r->residual_log,
                p->Ju, p->Kau, p->Kcu, p->M1u, p->M2u, p->M3u,
                p->Jl, p->Kal, p->Kcl, p->M1l, p->M2l, p->M3l);
    }
    fclose(fp);
    return 1;
}
