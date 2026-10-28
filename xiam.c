#include "xiam.h"

#include "controller.h"
#include "layout.h"
#include "loader.h"
#include "predfit.h"
#include "settings.h"
#include "ui_chrome.h"
#include "ui_theme.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define XIAM_DIR_NAME ".xiam"
#define XIAM_SOURCE_MAX 255
/* The workspace mirrors Pred&Fit Advanced page for page, and sits on the same
 * chrome: predfit.c's ADV_* constants, repeated here because both windows are
 * meant to line up when they are open side by side. */
#define XIAM_TAB_COUNT 6
#define XIAM_PAD        18
#define XIAM_HEADER_H   42
#define XIAM_TAB_Y      48
#define XIAM_TAB_H      30
#define XIAM_CONTENT_Y  92
#define XIAM_FOOTER_H   46
#define XIAM_NAV_W     270
#define XIAM_MIN_W    1120
#define XIAM_MIN_H     620
/* Block V symmetries plus block VI states: a generous bound on what one
 * native input declares, active records and commented-out ones together. */
#define MAX_XIAM_STATE_ROWS 128

enum { XIAM_EDIT_NONE = -1, XIAM_EDIT_FMIN = -2, XIAM_EDIT_FMAX = -3,
       XIAM_EDIT_TEMP = -4, XIAM_EDIT_JMAX = -5,
       XIAM_EDIT_LINE_OBS = -6, XIAM_EDIT_LINE_ERR = -7,
       XIAM_EDIT_CONCENTRATION = -8, XIAM_EDIT_PARAM_NAME = -9,
       XIAM_EDIT_H_NAME = -10, XIAM_EDIT_CONTROL = -11,
       XIAM_EDIT_STATE_ROW = -12, XIAM_EDIT_LINE_DEFAULT = -13,
       XIAM_EDIT_STATE_W = -14 };

static int xiam_same_input_transition(const PredLine *a, const PredLine *b);
static int xiam_prefix_w(int font, const char *text, int n);

static int xiam_is_comment(const char *line) {
    while (line && isspace((unsigned char)*line)) line++;
    return !line || *line == '\\' || *line == '!' || *line == '\0';
}

static char *xiam_ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void xiam_lower(char *out, size_t n, const char *in) {
    if (!n) return;
    size_t i = 0;
    for (; in && in[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = '\0';
}

static int xiam_equal(const char *a, const char *b) {
    char aa[64], bb[64];
    xiam_lower(aa, sizeof(aa), a); xiam_lower(bb, sizeof(bb), b);
    return strcmp(aa, bb) == 0;
}

static double xiam_number(const char *s, char **end) {
    char work[128];
    size_t n = 0;
    while (s && s[n] && !isspace((unsigned char)s[n]) && n + 1 < sizeof(work)) {
        work[n] = (s[n] == 'd' || s[n] == 'D') ? 'E' : s[n]; n++;
    }
    work[n] = '\0';
    char *tail = NULL;
    double v = strtod(work, &tail);
    if (end) *end = (char *)(s + (tail ? (tail - work) : 0));
    return v;
}

static int xiam_parse_int(const char *s, char **end, int *value) {
    char *tail = NULL;
    long v = strtol(s, &tail, 10);
    if (tail == s || v < -1000000000L || v > 1000000000L) return 0;
    if (end) *end = tail;
    if (value) *value = (int)v;
    return 1;
}

static int xiam_is_quadrupole(const char *name) {
    char low[64]; xiam_lower(low, sizeof(low), name);
    return strncmp(low, "chi", 3) == 0;
}

static int xiam_is_frequency(const char *name) {
    char low[64]; xiam_lower(low, sizeof(low), name);
    if (strncmp(low, "mu_", 3) == 0) return 0;
    return strcmp(low, "rho") != 0 && strncmp(low, "rho_", 4) != 0 &&
           strcmp(low, "beta") != 0 && strncmp(low, "beta_", 5) != 0 &&
           strcmp(low, "gamma") != 0 && strncmp(low, "gamma_", 6) != 0 &&
           strcmp(low, "delta") != 0 && strncmp(low, "delta_", 6) != 0 &&
           strcmp(low, "epsil") != 0 && strncmp(low, "epsil_", 6) != 0;
}

/* Distinct from Pred&Fit's state palette: in a mixed plot the eye has to
 * separate a native XIAM model from a Pickett state at a glance. */
static const SDL_Color XIAM_TRACE_PALETTE[8] = {
    { 96, 186, 240, 235}, {242, 158,  74, 235}, {132, 214, 150, 235}, {224, 126, 196, 235},
    {240, 214, 104, 235}, {150, 160, 246, 235}, {238, 122, 118, 235}, {118, 216, 214, 235}
};

/* mu_x / mu_y / mu_z, in XIAM's own order. */
static const char *const XIAM_DIPOLE[3] = {"mu_x", "mu_y", "mu_z"};

static double xiam_display_value(const XiamParameter *p) {
    if (!p || !p->is_frequency) return p ? p->value_ghz : 0.0;
    return p->value_ghz * 1000.0 / (p->is_quadrupole ? 2.0 : 1.0);
}

static double xiam_display_error(const XiamParameter *p) {
    if (!p || !p->is_frequency) return p ? p->error_ghz : 0.0;
    return p->error_ghz * 1000.0 / (p->is_quadrupole ? 2.0 : 1.0);
}

static double xiam_source_value(const XiamParameter *p, double shown) {
    if (!p || !p->is_frequency) return shown;
    return shown * (p->is_quadrupole ? 2.0 : 1.0) / 1000.0;
}

/* XIAM's x/y/z are principal inertial axes, not immutable A/B/C labels.
 * Present the physical alias beside the native keyword but always write the
 * native keyword back to the .xi file.  This avoids the particularly easy
 * mistake of copying prolate quadrupole components into an oblate model. */
static void xiam_parameter_label(const XiamState *x, const XiamParameter *p, char *out, size_t size) {
    const char *alias = NULL;
    char low[64]; xiam_lower(low, sizeof(low), p->name);
    if (x->axis_convention == 1) { /* x/y/z = b/c/a */
        if (!strncmp(low, "mu_x", 4)) alias = "mu_b";
        else if (!strncmp(low, "mu_y", 4)) alias = "mu_c";
        else if (!strncmp(low, "mu_z", 4)) alias = "mu_a";
        else if (strstr(low, "_z")) alias = "aa";
        else if (strstr(low, "_-") || strstr(low, "_minus")) alias = "bb-cc";
        else if (strstr(low, "_xy")) alias = "bc";
        else if (strstr(low, "_xz")) alias = "ab";
        else if (strstr(low, "_yz")) alias = "ac";
    } else if (x->axis_convention == 2) { /* x/y/z = a/b/c */
        if (!strncmp(low, "mu_x", 4)) alias = "mu_a";
        else if (!strncmp(low, "mu_y", 4)) alias = "mu_b";
        else if (!strncmp(low, "mu_z", 4)) alias = "mu_c";
        else if (strstr(low, "_z")) alias = "cc";
        else if (strstr(low, "_-") || strstr(low, "_minus")) alias = "aa-bb";
        else if (strstr(low, "_xy")) alias = "ab";
        else if (strstr(low, "_xz")) alias = "ac";
        else if (strstr(low, "_yz")) alias = "bc";
    }
    if (alias && (p->is_quadrupole || !strncmp(low, "mu_", 3)))
        snprintf(out, size, "%s  -> %s", p->name, alias);
    else snprintf(out, size, "%s", p->name);
}

static int xiam_known_control(const char *name) {
    static const char *keys[] = {
        "ncyc", "nzyk", "ncycl", "ints", "ntop", "print", "reduc", "freq", "freq_l", "freq_h",
        "temp", "eval", "dfreq", "nfold", "limit", "aprint", "xprint", "spin", "spin2", "maxm",
        "maxvm", "convg", "woods", "ndata", "adjf", "rofit", "defer", "eps", "weigf", "lambda",
        "fitscl", "svderr", "orger", "j", "v", "s", "f", "f1", "b", "k", "t", "fit", "dqu", "dqx"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (xiam_equal(name, keys[i])) return 1;
    return 0;
}

static int xiam_find_parameter(const XiamState *x, const char *name) {
    /* Native XIAM identifiers are case-sensitive: DJ/dj and DK/dk are
       distinct centrifugal-distortion constants.  A case-insensitive lookup
       merged those rows, rewrote the wrong molecular value and emitted two
       fit directives for one uppercase parameter on the next run. */
    for (int i = 0; x && i < x->n_params; i++)
        if (!strcmp(x->params[i].name, name)) return i;
    /* XIAM's report may print the explicit first-top suffix while a supplied
       input spells the same native parameter without it.  This is the only
       safe normalisation: preserve letter case for the actual keyword. */
    if (name) {
        size_t n = strlen(name);
        if (n > 2 && name[n - 2] == '_' && name[n - 1] == '1') {
            for (int i = 0; x && i < x->n_params; i++)
                if (strlen(x->params[i].name) == n - 2 &&
                    !strncmp(x->params[i].name, name, n - 2)) return i;
        }
    }
    return -1;
}

/* Pickett owns positive Hamiltonian ids.  A stable negative id means a saved
 * XIAM assignment returns to its own .xi model, rather than becoming part of
 * whatever XIAM input was opened most recently. */
static int xiam_model_id_from_path(const char *path) {
    unsigned int h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(path ? path : ""); *p; p++) {
        h ^= *p; h *= 16777619u;
    }
    return -1 - (int)(h % 1000000000u);
}

static const char *xiam_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path && path[0] ? path : "XIAM model");
}

/* Releases before the current `fit.xi`/`prediction.xi` workspace used a
 * mutable `.xiam/.../model.xi` as input.  It was overwritten after
 * calculations and could retain an aborted fit's constants; it is therefore
 * never a model a user can safely reopen as a source .xi. */
static int xiam_is_legacy_workspace_input(const char *path) {
    if (!path) return 0;
    const char *base = xiam_basename(path);
    if (strcmp(base, "model.xi")) return 0;
    return !strncmp(path, ".xiam/", 6) || strstr(path, "/.xiam/") != NULL;
}

/* The UI edits an active projection, exactly like the Pickett project does.
 * Keeping the native source beside every projection is what lets two .xi
 * models retain different control cards, fit directives and Block VIII data. */
static void xiam_store_active(XiamState *x) {
    if (!x || x->active_hamiltonian < 0 || x->active_hamiltonian >= x->n_hamiltonians) return;
    XiamHamiltonian *h = &x->hamiltonian[x->active_hamiltonian];
    h->simulate_excluded = x->simulate_excluded; h->loaded = x->loaded;
    snprintf(h->input_path, sizeof(h->input_path), "%s", x->input_path);
    snprintf(h->output_path, sizeof(h->output_path), "%s", x->output_path);
    snprintf(h->fit_output_path, sizeof(h->fit_output_path), "%s", x->fit_output_path);
    snprintf(h->work_dir, sizeof(h->work_dir), "%s", x->work_dir);
    memcpy(h->source_lines, x->source_lines, sizeof(h->source_lines)); h->n_source_lines = x->n_source_lines;
    memcpy(h->params, x->params, sizeof(h->params)); h->n_params = x->n_params;
    h->spin = x->spin; h->spin2 = x->spin2; h->model_id = x->model_id; h->max_j = x->max_j;
    h->fmin_mhz = x->fmin_mhz; h->fmax_mhz = x->fmax_mhz; h->temperature_k = x->temperature_k;
    h->concentration = x->concentration;
    h->line_error_mhz = x->line_error_mhz;
    for (int k = 0; k < 3; k++) h->mu_excluded[k] = x->mu_excluded[k];
    h->has_freq = x->has_freq; h->has_ints = x->has_ints; h->has_j = x->has_j;
    h->axis_convention = x->axis_convention; h->axis_auto_convention = x->axis_auto_convention;
    h->axis_override = x->axis_override;
    snprintf(h->status, sizeof(h->status), "%s", x->status);
    snprintf(h->diagnostics, sizeof(h->diagnostics), "%s", x->diagnostics);
}

static void xiam_refresh_fit_summary(XiamState *x);

static void xiam_restore_active(XiamState *x, int index) {
    if (!x || index < 0 || index >= x->n_hamiltonians) return;
    const XiamHamiltonian *h = &x->hamiltonian[index];
    x->active_hamiltonian = index; x->simulate_excluded = h->simulate_excluded; x->loaded = h->loaded;
    snprintf(x->input_path, sizeof(x->input_path), "%s", h->input_path);
    snprintf(x->output_path, sizeof(x->output_path), "%s", h->output_path);
    snprintf(x->fit_output_path, sizeof(x->fit_output_path), "%s", h->fit_output_path);
    snprintf(x->work_dir, sizeof(x->work_dir), "%s", h->work_dir);
    memcpy(x->source_lines, h->source_lines, sizeof(x->source_lines)); x->n_source_lines = h->n_source_lines;
    memcpy(x->params, h->params, sizeof(x->params)); x->n_params = h->n_params;
    x->spin = h->spin; x->spin2 = h->spin2; x->model_id = h->model_id; x->max_j = h->max_j;
    x->fmin_mhz = h->fmin_mhz; x->fmax_mhz = h->fmax_mhz; x->temperature_k = h->temperature_k;
    x->concentration = h->concentration;
    x->line_error_mhz = h->line_error_mhz > 0.0 ? h->line_error_mhz : 0.010;
    for (int k = 0; k < 3; k++) x->mu_excluded[k] = h->mu_excluded[k];
    x->has_freq = h->has_freq; x->has_ints = h->has_ints; x->has_j = h->has_j;
    x->axis_convention = h->axis_convention; x->axis_auto_convention = h->axis_auto_convention;
    x->axis_override = h->axis_override;
    snprintf(x->status, sizeof(x->status), "%s", h->status);
    snprintf(x->diagnostics, sizeof(x->diagnostics), "%s", h->diagnostics);
    x->params_scroll = x->lines_scroll = x->assignments_scroll = x->fitting_scroll = x->fit_params_scroll = 0;
    xiam_refresh_fit_summary(x);
}

static void xiam_clear_active(XiamState *x) {
    if (!x) return;
    x->loaded = 0; x->running = 0; x->simulate_excluded = 0;
    x->input_path[0] = x->output_path[0] = x->fit_output_path[0] = x->work_dir[0] = '\0';
    x->n_source_lines = x->n_params = 0; x->spin = x->spin2 = 0; x->model_id = 0;
    x->max_j = 30; x->fmin_mhz = 0.0; x->fmax_mhz = 10000.0; x->temperature_k = 1.0;
    x->concentration = 1.0; x->line_error_mhz = 0.010;
    x->mu_excluded[0] = x->mu_excluded[1] = x->mu_excluded[2] = 0;
    x->has_freq = x->has_ints = x->has_j = 0;
    x->axis_convention = x->axis_auto_convention = x->axis_override = 0;
    x->status[0] = x->diagnostics[0] = '\0';
    x->has_fit_summary = 0; x->fit_std_mhz = 0.0; x->fit_iterations = 0;
}

static int xiam_select_hamiltonian(XiamState *x, int index) {
    if (!x || index < 0 || index >= x->n_hamiltonians) return 0;
    xiam_store_active(x); xiam_restore_active(x, index); return 1;
}

static void xiam_mark_session_dirty(AppState *s) {
    if (!s) return;
    s->xiam.session_dirty = 1;
    s->predfit.session_dirty = 1;
}

double xiam_hamiltonian_concentration(const AppState *s, int hamiltonian_id) {
    if (!s) return 1.0;
    const XiamState *x = &s->xiam;
    for (int i = 0; i < x->n_hamiltonians; i++)
        if (x->hamiltonian[i].model_id == hamiltonian_id) {
            double c = x->hamiltonian[i].concentration;
            return isfinite(c) && c >= 0.0 ? c : 1.0;
        }
    return 1.0;
}

void xiam_apply_concentrations(AppState *s) {
    if (!s || !s->pred_lines || s->n_pred <= 0) return;
    double top = 0.0;
    for (int i = 0; i < s->n_pred; i++) {
        PredLine *p = &s->pred_lines[i];
        if (p->backend == PRED_BACKEND_XIAM) {
            double base = isfinite(p->cat_lgint) ? pow(10.0, p->cat_lgint) : 0.0;
            p->line_strength = 0.0;
            p->linear_int = base * xiam_hamiltonian_concentration(s, p->hamiltonian_id);
            p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
        }
        if (p->linear_int > top) top = p->linear_int;
    }
    s->pred_global_max = top > 0.0 ? top : 1.0;
}

SDL_Color xiam_trace_color(const AppState *s, int hamiltonian_id) {
    if (s) {
        const XiamState *x = &s->xiam;
        for (int i = 0; i < x->n_hamiltonians; i++)
            if (x->hamiltonian[i].model_id == hamiltonian_id && x->hamiltonian[i].trace_color.a > 0)
                return x->hamiltonian[i].trace_color;
    }
    return XIAM_TRACE_PALETTE[0];
}

int xiam_plot_traces(const AppState *s, PredfitSpeciesTrace *out, int max) {
    if (!s || !out || max <= 0) return 0;
    const XiamState *x = &s->xiam;
    int n = 0;
    for (int i = 0; i < x->n_hamiltonians && n < max; i++) {
        const XiamHamiltonian *h = &x->hamiltonian[i];
        if (h->simulate_excluded || !h->loaded) continue;
        /* Only a model that actually has rows in the plot gets a trace. */
        int rows = 0;
        for (int k = 0; k < s->n_pred && !rows; k++)
            if (s->pred_lines[k].backend == PRED_BACKEND_XIAM &&
                s->pred_lines[k].hamiltonian_id == h->model_id) rows = 1;
        if (!rows) continue;
        out[n].hamiltonian_id = h->model_id;
        out[n].state_index = 0;
        out[n].any_state = 1;
        out[n].color = h->trace_color.a > 0 ? h->trace_color : XIAM_TRACE_PALETTE[i % 8];
        snprintf(out[n].label, sizeof(out[n].label), "%s", h->name);
        n++;
    }
    return n;
}

int xiam_set_hamiltonian_concentration(AppState *s, int hamiltonian_id,
                                       double concentration) {
    if (!s || !isfinite(concentration) || concentration < 0.0) return 0;
    XiamState *x = &s->xiam;
    for (int i = 0; i < x->n_hamiltonians; i++) {
        if (x->hamiltonian[i].model_id != hamiltonian_id) continue;
        x->hamiltonian[i].concentration = concentration;
        if (i == x->active_hamiltonian) x->concentration = concentration;
        xiam_store_active(x);
        xiam_apply_concentrations(s);
        s->intensity_window.species_sync_pending = 1;
        xiam_mark_session_dirty(s);
        return 1;
    }
    return 0;
}

static int xiam_find_hamiltonian_path(const XiamState *x, const char *path) {
    if (!x || !path) return -1;
    for (int i = 0; i < x->n_hamiltonians; i++)
        if (!strcmp(x->hamiltonian[i].input_path, path)) return i;
    return -1;
}

static void xiam_assign_default_trace_color(XiamState *x, XiamHamiltonian *h) {
    h->trace_color = XIAM_TRACE_PALETTE[(x->trace_color_next++) % 8];
}

static void xiam_register_active(XiamState *x) {
    if (!x || x->n_hamiltonians >= MAX_XIAM_HAMILTONIANS) return;
    int index = x->n_hamiltonians++;
    memset(&x->hamiltonian[index], 0, sizeof(x->hamiltonian[index]));
    snprintf(x->hamiltonian[index].name, sizeof(x->hamiltonian[index].name), "%s", xiam_basename(x->input_path));
    xiam_assign_default_trace_color(x, &x->hamiltonian[index]);
    x->active_hamiltonian = index; xiam_store_active(x);
}

static void xiam_update_axis_convention(XiamState *x) {
    int bj = xiam_find_parameter(x, "BJ"), bk = xiam_find_parameter(x, "BK"), bm = xiam_find_parameter(x, "B-");
    x->axis_auto_convention = 0;
    if (bj < 0 || bk < 0 || bm < 0) {
        x->axis_convention = x->axis_override;
        return;
    }
    double bx = x->params[bj].value_ghz + x->params[bm].value_ghz;
    double by = x->params[bj].value_ghz - x->params[bm].value_ghz;
    double bz = x->params[bj].value_ghz + x->params[bk].value_ghz;
    if (bz >= bx && bz >= by) x->axis_auto_convention = 1; /* z is a, x/y are b/c */
    else if (bz <= bx && bz <= by) x->axis_auto_convention = 2; /* z is c, x/y are a/b */
    x->axis_convention = x->axis_override ? x->axis_override : x->axis_auto_convention;
}

static void xiam_parse_source(AppState *s) {
    XiamState *x = &s->xiam;
    x->n_params = 0; x->spin = x->spin2 = 0; x->has_freq = x->has_ints = x->has_j = 0;
    int block = 0;
    for (int line_no = 0; line_no < x->n_source_lines; line_no++) {
        char work[256], lower[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[line_no]);
        char *at = xiam_ltrim(work);
        xiam_lower(lower, sizeof(lower), at);
        if (xiam_is_comment(at)) {
            /* Roman numeral headings share prefixes: VIII starts with VII,
               which starts with VI and V.  Test the longest strings first
               so later Block VIII data can never be mistaken for Block V. */
            if (strstr(lower, "block viii")) block = 8;
            else if (strstr(lower, "block vii")) block = 7;
            else if (strstr(lower, "block vi")) block = 6;
            else if (strstr(lower, "block v")) block = 5;
            else if (strstr(lower, "block iv")) block = 4;
            else if (strstr(lower, "block iii")) block = 3;
            continue;
        }
        char key[64] = "", *tail = at;
        if (sscanf(at, "%63s", key) != 1) continue;
        while (*tail && !isspace((unsigned char)*tail)) tail++;
        tail = xiam_ltrim(tail);
        if (xiam_equal(key, "freq")) {
            char *e = NULL; x->fmin_mhz = xiam_number(tail, &e) * 1000.0;
            x->fmax_mhz = xiam_number(xiam_ltrim(e), NULL) * 1000.0; x->has_freq = 1; continue;
        }
        if (xiam_equal(key, "freq_l")) { x->fmin_mhz = xiam_number(tail, NULL) * 1000.0; x->has_freq = 1; continue; }
        if (xiam_equal(key, "freq_h")) { x->fmax_mhz = xiam_number(tail, NULL) * 1000.0; x->has_freq = 1; continue; }
        if (xiam_equal(key, "temp")) { x->temperature_k = xiam_number(tail, NULL); continue; }
        if (xiam_equal(key, "ints")) { x->has_ints = 1; continue; }
        if (xiam_equal(key, "j")) { x->max_j = (int)lround(xiam_number(tail, NULL)); x->has_j = 1; continue; }
        if (xiam_equal(key, "spin")) { x->spin = (int)lround(xiam_number(tail, NULL)); continue; }
        if (xiam_equal(key, "spin2")) { x->spin2 = (int)lround(xiam_number(tail, NULL)); continue; }
        if (block == 4 && (xiam_equal(key, "fit") || xiam_equal(key, "dqu") || xiam_equal(key, "dqx"))) {
            char pname[64] = "", rest[64] = ""; int words = sscanf(tail, "%63s %63s", pname, rest);
            if (words == 1 || (words == 2 && rest[0] == '!')) {
                int pi = xiam_find_parameter(x, pname);
                if (pi >= 0) {
                    x->params[pi].fit_method = xiam_equal(key, "fit") ? 1 : xiam_equal(key, "dqu") ? 2 : 3;
                    x->params[pi].fit_source_line = line_no;
                }
            }
            continue;
        }
        if (block != 3 || xiam_known_control(key) || x->n_params >= MAX_XIAM_PARAMS) continue;
        char *end = NULL; double value = xiam_number(tail, &end);
        if (end == tail) continue;
        int existing = xiam_find_parameter(x, key);
        XiamParameter *p = existing >= 0 ? &x->params[existing] : &x->params[x->n_params++];
        if (existing < 0) {
            memset(p, 0, sizeof(*p));
            snprintf(p->name, sizeof(p->name), "%s", key);
            p->fit_source_line = -1;
            p->is_quadrupole = xiam_is_quadrupole(key); p->is_frequency = xiam_is_frequency(key);
        }
        /* XIAM reads the later declaration; editing therefore changes exactly
           that declaration while retaining an earlier historical/commented
           value verbatim. */
        p->value_ghz = value; p->source_line = line_no;
    }
    if (!(x->temperature_k > 0.0)) x->temperature_k = 1.0;
    if (!x->has_freq || !(x->fmax_mhz > x->fmin_mhz)) { x->fmin_mhz = 0.0; x->fmax_mhz = 10000.0; }
    if (!x->has_j) x->max_j = 30;
    xiam_update_axis_convention(x);
}

static int xiam_read_source(XiamState *x, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    x->n_source_lines = 0;
    char raw[1024];
    while (fgets(raw, sizeof(raw), fp) && x->n_source_lines < MAX_XIAM_SOURCE_LINES) {
        for (char *p = raw; *p; p++) if (*p == '\0') *p = ' ';
        raw[strcspn(raw, "\r\n")] = '\0';
        snprintf(x->source_lines[x->n_source_lines++], sizeof(x->source_lines[0]), "%s", raw);
    }
    int good = !ferror(fp); fclose(fp); return good;
}

static int xiam_cmp_line(const void *aa, const void *bb) {
    const PredLine *a = aa, *b = bb;
    return a->freq_mhz < b->freq_mhz ? -1 : a->freq_mhz > b->freq_mhz ? 1 : 0;
}

static void xiam_branch_mu(PredLine *p) {
    int dj = p->Ju - p->Jl;
    p->branch = dj == 1 ? 'R' : dj == 0 ? 'Q' : dj == -1 ? 'P' : '?';
    int dka = abs(p->Kau - p->Kal) & 1, dkc = abs(p->Kcu - p->Kcl) & 1;
    p->mu = !dka && dkc ? 'a' : dka && dkc ? 'b' : dka && !dkc ? 'c' : '?';
}

static int xiam_parse_output_line(const char *line, PredLine *out) {
    char work[1024], *v[96]; int n = 0;
    snprintf(work, sizeof(work), "%s", line);
    for (char *tok = strtok(work, " \t\r\n"); tok && n < 96; tok = strtok(NULL, " \t\r\n")) v[n++] = tok;
    if (n < 10) return 0;
    int q[6];
    for (int i = 0; i < 6; i++) if (!xiam_parse_int(v[i], NULL, &q[i])) return 0;
    int is = -1, iv = -1, ib = -1, if1 = -1, iff = -1, ik = -1, it = -1;
    for (int i = 6; i < n; i++) {
        if (xiam_equal(v[i], "S")) is = i;
        else if (xiam_equal(v[i], "V")) iv = i;
        else if (xiam_equal(v[i], "B")) ib = i;
        else if (xiam_equal(v[i], "F1")) if1 = i;
        else if (xiam_equal(v[i], "F")) iff = i;
        else if (xiam_equal(v[i], "K")) ik = i;
        else if (xiam_equal(v[i], "t")) it = i;
    }
    if (is < 0 || iv < 0 || ib < 3 || iv + 1 >= n || ib - 1 < 0) return 0;
    int su = 0, vv = 0; double ghz = 0.0, lg = 0.0;
    if (!xiam_parse_int(v[is + 1], NULL, &su) || !xiam_parse_int(v[iv + 1], NULL, &vv)) return 0;
    ghz = xiam_number(v[iv + 2], NULL); lg = xiam_number(v[ib - 1], NULL);
    if (!isfinite(ghz) || !isfinite(lg)) return 0;
    memset(out, 0, sizeof(*out));
    out->freq_mhz = ghz * 1000.0; out->lgint = out->cat_lgint = lg;
    out->linear_int = isfinite(lg) && lg > -300.0 ? pow(10.0, lg) : 0.0;
    out->backend = PRED_BACKEND_XIAM; out->intensity_native = 1; out->hamiltonian_id = -1;
    out->Ju = q[0]; out->Kau = q[1]; out->Kcu = q[2]; out->Jl = q[3]; out->Kal = q[4]; out->Kcl = q[5];
    out->xiam.present = 1; out->xiam.su = out->xiam.sl = su; out->xiam.vu = out->xiam.vl = vv;
    if (if1 >= 0 && if1 + 2 < n) { xiam_parse_int(v[if1 + 1], NULL, &out->xiam.f1u); xiam_parse_int(v[if1 + 2], NULL, &out->xiam.f1l); }
    if (iff >= 0 && iff + 2 < n) { xiam_parse_int(v[iff + 1], NULL, &out->xiam.fu); xiam_parse_int(v[iff + 2], NULL, &out->xiam.fl); }
    if (ib + 1 < n) { xiam_parse_int(v[ib + 1], NULL, &out->xiam.bu); out->xiam.bl = out->xiam.bu; }
    if (ik >= 0 && ik + 2 < n) { xiam_parse_int(v[ik + 1], NULL, &out->xiam.ku); xiam_parse_int(v[ik + 2], NULL, &out->xiam.kl); }
    if (it >= 0 && it + 2 < n) { xiam_parse_int(v[it + 1], NULL, &out->xiam.tu); xiam_parse_int(v[it + 2], NULL, &out->xiam.tl); }
    if (if1 >= 0 || iff >= 0) {
        out->n_qn = 6; out->M1u = out->xiam.f1u; out->M1l = out->xiam.f1l; out->M2u = out->xiam.fu; out->M2l = out->xiam.fl; out->M3u = su; out->M3l = su;
    } else {
        out->n_qn = 4; out->M1u = out->M1l = su;
    }
    xiam_branch_mu(out); return 1;
}

int xiam_same_transition(const PredLine *a, const PredLine *b) {
    if (!a || !b) return 0;
    if (a->backend != PRED_BACKEND_XIAM || b->backend != PRED_BACKEND_XIAM ||
        !a->xiam.present || !b->xiam.present) return 0;
    return a->Ju == b->Ju && a->Kau == b->Kau && a->Kcu == b->Kcu &&
           a->Jl == b->Jl && a->Kal == b->Kal && a->Kcl == b->Kcl &&
           memcmp(&a->xiam, &b->xiam, sizeof(a->xiam)) == 0;
}

static int xiam_read_output_records(const char *path, int model_id, PredLine **out, int *count, double *lo, double *hi, double *top) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    PredLine *rows = NULL; int n = 0, cap = 0; char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        PredLine p; if (!xiam_parse_output_line(line, &p)) continue;
        if (n == cap) { int next = cap ? cap * 2 : 2048; PredLine *grown = realloc(rows, (size_t)next * sizeof(*rows)); if (!grown) { free(rows); fclose(fp); return 0; } rows = grown; cap = next; }
        p.hamiltonian_id = model_id;
        rows[n++] = p;
    }
    fclose(fp);
    if (!n) { free(rows); return 0; }
    qsort(rows, (size_t)n, sizeof(*rows), xiam_cmp_line);
    *lo = rows[0].freq_mhz; *hi = rows[n - 1].freq_mhz; *top = 0.0;
    for (int i = 0; i < n; i++) if (rows[i].linear_int > *top) *top = rows[i].linear_int;
    *out = rows; *count = n; return 1;
}

static void xiam_adopt_prediction(AppState *s, PredLine *rows, int n, double lo, double hi, double top, const char *path) {
    int model_id = n ? rows[0].hamiltonian_id : 0;
    /* A post-fit prediction replaces calculated frequencies and native
       intensities in the existing assignment rows, never their observations
       or the user's Fit inclusion choice. */
    for (int a = 0; a < s->n_assignments; a++) {
        Assignment *old = &s->assignments[a];
        if (old->pred.backend != PRED_BACKEND_XIAM) continue;
        for (int i = 0; i < n; i++) {
            if (rows[i].hamiltonian_id != old->hamiltonian_id ||
                (!xiam_same_transition(&rows[i], &old->pred) &&
                 !xiam_same_input_transition(&rows[i], &old->pred))) continue;
            old->pred = rows[i]; old->needs_reassign = 0; break;
        }
    }
    /* A XIAM project may contain several separate .xi Hamiltonians.  Refresh
       only this model's catalogue: dropping the complete global prediction
       here was what made a second XIAM calculation silently erase the first
       (and would also prevent the later XIAM + Pickett overlay). */
    int kept = 0;
    for (int i = 0; i < s->n_pred; i++)
        if (s->pred_lines[i].backend != PRED_BACKEND_XIAM ||
            s->pred_lines[i].hamiltonian_id != model_id) kept++;
    PredLine *merged = malloc((size_t)(kept + n) * sizeof(*merged));
    if (!merged) { free(rows); snprintf(s->xiam.status, sizeof(s->xiam.status), "Out of memory while merging XIAM predictions."); return; }
    int at = 0;
    for (int i = 0; i < s->n_pred; i++)
        if (s->pred_lines[i].backend != PRED_BACKEND_XIAM ||
            s->pred_lines[i].hamiltonian_id != model_id) merged[at++] = s->pred_lines[i];
    memcpy(merged + at, rows, (size_t)n * sizeof(*rows)); at += n;
    free(rows); free(s->pred_lines);
    s->pred_lines = merged; s->n_pred = at;
    qsort(s->pred_lines, (size_t)s->n_pred, sizeof(*s->pred_lines), xiam_cmp_line);
    lo = s->pred_lines[0].freq_mhz; hi = s->pred_lines[s->n_pred - 1].freq_mhz;
    (void)top;
    xiam_apply_concentrations(s);
    s->n_selected = 0; s->pxmin = lo; s->pxmax = hi;
    /* A mixed prediction keeps Pred&Fit's display controls authoritative for
       its own rows.  XIAM native intensities do not have a CAT temperature
       that can be rescaled, so changing these shared fields after adding one
       would be misleading and can restyle the Pickett side. */
    if (!s->predfit.generated_catalog_active) {
        s->cat_temp_k = s->xiam.temperature_k;
        s->rot_temp_k = s->xiam.temperature_k;
    }
    snprintf(s->pred_path, sizeof(s->pred_path), "%s", path ? path : "XIAM .xo");
    int first = !s->data_loaded; s->data_loaded = 1;
    if (first) { s->xmin=lo; s->xmax=hi; s->vxmin=lo; s->vxmax=hi; s->pvxmin=lo; s->pvxmax=hi; s->ymin=s->vymin=0.0; s->ymax=s->vymax=1.0; s->bar_x=s->pbar_x=(lo+hi)*0.5; }
    snprintf(s->status_message, sizeof(s->status_message), "XIAM: loaded %d native prediction lines (MHz; SPCAT intensity retained).", n);
    s->error_message[0] = '\0';
}

/* `fitting` selects a second, more reliable failure signal on top of the
 * literal error text below.  XIAM-2NQ's Fortran source guards dozens of
 * numerical/input conditions with a bare `stop '<message>'` (e.g. the SVD
 * going singular mid-fit: "SVDSYDC: Error in hdiag (sydsycd)"), and a bare
 * Fortran STOP exits with status 0 — invisible to the caller's exit-code
 * check.  Enumerating every one of those messages is a losing game, and a
 * blanket "contains the word error" match false-positives on legitimate
 * output ("Errors of fitted linear combinations" prints on every successful
 * fit).  What a completed fit always prints, and a fit that stopped early
 * never does, is its closing "Parameters and Errors" report — so for a
 * fitting run, never seeing that report is treated as fatal regardless of
 * which of XIAM's own guards produced it. */
static int xiam_fatal_output(const char *path, char *diag, size_t diag_size, int fitting) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 1;
    char line[512]; int fatal = 0, reached_report = 0; size_t used = 0;
    if (diag_size) diag[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char low[512]; xiam_lower(low, sizeof(low), line);
        if (strstr(low, "input error") || strstr(low, "fatal error") || strstr(low, "segmentation")) fatal = 1;
        if (strstr(line, "Parameters") && strstr(line, "Errors")) reached_report = 1;
        if (fatal && used + strlen(line) + 1 < diag_size) { strcpy(diag + used, line); used += strlen(line); }
    }
    fclose(fp);
    if (fitting && !fatal && !reached_report) {
        fatal = 1;
        snprintf(diag, diag_size,
                 "XIAM stopped before reporting fitted parameters (likely a numerical "
                 "failure such as a singular fit matrix); see the retained fit.xo.");
    }
    return fatal;
}

static void xiam_replace_number(char *out, size_t size, const char *line, double value) {
    const char *at = line; while (*at && isspace((unsigned char)*at)) at++;
    while (*at && !isspace((unsigned char)*at)) at++;
    while (*at && isspace((unsigned char)*at)) at++;
    const char *number = at; char *end = NULL; (void)xiam_number(number, &end);
    if (end == number) { snprintf(out, size, "%s", line); return; }
    size_t prefix = (size_t)(number - line); if (prefix >= size) prefix = size - 1;
    snprintf(out, size, "%.*s%.12g%s", (int)prefix, line, value, end);
}

/* Rewriting only the leading keyword keeps the native spacing and any
 * trailing `!` comment of a declaration the user renames. */
static void xiam_replace_keyword(char *out, size_t size, const char *line, const char *name) {
    const char *at = line; while (*at && isspace((unsigned char)*at)) at++;
    size_t indent = (size_t)(at - line);
    const char *tail = at; while (*tail && !isspace((unsigned char)*tail)) tail++;
    snprintf(out, size, "%.*s%s%s", (int)indent, line, name, tail);
}

/* Insert a native declaration into the retained source text.  Every stored
 * line index has to move with it: xiam_write_input finds a parameter's
 * declaration by its source_line, so a stale index would rewrite the wrong
 * line of the generated .xi. */
static int xiam_source_insert_line(XiamState *x, int at, const char *text) {
    if (!x || at < 0 || at > x->n_source_lines ||
        x->n_source_lines >= MAX_XIAM_SOURCE_LINES) return -1;
    for (int i = x->n_source_lines; i > at; i--)
        memcpy(x->source_lines[i], x->source_lines[i - 1], sizeof(x->source_lines[0]));
    snprintf(x->source_lines[at], sizeof(x->source_lines[0]), "%s", text);
    x->n_source_lines++;
    for (int i = 0; i < x->n_params; i++) {
        if (x->params[i].source_line >= at) x->params[i].source_line++;
        if (x->params[i].fit_source_line >= at) x->params[i].fit_source_line++;
    }
    return at;
}

/* Where a new record of a native block belongs: after the last line that
 * block already holds.  Appending at the end of the file would land in block
 * VIII, and XIAM would read a constant there as a malformed transition. */
static int xiam_block_end(const XiamState *x, int wanted) {
    int block = 0, tail = -1;
    for (int i = 0; x && i < x->n_source_lines; i++) {
        char work[256], lower[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[i]);
        char *at = xiam_ltrim(work);
        xiam_lower(lower, sizeof(lower), at);
        if (xiam_is_comment(at)) {
            /* XIAM itself ends a block at the blank line following its
               content (xiam_write_input relies on the same rule for block
               II).  The decorative "\----" separator that introduces the
               next block's heading is not part of the current block, even
               though it is a non-blank comment: stop here, before it, so a
               newly inserted line lands inside the wanted block rather than
               after XIAM's own terminator. */
            if (!*at) {
                if (block == wanted && tail >= 0) break;
                continue;
            }
            /* Longest Roman numeral first: II is a prefix of III, VII of VIII. */
            int heading = strstr(lower, "block viii") ? 8 : strstr(lower, "block vii") ? 7 :
                          strstr(lower, "block vi") ? 6 : strstr(lower, "block v") ? 5 :
                          strstr(lower, "block iv") ? 4 : strstr(lower, "block iii") ? 3 :
                          strstr(lower, "block ii") ? 2 : strstr(lower, "block i") ? 1 : 0;
            if (heading) {
                block = heading;
                if (block == wanted) tail = i;
                else if (tail >= 0) break;     /* the wanted block has ended */
                continue;
            }
            if (block == wanted) tail = i;
            continue;
        }
        if (block == wanted) tail = i;
    }
    return tail < 0 ? -1 : tail + 1;
}

/* Block III in the native XIAM-2NQ (Sven Herbers) source's own parstr table:
 * rotation, D-type CD, H-type CD, quadrupole, top-top, spin-rotation,
 * dipole, then per-top internal rotation.  A constant named after XIAM starts
 * unranked ("NEW"); it only snaps into this order once it is renamed to a
 * keyword XIAM recognises. */
static const char *const XIAM_CANONICAL_ORDER[] = {
    "BJ", "BK", "B-",
    "DJ", "DJK", "DK", "dj", "dk",
    "H_J", "HJK", "HKJ", "H_K", "h_j", "hjk", "h_k",
    "chi_z", "chi-", "chi_xy", "chi_xz", "chi_yz",
    "F12", "Vss", "Vcc",
    "C+", "C_z", "C-",
    "mu_x", "mu_y", "mu_z",
    "V1n", "V2n", "F", "rho", "beta", "gamma",
    "Dpi2J", "Dpi2K", "Dpi2-", "Dc3J", "F0", "epsil", "delta",
};
#define XIAM_CANONICAL_ORDER_N ((int)(sizeof(XIAM_CANONICAL_ORDER) / sizeof(XIAM_CANONICAL_ORDER[0])))

/* Case-sensitive: XIAM tells DJ from dj, and H_K from h_k, by case alone. A
 * trailing "_<digit>" (a top/state suffix, e.g. "V1n_2") is matched on its
 * base keyword and ranked just after the bare/"_1" form of the same name. */
static int xiam_canonical_rank(const char *name) {
    if (!name || !*name) return INT_MAX;
    char base[48]; size_t n = strlen(name); int suffix = 0;
    if (n >= 2 && name[n - 2] == '_' && isdigit((unsigned char)name[n - 1])) {
        suffix = name[n - 1] - '0'; n -= 2;
    }
    if (n >= sizeof(base)) n = sizeof(base) - 1;
    memcpy(base, name, n); base[n] = '\0';
    for (int i = 0; i < XIAM_CANONICAL_ORDER_N; i++)
        if (!strcmp(base, XIAM_CANONICAL_ORDER[i])) return i * 8 + suffix;
    return INT_MAX;
}

/* Relocate one source line to index `to` (counted as if `from` were already
 * removed), fixing up every parameter's source_line/fit_source_line so each
 * still points at the line it actually owns. Mirrors xiam_source_insert_line's
 * index bookkeeping, but for a move instead of a fresh insertion. */
static void xiam_source_move_line(XiamState *x, int from, int to) {
    if (!x || from < 0 || from >= x->n_source_lines) return;
    if (to < 0) to = 0;
    if (to > x->n_source_lines - 1) to = x->n_source_lines - 1;
    if (from == to) return;
    char moved[256]; snprintf(moved, sizeof(moved), "%s", x->source_lines[from]);
    int dir = to > from ? 1 : -1;
    for (int i = from; i != to; i += dir)
        memcpy(x->source_lines[i], x->source_lines[i + dir], sizeof(x->source_lines[0]));
    snprintf(x->source_lines[to], sizeof(x->source_lines[0]), "%s", moved);
    for (int k = 0; k < x->n_params; k++) {
        int *idx[2] = { &x->params[k].source_line, &x->params[k].fit_source_line };
        for (int j = 0; j < 2; j++) {
            if (*idx[j] == from) { *idx[j] = to; continue; }
            if (dir > 0) { if (*idx[j] > from && *idx[j] <= to) (*idx[j])--; }
            else         { if (*idx[j] < from && *idx[j] >= to) (*idx[j])++; }
        }
    }
}

/* Keep the on-screen table in the same order as the file: a plain insertion
 * sort by source_line, stable, cheap enough for a parameter table's size. */
static void xiam_resort_parameters(XiamState *x) {
    for (int i = 1; i < x->n_params; i++) {
        XiamParameter key = x->params[i];
        int j = i - 1;
        while (j >= 0 && x->params[j].source_line > key.source_line) {
            x->params[j + 1] = x->params[j]; j--;
        }
        x->params[j + 1] = key;
    }
}

/* Move a just-(re)named constant's Block III declaration next to its
 * canonical neighbours, so adding constants in any order still leaves the
 * generated .xi — and the parameter table above it — in a logical sequence
 * rather than piling new ones up at the block's tail. */
static void xiam_reposition_parameter(XiamState *x, int row) {
    if (!x || row < 0 || row >= x->n_params) return;
    int from = x->params[row].source_line;
    int rank = xiam_canonical_rank(x->params[row].name);
    if (from < 0 || from >= x->n_source_lines || rank == INT_MAX) return;
    /* A restored session can carry a params[] row whose declaration was
       since commented out (deleted); never shuffle a disabled line around
       as if it were live. */
    if (xiam_is_comment(x->source_lines[from])) return;
    int lower_line = -1, lower_rank = -1, higher_line = -1, higher_rank = INT_MAX;
    for (int k = 0; k < x->n_params; k++) {
        if (k == row) continue;
        int line = x->params[k].source_line;
        int r = xiam_canonical_rank(x->params[k].name);
        if (line < 0 || line >= x->n_source_lines || r == INT_MAX ||
            xiam_is_comment(x->source_lines[line])) continue;
        if (r < rank && r > lower_rank) { lower_rank = r; lower_line = line; }
        if (r > rank && r < higher_rank) { higher_rank = r; higher_line = line; }
    }
    int to = higher_line >= 0 ? higher_line : lower_line >= 0 ? lower_line + 1 : from;
    if (to > from) to--; /* removing `from` first shifts a later target back by one */
    xiam_source_move_line(x, from, to);
    xiam_resort_parameters(x);
}

/* Repair a whole hamiltonian at once: used when restoring a session saved
 * before this ordering existed (or any other already-scrambled .xi), so a
 * constant misplaced by the old bug straightens out on load instead of
 * needing to be deleted and re-added by hand.  One pass only fixes each
 * constant against its *current* neighbours, so it is repeated until a pass
 * makes no move, the same way repeated bubble passes converge on a sort. */
static void xiam_reposition_all_parameters(XiamState *x) {
    if (!x) return;
    int n = x->n_params;
    if (n > MAX_XIAM_PARAMS) n = MAX_XIAM_PARAMS;
    if (n <= 0) return;
    char names[MAX_XIAM_PARAMS][48];
    for (int i = 0; i < n; i++) snprintf(names[i], sizeof(names[i]), "%s", x->params[i].name);
    for (int pass = 0; pass < n; pass++) {
        int moved = 0;
        for (int i = 0; i < n; i++) {
            int row = xiam_find_parameter(x, names[i]);
            if (row < 0) continue;
            int before = x->params[row].source_line;
            xiam_reposition_parameter(x, row);
            row = xiam_find_parameter(x, names[i]);
            if (row >= 0 && x->params[row].source_line != before) moved = 1;
        }
        if (!moved) break;
    }
}

/* ---------------------------------------------------------------------------
 *  Blocks II, V and VI: the calculation controls and the torsional states.
 *
 *  Pred&Fit's States page owns the .int control record and one row per
 *  Pickett state.  XIAM's equivalents are its block II control cards, its
 *  block V torsional symmetries and its block VI torsional states, and they
 *  live only in the retained source text.  Everything below reads and edits
 *  them there, which is also what makes them survive a session save.
 * ------------------------------------------------------------------------- */

/* The uncertainty a row carries, or the model's default for a row that has
 * none.  Pred&Fit keeps the same field for its .lin; both windows show it in
 * the corner of the Parameters page. */
static double xiam_line_error(const XiamState *x, const Assignment *a) {
    if (a && a->line_error_mhz > 0.0) return a->line_error_mhz;
    return x && x->line_error_mhz > 0.0 ? x->line_error_mhz : 0.010;
}

/* One row of the States table: a block V torsional symmetry or a block VI
 * torsional state, active or commented out.  A commented record is still a
 * row, because turning it back on is the whole point of the page. */
typedef struct {
    int line;       /* index in source_lines                       */
    int block;      /* 5 = torsional symmetry, 6 = torsional state */
    int enabled;    /* 0 when the record is commented out          */
} XiamStateRow;

/* The controls XIAM calls block II.  `ints`, like Pred&Fit's QROT, is not a
 * choice: it is 1 for a fit and 2 for a prediction and is regenerated on
 * every run, so it is shown and not edited. */
static const char *const XIAM_CONTROL_KEY[10] = {
    "ints", "ntop", "nfold", "reduc", "spin", "spin2", "maxm", "maxvm", "ndata", "convg"
};
static const char *const XIAM_CONTROL_LABEL[10] = {
    "ints (auto)", "ntop", "nfold", "reduc", "spin", "spin2", "maxm", "maxvm", "ndata", "convg"
};
static const double XIAM_CONTROL_DEFAULT[10] = {2, 1, 3, 1, 0, 0, 10, 3, 10000, 0.99999};
static const char *const XIAM_CONTROL_FORMAT[10] = {
    "%.0f", "%.0f", "%.0f", "%.0f", "%.0f", "%.0f", "%.0f", "%.0f", "%.0f", "%.12g"
};

static void xiam_state_row_text(const XiamState *x, const XiamStateRow *row,
                                char *out, size_t size) {
    char work[256];
    snprintf(work, sizeof(work), "%s", x->source_lines[row->line]);
    char *at = xiam_ltrim(work);
    if (*at == '\\' || *at == '!') { at++; while (*at == ' ') at++; }
    snprintf(out, size, "%s", at);
}

/* The optional spin-statistical weight on a block V symmetry record:
   `/label S sigma... W value`.  Absent means 1, matching XIAM's own default
   and its own echo of block V, which omits W entirely unless a species'
   weight differs from 1 - see iamio.f/iamint.f ("spin stat. weights W,
   block 5"). */
static double xiam_state_row_w(const XiamState *x, const XiamStateRow *row) {
    if (row->block != 5) return 1.0;
    char text[256]; xiam_state_row_text(x, row, text, sizeof(text));
    char *bang = strchr(text, '!'); if (bang) *bang = '\0';
    char *v[32]; int n = 0;
    for (char *tok = strtok(text, " \t"); tok && n < 32; tok = strtok(NULL, " \t")) v[n++] = tok;
    for (int i = 0; i + 1 < n; i++)
        if (xiam_equal(v[i], "W")) {
            char *end = NULL; double val = xiam_number(v[i + 1], &end);
            if (end != v[i + 1] && isfinite(val)) return val;
        }
    return 1.0;
}


/* Which native block a source line belongs to, by walking the headings. */
static int xiam_line_block(const XiamState *x, int upto) {
    int block = 0;
    for (int i = 0; i <= upto && i < x->n_source_lines; i++) {
        char work[256], lower[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[i]);
        char *at = xiam_ltrim(work);
        if (!xiam_is_comment(at)) continue;
        xiam_lower(lower, sizeof(lower), at);
        if (strstr(lower, "block viii")) block = 8;
        else if (strstr(lower, "block vii")) block = 7;
        else if (strstr(lower, "block vi")) block = 6;
        else if (strstr(lower, "block v")) block = 5;
        else if (strstr(lower, "block iv")) block = 4;
        else if (strstr(lower, "block iii")) block = 3;
        else if (strstr(lower, "block ii")) block = 2;
    }
    return block;
}

/* The active declaration of a control card, or -1 when the .xi leaves it to
 * XIAM's own default. */
static int xiam_control_line(const XiamState *x, const char *key) {
    for (int i = 0; x && i < x->n_source_lines; i++) {
        char work[256], name[64] = "";
        snprintf(work, sizeof(work), "%s", x->source_lines[i]);
        char *at = xiam_ltrim(work);
        if (xiam_is_comment(at)) continue;
        if (sscanf(at, "%63s", name) == 1 && xiam_equal(name, key)) return i;
    }
    return -1;
}

static int xiam_control_value(const XiamState *x, const char *key, double fallback, double *out) {
    int at = xiam_control_line(x, key);
    if (at < 0) { if (out) *out = fallback; return 0; }
    char work[256];
    snprintf(work, sizeof(work), "%s", x->source_lines[at]);
    char *tail = xiam_ltrim(work);
    while (*tail && !isspace((unsigned char)*tail)) tail++;
    char *end = NULL;
    double v = xiam_number(xiam_ltrim(tail), &end);
    if (end == tail || !isfinite(v)) { if (out) *out = fallback; return 0; }
    if (out) *out = v;
    return 1;
}

/* Set a control card, rewriting the existing declaration in place so its
 * trailing `!` comment is preserved, or adding one to block II. */
static int xiam_set_control(AppState *s, const char *key, double value, const char *format) {
    XiamState *x = &s->xiam;
    int at = xiam_control_line(x, key);
    if (at >= 0) {
        char changed[512];
        xiam_replace_number(changed, sizeof(changed), x->source_lines[at], value);
        snprintf(x->source_lines[at], sizeof(x->source_lines[0]), "%s", changed);
    } else {
        int insert = xiam_block_end(x, 2);
        if (insert < 0) {
            snprintf(x->status, sizeof(x->status),
                     "This .xi has no block II: add a `\\ block II` heading before declaring controls.");
            return 0;
        }
        char line[256], number[64];
        snprintf(number, sizeof(number), format ? format : "%.12g", value);
        snprintf(line, sizeof(line), " %-7s %s", key, number);
        if (xiam_source_insert_line(x, insert, line) < 0) {
            snprintf(x->status, sizeof(x->status), "The retained .xi source is full; cannot add a control.");
            return 0;
        }
    }
    /* spin/spin2 are also cached on the workspace, because the assignment
       identity depends on how many hyperfine nuclei the model has. */
    if (xiam_equal(key, "spin")) x->spin = (int)lround(value);
    if (xiam_equal(key, "spin2")) x->spin2 = (int)lround(value);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    return 1;
}

/* `\/E2 S 1` is a disabled record; `\ E1 species: sigma = +1` is prose.
 * Tell them apart by parsing what follows the comment marker. */
static int xiam_state_record_kind(const char *line) {
    char work[256];
    snprintf(work, sizeof(work), "%s", line);
    char *at = xiam_ltrim(work);
    if (*at == '\\' || *at == '!') at = xiam_ltrim(at + 1);
    if (*at == '/') {
        /* /label S <sigma> [<sigma2>] */
        char label[64] = "", key[16] = "";
        int sigma = 0;
        if (sscanf(at, "%63s %15s %d", label, key, &sigma) == 3 && xiam_equal(key, "S")) return 5;
        return 0;
    }
    char key[16] = "";
    int v = 0;
    if (sscanf(at, "%15s %d", key, &v) == 2 && xiam_equal(key, "V")) return 6;
    return 0;
}

/* Rewrites a block V record's W clause in place, keeping its label, sigma(s),
   disabled marker and any inline `!` comment untouched.  value == 1.0 drops
   the clause instead of writing a redundant `W 1`, mirroring XIAM's own
   block-5 echo. */
static int xiam_set_state_row_w(AppState *s, int line, double value) {
    XiamState *x = &s->xiam;
    if (line < 0 || line >= x->n_source_lines) return 0;
    if (xiam_state_record_kind(x->source_lines[line]) != 5) return 0;
    int enabled = !xiam_is_comment(x->source_lines[line]);
    XiamStateRow row = {line, 5, enabled};
    char text[256]; xiam_state_row_text(x, &row, text, sizeof(text));

    char data[256], tail[96] = "";
    char *comment = strchr(text, '!');
    if (comment) {
        size_t dn = (size_t)(comment - text);
        if (dn >= sizeof(data)) dn = sizeof(data) - 1;
        memcpy(data, text, dn); data[dn] = '\0';
        snprintf(tail, sizeof(tail), "%s", comment);
    } else snprintf(data, sizeof(data), "%s", text);
    for (int i = (int)strlen(data) - 1; i >= 0 && isspace((unsigned char)data[i]); i--) data[i] = '\0';

    char *v2[32]; int n = 0;
    for (char *tok = strtok(data, " \t"); tok && n < 32; tok = strtok(NULL, " \t")) v2[n++] = tok;
    int w_idx = -1;
    for (int i = 0; i + 1 < n; i++) if (xiam_equal(v2[i], "W")) { w_idx = i; break; }
    int keep = w_idx >= 0 ? w_idx : n;

    char rebuilt[256]; size_t off = 0;
    for (int i = 0; i < keep; i++)
        off += (size_t)snprintf(rebuilt + off, off < sizeof(rebuilt) ? sizeof(rebuilt) - off : 0, "%s ", v2[i]);
    if (isfinite(value) && value >= 0.0 && fabs(value - 1.0) > 1e-9)
        off += (size_t)snprintf(rebuilt + off, off < sizeof(rebuilt) ? sizeof(rebuilt) - off : 0, "W %.6g ", value);
    if (tail[0]) snprintf(rebuilt + off, off < sizeof(rebuilt) ? sizeof(rebuilt) - off : 0, "%s", tail);
    else if (off > 0) rebuilt[off - 1] = '\0';

    snprintf(x->source_lines[line], sizeof(x->source_lines[0]), enabled ? " %s" : "\\ %s", rebuilt);
    return 1;
}

static int xiam_state_rows(const XiamState *x, XiamStateRow *out, int max) {
    int n = 0;
    for (int i = 0; x && i < x->n_source_lines && n < max; i++) {
        /* Classify first: only the handful of lines that look like a record
           are worth walking the headings for. */
        int kind = xiam_state_record_kind(x->source_lines[i]);
        if (!kind) continue;
        int block = xiam_line_block(x, i);
        /* A record only counts in the block that defines it: block VIII
           transitions and block III constants never look like one. */
        if (block != kind) continue;
        out[n].line = i;
        out[n].block = kind;
        out[n].enabled = !xiam_is_comment(x->source_lines[i]);
        n++;
    }
    return n;
}

/* Pred&Fit's States page can add a state; XIAM's equivalent is a block VI
 * torsional state record. */
static int xiam_add_state_row(AppState *s) {
    XiamState *x = &s->xiam;
    if (!x->loaded) {
        snprintf(x->status, sizeof(x->status), "Open a .xi input before adding a torsional state.");
        return -1;
    }
    int at = xiam_block_end(x, 6);
    if (at < 0) {
        snprintf(x->status, sizeof(x->status),
                 "This .xi has no block VI: add a `\\ block VI` heading before declaring states.");
        return -1;
    }
    /* The next unused v, so the new record is not a duplicate of an existing
       torsional state that XIAM would then solve twice. */
    int next = 0;
    for (int i = 0; i < x->n_source_lines; i++) {
        if (xiam_state_record_kind(x->source_lines[i]) != 6) continue;
        if (xiam_line_block(x, i) != 6) continue;
        char work[256], key[16] = "";
        int v = 0;
        snprintf(work, sizeof(work), "%s", x->source_lines[i]);
        char *p = xiam_ltrim(work);
        if (*p == '\\' || *p == '!') p = xiam_ltrim(p + 1);
        if (sscanf(p, "%15s %d", key, &v) == 2 && v >= next) next = v + 1;
    }
    char line[256];
    snprintf(line, sizeof(line), " V  %d", next);
    int inserted = xiam_source_insert_line(x, at, line);
    if (inserted < 0) {
        snprintf(x->status, sizeof(x->status), "The retained .xi source is full; cannot add a state.");
        return -1;
    }
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    snprintf(x->status, sizeof(x->status), "Added the block VI torsional state V %d.", next);
    return inserted;
}

/* USE: commenting a record out is how XIAM drops a symmetry species or a
 * torsional state, so that is exactly what the checkbox does. */
static int xiam_toggle_state_row(AppState *s, int line) {
    XiamState *x = &s->xiam;
    if (line < 0 || line >= x->n_source_lines) return 0;
    char work[256];
    snprintf(work, sizeof(work), "%s", x->source_lines[line]);
    char *at = xiam_ltrim(work);
    if (xiam_is_comment(at)) {
        char *body = at + 1;
        while (*body == ' ') body++;
        snprintf(x->source_lines[line], sizeof(x->source_lines[0]), " %s", body);
    } else {
        snprintf(x->source_lines[line], sizeof(x->source_lines[0]), "\\%s", work);
    }
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    return 1;
}

static int xiam_line_is_transition(const char *line) {
    if (xiam_is_comment(line)) return 0;
    const char *at = line; int v = 0;
    for (int i = 0; i < 6; i++) { char *end = NULL; at = xiam_ltrim((char *)at); if (!xiam_parse_int(at, &end, &v)) return 0; at = end; }
    return 1;
}

/* A Block VIII record is an observation, not a prediction record.  It has the
 * same first six rotor quantum numbers, but no calculated frequency or
 * SPCAT-intensity columns.  Read it directly rather than pretending it is a
 * CAT/XO row: that keeps a .xi self-contained in exactly the same way a
 * Pickett project imports its .lin. */
static int xiam_parse_input_transition_line(const char *line, PredLine *out,
                                            double *observed_mhz,
                                            double *error_mhz) {
    char work[1024], *v[96];
    int n = 0, q[6];
    if (!line || xiam_is_comment(line)) return 0;
    snprintf(work, sizeof(work), "%s", line);
    for (char *tok = strtok(work, " \t\r\n"); tok && n < 96;
         tok = strtok(NULL, " \t\r\n")) v[n++] = tok;
    if (n < 8) return 0;
    for (int i = 0; i < 6; i++) if (!xiam_parse_int(v[i], NULL, &q[i])) return 0;

    const char *equal = strchr(line, '=');
    if (!equal) return 0;
    char *freq_end = NULL;
    double value = xiam_number(xiam_ltrim((char *)equal + 1), &freq_end);
    if (freq_end == equal + 1 || !isfinite(value)) return 0;
    char after_equal[256];
    snprintf(after_equal, sizeof(after_equal), "%s", equal + 1);
    char lower[256];
    xiam_lower(lower, sizeof(lower), after_equal);
    if (strstr(lower, "ghz")) value *= 1000.0;
    /* SpectraVisual presents XIAM fitting data in MHz.  A unit-less legacy
       line is therefore treated as MHz too; normal XIAM files say MHz. */

    int got_error = 0;
    double uncertainty = 0.010;
    for (int i = 6; i < n; i++) {
        if (!xiam_equal(v[i], "Err") || i + 1 >= n) continue;
        char *end = NULL;
        double parsed = xiam_number(v[i + 1], &end);
        if (end != v[i + 1] && isfinite(parsed) && parsed > 0.0) {
            uncertainty = parsed;
            got_error = 1;
        }
        break;
    }

    memset(out, 0, sizeof(*out));
    out->freq_mhz = value;
    out->lgint = out->cat_lgint = -INFINITY;
    out->linear_int = 0.0;
    out->backend = PRED_BACKEND_XIAM;
    out->intensity_native = 1;
    out->Ju = q[0]; out->Kau = q[1]; out->Kcu = q[2];
    out->Jl = q[3]; out->Kal = q[4]; out->Kcl = q[5];
    out->xiam.present = 1;
    /* XIAM's native output is one-indexed for V and B when the corresponding
       optional labels were omitted from the input record. */
    out->xiam.vu = out->xiam.vl = 1;
    out->xiam.bu = out->xiam.bl = 1;
    int saw_f1 = 0, saw_f = 0;
    for (int i = 6; i < n; i++) {
        int a = 0, b = 0;
        if (xiam_equal(v[i], "F1") && i + 2 < n &&
            xiam_parse_int(v[i + 1], NULL, &a) && xiam_parse_int(v[i + 2], NULL, &b)) {
            out->xiam.f1u = a; out->xiam.f1l = b; saw_f1 = 1;
        } else if (xiam_equal(v[i], "F") && i + 2 < n &&
                   xiam_parse_int(v[i + 1], NULL, &a) && xiam_parse_int(v[i + 2], NULL, &b)) {
            out->xiam.fu = a; out->xiam.fl = b; saw_f = 1;
        } else if (xiam_equal(v[i], "S") && i + 1 < n && xiam_parse_int(v[i + 1], NULL, &a)) {
            out->xiam.su = out->xiam.sl = a;
        } else if (xiam_equal(v[i], "V") && i + 1 < n && xiam_parse_int(v[i + 1], NULL, &a)) {
            out->xiam.vu = out->xiam.vl = a;
        } else if (xiam_equal(v[i], "B") && i + 1 < n && xiam_parse_int(v[i + 1], NULL, &a)) {
            out->xiam.bu = out->xiam.bl = a;
        }
    }
    if (saw_f1 || saw_f) {
        out->n_qn = 6;
        out->M1u = out->xiam.f1u; out->M1l = out->xiam.f1l;
        out->M2u = out->xiam.fu;  out->M2l = out->xiam.fl;
        out->M3u = out->xiam.su;  out->M3l = out->xiam.sl;
    } else {
        out->n_qn = 4;
        out->M1u = out->M1l = out->xiam.su;
    }
    xiam_branch_mu(out);
    if (observed_mhz) *observed_mhz = value;
    if (error_mhz) *error_mhz = got_error ? uncertainty : 0.0;
    return 1;
}

/* K and tau are output annotations derived by XIAM from the six rotor QNs.
 * They do not occur in a normal Block VIII input line, so they cannot be part
 * of the first reconciliation after a .xi is opened.  Once a native .xo is
 * available xiam_same_transition() resumes using the complete identity. */
static int xiam_same_input_transition(const PredLine *a, const PredLine *b) {
    if (!a || !b || a->backend != PRED_BACKEND_XIAM || b->backend != PRED_BACKEND_XIAM ||
        !a->xiam.present || !b->xiam.present) return 0;
    return a->Ju == b->Ju && a->Kau == b->Kau && a->Kcu == b->Kcu &&
           a->Jl == b->Jl && a->Kal == b->Kal && a->Kcl == b->Kcl &&
           a->xiam.f1u == b->xiam.f1u && a->xiam.f1l == b->xiam.f1l &&
           a->xiam.fu == b->xiam.fu && a->xiam.fl == b->xiam.fl &&
           a->xiam.su == b->xiam.su && a->xiam.sl == b->xiam.sl &&
           a->xiam.vu == b->xiam.vu && a->xiam.vl == b->xiam.vl &&
           a->xiam.bu == b->xiam.bu && a->xiam.bl == b->xiam.bl;
}

static int xiam_import_source_assignments(AppState *s) {
    if (!s || !s->xiam.loaded) return 0;
    XiamState *x = &s->xiam;
    int block = 0, imported = 0, changed = 0;
    /* Do not collapse repeated observations.  A source .xi can contain the
       same transition and even the same observed value more than once; XIAM
       treats each occurrence as an independent weighted datum.  Marking a
       record as consumed also makes a reload idempotent. */
    unsigned char matched[MAX_ASSIGNMENTS] = {0};
    for (int line_no = 0; line_no < x->n_source_lines; line_no++) {
        char work[256], lower[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[line_no]);
        char *at = xiam_ltrim(work);
        xiam_lower(lower, sizeof(lower), at);
        if (xiam_is_comment(at)) {
            if (strstr(lower, "block viii")) block = 8;
            else if (strstr(lower, "block vii")) block = 7;
            else if (strstr(lower, "block vi")) block = 6;
            else if (strstr(lower, "block v")) block = 5;
            continue;
        }
        if (block != 8) continue;
        PredLine p;
        double observed = 0.0, uncertainty = 0.010;
        if (!xiam_parse_input_transition_line(at, &p, &observed, &uncertainty)) continue;
        p.hamiltonian_id = x->model_id;
        int found = -1, fallback = -1;
        for (int i = 0; i < s->n_assignments; i++) {
            Assignment *a = &s->assignments[i];
            /* A Block VIII file can intentionally contain several measured
               occurrences of the same transition.  They are independent
               weighted observations in XIAM, not duplicate GUI rows. */
            if (matched[i] || a->hamiltonian_id != x->model_id ||
                !xiam_same_input_transition(&a->pred, &p)) continue;
            if (fabs(a->exp_freq - observed) < 1e-7 &&
                fabs(a->line_error_mhz - uncertainty) < 1e-10) {
                found = i;
                break;
            }
            /* Reloading the original .xi deliberately resets an edited
               observation rather than adding a second copy of it.  Keep
               this only as a fallback so repeated native observations still
               receive one independent Assignment each. */
            if (fallback < 0) fallback = i;
        }
        if (found < 0) found = fallback;
        if (found >= 0) {
            matched[found] = 1;
            Assignment *a = &s->assignments[found];
            a->exp_freq = observed;
            a->line_error_mhz = uncertainty;
            /* Preserve the richer calculated XIAM row already in memory.
               It carries the K/t annotations unavailable in a .xi source. */
            if (!a->pred.xiam.ku && !a->pred.xiam.kl && !a->pred.xiam.tu && !a->pred.xiam.tl)
                a->pred = p;
            changed++;
        } else if (s->n_assignments < MAX_ASSIGNMENTS) {
            int index = s->n_assignments++;
            Assignment *a = &s->assignments[index];
            memset(a, 0, sizeof(*a));
            a->pred = p;
            a->exp_freq = observed;
            a->line_error_mhz = uncertainty;
            a->fit_enabled = 1;
            a->hamiltonian_id = x->model_id;
            matched[index] = 1;
            imported++;
        }
    }
    if (imported || changed) save_assignments(s);
    return imported;
}

static void xiam_write_assignment(FILE *fp, const XiamState *x, const Assignment *a) {
    const PredLine *p = &a->pred;
    fprintf(fp, " %d %d %d  %d %d %d", p->Ju,p->Kau,p->Kcu,p->Jl,p->Kal,p->Kcl);
    if (p->xiam.f1u || p->xiam.f1l) fprintf(fp, " F1 %d %d", p->xiam.f1u,p->xiam.f1l);
    if (p->xiam.fu || p->xiam.fl) fprintf(fp, " F %d %d", p->xiam.fu,p->xiam.fl);
    fprintf(fp, " S %d", p->xiam.su);
    if (p->xiam.vu || p->xiam.vl) fprintf(fp, " V %d", p->xiam.vu);
    if (p->xiam.bu || p->xiam.bl) fprintf(fp, " B %d", p->xiam.bu);
    /* K and t appended to a native prediction line are output annotations,
       not a safe inverse-input syntax.  The first six rotor QNs are XIAM's
       canonical K-/K+ form and v0.44 derives tau from them when a fit line is
       read.  Re-emitting both annotations can instead zero that derivation. */
    fprintf(fp, " = %.9f MHz Err %.9g\n", a->exp_freq,
            xiam_line_error(x, a));
}

static void xiam_write_fit_choices(FILE *fp, const XiamState *x) {
    fprintf(fp, "\\ SpectraVisual XIAM fitting choices (simple directives)\n");
    for (int i = 0; i < x->n_params; i++) {
        const char *method = x->params[i].fit_method == 1 ? "fit" : x->params[i].fit_method == 2 ? "dqu" : x->params[i].fit_method == 3 ? "dqx" : NULL;
        if (method) fprintf(fp, " %s %s\n", method, x->params[i].name);
    }
}

/* These values are the calculation mode, not molecular constants.  Always
 * regenerate them in Block II so a fitting input can predict and a prediction
 * input can fit.  Leaving, for example, a source `ncycl 0` in a generated fit
 * was the reason an imported prediction could not actually be fitted. */
static int xiam_runtime_control(const char *key) {
    return xiam_equal(key, "ncyc") || xiam_equal(key, "nzyk") || xiam_equal(key, "ncycl") ||
           xiam_equal(key, "ints") || xiam_equal(key, "freq") ||
           xiam_equal(key, "freq_l") || xiam_equal(key, "freq_h") ||
           xiam_equal(key, "temp") || xiam_equal(key, "eval");
}

static int xiam_source_has_active_control(const XiamState *x, const char *wanted) {
    if (!x || !wanted) return 0;
    for (int i = 0; i < x->n_source_lines; i++) {
        char line[256], key[64] = "";
        snprintf(line, sizeof(line), "%s", x->source_lines[i]);
        char *at = xiam_ltrim(line);
        if (xiam_is_comment(at)) continue;
        if (sscanf(at, "%63s", key) == 1 && xiam_equal(key, wanted)) return 1;
    }
    return 0;
}

static void xiam_write_runtime_controls(FILE *fp, const XiamState *x, int fitting) {
    fprintf(fp, "\\ SpectraVisual XIAM runtime controls (generated; values in GHz where XIAM requires them)\n");
    /* XIAM-2NQ v0.44 recognises nzyk/ncycl; its reader promotes nzyk to the
       maximum of the two.  `ncyc`, present in some hand-written templates,
       is not one of v0.44's declared control strings. */
    fprintf(fp, " ncycl %d\n", fitting ? 100 : 0);
    fprintf(fp, " ints %d\n", fitting ? 1 : 2);
    fprintf(fp, " freq_l %.12g\n", x->fmin_mhz / 1000.0);
    fprintf(fp, " freq_h %.12g\n", x->fmax_mhz / 1000.0);
    fprintf(fp, " temp %.12g\n", x->temperature_k);
    if (!fitting) {
        /* A fitting template often has `eval 0` (or no card at all).  A
           post-fit prediction must explicitly ask XIAM to enumerate lines.
           An active native `limit` remains authoritative; an all-fit input
           commonly has no limit card, for which -10 avoids XIAM's 0.1
           default silently producing an empty updated catalogue. */
        fprintf(fp, " eval 1\n");
        if (!xiam_source_has_active_control(x, "limit")) fprintf(fp, " limit -10\n");
    }
}

static void xiam_write_runtime_j(FILE *fp, const XiamState *x) {
    fprintf(fp, "\\ SpectraVisual prediction limit (generated)\n J %d\n", x->max_j);
}

/* A XIAM input is one Hamiltonian, even if it resolves several torsional or
 * hyperfine states.  Only rows owned by that exact .xi belong in its fit. */
static int xiam_assignment_index(const AppState *s, int ordinal) {
    if (!s || ordinal < 0) return -1;
    int seen = 0;
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        if (a->pred.backend != PRED_BACKEND_XIAM ||
            a->hamiltonian_id != s->xiam.model_id) continue;
        if (seen++ == ordinal) return i;
    }
    return -1;
}

static int xiam_assignment_count(const AppState *s, int *enabled) {
    int total = 0, used = 0;
    if (s) for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        if (a->pred.backend != PRED_BACKEND_XIAM ||
            a->hamiltonian_id != s->xiam.model_id) continue;
        total++;
        if (a->fit_enabled) used++;
    }
    if (enabled) *enabled = used;
    return total;
}

static int xiam_fitted_parameter_count(const XiamState *x) {
    int count = 0;
    for (int i = 0; x && i < x->n_params; i++)
        if (x->params[i].fit_method || x->params[i].error_ghz != 0.0) count++;
    return count;
}

static int xiam_fitted_parameter_index(const XiamState *x, int ordinal) {
    if (!x || ordinal < 0) return -1;
    int seen = 0;
    for (int i = 0; i < x->n_params; i++) {
        if (!x->params[i].fit_method && x->params[i].error_ghz == 0.0) continue;
        if (seen++ == ordinal) return i;
    }
    return -1;
}

/* Match one native Block VIII line to one editable Assignment.  We retain a
 * line byte-for-byte whenever it has not been edited: this preserves all
 * XIAM-specific optional syntax and, critically, the multiplicity/order of
 * weighted observations.  Newly created or edited assignments are emitted
 * through xiam_write_assignment below. */
static int xiam_find_source_assignment(const AppState *s, const XiamState *x,
                                       const PredLine *source, double observed,
                                       double uncertainty, const unsigned char *used) {
    int fallback = -1;
    if (!s || !x || !source) return -1;
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        if ((used && used[i]) || a->pred.backend != PRED_BACKEND_XIAM ||
            a->hamiltonian_id != x->model_id ||
            !xiam_same_input_transition(&a->pred, source)) continue;
        if (fabs(a->exp_freq - observed) < 1e-7 &&
            fabs(a->line_error_mhz - uncertainty) < 1e-10)
            return i;
        if (fallback < 0) fallback = i;
    }
    return fallback;
}

static int xiam_write_input(AppState *s, const char *path, int fitting) {
    XiamState *x = &s->xiam; FILE *fp = fopen(path, "w"); if (!fp) return 0;
    int wrote_fit_choices = 0, wrote_controls = 0, wrote_j = 0, source_block = 0;
    unsigned char source_assignment_used[MAX_ASSIGNMENTS] = {0};
    for (int i = 0; i < x->n_source_lines; i++) {
        char current[256], key[64] = "", *at;
        snprintf(current, sizeof(current), "%s", x->source_lines[i]); at = xiam_ltrim(current);
        char lower[256]; xiam_lower(lower, sizeof(lower), at);
        /* XIAM uses the blank line immediately after Block IV as the end of
           its fitting-directive section.  Inserting just before the Block V
           title would be one block too late and makes “dqu BJ” look like a
           malformed torsional-symmetry/transition record. */
        if (fitting && !wrote_fit_choices && source_block == 4 && !*at) {
            xiam_write_fit_choices(fp, x); wrote_fit_choices = 1;
        }
        /* Block II has the same blank-line delimiter.  The Block III title
           follows a decorative comment separator, so adding controls at that
           title puts them in Block III and v0.44 reports them as undefined
           molecular parameters. */
        if (!wrote_controls && source_block == 2 && !*at) {
            xiam_write_runtime_controls(fp, x, fitting); wrote_controls = 1;
        }
        if (xiam_is_comment(at)) {
            /* Match the longer Roman numerals first: "block II" is a
               prefix of "block III", just as VII is a prefix of VIII. */
            if (strstr(lower, "block viii")) {
                if (!wrote_j) { xiam_write_runtime_j(fp, x); wrote_j = 1; }
                source_block = 8;
            } else if (strstr(lower, "block vii")) source_block = 7;
            else if (strstr(lower, "block vi")) source_block = 6;
            else if (strstr(lower, "block v")) {
                if (fitting && !wrote_fit_choices) { xiam_write_fit_choices(fp, x); wrote_fit_choices = 1; }
                source_block = 5;
            } else if (strstr(lower, "block iv")) source_block = 4;
            else if (strstr(lower, "block iii")) {
                if (!wrote_controls) { xiam_write_runtime_controls(fp, x, fitting); wrote_controls = 1; }
                source_block = 3;
            } else if (strstr(lower, "block ii")) source_block = 2;
        }
        if (!xiam_is_comment(at)) sscanf(at, "%63s", key);
        if (!xiam_is_comment(at) && xiam_runtime_control(key)) continue;
        if (!xiam_is_comment(at) && source_block == 7 && xiam_equal(key, "J")) continue;
        if (fitting && source_block == 8 && xiam_line_is_transition(current)) {
            PredLine source; double observed = 0.0, uncertainty = 0.010;
            if (xiam_parse_input_transition_line(at, &source, &observed, &uncertainty)) {
                int ai = xiam_find_source_assignment(s, x, &source, observed, uncertainty,
                                                      source_assignment_used);
                if (ai >= 0) {
                    Assignment *a = &s->assignments[ai];
                    source_assignment_used[ai] = 1;
                    if (a->fit_enabled) {
                        /* Exact match: native spelling/order survives.  The
                           fallback represents a user-edited datum. */
                        if (fabs(a->exp_freq - observed) < 1e-7 &&
                            fabs(a->line_error_mhz - uncertainty) < 1e-10)
                            fprintf(fp, "%s\n", x->source_lines[i]);
                        else xiam_write_assignment(fp, x, a);
                    } else fprintf(fp, "\\%s\n", current);
                    continue;
                }
            }
            /* A malformed/unowned source record must never reach an
               automatically generated fit unnoticed. */
            fprintf(fp, "\\%s\n", current); continue;
        }
        int pi = -1; for (int k = 0; k < x->n_params; k++) if (x->params[k].source_line == i) { pi = k; break; }
        if (pi >= 0) {
            /* An unchecked dipole is written as zero and keeps its stored
               value, exactly as Pred&Fit's mu checkboxes do to the .int. */
            double value = x->params[pi].value_ghz;
            for (int k = 0; k < 3; k++)
                if (x->mu_excluded[k] && !strcmp(x->params[pi].name, XIAM_DIPOLE[k])) value = 0.0;
            char changed[512];
            xiam_replace_number(changed, sizeof(changed), x->source_lines[i], value);
            fprintf(fp, "%s\n", changed); continue;
        }
        int fit_pi = -1; for (int k = 0; k < x->n_params; k++) if (x->params[k].fit_source_line == i) { fit_pi = k; break; }
        if (fitting && fit_pi >= 0) { fprintf(fp, "\\%s\n", x->source_lines[i]); continue; }
        fprintf(fp, "%s\n", x->source_lines[i]);
    }
    /* Standard XIAM files name their blocks in comments.  Keep a recoverable
       fallback for a hand-written minimal input, while all supplied templates
       take the correct in-block path above. */
    if (!wrote_controls) xiam_write_runtime_controls(fp, x, fitting);
    if (!wrote_j) xiam_write_runtime_j(fp, x);
    if (fitting) {
        if (!wrote_fit_choices) xiam_write_fit_choices(fp, x);
        int n = 0; for (int i = 0; i < s->n_assignments; i++) if (s->assignments[i].fit_enabled && s->assignments[i].pred.backend == PRED_BACKEND_XIAM && s->assignments[i].hamiltonian_id == x->model_id) n++;
        fprintf(fp, "\\ SpectraVisual XIAM fit assignments (%d selected)\n", n);
        for (int i = 0; i < s->n_assignments; i++)
            if (!source_assignment_used[i] && s->assignments[i].fit_enabled &&
                s->assignments[i].pred.backend == PRED_BACKEND_XIAM &&
                s->assignments[i].hamiltonian_id == x->model_id)
                xiam_write_assignment(fp, x, &s->assignments[i]);
    }
    int bad = ferror(fp) || fclose(fp) != 0; return !bad;
}

static void xiam_work_file(const XiamState *x, const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s", x->work_dir, name);
}

/* The session and the import folder are the project's, not one backend's:
 * Pred&Fit's Hamiltonians page reads and writes exactly these two paths. */
static void xiam_session_file(const AppState *s, char *out, size_t size) {
    if (s->predfit.work_dir[0]) snprintf(out, size, "%s/spectravisual.state", s->predfit.work_dir);
    else snprintf(out, size, ".fit/spectravisual.state");
}

static void xiam_load_folder(const AppState *s, char *out, size_t size) {
    if (s->predfit.work_dir[0]) snprintf(out, size, "%s/load", s->predfit.work_dir);
    else snprintf(out, size, ".fit/load");
}

static int xiam_prepare_work_dir(AppState *s) {
    XiamState *x = &s->xiam;
    char parent[512];
    if (s->settings.data_dir[0]) snprintf(parent, sizeof(parent), "%s/%s", s->settings.data_dir, XIAM_DIR_NAME);
    else snprintf(parent, sizeof(parent), "%s", XIAM_DIR_NAME);
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) { snprintf(x->status,sizeof(x->status),"Cannot create XIAM workspace: %s",strerror(errno)); return 0; }
    snprintf(x->work_dir, sizeof(x->work_dir), "%s/H%d", parent, x->model_id < 0 ? -x->model_id : x->model_id);
    if (mkdir(x->work_dir, 0700) != 0 && errno != EEXIST) { snprintf(x->status,sizeof(x->status),"Cannot create XIAM Hamiltonian workspace: %s",strerror(errno)); return 0; }
    return 1;
}

static int xiam_run(AppState *s, int fitting) {
    XiamState *x = &s->xiam;
    if (!x->loaded) { snprintf(x->status,sizeof(x->status),"Open a .xi input first (drop it onto the main window)."); return 0; }
    /* The model may already have been open when an updated program starts.
       Check here too, not just at Open, so an in-memory legacy workspace can
       never be fitted or overwrite a newer working copy. */
    if (xiam_is_legacy_workspace_input(x->input_path)) {
        snprintf(x->status, sizeof(x->status),
                 "Refusing legacy .xiam/.../model.xi workspace: reopen the original source .xi before fitting.");
        return 0;
    }
    if (!s->settings.xiam_path[0] || access(s->settings.xiam_path, X_OK) != 0) { snprintf(x->status,sizeof(x->status),"Set the executable XIAM-2NQ v0.44 path in Settings."); return 0; }
    if (fitting) { int n = 0; for (int i=0;i<s->n_assignments;i++) if (s->assignments[i].fit_enabled && s->assignments[i].pred.backend == PRED_BACKEND_XIAM && s->assignments[i].hamiltonian_id == x->model_id) n++; if (!n) { snprintf(x->status,sizeof(x->status),"Select and assign at least one prediction from this .xi model before fitting."); return 0; } }
    if (!xiam_prepare_work_dir(s)) return 0;
    char in[700], out[700];
    xiam_work_file(x, fitting ? "fit.xi" : "prediction.xi", in, sizeof(in));
    xiam_work_file(x, fitting ? "fit.xo" : "prediction.xo", out, sizeof(out));
    if (!xiam_write_input(s, in, fitting)) { snprintf(x->status,sizeof(x->status),"Cannot write XIAM input: %s",strerror(errno)); return 0; }
    pid_t pid = fork();
    if (pid < 0) { snprintf(x->status,sizeof(x->status),"Cannot start XIAM: %s",strerror(errno)); return 0; }
    if (pid == 0) {
        int fi = open(in, O_RDONLY), fo = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0600);
        if (fi < 0 || fo < 0 || chdir(x->work_dir) != 0) _exit(127);
        dup2(fi, STDIN_FILENO); dup2(fo, STDOUT_FILENO); dup2(fo, STDERR_FILENO); close(fi); close(fo);
        char *const argv[] = {s->settings.xiam_path, NULL}; execv(s->settings.xiam_path, argv); _exit(127);
    }
    int status = 0; if (waitpid(pid, &status, 0) < 0) { snprintf(x->status,sizeof(x->status),"XIAM wait failed: %s",strerror(errno)); return 0; }
    if (fitting) snprintf(x->fit_output_path, sizeof(x->fit_output_path), "%s", out);
    else snprintf(x->output_path, sizeof(x->output_path), "%s", out);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || xiam_fatal_output(out,x->diagnostics,sizeof(x->diagnostics),fitting)) {
        snprintf(x->status,sizeof(x->status),"XIAM did not complete; see retained %s diagnostics.", fitting ? "fit.xo" : "prediction.xo"); return 0;
    }
    return 1;
}

/* The closing "Standard Deviation  0.048002 MHz" is XIAM's own figure of
   merit for a fit (the counterpart of SPFIT's MICROWAVE RMS).  A report
   without it, e.g. a prediction-only .xo, simply has no summary. */
static void xiam_refresh_fit_summary(XiamState *x) {
    x->has_fit_summary = 0; x->fit_std_mhz = 0.0; x->fit_iterations = 0;
    if (!x->fit_output_path[0]) return;
    FILE *fp = fopen(x->fit_output_path, "rb"); if (!fp) return;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        int iteration = 0; double std = 0.0;
        if (sscanf(line, " Iteration : %d", &iteration) == 1) x->fit_iterations = iteration;
        else if (sscanf(line, " Standard Deviation %lf", &std) == 1 && isfinite(std)) {
            x->fit_std_mhz = std; x->has_fit_summary = 1;
        }
    }
    fclose(fp);
}

static int xiam_parse_fit_parameters(XiamState *x, const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    char line[1024]; int in = 0, values_are_errors = 0, updated = 0;
    while (fgets(line,sizeof(line),fp)) {
        /* v0.44 writes each iteration as “Parameters Change”, then closes
           with “Parameters and Errors”.  Both contain the fitted value; only
           the latter's braced number is an uncertainty rather than a step. */
        if (strstr(line,"Parameters") && strstr(line,"Errors")) { in = 1; values_are_errors = 1; continue; }
        if (strstr(line,"Parameters") && strstr(line,"Change")) { in = 1; values_are_errors = 0; continue; }
        if (!in) continue;
        char name[64]="", value[128]="", error[128]="";
        if (sscanf(line," %63s %127s { %127[^}]",name,value,error) < 3) continue;
        int pi = xiam_find_parameter(x,name); if (pi < 0) continue;
        x->params[pi].value_ghz = xiam_number(value,NULL);
        if (values_are_errors) x->params[pi].error_ghz = xiam_number(error,NULL);
        updated++;
    }
    fclose(fp); xiam_update_axis_convention(x); return updated;
}

void xiam_init(AppState *s) {
    if (!s) return;
    s->xiam.edit_param = XIAM_EDIT_NONE;
    s->xiam.edit_assignment = -1;
    s->xiam.edit_name_row = -1;
    s->xiam.edit_hamiltonian_row = -1;
    s->xiam.hover_line = -1;
    s->xiam.edit_control = -1;
    s->xiam.edit_state_line = -1;
    s->xiam.line_error_mhz = 0.010;
    s->xiam.active_hamiltonian = -1;
    s->xiam.active_tab = 0;
    s->xiam.max_j = 30;
    s->xiam.temperature_k = 1.0;
    s->xiam.concentration = 1.0;
}

void xiam_close(AppState *s) {
    if (!s) return; XiamState *x=&s->xiam;
    if (x->renderer) SDL_DestroyRenderer(x->renderer); if (x->window) SDL_DestroyWindow(x->window);
    xiam_store_active(x);
    x->renderer=NULL; x->window=NULL; x->window_id=0; x->open=0; x->edit_param=XIAM_EDIT_NONE;
}
void xiam_dispose(AppState *s) {
    xiam_close(s);
    if (!s) return;
    free(s->xiam.history);
    s->xiam.history = NULL;
    s->xiam.history_count = s->xiam.history_capacity = 0;
}

void xiam_open(AppState *s) {
    if (!s) return; XiamState *x=&s->xiam;
    if (x->window) { SDL_RaiseWindow(x->window); return; }
    x->window=SDL_CreateWindow("XIAM — native Hamiltonian workspace",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1300,820,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!x->window) { snprintf(x->status,sizeof(x->status),"Could not open XIAM window."); return; }
    SDL_SetWindowMinimumSize(x->window,XIAM_MIN_W,XIAM_MIN_H); x->renderer=SDL_CreateRenderer(x->window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!x->renderer) { SDL_DestroyWindow(x->window); x->window=NULL; snprintf(x->status,sizeof(x->status),"Could not create XIAM renderer."); return; }
    SDL_SetRenderDrawBlendMode(x->renderer,SDL_BLENDMODE_BLEND); x->window_id=SDL_GetWindowID(x->window); x->open=1;
}

int xiam_load_input_file(AppState *s, const char *path) {
    if (!s || !path || !path[0]) return 0; XiamState *x=&s->xiam;
    if (xiam_is_legacy_workspace_input(path)) {
        snprintf(x->status, sizeof(x->status),
                 "Refusing legacy .xiam/.../model.xi workspace: it may contain overwritten fit constants. Open the original source .xi instead.");
        return 0;
    }
    int existing = xiam_find_hamiltonian_path(x, path);
    if (existing >= 0) {
        xiam_select_hamiltonian(x, existing);
        snprintf(x->status, sizeof(x->status), "Selected XIAM Hamiltonian %s.", xiam_basename(path));
        if (SDL_WasInit(SDL_INIT_VIDEO)) xiam_open(s);
        return 1;
    }
    if (x->n_hamiltonians >= MAX_XIAM_HAMILTONIANS) {
        snprintf(x->status, sizeof(x->status), "A XIAM project supports at most %d Hamiltonians.", MAX_XIAM_HAMILTONIANS);
        return 0;
    }
    int previous = x->active_hamiltonian;
    xiam_store_active(x); xiam_clear_active(x);
    if (!xiam_read_source(x,path)) {
        if (previous >= 0) xiam_restore_active(x, previous);
        snprintf(x->status,sizeof(x->status),"Cannot read XIAM input: %s",path); return 0;
    }
    snprintf(x->input_path,sizeof(x->input_path),"%s",path); x->loaded=1; xiam_parse_source(s);
    x->model_id = xiam_model_id_from_path(x->input_path);
    xiam_register_active(x);
    int imported = xiam_import_source_assignments(s);
    if (SDL_WasInit(SDL_INIT_VIDEO)) xiam_open(s);
    snprintf(x->status,sizeof(x->status),"Loaded Hamiltonian %d/%d (%s): %d molecular parameters, %d Block VIII transitions imported; input/output are in MHz.",x->active_hamiltonian+1,x->n_hamiltonians,xiam_basename(path),x->n_params,imported);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    return 1;
}

/* An output is often opened from Finder after another XIAM input is already
 * selected.  Associate same-stem source/output pairs before parsing: without
 * this, `alpha.xo` could silently replace the catalogue of the active beta.xi
 * model and give its rows beta's owner id. */
static int xiam_same_file_stem(const char *left, const char *right) {
    const char *a = xiam_basename(left), *b = xiam_basename(right);
    const char *adot = strrchr(a, '.'), *bdot = strrchr(b, '.');
    size_t an = adot ? (size_t)(adot - a) : strlen(a);
    size_t bn = bdot ? (size_t)(bdot - b) : strlen(b);
    return an > 0 && an == bn && strncasecmp(a, b, an) == 0;
}

static void xiam_select_output_owner(XiamState *x, const char *output_path) {
    if (!x || !output_path || !output_path[0]) return;
    for (int i = 0; i < x->n_hamiltonians; i++)
        if (xiam_same_file_stem(x->hamiltonian[i].input_path, output_path)) {
            xiam_select_hamiltonian(x, i);
            return;
        }
}

int xiam_load_output_file(AppState *s, const char *path) {
    PredLine *rows=NULL; int n=0; double lo=0,hi=0,top=0;
    if (!s || !path) return 0;
    xiam_select_output_owner(&s->xiam, path);
    int model_id = s->xiam.loaded ? s->xiam.model_id : xiam_model_id_from_path(path);
    int fit_parameters = s->xiam.loaded ? xiam_parse_fit_parameters(&s->xiam,path) : 0;
    /* XIAM can be asked to print a catalogue together with a fit report.
       Preserve the report pathname even in that mixed form: otherwise the
       Fitting page loses its provenance as soon as it displays the sticks. */
    if (fit_parameters) { snprintf(s->xiam.fit_output_path,sizeof(s->xiam.fit_output_path),"%s",path); xiam_refresh_fit_summary(&s->xiam); }
    if (!xiam_read_output_records(path,model_id,&rows,&n,&lo,&hi,&top)) {
        if (fit_parameters) {
            snprintf(s->xiam.status,sizeof(s->xiam.status),"Loaded %d fitted XIAM parameters; this .xo contains no native prediction list.",fit_parameters);
            xiam_store_active(&s->xiam);
            xiam_mark_session_dirty(s);
            return 1;
        }
        snprintf(s->xiam.status,sizeof(s->xiam.status),"No readable native XIAM prediction records in %s.",path);
        return 0;
    }
    xiam_adopt_prediction(s,rows,n,lo,hi,top,path); snprintf(s->xiam.output_path,sizeof(s->xiam.output_path),"%s",path); xiam_store_active(&s->xiam); xiam_mark_session_dirty(s); if (SDL_WasInit(SDL_INIT_VIDEO)) xiam_open(s); return 1;
}

/* .fit/load is shared with Pred&Fit.  Native XIAM models are independent
 * inputs rather than Pickett states, so import the source first and only then
 * read the matching output while that source is its active Hamiltonian. */
#define MAX_XIAM_LOAD_MODELS 64
typedef struct {
    char stem[256];
    char xi_name[256];
    char xo_name[256];
    int has_xi, has_xo;
} XiamLoadCandidate;

static int xiam_load_candidate_slot(XiamLoadCandidate *list, int *count,
                                    const char *stem) {
    for (int i = 0; i < *count; i++)
        if (strcasecmp(list[i].stem, stem) == 0) return i;
    if (*count >= MAX_XIAM_LOAD_MODELS) return -1;
    memset(&list[*count], 0, sizeof(list[*count]));
    snprintf(list[*count].stem, sizeof(list[*count].stem), "%s", stem);
    return (*count)++;
}

static int xiam_load_candidate_cmp(const void *left, const void *right) {
    const XiamLoadCandidate *a = left, *b = right;
    return strcasecmp(a->stem, b->stem);
}

int xiam_import_load_dir(AppState *s, const char *directory) {
    if (!s || !directory || !directory[0]) return 0;
    DIR *dir = opendir(directory);
    if (!dir) {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "Cannot open XIAM import folder %s.", directory);
        return 0;
    }

    XiamLoadCandidate found[MAX_XIAM_LOAD_MODELS];
    int n_found = 0, ignored = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || dot == entry->d_name) continue;
        int is_xi = strcasecmp(dot, ".xi") == 0;
        int is_xo = strcasecmp(dot, ".xo") == 0;
        if (!is_xi && !is_xo) continue;
        size_t stem_len = (size_t)(dot - entry->d_name);
        if (stem_len == 0 || stem_len >= sizeof(found[0].stem) ||
            strlen(entry->d_name) >= sizeof(found[0].xi_name)) {
            ignored++;
            continue;
        }
        char stem[256];
        memcpy(stem, entry->d_name, stem_len);
        stem[stem_len] = '\0';
        int slot = xiam_load_candidate_slot(found, &n_found, stem);
        if (slot < 0) { ignored++; continue; }
        if (is_xi) {
            found[slot].has_xi = 1;
            snprintf(found[slot].xi_name, sizeof(found[slot].xi_name), "%s", entry->d_name);
        } else {
            found[slot].has_xo = 1;
            snprintf(found[slot].xo_name, sizeof(found[slot].xo_name), "%s", entry->d_name);
        }
    }
    closedir(dir);
    qsort(found, (size_t)n_found, sizeof(found[0]), xiam_load_candidate_cmp);

    int imported = 0, output_failed = 0, orphan_output = 0;
    for (int i = 0; i < n_found; i++) {
        if (!found[i].has_xi) { orphan_output++; continue; }
        char input_path[700];
        snprintf(input_path, sizeof(input_path), "%s/%s", directory, found[i].xi_name);
        if (!xiam_load_input_file(s, input_path)) { ignored++; continue; }
        imported++;
        if (!found[i].has_xo) continue;
        char output_path[700];
        snprintf(output_path, sizeof(output_path), "%s/%s", directory, found[i].xo_name);
        if (!xiam_load_output_file(s, output_path)) output_failed++;
    }
    if (imported) {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "Imported %d XIAM Hamiltonian%s from .fit/load%s%s.",
                 imported, imported == 1 ? "" : "s",
                 output_failed ? "; one or more .xo files were not readable" : "",
                 orphan_output ? "; .xo without a matching .xi was ignored" : "");
    } else if (orphan_output) {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "No XIAM input imported: .xo files need a same-stem .xi source in %s.", directory);
    } else if (!n_found) {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "No XIAM .xi/.xo files found in %s.", directory);
    } else {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "No XIAM input could be imported from %s.", directory);
    }
    (void)ignored;
    return imported;
}

int xiam_save_fit_file(AppState *s, const char *path) {
    if (!s || !s->xiam.loaded || !path || !path[0]) return 0;
    if (!xiam_write_input(s, path, 1)) {
        snprintf(s->xiam.status, sizeof(s->xiam.status), "Cannot save XIAM fit input: %s", strerror(errno));
        return 0;
    }
    snprintf(s->xiam.status, sizeof(s->xiam.status),
             "Saved XIAM fit input to %s; the imported .xi was left unchanged.", path);
    xiam_store_active(&s->xiam);
    return 1;
}

int xiam_calculate(AppState *s) {
    if (!xiam_run(s,0)) return 0;
    int ok=xiam_load_output_file(s,s->xiam.output_path);
    if (ok) snprintf(s->xiam.status,sizeof(s->xiam.status),"Prediction complete: %d lines; native SPCAT intensity is used unchanged.",s->n_pred);
    xiam_store_active(&s->xiam);
    return ok;
}

/* One entry per fit, tagged with its model: the newest entry of the active
 * Hamiltonian is what Undo fit restores, so a project with several .xi models
 * cannot undo one model's fit into another's constants. */
static int xiam_push_fit_snapshot(AppState *s) {
    XiamState *x = &s->xiam;
    if (x->history_count == x->history_capacity) {
        int capacity = x->history_capacity ? x->history_capacity * 2 : 4;
        XiamFitSnapshot *grown = realloc(x->history, (size_t)capacity * sizeof(*grown));
        if (!grown) {
            snprintf(x->status, sizeof(x->status), "Cannot reserve XIAM fit-history memory.");
            return 0;
        }
        x->history = grown;
        x->history_capacity = capacity;
    }
    XiamFitSnapshot *snap = &x->history[x->history_count++];
    snap->model_id = x->model_id;
    snap->n_params = x->n_params;
    memcpy(snap->params, x->params, sizeof(snap->params));
    return 1;
}

/* Restoring by native name, not by row: a constant added or removed after the
 * fit must not shift the previous values onto the wrong keywords. */
static int xiam_undo_last_fit(AppState *s) {
    if (!s) return 0;
    XiamState *x = &s->xiam;
    int at = -1;
    for (int i = x->history_count - 1; i >= 0; i--)
        if (x->history[i].model_id == x->model_id) { at = i; break; }
    if (at < 0) {
        snprintf(x->status, sizeof(x->status), "No earlier fit state for this Hamiltonian in this session.");
        return 0;
    }
    const XiamFitSnapshot *snap = &x->history[at];
    int restored = 0;
    for (int i = 0; i < snap->n_params; i++) {
        int pi = xiam_find_parameter(x, snap->params[i].name);
        if (pi < 0) continue;
        x->params[pi].value_ghz = snap->params[i].value_ghz;
        x->params[pi].error_ghz = snap->params[i].error_ghz;
        restored++;
    }
    for (int i = at; i + 1 < x->history_count; i++) x->history[i] = x->history[i + 1];
    x->history_count--;
    x->fit_output_path[0] = '\0';
    xiam_refresh_fit_summary(x);
    xiam_update_axis_convention(x);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    int predicted = xiam_calculate(s);
    snprintf(x->status, sizeof(x->status),
             predicted ? "Restored the constants of %d parameters from before the fit; prediction refreshed."
                       : "Restored the constants of %d parameters from before the fit; run Predict to refresh the catalogue.",
             restored);
    xiam_store_active(x);
    return 1;
}

int xiam_fit(AppState *s) {
    if (!xiam_push_fit_snapshot(s)) return 0;
    if (!xiam_run(s,1)) { s->xiam.history_count--; return 0; }
    int updated = xiam_parse_fit_parameters(&s->xiam, s->xiam.fit_output_path);
    if (!updated) {
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "XIAM completed but no fitted-parameter report was recognised; see %s.",
                 s->xiam.fit_output_path);
        /* Nothing was rewritten, so there is nothing to undo. */
        s->xiam.history_count--;
        xiam_store_active(&s->xiam);
        return 0;
    }
    xiam_refresh_fit_summary(&s->xiam);
    char quality[96] = "";
    if (s->xiam.has_fit_summary)
        snprintf(quality, sizeof(quality), "; std. deviation %.6g MHz after %d iterations",
                 s->xiam.fit_std_mhz, s->xiam.fit_iterations);
    int predicted = xiam_calculate(s);
    if (predicted)
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "Fit completed (%d constants)%s; fit.xo is retained and the updated MHz prediction is refreshed.", updated, quality);
    else
        snprintf(s->xiam.status, sizeof(s->xiam.status),
                 "Fit completed (%d constants)%s; fit.xo is retained, but the updated prediction could not be read.", updated, quality);
    xiam_store_active(&s->xiam);
    xiam_mark_session_dirty(s);
    /* The fit itself is valid even if the optional catalogue refresh fails. */
    return 1;
}

static int xiam_reload_active_input(AppState *s) {
    if (!s || !s->xiam.loaded || !s->xiam.input_path[0]) return 0;
    XiamState *x = &s->xiam;
    char path[512]; snprintf(path, sizeof(path), "%s", x->input_path);
    if (!xiam_read_source(x, path)) {
        snprintf(x->status, sizeof(x->status), "Cannot reload XIAM input: %s", path);
        return 0;
    }
    xiam_parse_source(s); x->model_id = xiam_model_id_from_path(path);
    int imported = xiam_import_source_assignments(s);
    snprintf(x->status, sizeof(x->status), "Reloaded %s; %d Block VIII transitions refreshed.",
             xiam_basename(path), imported);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    return 1;
}

static int xiam_simulate_checked(AppState *s) {
    if (!s || s->xiam.n_hamiltonians <= 0) return 0;
    XiamState *x = &s->xiam;
    int origin = x->active_hamiltonian, attempted = 0, completed = 0;
    xiam_store_active(x);
    for (int i = 0; i < x->n_hamiltonians; i++) {
        xiam_restore_active(x, i);
        if (x->simulate_excluded || !x->loaded) continue;
        attempted++;
        if (xiam_calculate(s)) completed++;
        xiam_store_active(x);
    }
    if (origin >= 0 && origin < x->n_hamiltonians) xiam_restore_active(x, origin);
    snprintf(x->status, sizeof(x->status), "Simulation completed for %d of %d selected XIAM Hamiltonians; their catalogues are overlaid.", completed, attempted);
    xiam_store_active(x);
    return attempted > 0 && completed == attempted;
}

static void xiam_remove_active_hamiltonian(AppState *s) {
    if (!s || s->xiam.active_hamiltonian < 0) return;
    XiamState *x = &s->xiam;
    int index = x->active_hamiltonian, id = x->model_id;
    int aa = 0;
    for (int i = 0; i < s->n_assignments; i++)
        if (s->assignments[i].hamiltonian_id != id) s->assignments[aa++] = s->assignments[i];
    s->n_assignments = aa; save_assignments(s);
    int pp = 0;
    for (int i = 0; i < s->n_pred; i++)
        if (s->pred_lines[i].backend != PRED_BACKEND_XIAM || s->pred_lines[i].hamiltonian_id != id)
            s->pred_lines[pp++] = s->pred_lines[i];
    s->n_pred = pp;
    for (int i = index; i + 1 < x->n_hamiltonians; i++) x->hamiltonian[i] = x->hamiltonian[i + 1];
    x->n_hamiltonians--;
    x->delete_hamiltonian_id = 0;
    if (x->n_hamiltonians) {
        int next = index < x->n_hamiltonians ? index : x->n_hamiltonians - 1;
        xiam_restore_active(x, next);
        snprintf(x->status, sizeof(x->status), "Removed XIAM Hamiltonian; its assignments and catalogue were removed too.");
        xiam_store_active(x);
    } else {
        xiam_clear_active(x); x->active_hamiltonian = -1;
        snprintf(x->status, sizeof(x->status), "No XIAM Hamiltonians are open.");
    }
    xiam_mark_session_dirty(s);
}

/* A copy of the active model, the way Pred&Fit's Duplicate works: the same
 * native source and constants under a new owner, so a variant can be fitted
 * without disturbing the model it came from.  The copy keeps its source
 * pathname so Reload still works; reopening that .xi selects the original,
 * because the project is searched in order. */
static int xiam_duplicate_active(AppState *s) {
    if (!s) return 0;
    XiamState *x = &s->xiam;
    if (x->active_hamiltonian < 0) {
        snprintf(x->status, sizeof(x->status), "Open a .xi input before duplicating a Hamiltonian.");
        return 0;
    }
    if (x->n_hamiltonians >= MAX_XIAM_HAMILTONIANS) {
        snprintf(x->status, sizeof(x->status), "A XIAM project supports at most %d Hamiltonians.", MAX_XIAM_HAMILTONIANS);
        return 0;
    }
    xiam_store_active(x);
    int index = x->n_hamiltonians++;
    x->hamiltonian[index] = x->hamiltonian[x->active_hamiltonian];
    XiamHamiltonian *copy = &x->hamiltonian[index];
    /* A distinct owner is what keeps the two models' assignments, catalogues
       and workspaces apart; derive it from the source path as usual. */
    int id = 0;
    for (int k = 1; k < 1000; k++) {
        char key[600];
        snprintf(key, sizeof(key), "%s#copy%d", copy->input_path, k);
        id = xiam_model_id_from_path(key);
        int clash = 0;
        for (int i = 0; i < x->n_hamiltonians; i++)
            if (i != index && x->hamiltonian[i].model_id == id) clash = 1;
        if (!clash) break;
    }
    copy->model_id = id;
    copy->work_dir[0] = copy->output_path[0] = copy->fit_output_path[0] = '\0';
    char base[96]; snprintf(base, sizeof(base), "%s", copy->name);
    snprintf(copy->name, sizeof(copy->name), "%.80s copy", base);
    /* A copy is a different model in the same plot: give it its own colour. */
    xiam_assign_default_trace_color(x, copy);
    xiam_restore_active(x, index);
    /* The .xi's own block VIII belongs to the model, so the copy gets its own
       independent observations rather than sharing the original's rows. */
    int imported = xiam_import_source_assignments(s);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    snprintf(x->status, sizeof(x->status),
             "%s created from the active Hamiltonian (%d transitions copied); calculate it before fitting.",
             copy->name, imported);
    return 1;
}

/* The sidebar order is the order of the project, exactly as in Pred&Fit. */
static int xiam_move_hamiltonian(AppState *s, int index, int delta) {
    if (!s) return 0;
    XiamState *x = &s->xiam;
    int to = index + delta;
    if (index < 0 || index >= x->n_hamiltonians || to < 0 || to >= x->n_hamiltonians) return 0;
    xiam_store_active(x);
    XiamHamiltonian swap = x->hamiltonian[index];
    x->hamiltonian[index] = x->hamiltonian[to];
    x->hamiltonian[to] = swap;
    if (x->active_hamiltonian == index) x->active_hamiltonian = to;
    else if (x->active_hamiltonian == to) x->active_hamiltonian = index;
    xiam_restore_active(x, x->active_hamiltonian);
    xiam_mark_session_dirty(s);
    snprintf(x->status, sizeof(x->status), "%s moved %s.", x->hamiltonian[to].name, delta < 0 ? "up" : "down");
    return 1;
}

static void xiam_format_transition(const PredLine *p, char *out, size_t size) {
    if (!p) { snprintf(out, size, "—"); return; }
    snprintf(out, size, "%d %d %d  –  %d %d %d", p->Ju, p->Kau, p->Kcu,
             p->Jl, p->Kal, p->Kcl);
}

static double xiam_assignment_calculated(const AppState *s, const Assignment *a) {
    if (!s || !a) return 0.0;
    for (int i = 0; i < s->n_pred; i++) {
        const PredLine *p = &s->pred_lines[i];
        if (p->backend == PRED_BACKEND_XIAM && p->hamiltonian_id == a->hamiltonian_id &&
            (xiam_same_transition(p, &a->pred) || xiam_same_input_transition(p, &a->pred)))
            return p->freq_mhz;
    }
    return 0.0;
}

static void xiam_delete_assignment(AppState *s, int index) {
    if (!s || index < 0 || index >= s->n_assignments) return;
    /* Keep the editable native project coherent too.  The imported source is
       never overwritten on disk, but a subsequently saved/generated .xi must
       not resurrect a row that the user deleted in Lines. */
    XiamState *x = &s->xiam;
    const Assignment *victim = &s->assignments[index];
    int block = 0;
    for (int line_no = 0; line_no < x->n_source_lines; line_no++) {
        char work[256], lower[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[line_no]);
        char *at = xiam_ltrim(work); xiam_lower(lower, sizeof(lower), at);
        if (xiam_is_comment(at)) {
            if (strstr(lower, "block viii")) block = 8;
            else if (strstr(lower, "block vii")) block = 7;
            continue;
        }
        if (block != 8) continue;
        PredLine source;
        if (!xiam_parse_input_transition_line(at, &source, NULL, NULL) ||
            !xiam_same_input_transition(&source, &victim->pred)) continue;
        char commented[256]; snprintf(commented, sizeof(commented), "\\%s", x->source_lines[line_no]);
        snprintf(x->source_lines[line_no], sizeof(x->source_lines[line_no]), "%s", commented);
        break;
    }
    for (int i = index; i + 1 < s->n_assignments; i++) s->assignments[i] = s->assignments[i + 1];
    s->n_assignments--; save_assignments(s);
    xiam_mark_session_dirty(s);
}

/* Pred&Fit can add and remove Pickett parameters; XIAM could only ever show
 * the constants its .xi happened to declare.  A new constant is a new block
 * III declaration in the retained source: that is the only place XIAM reads
 * a molecular parameter from, and the only place a value edit can reach. */
static int xiam_add_parameter(AppState *s) {
    if (!s) return -1;
    XiamState *x = &s->xiam;
    if (!x->loaded) {
        snprintf(x->status, sizeof(x->status), "Open a .xi input before adding a constant.");
        return -1;
    }
    if (x->n_params >= MAX_XIAM_PARAMS) {
        snprintf(x->status, sizeof(x->status), "A XIAM model supports at most %d constants.", MAX_XIAM_PARAMS);
        return -1;
    }
    int at = xiam_block_end(x, 3);
    if (at < 0) {
        snprintf(x->status, sizeof(x->status),
                 "This .xi has no block III: add a `\\ block III` heading before declaring constants.");
        return -1;
    }
    /* A name XIAM does not know is refused by XIAM, not silently kept: the
       placeholder therefore stays obviously unfinished until it is renamed. */
    char name[48] = "NEW";
    for (int n = 2; xiam_find_parameter(x, name) >= 0 && n < 1000; n++)
        snprintf(name, sizeof(name), "NEW%d", n);
    char line[256];
    snprintf(line, sizeof(line), " %-12s 0.0", name);
    int inserted = xiam_source_insert_line(x, at, line);
    if (inserted < 0) {
        snprintf(x->status, sizeof(x->status), "The retained .xi source is full; cannot add a constant.");
        return -1;
    }
    XiamParameter *p = &x->params[x->n_params++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->source_line = inserted;
    p->fit_source_line = -1;
    p->is_quadrupole = xiam_is_quadrupole(name);
    p->is_frequency = xiam_is_frequency(name);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    snprintf(x->status, sizeof(x->status),
             "Added a block III constant; rename it to a native XIAM keyword (BJ, DJ, chi_z, V1n, mu_z, ...).");
    return x->n_params - 1;
}

/* Removing a constant comments its native declaration out instead of
 * deleting the line: the historical value stays readable in the generated
 * .xi, exactly like a transition removed in Lines. */
static int xiam_delete_parameter(AppState *s, int row) {
    if (!s) return 0;
    XiamState *x = &s->xiam;
    if (row < 0 || row >= x->n_params) return 0;
    const XiamParameter *victim = &x->params[row];
    int lines[2] = {victim->source_line, victim->fit_source_line};
    for (int k = 0; k < 2; k++) {
        int at = lines[k];
        if (at < 0 || at >= x->n_source_lines) continue;
        char work[256];
        snprintf(work, sizeof(work), "%s", x->source_lines[at]);
        if (xiam_is_comment(work)) continue;
        snprintf(x->source_lines[at], sizeof(x->source_lines[0]), "\\%s", work);
    }
    char gone[48]; snprintf(gone, sizeof(gone), "%s", victim->name);
    for (int i = row; i + 1 < x->n_params; i++) x->params[i] = x->params[i + 1];
    x->n_params--;
    if (x->edit_param == row) { x->edit_param = XIAM_EDIT_NONE; SDL_StopTextInput(); }
    else if (x->edit_param > row) x->edit_param--;
    if (x->edit_name_row == row) { x->edit_name_row = -1; if (x->edit_param == XIAM_EDIT_PARAM_NAME) { x->edit_param = XIAM_EDIT_NONE; SDL_StopTextInput(); } }
    else if (x->edit_name_row > row) x->edit_name_row--;
    if (x->params_scroll > 0 && x->params_scroll >= x->n_params) x->params_scroll--;
    xiam_update_axis_convention(x);
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
    snprintf(x->status, sizeof(x->status), "Removed %s; its native declaration is kept commented out.", gone);
    return 1;
}

static void xiam_edit_begin(XiamState *x, int what) {
    x->edit_param=what; x->edit_replace=1;
    if (what >= 0) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",xiam_display_value(&x->params[what]));
    else if (what==XIAM_EDIT_CONTROL) {
        double v = 0.0; int i = x->edit_control;
        if (i >= 0 && i < 10) {
            xiam_control_value(x, XIAM_CONTROL_KEY[i], XIAM_CONTROL_DEFAULT[i], &v);
            snprintf(x->edit_buf,sizeof(x->edit_buf),XIAM_CONTROL_FORMAT[i],v);
        } else x->edit_buf[0]='\0';
    }
    else if (what==XIAM_EDIT_STATE_ROW) {
        x->edit_buf[0]='\0';
        if (x->edit_state_line >= 0 && x->edit_state_line < x->n_source_lines) {
            XiamStateRow row = {x->edit_state_line, 0, 0};
            xiam_state_row_text(x, &row, x->edit_buf, sizeof(x->edit_buf));
        }
    }
    else if (what==XIAM_EDIT_STATE_W) {
        double w = 1.0;
        if (x->edit_state_line >= 0 && x->edit_state_line < x->n_source_lines) {
            XiamStateRow row = {x->edit_state_line, 5, 0};
            w = xiam_state_row_w(x, &row);
        }
        snprintf(x->edit_buf, sizeof(x->edit_buf), "%.6g", w);
    }
    else if (what==XIAM_EDIT_LINE_DEFAULT)
        snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->line_error_mhz > 0.0 ? x->line_error_mhz : 0.010);
    else if (what==XIAM_EDIT_H_NAME)
        snprintf(x->edit_buf,sizeof(x->edit_buf),"%s",
                 x->edit_hamiltonian_row >= 0 && x->edit_hamiltonian_row < x->n_hamiltonians
                     ? x->hamiltonian[x->edit_hamiltonian_row].name : "");
    else if (what==XIAM_EDIT_PARAM_NAME)
        snprintf(x->edit_buf,sizeof(x->edit_buf),"%s",
                 x->edit_name_row >= 0 && x->edit_name_row < x->n_params ? x->params[x->edit_name_row].name : "");
    else if (what==XIAM_EDIT_FMIN) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->fmin_mhz);
    else if (what==XIAM_EDIT_FMAX) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->fmax_mhz);
    else if (what==XIAM_EDIT_TEMP) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->temperature_k);
    else if (what==XIAM_EDIT_CONCENTRATION) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->concentration);
    else snprintf(x->edit_buf,sizeof(x->edit_buf),"%d",x->max_j);
    x->edit_caret=x->edit_anchor=(int)strlen(x->edit_buf); SDL_StartTextInput();
}

static void xiam_edit_assignment_begin(AppState *s, int index, int field) {
    if (!s || index < 0 || index >= s->n_assignments) return;
    XiamState *x = &s->xiam;
    x->edit_assignment = index; x->edit_assignment_field = field;
    xiam_edit_begin(x, field == 0 ? XIAM_EDIT_LINE_OBS : XIAM_EDIT_LINE_ERR);
    snprintf(x->edit_buf, sizeof(x->edit_buf), "%.12g", field == 0 ?
             s->assignments[index].exp_freq :
             xiam_line_error(x, &s->assignments[index]));
    x->edit_caret = x->edit_anchor = (int)strlen(x->edit_buf);
}

static void xiam_edit_commit(AppState *s) {
    if (!s) return;
    XiamState *x = &s->xiam;
    if (x->edit_param == XIAM_EDIT_NONE) return;
    double v=xiam_number(x->edit_buf,NULL);
    if (x->edit_param >= 0 && x->edit_param < x->n_params) x->params[x->edit_param].value_ghz=xiam_source_value(&x->params[x->edit_param],v);
    else if (x->edit_param==XIAM_EDIT_FMIN) x->fmin_mhz=v;
    else if (x->edit_param==XIAM_EDIT_FMAX) x->fmax_mhz=v;
    else if (x->edit_param==XIAM_EDIT_TEMP && v>0.0) x->temperature_k=v;
    else if (x->edit_param==XIAM_EDIT_JMAX) x->max_j=(int)lround(v);
    else if (x->edit_param==XIAM_EDIT_CONCENTRATION && isfinite(v) && v>=0.0) {
        x->concentration=v;
        xiam_store_active(x);
        xiam_apply_concentrations(s);
        s->intensity_window.species_sync_pending = 1;
    }
    else if (x->edit_param == XIAM_EDIT_CONTROL) {
        int i = x->edit_control;
        if (i > 0 && i < 10 && isfinite(v))
            xiam_set_control(s, XIAM_CONTROL_KEY[i], v, XIAM_CONTROL_FORMAT[i]);
        x->edit_control = -1;
    }
    else if (x->edit_param == XIAM_EDIT_STATE_ROW) {
        int at = x->edit_state_line;
        char wanted[256]; snprintf(wanted, sizeof(wanted), "%s", xiam_ltrim(x->edit_buf));
        for (int i = (int)strlen(wanted) - 1; i >= 0 && isspace((unsigned char)wanted[i]); i--) wanted[i] = '\0';
        if (at >= 0 && at < x->n_source_lines && wanted[0] &&
            xiam_state_record_kind(wanted) == xiam_state_record_kind(x->source_lines[at])) {
            int enabled = !xiam_is_comment(x->source_lines[at]);
            snprintf(x->source_lines[at], sizeof(x->source_lines[0]),
                     enabled ? " %s" : "\\ %s", wanted);
        } else if (at >= 0) {
            snprintf(x->status, sizeof(x->status),
                     "A block V record is `/label S sigma` and a block VI record is `V n`.");
        }
        x->edit_state_line = -1;
    }
    else if (x->edit_param == XIAM_EDIT_STATE_W) {
        if (!isfinite(v) || v < 0.0 || !xiam_set_state_row_w(s, x->edit_state_line, v))
            snprintf(x->status, sizeof(x->status), "W (spin statistical weight) must be a number >= 0.");
        x->edit_state_line = -1;
    }
    else if (x->edit_param == XIAM_EDIT_LINE_DEFAULT) {
        if (isfinite(v) && v > 0.0) x->line_error_mhz = v;
    }
    else if (x->edit_param == XIAM_EDIT_H_NAME) {
        int row = x->edit_hamiltonian_row;
        char wanted[96]; snprintf(wanted, sizeof(wanted), "%s", xiam_ltrim(x->edit_buf));
        for (int i = (int)strlen(wanted) - 1; i >= 0 && isspace((unsigned char)wanted[i]); i--) wanted[i] = '\0';
        if (row >= 0 && row < x->n_hamiltonians && wanted[0]) {
            snprintf(x->hamiltonian[row].name, sizeof(x->hamiltonian[row].name), "%s", wanted);
            snprintf(x->status, sizeof(x->status), "Hamiltonian renamed to %s.", wanted);
        } else if (row >= 0) {
            snprintf(x->status, sizeof(x->status), "A Hamiltonian name cannot be empty.");
        }
        x->edit_hamiltonian_row = -1;
    }
    else if (x->edit_param == XIAM_EDIT_PARAM_NAME) {
        int row = x->edit_name_row;
        char wanted[48]; snprintf(wanted, sizeof(wanted), "%s", xiam_ltrim(x->edit_buf));
        for (int i = (int)strlen(wanted) - 1; i >= 0 && isspace((unsigned char)wanted[i]); i--) wanted[i] = '\0';
        int clash = xiam_find_parameter(x, wanted);
        if (row < 0 || row >= x->n_params) {
            /* nothing to rename */
        } else if (!wanted[0] || strchr(wanted, ' ') || strchr(wanted, '\t')) {
            snprintf(x->status, sizeof(x->status), "A XIAM parameter name is a single native keyword.");
        } else if (clash >= 0 && clash != row) {
            snprintf(x->status, sizeof(x->status), "%s is already declared in this model.", wanted);
        } else {
            XiamParameter *p = &x->params[row];
            snprintf(p->name, sizeof(p->name), "%s", wanted);
            p->is_quadrupole = xiam_is_quadrupole(wanted);
            p->is_frequency = xiam_is_frequency(wanted);
            if (p->source_line >= 0 && p->source_line < x->n_source_lines) {
                char changed[256];
                xiam_replace_keyword(changed, sizeof(changed), x->source_lines[p->source_line], wanted);
                snprintf(x->source_lines[p->source_line], sizeof(x->source_lines[0]), "%s", changed);
            }
            /* The fit directive is regenerated from the name on every run, so
               an old `fit <previous name>` card must not survive. */
            if (p->fit_source_line >= 0 && p->fit_source_line < x->n_source_lines) {
                char changed[256];
                snprintf(changed, sizeof(changed), " %s %s",
                         p->fit_method == 2 ? "dqu" : p->fit_method == 3 ? "dqx" : "fit", wanted);
                snprintf(x->source_lines[p->fit_source_line], sizeof(x->source_lines[0]), "%s", changed);
            }
            xiam_reposition_parameter(x, row);
            xiam_update_axis_convention(x);
            snprintf(x->status, sizeof(x->status), "Renamed the constant to %s.", wanted);
        }
        x->edit_name_row = -1;
    }
    else if ((x->edit_param == XIAM_EDIT_LINE_OBS || x->edit_param == XIAM_EDIT_LINE_ERR) &&
             x->edit_assignment >= 0 && x->edit_assignment < s->n_assignments) {
        Assignment *a = &s->assignments[x->edit_assignment];
        if (x->edit_param == XIAM_EDIT_LINE_OBS && isfinite(v)) a->exp_freq = v;
        else if (x->edit_param == XIAM_EDIT_LINE_ERR && isfinite(v) && v > 0.0) a->line_error_mhz = v;
        save_assignments(s);
    }
    x->edit_param=XIAM_EDIT_NONE; x->edit_assignment=-1; SDL_StopTextInput();
    xiam_store_active(x);
    xiam_mark_session_dirty(s);
}

/* Field editing below is deliberately the same as Pred&Fit's: a workspace
 * where Home, Delete, shift-selection or ⌘V do nothing is not the same
 * program as the one next to it. */
static void xiam_edit_clamp(XiamState *x) {
    int n = (int)strlen(x->edit_buf);
    if (x->edit_caret < 0) x->edit_caret = 0;
    if (x->edit_caret > n) x->edit_caret = n;
    if (x->edit_anchor < 0) x->edit_anchor = 0;
    if (x->edit_anchor > n) x->edit_anchor = n;
}

static int xiam_edit_delete_selection(XiamState *x) {
    xiam_edit_clamp(x);
    int lo = x->edit_caret < x->edit_anchor ? x->edit_caret : x->edit_anchor;
    int hi = x->edit_caret > x->edit_anchor ? x->edit_caret : x->edit_anchor;
    if (lo == hi) return 0;
    memmove(x->edit_buf + lo, x->edit_buf + hi, strlen(x->edit_buf + hi) + 1);
    x->edit_anchor = x->edit_caret = lo;
    return 1;
}

static void xiam_edit_insert(XiamState *x, const char *text) {
    if (!text || !*text) return;
    /* The whole value is selected when an edit opens, so the first keystroke
       replaces it: keep that by turning the flag into a real selection. */
    if (x->edit_replace) { x->edit_anchor = 0; x->edit_caret = (int)strlen(x->edit_buf); x->edit_replace = 0; }
    xiam_edit_delete_selection(x);
    int n = (int)strlen(x->edit_buf), add = (int)strlen(text);
    int room = (int)sizeof(x->edit_buf) - 1 - n;
    if (add > room) add = room;
    if (add <= 0) return;
    memmove(x->edit_buf + x->edit_caret + add, x->edit_buf + x->edit_caret,
            (size_t)(n - x->edit_caret) + 1);
    memcpy(x->edit_buf + x->edit_caret, text, (size_t)add);
    x->edit_caret += add;
    x->edit_anchor = x->edit_caret;
}

static void xiam_edit_copy(XiamState *x, int cut) {
    xiam_edit_clamp(x);
    int lo = x->edit_caret < x->edit_anchor ? x->edit_caret : x->edit_anchor;
    int hi = x->edit_caret > x->edit_anchor ? x->edit_caret : x->edit_anchor;
    if (lo == hi) return;
    char text[sizeof(x->edit_buf)];
    int n = hi - lo;
    memcpy(text, x->edit_buf + lo, (size_t)n);
    text[n] = '\0';
    SDL_SetClipboardText(text);
    if (cut) { x->edit_replace = 0; xiam_edit_delete_selection(x); }
}

static void xiam_edit_paste(XiamState *x) {
    char *clip = SDL_GetClipboardText();
    if (!clip) return;
    char clean[sizeof(x->edit_buf)];
    int n = 0;
    for (const char *q = clip; *q && n < (int)sizeof(clean) - 1; q++)
        if (*q != '\r' && *q != '\n' && *q != '\t') clean[n++] = *q;
    clean[n] = '\0';
    SDL_free(clip);
    xiam_edit_insert(x, clean);
}

/* Where a click lands inside an edited cell: the caret goes between the two
 * characters the pointer is closest to. */
static void xiam_place_caret(XiamState *x, int font, int text_x, int click_x) {
    const char *text = x->edit_buf;
    int n = (int)strlen(text), best = 0;
    double best_d = 1e18;
    for (int i = 0; i <= n; i++) {
        double d = fabs((double)(text_x + xiam_prefix_w(font, text, i)) - (double)click_x);
        if (d < best_d) { best_d = d; best = i; }
    }
    x->edit_caret = x->edit_anchor = best;
    x->edit_replace = 0;
}

/* Keep file selection in the dedicated XIAM workflow.  Drag-and-drop and the
 * command line remain useful for batch work; this is the ordinary interactive
 * route on macOS. */
static int xiam_pick_input(char *out, size_t size) {
#ifdef __APPLE__
    FILE *fp = popen("osascript -e 'POSIX path of (choose file with prompt \"Select XIAM .xi input\")' 2>/dev/null", "r");
    if (!fp) return 0;
    char path[600] = ""; int ok = fgets(path, sizeof(path), fp) != NULL;
    pclose(fp); path[strcspn(path, "\r\n")] = '\0';
    if (ok && path[0]) { snprintf(out, size, "%s", path); return 1; }
#else
    (void)out; (void)size;
#endif
    return 0;
}

static int xiam_pick_fit_destination(char *out, size_t size) {
#ifdef __APPLE__
    FILE *fp = popen("osascript -e 'POSIX path of (choose file name with prompt \"Save XIAM fit input\" default name \"xiam_fit.xi\")' 2>/dev/null", "r");
    if (!fp) return 0;
    char path[600] = ""; int ok = fgets(path, sizeof(path), fp) != NULL;
    pclose(fp); path[strcspn(path, "\r\n")] = '\0';
    if (ok && path[0]) { snprintf(out, size, "%s", path); return 1; }
#else
    (void)out; (void)size;
#endif
    return 0;
}

/* Every rectangle the XIAM workspace draws is derived here, exactly like
 * Pred&Fit's AdvUI.  The renderer, the wheel handler and the click handler all
 * read the same geometry: a table whose scroll limit was computed from a
 * different row count than the one being drawn is how rows became unreachable
 * in the Hamiltonians and Simulation pages. */
typedef struct {
    int w, h, row_h, rows_visible;
    SDL_Rect tab[XIAM_TAB_COUNT], caption, subcaption, nav, nav_rows, table, rows, footer;
    SDL_Rect btn_a, btn_b, btn_c, btn_d;
    /* The sidebar is on every page, and it manages the project: the same
       pulsantiera as Pred&Fit's, plus the two .xi-specific actions. */
    SDL_Rect nav_up, nav_down, nav_open, nav_reload, nav_rename, nav_duplicate, nav_delete;
    int nav_rows_visible;
    /* Parameters: the run-control strip pushes its own table down. */
    SDL_Rect fmin, fmax, temp, conc, jmax, axis, unc;
    SDL_Rect param_table, param_rows;
    int param_rows_visible;
    /* States: the block II control record above its own table. */
    SDL_Rect state_cell[10], state_table, state_rows;
    int state_rows_visible;
    /* Fitting: fitted constants above, residuals below. */
    int split;
    SDL_Rect fit_param_rows, fit_obs_rows;
    int fit_param_rows_visible, fit_obs_rows_visible;
} XiamUI;

static XiamUI xiam_ui(XiamState *x) {
    XiamUI u; memset(&u, 0, sizeof(u));
    SDL_GetWindowSize(x->window, &u.w, &u.h);
    /* The control strip of Parameters is wider than Pred&Fit's PAR option
       line, so the workspace needs a wider minimum before anything in the
       footer would slide under the uncertainty field. */
    if (u.w < XIAM_MIN_W) u.w = XIAM_MIN_W;
    if (u.h < XIAM_MIN_H) u.h = XIAM_MIN_H;
    const int widths[XIAM_TAB_COUNT] = {124, 84, 92, 88, 130, 116};
    int tx = XIAM_PAD - 2;
    for (int i = 0; i < XIAM_TAB_COUNT; i++) { u.tab[i] = (SDL_Rect){tx, XIAM_TAB_Y, widths[i], XIAM_TAB_H}; tx += widths[i] + 7; }
    u.footer = (SDL_Rect){0, u.h - XIAM_FOOTER_H, u.w, XIAM_FOOTER_H};
    u.nav = (SDL_Rect){XIAM_PAD, XIAM_CONTENT_Y, XIAM_NAV_W, u.footer.y - XIAM_CONTENT_Y - 10};
    u.nav_rows = (SDL_Rect){u.nav.x + 2, u.nav.y + 32, u.nav.w - 4, u.nav.h - 238};
    if (u.nav_rows.h < 27) u.nav_rows.h = 27;
    u.nav_up = (SDL_Rect){u.nav.x + 10, u.nav.y + u.nav.h - 200, (u.nav.w - 26) / 2, 26};
    u.nav_down = (SDL_Rect){u.nav_up.x + u.nav_up.w + 6, u.nav_up.y, u.nav_up.w, 26};
    u.nav_open = (SDL_Rect){u.nav.x + 10, u.nav.y + u.nav.h - 166, u.nav.w - 20, 28};
    u.nav_reload = (SDL_Rect){u.nav.x + 10, u.nav.y + u.nav.h - 130, u.nav.w - 20, 26};
    u.nav_rename = (SDL_Rect){u.nav.x + 10, u.nav.y + u.nav.h - 96, (u.nav.w - 26) / 2, 26};
    u.nav_duplicate = (SDL_Rect){u.nav_rename.x + u.nav_rename.w + 6, u.nav_rename.y, u.nav_rename.w, 26};
    u.nav_delete = (SDL_Rect){u.nav.x + 10, u.nav.y + u.nav.h - 62, u.nav.w - 20, 26};
    int content_x = XIAM_PAD + XIAM_NAV_W + 12;
    u.caption = (SDL_Rect){content_x, XIAM_CONTENT_Y, u.w - content_x - XIAM_PAD, 18};
    u.subcaption = (SDL_Rect){content_x, XIAM_CONTENT_Y + 16, u.caption.w, 16};
    u.table = (SDL_Rect){content_x, XIAM_CONTENT_Y + 38, u.caption.w, u.footer.y - XIAM_CONTENT_Y - 48};
    u.rows = (SDL_Rect){u.table.x + 1, u.table.y + 33, u.table.w - 2, u.table.h - 35};
    u.row_h = 27; u.rows_visible = u.rows.h / u.row_h;
    if (u.rows_visible < 1) u.rows_visible = 1;
    u.nav_rows_visible = u.nav_rows.h / u.row_h;
    if (u.nav_rows_visible < 1) u.nav_rows_visible = 1;

    u.fmin = (SDL_Rect){u.table.x,       u.table.y + 22, 130, 28};
    u.fmax = (SDL_Rect){u.table.x + 140, u.table.y + 22, 130, 28};
    u.temp = (SDL_Rect){u.table.x + 280, u.table.y + 22, 105, 28};
    u.conc = (SDL_Rect){u.table.x + 395, u.table.y + 22,  90, 28};
    u.jmax = (SDL_Rect){u.table.x + 495, u.table.y + 22,  75, 28};
    u.axis = (SDL_Rect){u.table.x + 580, u.table.y + 22, 210, 28};
    u.unc  = (SDL_Rect){u.w - XIAM_PAD - 300, u.footer.y + 8, 300, 30};
    u.param_table = (SDL_Rect){u.table.x, u.table.y + 64, u.table.w, u.table.h - 64};
    u.param_rows  = (SDL_Rect){u.param_table.x + 1, u.param_table.y + 31,
                               u.param_table.w - 2, u.param_table.h - 33};
    u.param_rows_visible = u.param_rows.h / u.row_h;
    if (u.param_rows_visible < 1) u.param_rows_visible = 1;

    {
        int gap = 6, cw = (u.table.w - 4 * gap) / 5;
        for (int i = 0; i < 10; i++)
            u.state_cell[i] = (SDL_Rect){u.table.x + (i % 5) * (cw + gap),
                                        u.table.y + 22 + (i / 5) * 33, cw, 29};
    }
    u.state_table = (SDL_Rect){u.table.x, u.table.y + 96, u.table.w, u.table.h - 96};
    u.state_rows = (SDL_Rect){u.state_table.x + 1, u.state_table.y + 31,
                              u.state_table.w - 2, u.state_table.h - 33};
    u.state_rows_visible = u.state_rows.h / u.row_h;
    if (u.state_rows_visible < 1) u.state_rows_visible = 1;

    u.split = u.table.y + (u.table.h * 38) / 100;
    u.fit_param_rows = (SDL_Rect){u.table.x + 1, u.table.y + 33,
                                  u.table.w - 2, u.split - u.table.y - 35};
    u.fit_param_rows_visible = (u.split - (u.table.y + 33) - 2) / u.row_h;
    if (u.fit_param_rows_visible < 1) u.fit_param_rows_visible = 1;
    u.fit_obs_rows = (SDL_Rect){u.table.x + 1, u.split + 32, u.table.w - 2,
                                u.table.y + u.table.h - u.split - 34};
    u.fit_obs_rows_visible = (u.table.y + u.table.h - u.split - 34) / u.row_h;
    if (u.fit_obs_rows_visible < 1) u.fit_obs_rows_visible = 1;

    u.btn_a = (SDL_Rect){content_x, u.footer.y + 8, 150, 30};
    u.btn_b = (SDL_Rect){u.btn_a.x + u.btn_a.w + 10, u.footer.y + 8, 150, 30};
    u.btn_c = (SDL_Rect){u.btn_b.x + u.btn_b.w + 10, u.footer.y + 8, 150, 30};
    u.btn_d = (SDL_Rect){u.btn_c.x + u.btn_c.w + 10, u.footer.y + 8, 168, 30};
    return u;
}

static int xiam_scroll_limit(int total, int visible) {
    int max_scroll = total - visible;
    return max_scroll > 0 ? max_scroll : 0;
}

/* One clamp for every list in the workspace, applied before drawing and
 * before acting on a click.  A scroll offset can otherwise survive a window
 * resize, a deleted row or a Hamiltonian removal and leave the page blank. */
static void xiam_clamp_scrolls(AppState *s, const XiamUI *u) {
    XiamState *x = &s->xiam;
    int assignments = xiam_assignment_count(s, NULL);
    /* The States rows are counted by walking the native source, so only the
       page that actually scrolls them pays for it; the others keep their
       offset until they are opened. */
    int states = -1;
    if (x->active_tab == 3) {
        XiamStateRow probe[MAX_XIAM_STATE_ROWS];
        states = xiam_state_rows(x, probe, MAX_XIAM_STATE_ROWS);
    }
    int *scroll[] = {&x->nav_scroll, &x->params_scroll, &x->lines_scroll,
                     &x->fit_params_scroll, &x->fitting_scroll, &x->states_scroll,
                     &x->hamiltonians_scroll, &x->simulation_scroll};
    int limit[] = {xiam_scroll_limit(x->n_hamiltonians, u->nav_rows_visible),
                   xiam_scroll_limit(x->n_params, u->param_rows_visible),
                   xiam_scroll_limit(assignments, u->rows_visible),
                   xiam_scroll_limit(xiam_fitted_parameter_count(x), u->fit_param_rows_visible),
                   xiam_scroll_limit(assignments, u->fit_obs_rows_visible),
                   states < 0 ? INT_MAX : xiam_scroll_limit(states, u->state_rows_visible),
                   xiam_scroll_limit(x->n_hamiltonians, u->rows_visible),
                   xiam_scroll_limit(x->n_hamiltonians, u->rows_visible)};
    for (size_t i = 0; i < sizeof(scroll) / sizeof(scroll[0]); i++) {
        if (*scroll[i] < 0) *scroll[i] = 0;
        if (*scroll[i] > limit[i]) *scroll[i] = limit[i];
    }
}

static const char *xiam_axis_button_text(const XiamState *x) {
    if (x->axis_override == 1) return "Axes: prolate (manual)";
    if (x->axis_override == 2) return "Axes: oblate (manual)";
    if (x->axis_auto_convention == 1) return "Axes: auto prolate";
    if (x->axis_auto_convention == 2) return "Axes: auto oblate";
    return "Axes: automatic unknown";
}

/* Tables in the XIAM workspace use the same typography convention as the
 * SPFIT report: ordinary numbers finish at the right edge of their column,
 * while uncertainties share the decimal point.  The latter matters when a
 * fit mixes e.g. 0.01 MHz experimental errors and much smaller fitted
 * constant uncertainties. */
static void xiam_text_decimal(SDL_Renderer *r, int role, const char *text,
                              int decimal_x, int y, SDL_Color color) {
    if (!text || !text[0]) return;
    const char *pivot = strchr(text, '.');
    if (!pivot) {
        pivot = strchr(text, 'e');
        if (!pivot) pivot = strchr(text, 'E');
        if (!pivot) pivot = text + strlen(text);
    }
    char prefix[128];
    size_t n = (size_t)(pivot - text);
    if (n >= sizeof(prefix)) n = sizeof(prefix) - 1;
    memcpy(prefix, text, n); prefix[n] = '\0';
    ui_text(r, role, text, decimal_x - ui_text_w(role, prefix), y, color);
}

static int xiam_prefix_w(int font, const char *text, int n) {
    char prefix[sizeof(((XiamState *)0)->edit_buf)];
    if (n < 0) n = 0;
    if (n > (int)sizeof(prefix) - 1) n = (int)sizeof(prefix) - 1;
    memcpy(prefix, text, (size_t)n);
    prefix[n] = '\0';
    return ui_text_w(font, prefix);
}

/* An edited cell always shows its selection and its caret, like Pred&Fit's
 * tables: a field whose cursor cannot be seen cannot be aimed at. */
static void xiam_edit_text(SDL_Renderer *r, int font, const XiamState *x,
                           int px, int py, SDL_Color c) {
    const char *text = x->edit_buf;
    int h = ui_text_h(font);
    int lo = x->edit_caret < x->edit_anchor ? x->edit_caret : x->edit_anchor;
    int hi = x->edit_caret > x->edit_anchor ? x->edit_caret : x->edit_anchor;
    if (hi > lo) {
        int x0 = px + xiam_prefix_w(font, text, lo), x1 = px + xiam_prefix_w(font, text, hi);
        ui_fill(r, (SDL_Rect){x0, py - 1, x1 - x0, h + 2}, UI_ACCENT_SOFT);
    }
    ui_text(r, font, text, px, py, c);
    ui_fill(r, (SDL_Rect){px + xiam_prefix_w(font, text, x->edit_caret), py - 1, 2, h + 2}, UI_ACCENT);
}

static void xiam_cell_text(SDL_Renderer *r, int font, const XiamState *x, int editing,
                           const char *text, int px, int py, SDL_Color c) {
    if (editing) xiam_edit_text(r, font, x, px, py, c);
    else ui_text(r, font, text, px, py, c);
}

/* Kept intentionally identical to the SPFIT residual scale. */
static SDL_Color xiam_residual_color(double z) {
    double a = fabs(z);
    if (a > 4.0) return (SDL_Color){229,  83,  75, 255};
    if (a > 2.5) return (SDL_Color){232, 138, 58, 255};
    if (a > 1.5) return (SDL_Color){226, 190,  70, 255};
    return (SDL_Color){ 70, 196, 138, 255};
}

/* The stored value is right-aligned on its column; while it is edited the
 * text is drawn from that same starting x so the caret does not travel with
 * the digits.  The click handler needs the identical number to aim a caret. */
static int xiam_param_value_text_x(const XiamUI *u, const XiamState *x, int row) {
    char b[64];
    snprintf(b, sizeof(b), "%.12g",
             row >= 0 && row < x->n_params ? xiam_display_value(&x->params[row]) : 0.0);
    int at = u->param_table.x + u->param_table.w * 52 / 100 - ui_text_w(UI_FONT_MONO_SM, b);
    int left = u->param_table.x + u->param_table.w * 30 / 100 + 8;
    return at < left ? left : at;
}

/* Same contract for the two editable cells of Lines. */
static int xiam_line_obs_text_x(const XiamUI *u, const Assignment *a) {
    char b[64]; snprintf(b, sizeof(b), "%.6f", a ? a->exp_freq : 0.0);
    int at = u->table.x + u->table.w * 55 / 100 - ui_text_w(UI_FONT_MONO_SM, b);
    int left = u->table.x + u->table.w * 38 / 100;
    return at < left ? left : at;
}

static int xiam_line_unc_text_x(const XiamUI *u, const XiamState *x, const Assignment *a) {
    char b[64];
    double unc = xiam_line_error(x, a);
    snprintf(b, sizeof(b), "%.6g", unc);
    const char *pivot = strchr(b, '.');
    if (!pivot) pivot = b + strlen(b);
    char prefix[64]; size_t n = (size_t)(pivot - b);
    if (n >= sizeof(prefix)) n = sizeof(prefix) - 1;
    memcpy(prefix, b, n); prefix[n] = '\0';
    return u->table.x + u->table.w * 84 / 100 - ui_text_w(UI_FONT_MONO_SM, prefix);
}

/* Pred&Fit prints the page title and, beside it, what the last run said.
 * Every XIAM page does the same through here, with the page's own
 * explanation on the line below. */
static void xiam_page_caption(SDL_Renderer *r, const XiamState *x, const XiamUI *u,
                              const char *title, const char *explain, SDL_Color explain_c) {
    ui_text(r, UI_FONT_SANS, title, u->caption.x, u->caption.y, UI_ACCENT_TEXT);
    int at = u->caption.x + ui_text_w(UI_FONT_SANS, title) + 20;
    const char *status = x->status[0] ? x->status : NULL;
    if (status && at < u->caption.x + u->caption.w - 60)
        ui_text(r, UI_FONT_MONO_SM, status, at, u->caption.y + 2, UI_TEXT);
    if (explain) ui_text(r, UI_FONT_SANS_SM, explain, u->subcaption.x, u->subcaption.y, explain_c);
}

static void xiam_render_navigator(SDL_Renderer *r, XiamState *x, const XiamUI *u,
                                  int mx, int my, int down) {
    ui_fill(r, u->nav, UI_INPUT); ui_frame(r, u->nav, UI_LINE);
    ui_text(r, UI_FONT_MONO_SM, "XIAM HAMILTONIANS", u->nav.x + 10, u->nav.y + 10, UI_DIM);
    ui_hline(r, u->nav.x + 2, u->nav.x + u->nav.w - 2, u->nav_rows.y - 3, UI_LINE);
    SDL_RenderSetClipRect(r, &u->nav_rows);
    for (int visible = 0; visible < u->nav_rows_visible; visible++) {
        int i = x->nav_scroll + visible;
        if (i >= x->n_hamiltonians) break;
        int y = u->nav_rows.y + visible * u->row_h;
        SDL_Rect row = {u->nav_rows.x, y, u->nav_rows.w, u->row_h - 2};
        if (i == x->active_hamiltonian) ui_fill(r, row, UI_ACCENT_SOFT);
        else if (visible & 1) ui_fill(r, row, UI_CHROME);
        char b[160];
        int editing = x->edit_param == XIAM_EDIT_H_NAME && x->edit_hamiltonian_row == i;
        snprintf(b, sizeof(b), "H%d  ", i + 1);
        int prefix_w = ui_text_w(UI_FONT_SANS_SM, b);
        SDL_Color name_c = i == x->active_hamiltonian ? UI_ACCENT_TEXT : UI_TEXT;
        ui_text(r, UI_FONT_SANS_SM, b, row.x + 10, row.y + 6, UI_DIM);
        xiam_cell_text(r, UI_FONT_SANS_SM, x, editing, x->hamiltonian[i].name,
                       row.x + 10 + prefix_w, row.y + 6, editing ? UI_ACCENT_TEXT : name_c);
    }
    SDL_RenderSetClipRect(r, NULL);
    if (x->n_hamiltonians > u->nav_rows_visible) {
        char b[64];
        snprintf(b, sizeof(b), "%d of %d", u->nav_rows_visible, x->n_hamiltonians);
        ui_text_right(r, UI_FONT_SANS_SM, b, u->nav.x + u->nav.w - 10, u->nav.y + 10, UI_FAINT);
    }
    int many = x->n_hamiltonians > 1;
    ui_button(r, u->nav_up, "H up", -1, UI_BTN_QUIET, 0, many ? mx : -999, my, down);
    ui_button(r, u->nav_down, "H down", -1, UI_BTN_QUIET, 0, many ? mx : -999, my, down);
    ui_button(r, u->nav_open, "+ Open .xi Hamiltonian", -1, UI_BTN_PRIMARY, 0, mx, my, down);
    ui_button(r, u->nav_reload, "Reload active .xi", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->nav_rename, "Rename", -1, UI_BTN_QUIET,
              x->edit_param == XIAM_EDIT_H_NAME, mx, my, down);
    ui_button(r, u->nav_duplicate, "Duplicate", -1, UI_BTN_QUIET, 0, mx, my, down);
    int confirming = x->active_hamiltonian >= 0 && x->delete_hamiltonian_id == x->model_id;
    ui_button(r, u->nav_delete, x->n_hamiltonians ? (confirming ? "Confirm" : "Delete") : "Empty", -1,
              x->n_hamiltonians ? UI_BTN_DANGER : UI_BTN_QUIET, confirming, mx, my, down);
}

static void xiam_render_parameters(SDL_Renderer *r, AppState *s, XiamState *x,
                                   const XiamUI *u, int mx, int my, int down) {
    char b[256];
    xiam_page_caption(r, x, u, "Parameters — native XIAM values and fit directives",
                      "Every frequency is shown in MHz; quadrupole values are XIAM / 2, like SPFIT.", UI_DIM);
    SDL_Rect fmin = u->fmin, fmax = u->fmax, temp = u->temp;
    SDL_Rect conc = u->conc, jmax = u->jmax, axis = u->axis;
    ui_text(r, UI_FONT_SANS_SM, "F min / MHz", fmin.x, fmin.y - 15, UI_FAINT);
    ui_text(r, UI_FONT_SANS_SM, "F max / MHz", fmax.x, fmax.y - 15, UI_FAINT);
    ui_text(r, UI_FONT_SANS_SM, "T / K", temp.x, temp.y - 15, UI_FAINT);
    ui_text(r, UI_FONT_SANS_SM, "Concentration", conc.x, conc.y - 15, UI_FAINT);
    ui_text(r, UI_FONT_SANS_SM, "Jmax", jmax.x, jmax.y - 15, UI_FAINT);
    snprintf(b, sizeof(b), "%.10g", x->fmin_mhz); ui_field_ex(r, fmin, NULL, x->edit_param == XIAM_EDIT_FMIN ? x->edit_buf : b, NULL, x->edit_param == XIAM_EDIT_FMIN, x->edit_caret, x->edit_anchor);
    snprintf(b, sizeof(b), "%.10g", x->fmax_mhz); ui_field_ex(r, fmax, NULL, x->edit_param == XIAM_EDIT_FMAX ? x->edit_buf : b, NULL, x->edit_param == XIAM_EDIT_FMAX, x->edit_caret, x->edit_anchor);
    snprintf(b, sizeof(b), "%.8g", x->temperature_k); ui_field_ex(r, temp, NULL, x->edit_param == XIAM_EDIT_TEMP ? x->edit_buf : b, NULL, x->edit_param == XIAM_EDIT_TEMP, x->edit_caret, x->edit_anchor);
    snprintf(b, sizeof(b), "%.8g", x->concentration); ui_field_ex(r, conc, NULL, x->edit_param == XIAM_EDIT_CONCENTRATION ? x->edit_buf : b, NULL, x->edit_param == XIAM_EDIT_CONCENTRATION, x->edit_caret, x->edit_anchor);
    snprintf(b, sizeof(b), "%d", x->max_j); ui_field_ex(r, jmax, NULL, x->edit_param == XIAM_EDIT_JMAX ? x->edit_buf : b, NULL, x->edit_param == XIAM_EDIT_JMAX, x->edit_caret, x->edit_anchor);
    ui_button(r, axis, xiam_axis_button_text(x), -1, UI_BTN_QUIET, 0, mx, my, down);
    SDL_Rect table = u->param_table, rows = u->param_rows;
    ui_fill(r, table, UI_INPUT); ui_frame(r, table, UI_LINE);
    int value_right = table.x + table.w * 52 / 100;
    int error_right = table.x + table.w * 71 / 100;
    int error_decimal = table.x + table.w * 66 / 100;
    int fit_x = table.x + table.w * 73 / 100;
    ui_text(r, UI_FONT_MONO_SM, "PARAMETER", table.x + 12, table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "VALUE / MHz", value_right, table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "FIT ERROR", error_right, table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "FIT (click cycles)", fit_x, table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "DEL", table.x + table.w - 10, table.y + 9, UI_DIM);
    ui_hline(r, table.x + 2, table.x + table.w - 2, rows.y - 3, UI_LINE);
    SDL_RenderSetClipRect(r, &rows);
    for (int visible = 0; visible < u->param_rows_visible; visible++) {
        int i = x->params_scroll + visible; if (i >= x->n_params) break;
        int y = rows.y + visible * u->row_h; SDL_Rect row = {rows.x, y, rows.w, u->row_h - 2};
        int editing_name = x->edit_param == XIAM_EDIT_PARAM_NAME && x->edit_name_row == i;
        int editing_value = x->edit_param == i;
        if (editing_name || editing_value) ui_fill(r, row, UI_ACCENT_SOFT);
        else if (visible & 1) ui_fill(r, row, UI_CHROME);
        XiamParameter *p = &x->params[i]; char label[96]; xiam_parameter_label(x, p, label, sizeof(label));
        xiam_cell_text(r, UI_FONT_MONO_SM, x, editing_name, label, row.x + 10, y + 6,
                       editing_name ? UI_ACCENT_TEXT : UI_TEXT);
        snprintf(b, sizeof(b), "%.12g", xiam_display_value(p));
        if (editing_value)
            xiam_edit_text(r, UI_FONT_MONO_SM, x, xiam_param_value_text_x(u, x, i), y + 6, UI_ACCENT_TEXT);
        else ui_text_right(r, UI_FONT_MONO_SM, b, value_right, y + 6, UI_TEXT);
        if (p->error_ghz != 0.0) { snprintf(b, sizeof(b), "%.6g", xiam_display_error(p)); xiam_text_decimal(r, UI_FONT_MONO_SM, b, error_decimal, y + 6, UI_DIM); }
        ui_text(r, UI_FONT_MONO_SM, p->fit_method == 1 ? "fit" : p->fit_method == 2 ? "dqu" : p->fit_method == 3 ? "dqx" : "—", fit_x, y + 6, p->fit_method ? UI_ACCENT_TEXT : UI_FAINT);
        ui_draw_icon(r, UI_ICON_CLOSE, (SDL_Rect){table.x + table.w - 26, y + (u->row_h - 2 - 14) / 2, 14, 14}, UI_DANGER_TEXT);
    }
    SDL_RenderSetClipRect(r, NULL);
    ui_button(r, u->btn_a, "+ parameter", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_b, "Predict", -1, UI_BTN_PRIMARY, 0, mx, my, down);
    ui_button(r, u->btn_c, "Save fit .xi…", -1, UI_BTN_QUIET, 0, mx, my, down);
    {
        char unc[96];
        snprintf(unc, sizeof(unc), ".xi uncertainty: %.8g MHz", x->line_error_mhz > 0.0 ? x->line_error_mhz : 0.010);
        ui_text(r, UI_FONT_SANS_SM,
                x->edit_param == XIAM_EDIT_LINE_DEFAULT ? x->edit_buf : unc,
                u->unc.x + 10, u->footer.y + 17,
                x->edit_param == XIAM_EDIT_LINE_DEFAULT ? UI_ACCENT_TEXT : UI_DIM);
    }
    {
        const char *hint = x->edit_param == XIAM_EDIT_PARAM_NAME
                ? "Enter applies the native keyword · Esc cancels · × comments the declaration out"
                : x->axis_convention == 1 ? "Prolate mapping: x/y/z = b/c/a; chi_z = aa; chi_- = bb-cc."
                : x->axis_convention == 2 ? "Oblate mapping: x/y/z = a/b/c; chi_z = cc; chi_- = aa-bb."
                : "Axis mapping unresolved: choose prolate or oblate before interpreting x/y/z labels.";
        int at = u->btn_c.x + u->btn_c.w + 16;
        if (at + ui_text_w(UI_FONT_SANS_SM, hint) < u->unc.x)
            ui_text(r, UI_FONT_SANS_SM, hint, at, u->footer.y + 17,
                    x->edit_param == XIAM_EDIT_PARAM_NAME ? UI_ACCENT_TEXT
                        : x->axis_convention ? UI_DIM : UI_WARN);
    }
    (void)s;
}

static void xiam_render_lines(SDL_Renderer *r, AppState *s, XiamState *x,
                              const XiamUI *u, int mx, int my, int down) {
    char b[256]; int enabled = 0, total = xiam_assignment_count(s, &enabled);
    snprintf(b, sizeof(b), "%d / %d selected; observed frequency and uncertainty are editable in MHz.", enabled, total);
    xiam_page_caption(r, x, u, "Assigned transitions — click a row to include/exclude it from the XIAM fit", b, UI_DIM);
    ui_fill(r, u->table, UI_INPUT); ui_frame(r, u->table, UI_LINE);
    int qns_right = u->table.x + u->table.w * 35 / 100;
    int obs_right = u->table.x + u->table.w * 55 / 100;
    int calc_right = u->table.x + u->table.w * 74 / 100;
    int unc_right = u->table.x + u->table.w * 89 / 100;
    int unc_decimal = u->table.x + u->table.w * 84 / 100;
    ui_text(r, UI_FONT_MONO_SM, "FIT", u->table.x + 12, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "UPPER  –  LOWER", qns_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "OBSERVED / MHz", obs_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "CALCULATED / MHz", calc_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "UNC / MHz", unc_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "DEL", u->table.x + u->table.w - 10, u->table.y + 9, UI_DIM);
    ui_hline(r, u->table.x + 2, u->table.x + u->table.w - 2, u->rows.y - 3, UI_LINE);
    const int del_x = u->table.x + u->table.w - 30;
    SDL_RenderSetClipRect(r, &u->rows);
    for (int visible = 0; visible < u->rows_visible; visible++) {
        int ai = xiam_assignment_index(s, x->lines_scroll + visible); if (ai < 0) break;
        Assignment *a = &s->assignments[ai]; int y = u->rows.y + visible * u->row_h;
        SDL_Rect row = {u->rows.x, y, u->rows.w, u->row_h - 2};
        if (ai == x->hover_line) ui_fill(r, row, UI_RAISED);
        else if (visible & 1) ui_fill(r, row, UI_CHROME);
        ui_text(r, UI_FONT_MONO_SM, a->fit_enabled ? "[x]" : "[ ]", row.x + 11, y + 6, a->fit_enabled ? UI_OK : UI_FAINT);
        xiam_format_transition(&a->pred, b, sizeof(b)); ui_text_right(r, UI_FONT_MONO_SM, b, qns_right, y + 6, a->fit_enabled ? UI_TEXT : UI_DIM);
        if (x->edit_param == XIAM_EDIT_LINE_OBS && x->edit_assignment == ai)
            xiam_edit_text(r, UI_FONT_MONO_SM, x, xiam_line_obs_text_x(u, a), y + 6, UI_ACCENT_TEXT);
        else { snprintf(b, sizeof(b), "%.6f", a->exp_freq); ui_text_right(r, UI_FONT_MONO_SM, b, obs_right, y + 6, a->fit_enabled ? UI_TEXT : UI_DIM); }
        double calc = xiam_assignment_calculated(s, a); snprintf(b, sizeof(b), calc ? "%.6f" : "—", calc); ui_text_right(r, UI_FONT_MONO_SM, b, calc_right, y + 6, calc ? UI_DIM : UI_FAINT);
        if (x->edit_param == XIAM_EDIT_LINE_ERR && x->edit_assignment == ai)
            xiam_edit_text(r, UI_FONT_MONO_SM, x, xiam_line_unc_text_x(u, x, a), y + 6, UI_ACCENT_TEXT);
        else { snprintf(b, sizeof(b), "%.6g", xiam_line_error(x, a)); xiam_text_decimal(r, UI_FONT_MONO_SM, b, unc_decimal, y + 6, a->fit_enabled ? UI_TEXT : UI_DIM); }
        ui_draw_icon(r, UI_ICON_CLOSE, (SDL_Rect){del_x, y + (u->row_h - 2 - 14) / 2, 14, 14}, UI_DANGER_TEXT);
    }
    SDL_RenderSetClipRect(r, NULL);
    ui_button(r, u->btn_a, "Save fit .xi…", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_b, "Fit selected rows", -1, enabled ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, mx, my, down);
    ui_text(r, UI_FONT_SANS_SM, "× removes the transition from this in-memory XIAM project and its saved assignment list.", u->btn_c.x, u->footer.y + 17, UI_FAINT);
}

static void xiam_render_fitting(SDL_Renderer *r, AppState *s, XiamState *x,
                                const XiamUI *u, int mx, int my, int down) {
    char b[256];
    xiam_page_caption(r, x, u, "XIAM fitting output — fitted constants and residuals for the active Hamiltonian",
                      x->fit_output_path[0] ? x->fit_output_path
                                            : "No XIAM fit report yet. Select assignments in Lines, then run Fit.",
                      x->fit_output_path[0] ? UI_DIM : UI_FAINT);
    ui_fill(r, u->table, UI_INPUT); ui_frame(r, u->table, UI_LINE);
    int split = u->split;
    int param_value_right = u->table.x + u->table.w * 57 / 100;
    int param_error_right = u->table.x + u->table.w * 82 / 100;
    int param_error_decimal = u->table.x + u->table.w * 77 / 100;
    ui_text(r, UI_FONT_MONO_SM, "FITTED PARAMETERS", u->table.x + 12, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "VALUE / MHz", param_value_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "EST. ERROR", param_error_right, u->table.y + 9, UI_DIM);
    int y = u->table.y + 33;
    int param_rows = u->fit_param_rows_visible;
    int param_total = xiam_fitted_parameter_count(x);
    for (int visible = 0; visible < param_rows; visible++) {
        int pi = xiam_fitted_parameter_index(x, x->fit_params_scroll + visible); if (pi < 0) break;
        XiamParameter *p = &x->params[pi];
        if ((x->fit_params_scroll + visible) & 1) ui_fill(r, (SDL_Rect){u->table.x + 1, y, u->table.w - 2, u->row_h - 2}, UI_CHROME);
        ui_text(r, UI_FONT_MONO_SM, p->name, u->table.x + 12, y + 6, UI_TEXT);
        snprintf(b, sizeof(b), "%.12g", xiam_display_value(p)); ui_text_right(r, UI_FONT_MONO_SM, b, param_value_right, y + 6, UI_TEXT);
        snprintf(b, sizeof(b), "%.6g", xiam_display_error(p)); xiam_text_decimal(r, UI_FONT_MONO_SM, b, param_error_decimal, y + 6, p->error_ghz ? UI_DIM : UI_FAINT);
        y += u->row_h;
    }
    if (!param_total) ui_text(r, UI_FONT_SANS_SM, "No fitted parameter report yet.", u->table.x + 12, y + 6, UI_FAINT);
    else if (param_total > param_rows)
        ui_text_right(r, UI_FONT_SANS_SM, "scroll", u->table.x + u->table.w - 12, u->table.y + 9, UI_FAINT);
    ui_hline(r, u->table.x + 2, u->table.x + u->table.w - 2, split, UI_LINE);
    int qns_right = u->table.x + u->table.w * 28 / 100;
    int obs_right = u->table.x + u->table.w * 46 / 100;
    int calc_right = u->table.x + u->table.w * 63 / 100;
    int residual_right = u->table.x + u->table.w * 79 / 100;
    int sigma_right = u->table.x + u->table.w * 93 / 100;
    ui_text(r, UI_FONT_MONO_SM, "FIT", u->table.x + 12, split + 10, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "QNS", qns_right, split + 10, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "OBSERVED", obs_right, split + 10, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "CALCULATED", calc_right, split + 10, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "OBS-CALC", residual_right, split + 10, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "/UNC", sigma_right, split + 10, UI_DIM);
    int first = x->fitting_scroll, max_rows = u->fit_obs_rows_visible;
    for (int visible = 0; visible < max_rows; visible++) {
        int ai = xiam_assignment_index(s, first + visible); if (ai < 0) break;
        Assignment *a = &s->assignments[ai]; int ry = split + 32 + visible * u->row_h;
        if (ai == x->hover_line) ui_fill(r, (SDL_Rect){u->table.x + 1, ry, u->table.w - 2, u->row_h - 2}, UI_RAISED);
        else if (visible & 1) ui_fill(r, (SDL_Rect){u->table.x + 1, ry, u->table.w - 2, u->row_h - 2}, UI_CHROME);
        double calc = xiam_assignment_calculated(s, a);
        int have_report = x->fit_output_path[0] != '\0';
        /* XIAM never saw an excluded line during the fit, but the refreshed
           native prediction still carries this transition like any other, so
           its calculated frequency is available regardless of fit_enabled -
           only the fit-weighted residual/sigma need the line to have actually
           been fit. */
        int have_calc = have_report && calc != 0.0;
        int have_residual = have_calc && a->fit_enabled;
        double uncertainty = xiam_line_error(x, a);
        double z = have_residual ? (a->exp_freq - calc) / uncertainty : 0.0;
        SDL_Color c = !a->fit_enabled ? UI_FAINT : have_residual ? xiam_residual_color(z) : have_report ? UI_DIM : UI_FAINT;
        if (have_residual) ui_fill(r, (SDL_Rect){u->table.x + 1, ry, 3, u->row_h - 2}, c);
        ui_text(r, UI_FONT_MONO_SM, a->fit_enabled ? "[x]" : "[ ]", u->table.x + 12, ry + 6, c);
        xiam_format_transition(&a->pred, b, sizeof(b)); ui_text_right(r, UI_FONT_MONO_SM, b, qns_right, ry + 6, c);
        snprintf(b, sizeof(b), "%.6f", a->exp_freq); ui_text_right(r, UI_FONT_MONO_SM, b, obs_right, ry + 6, c);
        if (have_residual) {
            snprintf(b, sizeof(b), "%.6f", calc); ui_text_right(r, UI_FONT_MONO_SM, b, calc_right, ry + 6, c);
            snprintf(b, sizeof(b), "%+.6f", a->exp_freq - calc); ui_text_right(r, UI_FONT_MONO_SM, b, residual_right, ry + 6, c);
            snprintf(b, sizeof(b), "%+.2f", z); ui_text_right(r, UI_FONT_MONO_SM, b, sigma_right, ry + 6, c);
        } else if (have_calc) {
            /* Excluded, but still predicted: the "[ ]" checkbox already says
               it was excluded, so just show where the model puts the line. */
            snprintf(b, sizeof(b), "%.6f", calc); ui_text_right(r, UI_FONT_MONO_SM, b, calc_right, ry + 6, c);
            snprintf(b, sizeof(b), "%+.6f", a->exp_freq - calc); ui_text_right(r, UI_FONT_MONO_SM, b, residual_right, ry + 6, c);
        } else if (!a->fit_enabled) {
            ui_text(r, UI_FONT_MONO_SM, "excluded", obs_right + 16, ry + 6, UI_FAINT);
        } else if (!have_report) {
            ui_text(r, UI_FONT_MONO_SM, "not fitted yet", obs_right + 16, ry + 6, UI_FAINT);
        } else {
            ui_text(r, UI_FONT_MONO_SM, "not in refreshed catalogue", obs_right + 16, ry + 6, UI_DIM);
        }
        ui_draw_icon(r, UI_ICON_CLOSE,
                     (SDL_Rect){u->table.x + u->table.w - 26, ry + (u->row_h - 2 - 14) / 2, 14, 14},
                     UI_DANGER_TEXT);
    }
    ui_text(r, UI_FONT_MONO_SM, "DEL", u->table.x + u->table.w - 32, split + 10, UI_DIM);
    ui_button(r, u->btn_a, "Fit", -1, UI_BTN_PRIMARY, 0, mx, my, down);
    int can_undo = 0;
    for (int i = 0; i < x->history_count; i++) if (x->history[i].model_id == x->model_id) can_undo = 1;
    ui_button(r, u->btn_b, "Undo fit", -1, UI_BTN_QUIET, 0, can_undo ? mx : -999, my, down);
    ui_button(r, u->btn_c, "Predict updated model", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_d, "Save fit .xi…", -1, UI_BTN_QUIET, 0, mx, my, down);
    int lx = u->btn_d.x + u->btn_d.w + 24, ly = u->footer.y + 17;
    const char *band[4] = {"0-1.5", "1.5-2.5", "2.5-4", ">4 sigma"};
    double mid[4] = {0.5, 2.0, 3.2, 5.0};
    for (int i = 0; i < 4; i++) {
        ui_fill(r, (SDL_Rect){lx, ly + 3, 8, 8}, xiam_residual_color(mid[i]));
        ui_text(r, UI_FONT_SANS_SM, band[i], lx + 13, ly, UI_FAINT);
        lx += 13 + ui_text_w(UI_FONT_SANS_SM, band[i]) + 14;
    }
    if (x->has_fit_summary) {
        int enabled = 0; xiam_assignment_count(s, &enabled);
        snprintf(b, sizeof(b), "Std. deviation  %.6f MHz   %d lines   %d iterations",
                 x->fit_std_mhz, enabled, x->fit_iterations);
        int bw = ui_text_w(UI_FONT_MONO_SM, b), right = u->w - XIAM_PAD;
        if (right - bw > lx + 20) ui_text_right(r, UI_FONT_MONO_SM, b, right, ly, UI_TEXT);
    }
}

static void xiam_render_states(SDL_Renderer *r, AppState *s, XiamState *x,
                               const XiamUI *u, int mx, int my, int down) {
    char b[256];
    xiam_page_caption(r, x, u, "States — block II controls, block V symmetries and block VI torsional states",
                      "Tunnelling and hyperfine states stay inside their XIAM Hamiltonian; USE comments the native record out.",
                      UI_DIM);
    for (int i = 0; i < 10; i++) {
        SDL_Rect cell = u->state_cell[i];
        int editing = x->edit_param == XIAM_EDIT_CONTROL && x->edit_control == i;
        int declared = xiam_control_line(x, XIAM_CONTROL_KEY[i]) >= 0;
        double value = 0.0;
        xiam_control_value(x, XIAM_CONTROL_KEY[i], XIAM_CONTROL_DEFAULT[i], &value);
        ui_fill(r, cell, i == 0 ? UI_PANEL : UI_INPUT);
        ui_frame(r, cell, editing ? UI_ACCENT : UI_LINE);
        ui_text(r, UI_FONT_SANS_SM, XIAM_CONTROL_LABEL[i], cell.x + 6, cell.y + 3,
                i == 0 ? UI_FAINT : UI_DIM);
        snprintf(b, sizeof(b), XIAM_CONTROL_FORMAT[i], value);
        xiam_cell_text(r, UI_FONT_MONO_SM, x, editing, b, cell.x + 6, cell.y + 15,
                       editing ? UI_ACCENT_TEXT : declared ? UI_TEXT : UI_FAINT);
    }
    ui_fill(r, u->state_table, UI_INPUT); ui_frame(r, u->state_table, UI_LINE);
    int kind_x = u->state_table.x + u->state_table.w * 10 / 100;
    int record_x = u->state_table.x + u->state_table.w * 34 / 100;
    int w_x = u->state_table.x + u->state_table.w * 88 / 100;
    ui_text(r, UI_FONT_MONO_SM, "USE", u->state_table.x + 12, u->state_table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "KIND", kind_x, u->state_table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "NATIVE RECORD", record_x, u->state_table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "W", w_x, u->state_table.y + 9, UI_DIM);
    ui_hline(r, u->state_table.x + 2, u->state_table.x + u->state_table.w - 2, u->state_rows.y - 3, UI_LINE);
    XiamStateRow rows[MAX_XIAM_STATE_ROWS];
    int total = xiam_state_rows(x, rows, MAX_XIAM_STATE_ROWS);
    SDL_RenderSetClipRect(r, &u->state_rows);
    for (int visible = 0; visible < u->state_rows_visible; visible++) {
        int i = x->states_scroll + visible;
        if (i >= total) break;
        int y = u->state_rows.y + visible * u->row_h;
        SDL_Rect row = {u->state_rows.x, y, u->state_rows.w, u->row_h - 2};
        int editing = x->edit_param == XIAM_EDIT_STATE_ROW && x->edit_state_line == rows[i].line;
        if (editing) ui_fill(r, row, UI_ACCENT_SOFT);
        else if (visible & 1) ui_fill(r, row, UI_CHROME);
        ui_text(r, UI_FONT_MONO_SM, rows[i].enabled ? "[x]" : "[ ]", row.x + 11, y + 6,
                rows[i].enabled ? UI_OK : UI_FAINT);
        ui_text(r, UI_FONT_SANS_SM, rows[i].block == 5 ? "symmetry" : "torsional state",
                kind_x, y + 6, rows[i].enabled ? UI_DIM : UI_FAINT);
        xiam_state_row_text(x, &rows[i], b, sizeof(b));
        xiam_cell_text(r, UI_FONT_MONO_SM, x, editing, b, record_x, y + 6,
                       editing ? UI_ACCENT_TEXT : rows[i].enabled ? UI_TEXT : UI_FAINT);
        /* Only a block V symmetry record carries a spin-statistical weight;
           block VI torsional states have none. */
        if (rows[i].block == 5) {
            int editing_w = x->edit_param == XIAM_EDIT_STATE_W && x->edit_state_line == rows[i].line;
            double w = xiam_state_row_w(x, &rows[i]);
            snprintf(b, sizeof(b), "%.6g", w);
            xiam_cell_text(r, UI_FONT_MONO_SM, x, editing_w, b, w_x, y + 6,
                           editing_w ? UI_ACCENT_TEXT : rows[i].enabled ? UI_TEXT : UI_FAINT);
        }
    }
    SDL_RenderSetClipRect(r, NULL);
    if (!total)
        ui_text(r, UI_FONT_SANS_SM, "This .xi declares no block V symmetry and no block VI state.",
                u->state_table.x + 12, u->state_rows.y + 6, UI_FAINT);
    ui_button(r, u->btn_a, "+ torsional state", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_b, "Predict", -1, UI_BTN_PRIMARY, 0, mx, my, down);
    ui_button(r, u->btn_c, "Save fit .xi…", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_text(r, UI_FONT_SANS_SM,
            "ints is the run mode and is regenerated on every run · a value shown dim follows XIAM's own default",
            u->btn_d.x, u->footer.y + 17, UI_FAINT);
    (void)s;
}

static void xiam_render_hamiltonians(SDL_Renderer *r, XiamState *x, const XiamUI *u,
                                     int mx, int my, int down) {
    char b[256];
    xiam_page_caption(r, x, u, "Hamiltonians — each row is one independent native .xi model",
                      "Open, reorder, rename, duplicate and remove a model in the sidebar; this page saves and imports the project.",
                      UI_DIM);
    ui_fill(r, u->table, UI_INPUT); ui_frame(r, u->table, UI_LINE);
    int temp_right = u->table.x + u->table.w * 72 / 100;
    int conc_right = u->table.x + u->table.w * 84 / 100;
    ui_text(r, UI_FONT_MONO_SM, "HAMILTONIAN", u->table.x + 12, u->table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "SOURCE .xi", u->table.x + u->table.w * 30 / 100, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "T / K", temp_right, u->table.y + 9, UI_DIM);
    ui_text_right(r, UI_FONT_MONO_SM, "CONC.", conc_right, u->table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "SIM", u->table.x + u->table.w * 87 / 100, u->table.y + 9, UI_DIM);
    for (int visible = 0; visible < u->rows_visible; visible++) {
        int i = x->hamiltonians_scroll + visible; if (i >= x->n_hamiltonians) break;
        XiamHamiltonian *h = &x->hamiltonian[i]; int y = u->rows.y + visible * u->row_h;
        SDL_Rect row = {u->rows.x, y, u->rows.w, u->row_h - 2};
        if (i == x->active_hamiltonian) ui_fill(r, row, UI_ACCENT_SOFT); else if (visible & 1) ui_fill(r, row, UI_CHROME);
        snprintf(b, sizeof(b), "H%d  %s", i + 1, h->name); ui_text(r, UI_FONT_SANS_SM, b, row.x + 10, y + 6, i == x->active_hamiltonian ? UI_ACCENT_TEXT : UI_TEXT);
        ui_text(r, UI_FONT_MONO_SM, h->input_path[0] ? h->input_path : "—", u->table.x + u->table.w * 30 / 100, y + 6, UI_DIM);
        snprintf(b, sizeof(b), "%.6g", h->temperature_k); ui_text_right(r, UI_FONT_MONO_SM, b, temp_right, y + 6, UI_DIM);
        snprintf(b, sizeof(b), "%.6g", h->concentration); ui_text_right(r, UI_FONT_MONO_SM, b, conc_right, y + 6, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, h->simulate_excluded ? "[ ]" : "[x]", u->table.x + u->table.w * 87 / 100, y + 6, h->simulate_excluded ? UI_FAINT : UI_OK);
    }
    ui_button(r, u->btn_a, x->session_dirty ? "Save session *" : "Save session", -1,
              x->session_dirty ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_b, "Restore session", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_c, "Import .xi/.xo", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_text(r, UI_FONT_SANS_SM,
            x->session_dirty
                ? "Unsaved changes: nothing is written to .fit until Save session."
                : "Import reads every name.xi (and its name.xo) from .fit/load; open, reorder and remove are in the sidebar.",
            u->btn_d.x, u->footer.y + 17, x->session_dirty ? UI_ACCENT_TEXT : UI_FAINT);
}

static void xiam_render_simulation(SDL_Renderer *r, AppState *s, XiamState *x, const XiamUI *u,
                                   int mx, int my, int down) {
    char b[256]; int checked = 0;
    xiam_page_caption(r, x, u, "Simulation — checked native XIAM Hamiltonians are calculated and overlaid together",
                      "One XIAM run per .xi; concentration is the native intensity multiplier (edit it in Parameters).",
                      UI_DIM);
    ui_fill(r, u->table, UI_INPUT); ui_frame(r, u->table, UI_LINE);
    int trace_x = u->table.x + u->table.w * 40 / 100;
    static const double mu_at[3] = {0.47, 0.58, 0.69};
    int conc_right = u->table.x + u->table.w * 88 / 100;
    ui_text(r, UI_FONT_MONO_SM, "SIM", u->table.x + 12, u->table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "HAMILTONIAN", u->table.x + u->table.w * 10 / 100, u->table.y + 9, UI_DIM);
    ui_text(r, UI_FONT_MONO_SM, "TRACE", trace_x, u->table.y + 9, UI_DIM);
    for (int k = 0; k < 3; k++) {
        snprintf(b, sizeof(b), "%s", XIAM_DIPOLE[k]);
        ui_text(r, UI_FONT_MONO_SM, b, u->table.x + (int)(u->table.w * mu_at[k]), u->table.y + 9, UI_DIM);
    }
    ui_text_right(r, UI_FONT_MONO_SM, "T / K  ·  CONC.", conc_right, u->table.y + 9, UI_DIM);
    for (int visible = 0; visible < u->rows_visible; visible++) {
        int i = x->simulation_scroll + visible; if (i >= x->n_hamiltonians) break;
        XiamHamiltonian *h = &x->hamiltonian[i]; int y = u->rows.y + visible * u->row_h;
        SDL_Rect row = {u->rows.x, y, u->rows.w, u->row_h - 2};
        if (i == x->active_hamiltonian) ui_fill(r, row, UI_ACCENT_SOFT); else if (visible & 1) ui_fill(r, row, UI_CHROME);
        int on = !h->simulate_excluded; if (on) checked++;
        ui_text(r, UI_FONT_MONO_SM, on ? "[x]" : "[ ]", row.x + 11, y + 6, on ? UI_OK : UI_FAINT);
        snprintf(b, sizeof(b), "H%d  %s", i + 1, h->name); ui_text(r, UI_FONT_SANS_SM, b, u->table.x + u->table.w * 10 / 100, y + 6, on ? UI_TEXT : UI_DIM);
        /* The colour this model's broadened trace is drawn in; click it. */
        SDL_Rect swatch = {trace_x, y + 3, 26, u->row_h - 8};
        SDL_Color sc = h->trace_color.a > 0 ? h->trace_color : XIAM_TRACE_PALETTE[i % 8];
        if (s->predfit.species_trace_mode == TRACE_SUM) { sc.r /= 3; sc.g /= 3; sc.b /= 3; }
        ui_fill(r, swatch, sc); ui_frame(r, swatch, UI_LINE);
        for (int k = 0; k < 3; k++) {
            /* The dipole's own stored value, whichever model owns the row. */
            double value = 0.0;
            for (int q = 0; q < h->n_params; q++)
                if (!strcmp(h->params[q].name, XIAM_DIPOLE[k])) { value = h->params[q].value_ghz; break; }
            int used = on && !h->mu_excluded[k] && value != 0.0;
            snprintf(b, sizeof(b), "[%c] %.4g", h->mu_excluded[k] ? ' ' : 'x', value);
            ui_text(r, UI_FONT_MONO_SM, b, u->table.x + (int)(u->table.w * mu_at[k]), y + 6,
                    used ? UI_TEXT : UI_FAINT);
        }
        snprintf(b, sizeof(b), "%.6g K  ·  %.4g", h->temperature_k, h->concentration);
        ui_text_right(r, UI_FONT_MONO_SM, b, conc_right, y + 6, on ? UI_DIM : UI_FAINT);
    }
    snprintf(b, sizeof(b), "Simulate (%d H)", checked); ui_button(r, u->btn_a, b, -1, checked ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_b, "Check all", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_c, "Uncheck all", -1, UI_BTN_QUIET, 0, mx, my, down);
    ui_button(r, u->btn_d,
              s->predfit.species_trace_mode == TRACE_SUM ? "Traces: sum" :
              s->predfit.species_trace_mode == TRACE_SUM_AND_STATES ? "Traces: sum + models"
                                                                    : "Traces: models", -1,
              UI_BTN_QUIET, s->predfit.species_trace_mode != TRACE_SUM, mx, my, down);
    ui_text(r, UI_FONT_SANS_SM,
            s->predfit.species_trace_mode != TRACE_SUM
                ? "With broadening on, each checked model is drawn in its own colour (also in the Broadening panel)."
                : "One XIAM run per checked .xi; the catalogues are shown in one plot.",
            u->btn_d.x + u->btn_d.w + 16, u->footer.y + 17, UI_FAINT);
}

int xiam_handle_event(AppState *s, const SDL_Event *e) {
    if (!s || !e || !s->xiam.open) return 0;
    XiamState *x = &s->xiam; Uint32 id = x->window_id;
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == id && e->window.event == SDL_WINDOWEVENT_CLOSE) { xiam_close(s); return 1; }
    if (x->edit_param != XIAM_EDIT_NONE && e->type == SDL_TEXTINPUT && e->text.windowID == id) { xiam_edit_insert(x, e->text.text); return 1; }
    if (x->edit_param != XIAM_EDIT_NONE && e->type == SDL_KEYDOWN && e->key.windowID == id) {
        SDL_Keycode k = e->key.keysym.sym;
        SDL_Keymod mod = e->key.keysym.mod;
        int cmd = (mod & (KMOD_GUI | KMOD_CTRL)) != 0, shift = (mod & KMOD_SHIFT) != 0;
        if (cmd && k == SDLK_a) { x->edit_anchor = 0; x->edit_caret = (int)strlen(x->edit_buf); x->edit_replace = 0; return 1; }
        if (cmd && k == SDLK_c) { xiam_edit_copy(x, 0); return 1; }
        if (cmd && k == SDLK_x) { xiam_edit_copy(x, 1); return 1; }
        if (cmd && k == SDLK_v) { xiam_edit_paste(x); return 1; }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) { xiam_edit_commit(s); return 1; }
        if (k == SDLK_ESCAPE) { x->edit_param = XIAM_EDIT_NONE; x->edit_assignment = -1; x->edit_name_row = -1; x->edit_hamiltonian_row = -1; x->edit_control = -1; x->edit_state_line = -1; SDL_StopTextInput(); return 1; }
        if (k == SDLK_BACKSPACE) {
            x->edit_replace = 0;
            if (!xiam_edit_delete_selection(x) && x->edit_caret > 0) {
                x->edit_anchor = x->edit_caret - 1;
                xiam_edit_delete_selection(x);
            }
            return 1;
        }
        if (k == SDLK_DELETE) {
            x->edit_replace = 0;
            if (!xiam_edit_delete_selection(x) && x->edit_caret < (int)strlen(x->edit_buf)) {
                x->edit_anchor = x->edit_caret + 1;
                xiam_edit_delete_selection(x);
            }
            return 1;
        }
        if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_HOME || k == SDLK_END) {
            int caret = x->edit_caret, length = (int)strlen(x->edit_buf);
            if (k == SDLK_HOME || (cmd && k == SDLK_LEFT)) caret = 0;
            else if (k == SDLK_END || (cmd && k == SDLK_RIGHT)) caret = length;
            else if (k == SDLK_LEFT) caret--;
            else caret++;
            x->edit_caret = caret;
            if (!shift) x->edit_anchor = caret;
            x->edit_replace = 0;
            xiam_edit_clamp(x);
            return 1;
        }
        return 1;
    }
    if (e->type == SDL_KEYDOWN && e->key.windowID == id && e->key.keysym.sym == SDLK_ESCAPE) { xiam_close(s); return 1; }
    if (x->edit_param != XIAM_EDIT_NONE && e->type == SDL_MOUSEBUTTONDOWN &&
        e->button.windowID == id) {
        XiamUI hit = xiam_ui(x);
        xiam_clamp_scrolls(s, &hit);
        /* Clicking inside the cell that is being edited aims the caret.  It
           must not commit and reopen the edit, which would select the whole
           text again and leave the click with nothing to show for it. */
        if (x->edit_param == XIAM_EDIT_H_NAME &&
            point_in_rect(e->button.x, e->button.y, hit.nav_rows)) {
            int row = x->nav_scroll + (e->button.y - hit.nav_rows.y) / hit.row_h;
            if (row == x->edit_hamiltonian_row) {
                char prefix[32]; snprintf(prefix, sizeof(prefix), "H%d  ", row + 1);
                xiam_place_caret(x, UI_FONT_SANS_SM,
                                 hit.nav_rows.x + 10 + ui_text_w(UI_FONT_SANS_SM, prefix),
                                 e->button.x);
                return 1;
            }
        }
        if (x->active_tab == 1 && point_in_rect(e->button.x, e->button.y, hit.rows) &&
            x->edit_assignment >= 0 && x->edit_assignment < s->n_assignments &&
            (x->edit_param == XIAM_EDIT_LINE_OBS || x->edit_param == XIAM_EDIT_LINE_ERR)) {
            int ai = xiam_assignment_index(s, x->lines_scroll + (e->button.y - hit.rows.y) / hit.row_h);
            if (ai == x->edit_assignment) {
                const Assignment *a = &s->assignments[ai];
                xiam_place_caret(x, UI_FONT_MONO_SM,
                                 x->edit_param == XIAM_EDIT_LINE_OBS ? xiam_line_obs_text_x(&hit, a)
                                                                     : xiam_line_unc_text_x(&hit, x, a),
                                 e->button.x);
                return 1;
            }
        }
        if (x->active_tab == 0 && point_in_rect(e->button.x, e->button.y, hit.param_rows)) {
            int row = x->params_scroll + (e->button.y - hit.param_rows.y) / hit.row_h;
            if (x->edit_param == XIAM_EDIT_PARAM_NAME && row == x->edit_name_row) {
                xiam_place_caret(x, UI_FONT_MONO_SM, hit.param_rows.x + 10, e->button.x);
                return 1;
            }
            if (x->edit_param >= 0 && x->edit_param == row) {
                xiam_place_caret(x, UI_FONT_MONO_SM, xiam_param_value_text_x(&hit, x, row), e->button.x);
                return 1;
            }
        }
        xiam_edit_commit(s);
    }
    XiamUI u = xiam_ui(x);
    xiam_clamp_scrolls(s, &u);
    if (e->type == SDL_MOUSEWHEEL && e->wheel.windowID == id) {
        int mx, my; SDL_GetMouseState(&mx, &my); int delta = -e->wheel.y * 3;
        /* The sidebar is on every page, so it takes the wheel wherever the
           pointer sits over it, before the page's own table. */
        if (point_in_rect(mx, my, u.nav)) x->nav_scroll += delta;
        else if (x->active_tab == 0) x->params_scroll += delta;
        else if (x->active_tab == 1) x->lines_scroll += delta;
        else if (x->active_tab == 2) {
            if (my < u.split) x->fit_params_scroll += delta;
            else x->fitting_scroll += delta;
        }
        else if (x->active_tab == 3) x->states_scroll += delta;
        else if (x->active_tab == 4) x->hamiltonians_scroll += delta;
        else x->simulation_scroll += delta;
        xiam_clamp_scrolls(s, &u);
        return 1;
    }
    if (e->type == SDL_MOUSEMOTION && e->motion.windowID == id) {
        x->hover_line = -1;
        if (x->active_tab == 1 && point_in_rect(e->motion.x, e->motion.y, u.rows))
            x->hover_line = xiam_assignment_index(s, x->lines_scroll + (e->motion.y - u.rows.y) / u.row_h);
        else if (x->active_tab == 2 && point_in_rect(e->motion.x, e->motion.y, u.fit_obs_rows))
            x->hover_line = xiam_assignment_index(s, x->fitting_scroll + (e->motion.y - u.fit_obs_rows.y) / u.row_h);
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.windowID != id) return 0;
    int px = e->button.x, py = e->button.y;
    for (int i = 0; i < XIAM_TAB_COUNT; i++) if (point_in_rect(px, py, u.tab[i])) { xiam_edit_commit(s); x->active_tab = i; x->delete_hamiltonian_id = 0; return 1; }
    /* The sidebar is global navigation, before the tab-specific editor. */
    if (point_in_rect(px, py, u.nav_open)) { char path[512]; if (xiam_pick_input(path, sizeof(path))) xiam_load_input_file(s, path); return 1; }
    if (point_in_rect(px, py, u.nav_reload)) { xiam_reload_active_input(s); return 1; }
    if (point_in_rect(px, py, u.nav_rows)) {
        int i = x->nav_scroll + (py - u.nav_rows.y) / u.row_h;
        if (i >= 0 && i < x->n_hamiltonians) {
            xiam_select_hamiltonian(x, i);
            x->delete_hamiltonian_id = 0;
            /* A second click on the selected row renames it, like a file. */
            if (e->button.clicks >= 2) { x->edit_hamiltonian_row = i; xiam_edit_begin(x, XIAM_EDIT_H_NAME); }
        }
        return 1;
    }
    if (point_in_rect(px, py, u.nav_up) || point_in_rect(px, py, u.nav_down)) {
        xiam_move_hamiltonian(s, x->active_hamiltonian, point_in_rect(px, py, u.nav_up) ? -1 : 1);
        return 1;
    }
    if (point_in_rect(px, py, u.nav_rename)) {
        if (x->active_hamiltonian >= 0) {
            x->edit_hamiltonian_row = x->active_hamiltonian;
            xiam_edit_begin(x, XIAM_EDIT_H_NAME);
        }
        return 1;
    }
    if (point_in_rect(px, py, u.nav_duplicate)) { xiam_duplicate_active(s); return 1; }
    if (point_in_rect(px, py, u.nav_delete)) {
        if (x->active_hamiltonian < 0) return 1;
        if (x->delete_hamiltonian_id == x->model_id) xiam_remove_active_hamiltonian(s);
        else {
            x->delete_hamiltonian_id = x->model_id;
            snprintf(x->status, sizeof(x->status),
                     "Click Confirm to remove %s and its assignments.",
                     x->hamiltonian[x->active_hamiltonian].name);
        }
        return 1;
    }
    if (x->active_tab == 0) {
        SDL_Rect fmin = u.fmin, fmax = u.fmax, temp = u.temp, conc = u.conc, jmax = u.jmax, axis = u.axis;
        if (point_in_rect(px, py, fmin)) { xiam_edit_begin(x, XIAM_EDIT_FMIN); return 1; }
        if (point_in_rect(px, py, fmax)) { xiam_edit_begin(x, XIAM_EDIT_FMAX); return 1; }
        if (point_in_rect(px, py, temp)) { xiam_edit_begin(x, XIAM_EDIT_TEMP); return 1; }
        if (point_in_rect(px, py, conc)) { xiam_edit_begin(x, XIAM_EDIT_CONCENTRATION); return 1; }
        if (point_in_rect(px, py, jmax)) { xiam_edit_begin(x, XIAM_EDIT_JMAX); return 1; }
        if (point_in_rect(px, py, axis)) { x->axis_override = (x->axis_override + 1) % 3; xiam_update_axis_convention(x); xiam_store_active(x); xiam_mark_session_dirty(s); return 1; }
        SDL_Rect rows = u.param_rows;
        if (point_in_rect(px, py, rows)) {
            int i = x->params_scroll + (py - rows.y) / u.row_h;
            if (i >= 0 && i < x->n_params) {
                int del_x = u.param_table.x + u.param_table.w - 30;
                int name_edge = u.param_table.x + u.param_table.w * 30 / 100;
                if (px >= del_x) xiam_delete_parameter(s, i);
                else if (px >= u.param_table.x + u.param_table.w * 72 / 100) {
                    x->params[i].fit_method = (x->params[i].fit_method + 1) % 4;
                    xiam_mark_session_dirty(s);
                }
                /* The name is the XIAM identity of a constant, so it is a
                   cell of its own: Pickett edits the .par id in the same
                   place for exactly the same reason. */
                else if (px < name_edge) { x->edit_name_row = i; xiam_edit_begin(x, XIAM_EDIT_PARAM_NAME); }
                else xiam_edit_begin(x, i);
                xiam_store_active(x);
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_a)) {
            xiam_edit_commit(s);
            int row = xiam_add_parameter(s);
            if (row >= 0) {
                x->params_scroll = xiam_scroll_limit(x->n_params, u.param_rows_visible);
                x->edit_name_row = row;
                xiam_edit_begin(x, XIAM_EDIT_PARAM_NAME);
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_b)) { xiam_edit_commit(s); xiam_calculate(s); return 1; }
        if (point_in_rect(px, py, u.btn_c)) { char path[512]; xiam_edit_commit(s); if (xiam_pick_fit_destination(path, sizeof(path))) xiam_save_fit_file(s, path); return 1; }
        /* The uncertainty a newly written .xi datum is given, in the same
           corner Pred&Fit keeps the .lin one. */
        if (point_in_rect(px, py, u.unc)) { xiam_edit_commit(s); xiam_edit_begin(x, XIAM_EDIT_LINE_DEFAULT); return 1; }
    } else if (x->active_tab == 1) {
        if (point_in_rect(px, py, u.rows)) { int ai = xiam_assignment_index(s, x->lines_scroll + (py - u.rows.y) / u.row_h); if (ai >= 0) { if (px >= u.table.x + u.table.w - 30) xiam_delete_assignment(s, ai); else if (px >= u.table.x + u.table.w * 78 / 100) xiam_edit_assignment_begin(s, ai, 1); else if (px >= u.table.x + u.table.w * 38 / 100 && px < u.table.x + u.table.w * 57 / 100) xiam_edit_assignment_begin(s, ai, 0); else { s->assignments[ai].fit_enabled = !s->assignments[ai].fit_enabled; save_assignments(s); xiam_mark_session_dirty(s); } xiam_store_active(x); } return 1; }
        if (point_in_rect(px, py, u.btn_a)) { char path[512]; if (xiam_pick_fit_destination(path, sizeof(path))) xiam_save_fit_file(s, path); return 1; }
        if (point_in_rect(px, py, u.btn_b)) { xiam_fit(s); return 1; }
    } else if (x->active_tab == 2) {
        /* Fitting and Lines are two views of the same rows, so the residual
           table includes and removes an observation the same way. */
        if (point_in_rect(px, py, u.fit_obs_rows)) {
            int ai = xiam_assignment_index(s, x->fitting_scroll + (py - u.fit_obs_rows.y) / u.row_h);
            if (ai >= 0) {
                if (px >= u.table.x + u.table.w - 30) xiam_delete_assignment(s, ai);
                else {
                    s->assignments[ai].fit_enabled = !s->assignments[ai].fit_enabled;
                    save_assignments(s);
                    xiam_mark_session_dirty(s);
                }
                xiam_clamp_scrolls(s, &u);
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_a)) { xiam_fit(s); return 1; }
        if (point_in_rect(px, py, u.btn_b)) { xiam_undo_last_fit(s); return 1; }
        if (point_in_rect(px, py, u.btn_c)) { xiam_calculate(s); return 1; }
        if (point_in_rect(px, py, u.btn_d)) { char path[512]; if (xiam_pick_fit_destination(path, sizeof(path))) xiam_save_fit_file(s, path); return 1; }
    } else if (x->active_tab == 3) {
        for (int i = 0; i < 10; i++) if (point_in_rect(px, py, u.state_cell[i])) {
            /* Cell 0 is `ints`, the run mode: regenerated, never chosen. */
            if (i > 0) { x->edit_control = i; xiam_edit_begin(x, XIAM_EDIT_CONTROL); }
            return 1;
        }
        if (point_in_rect(px, py, u.state_rows)) {
            XiamStateRow rows[MAX_XIAM_STATE_ROWS];
            int total = xiam_state_rows(x, rows, MAX_XIAM_STATE_ROWS);
            int i = x->states_scroll + (py - u.state_rows.y) / u.row_h;
            if (i >= 0 && i < total) {
                if (px < u.state_table.x + u.state_table.w * 10 / 100)
                    xiam_toggle_state_row(s, rows[i].line);
                else if (rows[i].block == 5 && px >= u.state_table.x + u.state_table.w * 86 / 100) {
                    x->edit_state_line = rows[i].line;
                    xiam_edit_begin(x, XIAM_EDIT_STATE_W);
                } else if (px >= u.state_table.x + u.state_table.w * 34 / 100) {
                    x->edit_state_line = rows[i].line;
                    xiam_edit_begin(x, XIAM_EDIT_STATE_ROW);
                }
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_a)) {
            if (xiam_add_state_row(s) >= 0) {
                XiamStateRow rows[MAX_XIAM_STATE_ROWS];
                int total = xiam_state_rows(x, rows, MAX_XIAM_STATE_ROWS);
                x->states_scroll = xiam_scroll_limit(total, u.state_rows_visible);
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_b)) { xiam_calculate(s); return 1; }
        if (point_in_rect(px, py, u.btn_c)) { char path[512]; if (xiam_pick_fit_destination(path, sizeof(path))) xiam_save_fit_file(s, path); return 1; }
    } else if (x->active_tab == 4) {
        if (point_in_rect(px, py, u.rows)) { int i = x->hamiltonians_scroll + (py - u.rows.y) / u.row_h; if (i >= 0 && i < x->n_hamiltonians) { xiam_select_hamiltonian(x, i); x->delete_hamiltonian_id = 0; } return 1; }
        if (point_in_rect(px, py, u.btn_a)) {
            int was_dirty = x->session_dirty;
            predfit_save_session(s);
            if (s->predfit.session_dirty)
                snprintf(x->status, sizeof(x->status), "Cannot write spectravisual.state in .fit.");
            else {
                x->session_dirty = 0;
                snprintf(x->status, sizeof(x->status), "Session saved%s.",
                         was_dirty ? "" : " (nothing had changed)");
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_b)) {
            char state_path[600];
            xiam_session_file(s, state_path, sizeof(state_path));
            app_enqueue_pending_load(s, PENDING_LOAD_SESSION, state_path, 0);
            return 1;
        }
        if (point_in_rect(px, py, u.btn_c)) {
            char folder[600];
            xiam_load_folder(s, folder, sizeof(folder));
            if (xiam_import_load_dir(s, folder) > 0)
                x->hamiltonians_scroll = xiam_scroll_limit(x->n_hamiltonians, u.rows_visible);
            return 1;
        }
    } else if (x->active_tab == 5) {
        if (point_in_rect(px, py, u.rows)) {
            int i = x->simulation_scroll + (py - u.rows.y) / u.row_h;
            if (i >= 0 && i < x->n_hamiltonians) {
                XiamHamiltonian *h = &x->hamiltonian[i];
                int trace_x = u.table.x + u.table.w * 40 / 100;
                static const double mu_at[3] = {0.47, 0.58, 0.69};
                if (px < u.table.x + u.table.w * 10 / 100) {
                    /* The only checkbox a row owns is its own. */
                    if (i == x->active_hamiltonian) x->simulate_excluded = !x->simulate_excluded;
                    else h->simulate_excluded = !h->simulate_excluded;
                } else if (px >= trace_x && px < trace_x + 26) {
                    SDL_Color chosen = h->trace_color.a > 0 ? h->trace_color : XIAM_TRACE_PALETTE[i % 8];
                    if (settings_pick_color(&chosen)) {
                        chosen.a = chosen.a ? chosen.a : 235;
                        h->trace_color = chosen;
                    }
                } else if (px >= u.table.x + (int)(u.table.w * mu_at[0]) &&
                           px < u.table.x + u.table.w * 80 / 100) {
                    for (int k = 2; k >= 0; k--) {
                        if (px < u.table.x + (int)(u.table.w * mu_at[k])) continue;
                        h->mu_excluded[k] = !h->mu_excluded[k];
                        if (i == x->active_hamiltonian) x->mu_excluded[k] = h->mu_excluded[k];
                        break;
                    }
                } else {
                    xiam_select_hamiltonian(x, i);
                }
                xiam_store_active(x); xiam_mark_session_dirty(s);
            }
            return 1;
        }
        if (point_in_rect(px, py, u.btn_a)) { xiam_simulate_checked(s); return 1; }
        if (point_in_rect(px, py, u.btn_b) || point_in_rect(px, py, u.btn_c)) {
            int excluded = point_in_rect(px, py, u.btn_c);
            x->simulate_excluded = excluded;
            for (int i = 0; i < x->n_hamiltonians; i++) {
                x->hamiltonian[i].simulate_excluded = excluded;
                for (int k = 0; k < 3; k++) x->hamiltonian[i].mu_excluded[k] = 0;
            }
            for (int k = 0; k < 3; k++) x->mu_excluded[k] = 0;
            xiam_store_active(x); xiam_mark_session_dirty(s);
            return 1;
        }
        if (point_in_rect(px, py, u.btn_d)) {
            /* The same three choices as the Broadening panel and Pred&Fit. */
            s->predfit.species_trace_mode = (s->predfit.species_trace_mode + 1) % 3;
            s->predfit.session_dirty = 1;
            snprintf(x->status, sizeof(x->status),
                     s->predfit.species_trace_mode == TRACE_SUM
                         ? "Broadening draws the total trace only."
                         : s->predfit.species_trace_mode == TRACE_SUM_AND_STATES
                           ? "Broadening draws the total trace and one per model, in its colour."
                           : "Broadening draws one trace per model only, each in its colour.");
            return 1;
        }
    }
    return 1;
}

void xiam_write_session(AppState *s, FILE *fp) {
    if (!s || !fp) return;
    XiamState *x = &s->xiam;
    xiam_store_active(x);
    fprintf(fp, "xiamproject %d %d\n", x->n_hamiltonians, x->active_hamiltonian);
    for (int i = 0; i < x->n_hamiltonians; i++) {
        const XiamHamiltonian *h = &x->hamiltonian[i];
        fprintf(fp, "xiambegin %d %s\n", h->model_id, h->name);
        fprintf(fp, "xiamctrl %d %d %d %.17g %.17g %.17g %.17g %d %d %d %d %d %d\n",
                h->simulate_excluded != 0, h->loaded != 0, h->max_j,
                h->fmin_mhz, h->fmax_mhz, h->temperature_k, h->concentration,
                h->has_freq != 0, h->has_ints != 0, h->has_j != 0,
                h->axis_convention, h->axis_auto_convention, h->axis_override);
        fprintf(fp, "xiamline %.17g\n", h->line_error_mhz);
        fprintf(fp, "xiamtrace %d %d %d %d %d %d %d\n",
                h->trace_color.r, h->trace_color.g, h->trace_color.b, h->trace_color.a,
                h->mu_excluded[0] != 0, h->mu_excluded[1] != 0, h->mu_excluded[2] != 0);
        fprintf(fp, "xiampath %s\n", h->input_path);
        fprintf(fp, "xiamoutput %s\n", h->output_path);
        fprintf(fp, "xiamfitoutput %s\n", h->fit_output_path);
        for (int k = 0; k < h->n_params; k++) {
            const XiamParameter *p = &h->params[k];
            fprintf(fp, "xiamparam %s %.17g %.17g %d %d %d %d %d\n", p->name,
                    p->value_ghz, p->error_ghz, p->source_line, p->fit_source_line,
                    p->fit_method, p->is_quadrupole != 0, p->is_frequency != 0);
        }
        for (int k = 0; k < h->n_source_lines; k++)
            fprintf(fp, "xiamsource %d %s\n", k, h->source_lines[k]);
        fputs("xiamend\n", fp);
    }
}

void xiam_session_begin(AppState *s) {
    if (!s) return;
    if (s->xiam.window) xiam_close(s);
    free(s->xiam.history);
    memset(&s->xiam, 0, sizeof(s->xiam));
    xiam_init(s);
    s->xiam.session_loading_hamiltonian = -1;
    s->xiam.session_active_hamiltonian = -1;
}

static XiamHamiltonian *xiam_session_current(XiamState *x) {
    int i = x ? x->session_loading_hamiltonian : -1;
    return x && i >= 0 && i < x->n_hamiltonians ? &x->hamiltonian[i] : NULL;
}

int xiam_read_session_line(AppState *s, const char *line) {
    if (!s || !line) return 0;
    XiamState *x = &s->xiam;
    if (strncmp(line, "xiamproject ", 12) == 0) {
        int ignored = 0, active = -1;
        if (sscanf(line + 12, "%d %d", &ignored, &active) >= 1)
            x->session_active_hamiltonian = active;
        return 1;
    }
    if (strncmp(line, "xiambegin ", 10) == 0) {
        int id = 0, used = 0;
        if (sscanf(line + 10, "%d%n", &id, &used) == 1 && id < 0 &&
            x->n_hamiltonians < MAX_XIAM_HAMILTONIANS) {
            int i = x->n_hamiltonians++;
            XiamHamiltonian *h = &x->hamiltonian[i];
            memset(h, 0, sizeof(*h));
            h->model_id = id; h->max_j = 30; h->fmax_mhz = 10000.0;
            h->temperature_k = h->concentration = 1.0;
            h->line_error_mhz = 0.010;
            const char *name = line + 10 + used;
            while (*name == ' ' || *name == '\t') name++;
            snprintf(h->name, sizeof(h->name), "%s", *name ? name : "XIAM model");
            x->session_loading_hamiltonian = i;
        } else x->session_loading_hamiltonian = -1;
        return 1;
    }
    if (strcmp(line, "xiamend") == 0) {
        x->session_loading_hamiltonian = -1;
        return 1;
    }
    XiamHamiltonian *h = xiam_session_current(x);
    if (!h) return strncmp(line, "xiam", 4) == 0;
    if (strncmp(line, "xiamctrl ", 9) == 0) {
        int excluded = 0, loaded = 0, max_j = 30;
        int has_freq = 0, has_ints = 0, has_j = 0, axis = 0, automatic = 0, override = 0;
        double fmin = 0.0, fmax = 10000.0, temp = 1.0, concentration = 1.0;
        if (sscanf(line + 9, "%d %d %d %lf %lf %lf %lf %d %d %d %d %d %d",
                   &excluded, &loaded, &max_j, &fmin, &fmax, &temp, &concentration,
                   &has_freq, &has_ints, &has_j, &axis, &automatic, &override) == 13) {
            h->simulate_excluded = excluded != 0; h->loaded = loaded != 0; h->max_j = max_j;
            h->fmin_mhz = fmin; h->fmax_mhz = fmax; h->temperature_k = temp;
            h->concentration = isfinite(concentration) && concentration >= 0.0 ? concentration : 1.0;
            h->has_freq = has_freq != 0; h->has_ints = has_ints != 0; h->has_j = has_j != 0;
            h->axis_convention = axis; h->axis_auto_convention = automatic; h->axis_override = override;
        }
        return 1;
    }
    if (strncmp(line, "xiampath ", 9) == 0) {
        snprintf(h->input_path, sizeof(h->input_path), "%s", line + 9);
        return 1;
    }
    if (strncmp(line, "xiamoutput ", 11) == 0) {
        snprintf(h->output_path, sizeof(h->output_path), "%s", line + 11);
        return 1;
    }
    if (strncmp(line, "xiamtrace ", 10) == 0) {
        int cr = 0, cg = 0, cb = 0, ca = 0, mx0 = 0, my0 = 0, mz0 = 0;
        if (sscanf(line + 10, "%d %d %d %d %d %d %d", &cr, &cg, &cb, &ca,
                   &mx0, &my0, &mz0) == 7) {
            h->trace_color = (SDL_Color){(Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca};
            h->mu_excluded[0] = mx0 != 0; h->mu_excluded[1] = my0 != 0; h->mu_excluded[2] = mz0 != 0;
        }
        return 1;
    }
    if (strncmp(line, "xiamline ", 9) == 0) {
        double value = 0.0;
        if (sscanf(line + 9, "%lf", &value) == 1 && isfinite(value) && value > 0.0)
            h->line_error_mhz = value;
        return 1;
    }
    if (strncmp(line, "xiamfitoutput ", 14) == 0) {
        snprintf(h->fit_output_path, sizeof(h->fit_output_path), "%s", line + 14);
        return 1;
    }
    if (strncmp(line, "xiamparam ", 10) == 0) {
        XiamParameter p = {0};
        int quadrupole = 0, frequency = 0;
        if (sscanf(line + 10, "%47s %lf %lf %d %d %d %d %d", p.name, &p.value_ghz,
                   &p.error_ghz, &p.source_line, &p.fit_source_line, &p.fit_method,
                   &quadrupole, &frequency) == 8 && h->n_params < MAX_XIAM_PARAMS) {
            p.is_quadrupole = quadrupole != 0; p.is_frequency = frequency != 0;
            h->params[h->n_params++] = p;
        }
        return 1;
    }
    if (strncmp(line, "xiamsource ", 11) == 0) {
        int index = -1, used = 0;
        if (sscanf(line + 11, "%d%n", &index, &used) == 1 &&
            index >= 0 && index < MAX_XIAM_SOURCE_LINES) {
            const char *text = line + 11 + used;
            if (*text == ' ') text++;
            snprintf(h->source_lines[index], sizeof(h->source_lines[index]), "%s", text);
            if (index + 1 > h->n_source_lines) h->n_source_lines = index + 1;
        }
        return 1;
    }
    return 0;
}

void xiam_session_finalize(AppState *s) {
    if (!s) return;
    XiamState *x = &s->xiam;
    for (int i = 0; i < x->n_hamiltonians; i++) {
        XiamHamiltonian *stored = &x->hamiltonian[i];
        XiamParameter saved[MAX_XIAM_PARAMS];
        int n_saved = stored->n_params;
        memcpy(saved, stored->params, (size_t)n_saved * sizeof(saved[0]));
        int loaded = stored->loaded, max_j = stored->max_j;
        int has_freq = stored->has_freq, has_ints = stored->has_ints, has_j = stored->has_j;
        int axis = stored->axis_convention, automatic = stored->axis_auto_convention;
        int override = stored->axis_override, excluded = stored->simulate_excluded;
        double fmin = stored->fmin_mhz, fmax = stored->fmax_mhz;
        double temp = stored->temperature_k, concentration = stored->concentration;
        xiam_restore_active(x, i);
        if (x->n_source_lines > 0) {
            xiam_parse_source(s);
            for (int k = 0; k < n_saved; k++) {
                int at = xiam_find_parameter(x, saved[k].name);
                if (at >= 0) {
                    x->params[at].value_ghz = saved[k].value_ghz;
                    x->params[at].error_ghz = saved[k].error_ghz;
                    x->params[at].fit_method = saved[k].fit_method;
                } else if (x->n_params < MAX_XIAM_PARAMS) {
                    x->params[x->n_params++] = saved[k];
                }
            }
        }
        x->loaded = loaded; x->max_j = max_j; x->fmin_mhz = fmin; x->fmax_mhz = fmax;
        x->temperature_k = temp > 0.0 ? temp : 1.0;
        x->concentration = isfinite(concentration) && concentration >= 0.0 ? concentration : 1.0;
        x->has_freq = has_freq; x->has_ints = has_ints; x->has_j = has_j;
        x->axis_override = override; x->axis_auto_convention = automatic; x->axis_convention = axis;
        x->simulate_excluded = excluded;
        xiam_update_axis_convention(x);
        /* A session saved before constants were kept in canonical order (or
           any other already-scrambled .xi) straightens out right here, once
           per hamiltonian, instead of requiring every stray constant to be
           deleted and re-added by hand. */
        xiam_reposition_all_parameters(x);
        xiam_store_active(x);
    }
    int active = x->session_active_hamiltonian;
    if (active < 0 || active >= x->n_hamiltonians) active = x->n_hamiltonians ? 0 : -1;
    if (active >= 0) xiam_restore_active(x, active);
    else xiam_clear_active(x);
    x->active_hamiltonian = active;
    x->session_loading_hamiltonian = -1;
    x->session_active_hamiltonian = active;
    x->session_dirty = 0;
}

int xiam_restore_session_predictions(AppState *s) {
    if (!s) return 0;
    XiamState *x = &s->xiam;
    int saved_active = x->active_hamiltonian, restored = 0;
    for (int i = 0; i < x->n_hamiltonians; i++) {
        if (!xiam_select_hamiltonian(x, i) || !x->output_path[0]) continue;
        FILE *fp = fopen(x->output_path, "r");
        if (!fp) continue;
        fclose(fp);
        int before = s->n_pred;
        if (xiam_load_output_file(s, x->output_path) && s->n_pred > before) restored++;
    }
    if (saved_active >= 0 && saved_active < x->n_hamiltonians)
        xiam_select_hamiltonian(x, saved_active);
    xiam_apply_concentrations(s);
    if (restored > 0) {
        snprintf(x->status, sizeof(x->status),
                 "Restored %d XIAM catalogue%s from the session.",
                 restored, restored == 1 ? "" : "s");
        xiam_store_active(x);
    }
    s->predfit.session_dirty = 0;
    x->session_dirty = 0;
    return restored;
}

void xiam_render(AppState *s) {
    if (!s || !s->xiam.open || !s->xiam.renderer) return;
    XiamState *x = &s->xiam; SDL_Renderer *r = x->renderer;
    int ww = 0, dw = 0, dh = 0; SDL_GetWindowSize(x->window, &ww, &dh); SDL_GetRendererOutputSize(r, &dw, &dh); if (ww > 0 && dw > 0) SDL_RenderSetScale(r, (float)dw / (float)ww, (float)dw / (float)ww);
    XiamUI u = xiam_ui(x); xiam_clamp_scrolls(s, &u); xiam_store_active(x);
    SDL_SetRenderDrawColor(r, 19, 20, 22, 255); SDL_RenderClear(r);
    ui_fill(r, (SDL_Rect){0, 0, u.w, XIAM_HEADER_H}, UI_TITLEBAR);
    ui_text(r, UI_FONT_TITLE, "XIAM — native Hamiltonian workspace", XIAM_PAD, 12, UI_TEXT);
    ui_hline(r, 0, u.w, u.footer.y, UI_LINE);
    /* Page for page the same workspace as Pred&Fit Advanced. */
    const char *tabs[XIAM_TAB_COUNT] = {"Parameters", "Lines", "Fitting", "States", "Hamiltonians", "Simulation"};
    int mx = 0, my = 0, down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (SDL_GetMouseFocus() != x->window) mx = my = -999;
    for (int i = 0; i < XIAM_TAB_COUNT; i++) ui_button(r, u.tab[i], tabs[i], -1, UI_BTN_QUIET, x->active_tab == i, mx, my, down);
    xiam_render_navigator(r, x, &u, mx, my, down);
    if (x->active_tab == 0) xiam_render_parameters(r, s, x, &u, mx, my, down);
    else if (x->active_tab == 1) xiam_render_lines(r, s, x, &u, mx, my, down);
    else if (x->active_tab == 2) xiam_render_fitting(r, s, x, &u, mx, my, down);
    else if (x->active_tab == 3) xiam_render_states(r, s, x, &u, mx, my, down);
    else if (x->active_tab == 4) xiam_render_hamiltonians(r, x, &u, mx, my, down);
    else xiam_render_simulation(r, s, x, &u, mx, my, down);
    if (!x->status[0])
        ui_text(r, UI_FONT_SANS_SM, "Open a .xi input to begin a native XIAM project.",
                XIAM_PAD, u.footer.y + 17, UI_FAINT);
    SDL_RenderPresent(r);
}
