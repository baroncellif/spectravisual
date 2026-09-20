#include "xiam.h"

#include "controller.h"
#include "layout.h"
#include "loader.h"
#include "settings.h"
#include "ui_chrome.h"
#include "ui_theme.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define XIAM_DIR_NAME ".xiam"
#define XIAM_SOURCE_MAX 255

enum { XIAM_EDIT_NONE = -1, XIAM_EDIT_FMIN = -2, XIAM_EDIT_FMAX = -3,
       XIAM_EDIT_TEMP = -4, XIAM_EDIT_JMAX = -5 };

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
    for (int i = 0; x && i < x->n_params; i++) if (xiam_equal(x->params[i].name, name)) return i;
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

static void xiam_update_axis_convention(XiamState *x) {
    int bj = xiam_find_parameter(x, "BJ"), bk = xiam_find_parameter(x, "BK"), bm = xiam_find_parameter(x, "B-");
    x->axis_convention = 0;
    if (bj < 0 || bk < 0 || bm < 0) return;
    double bx = x->params[bj].value_ghz + x->params[bm].value_ghz;
    double by = x->params[bj].value_ghz - x->params[bm].value_ghz;
    double bz = x->params[bj].value_ghz + x->params[bk].value_ghz;
    if (bz >= bx && bz >= by) x->axis_convention = 1; /* z is a, x/y are b/c */
    else if (bz <= bx && bz <= by) x->axis_convention = 2; /* z is c, x/y are a/b */
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
            if (strstr(lower, "block iii")) block = 3;
            else if (strstr(lower, "block iv")) block = 4;
            else if (strstr(lower, "block v")) block = 5;
            else if (strstr(lower, "block vii")) block = 7;
            else if (strstr(lower, "block viii")) block = 8;
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
            char pname[64] = ""; if (sscanf(tail, "%63s", pname) == 1) {
                int pi = xiam_find_parameter(x, pname);
                if (pi >= 0) x->params[pi].fit_method = xiam_equal(key, "fit") ? 1 : xiam_equal(key, "dqu") ? 2 : 3;
            }
            continue;
        }
        if (block != 3 || xiam_known_control(key) || x->n_params >= MAX_XIAM_PARAMS) continue;
        char *end = NULL; double value = xiam_number(tail, &end);
        if (end == tail) continue;
        XiamParameter *p = &x->params[x->n_params++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", key);
        p->value_ghz = value; p->source_line = line_no;
        p->is_quadrupole = xiam_is_quadrupole(key); p->is_frequency = xiam_is_frequency(key);
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
    free(s->pred_lines); s->pred_lines = rows; s->n_pred = n; s->pred_global_max = top > 0.0 ? top : 1.0;
    s->n_selected = 0; s->pxmin = lo; s->pxmax = hi; s->cat_temp_k = s->xiam.temperature_k; s->rot_temp_k = s->xiam.temperature_k;
    s->predfit.generated_catalog_active = 0; s->predfit.generated_catalog_pending = 0; s->predfit.n_simulated = 0;
    snprintf(s->pred_path, sizeof(s->pred_path), "%s", path ? path : "XIAM .xo");
    int first = !s->data_loaded; s->data_loaded = 1;
    if (first) { s->xmin=lo; s->xmax=hi; s->vxmin=lo; s->vxmax=hi; s->pvxmin=lo; s->pvxmax=hi; s->ymin=s->vymin=0.0; s->ymax=s->vymax=1.0; s->bar_x=s->pbar_x=(lo+hi)*0.5; }
    snprintf(s->status_message, sizeof(s->status_message), "XIAM: loaded %d native prediction lines (MHz; SPCAT intensity retained).", n);
    s->error_message[0] = '\0';
}

static int xiam_fatal_output(const char *path, char *diag, size_t diag_size) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 1;
    char line[512]; int fatal = 0; size_t used = 0;
    if (diag_size) diag[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char low[512]; xiam_lower(low, sizeof(low), line);
        if (strstr(low, "input error") || strstr(low, "fatal error") || strstr(low, "segmentation")) fatal = 1;
        if (fatal && used + strlen(line) + 1 < diag_size) { strcpy(diag + used, line); used += strlen(line); }
    }
    fclose(fp); return fatal;
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

static int xiam_line_is_transition(const char *line) {
    if (xiam_is_comment(line)) return 0;
    const char *at = line; int v = 0;
    for (int i = 0; i < 6; i++) { char *end = NULL; at = xiam_ltrim((char *)at); if (!xiam_parse_int(at, &end, &v)) return 0; at = end; }
    return 1;
}

static void xiam_write_assignment(FILE *fp, const Assignment *a) {
    const PredLine *p = &a->pred;
    fprintf(fp, " %d %d %d  %d %d %d", p->Ju,p->Kau,p->Kcu,p->Jl,p->Kal,p->Kcl);
    if (p->xiam.f1u || p->xiam.f1l) fprintf(fp, " F1 %d %d", p->xiam.f1u,p->xiam.f1l);
    if (p->xiam.fu || p->xiam.fl) fprintf(fp, " F %d %d", p->xiam.fu,p->xiam.fl);
    fprintf(fp, " S %d", p->xiam.su);
    if (p->xiam.vu || p->xiam.vl) fprintf(fp, " V %d", p->xiam.vu);
    if (p->xiam.bu || p->xiam.bl) fprintf(fp, " B %d", p->xiam.bu);
    if (p->xiam.ku || p->xiam.kl) fprintf(fp, " K %d %d", p->xiam.ku,p->xiam.kl);
    if (p->xiam.tu || p->xiam.tl) fprintf(fp, " t %d %d", p->xiam.tu,p->xiam.tl);
    fprintf(fp, " = %.9f MHz Err 0.010\n", a->exp_freq);
}

static int xiam_write_input(AppState *s, const char *path, int fitting) {
    XiamState *x = &s->xiam; FILE *fp = fopen(path, "w"); if (!fp) return 0;
    for (int i = 0; i < x->n_source_lines; i++) {
        char current[256], key[64] = "", *at;
        snprintf(current, sizeof(current), "%s", x->source_lines[i]); at = xiam_ltrim(current);
        if (!xiam_is_comment(at)) sscanf(at, "%63s", key);
        if (fitting && xiam_line_is_transition(current)) { fprintf(fp, "\\%s\n", current); continue; }
        int pi = -1; for (int k = 0; k < x->n_params; k++) if (x->params[k].source_line == i) { pi = k; break; }
        if (pi >= 0) { char changed[512]; xiam_replace_number(changed, sizeof(changed), x->source_lines[i], x->params[pi].value_ghz); fprintf(fp, "%s\n", changed); continue; }
        if (xiam_equal(key, "ints")) { fprintf(fp, " ints %d\n", fitting ? 1 : 2); continue; }
        if (xiam_equal(key, "freq")) { fprintf(fp, " freq %.12g %.12g\n", x->fmin_mhz/1000.0, x->fmax_mhz/1000.0); continue; }
        if (xiam_equal(key, "freq_l")) { fprintf(fp, " freq_l %.12g\n", x->fmin_mhz/1000.0); continue; }
        if (xiam_equal(key, "freq_h")) { fprintf(fp, " freq_h %.12g\n", x->fmax_mhz/1000.0); continue; }
        if (xiam_equal(key, "temp")) { fprintf(fp, " temp %.12g\n", x->temperature_k); continue; }
        if (xiam_equal(key, "j")) { fprintf(fp, " J %d\n", x->max_j); continue; }
        fprintf(fp, "%s\n", x->source_lines[i]);
    }
    if (!x->has_ints) fprintf(fp, "\n ints %d\n", fitting ? 1 : 2);
    if (!x->has_freq) fprintf(fp, " freq %.12g %.12g\n", x->fmin_mhz/1000.0, x->fmax_mhz/1000.0);
    if (!x->has_j) fprintf(fp, " J %d\n", x->max_j);
    if (fitting) {
        int n = 0; for (int i = 0; i < s->n_assignments; i++) if (s->assignments[i].fit_enabled && s->assignments[i].pred.backend == PRED_BACKEND_XIAM && s->assignments[i].hamiltonian_id == x->model_id) n++;
        fprintf(fp, "\\ SpectraVisual XIAM fit assignments (%d selected)\n", n);
        for (int i = 0; i < s->n_assignments; i++) if (s->assignments[i].fit_enabled && s->assignments[i].pred.backend == PRED_BACKEND_XIAM && s->assignments[i].hamiltonian_id == x->model_id) xiam_write_assignment(fp, &s->assignments[i]);
    }
    int bad = ferror(fp) || fclose(fp) != 0; return !bad;
}

static void xiam_work_file(const XiamState *x, const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s", x->work_dir, name);
}

static int xiam_prepare_work_dir(AppState *s) {
    XiamState *x = &s->xiam;
    if (s->settings.data_dir[0]) snprintf(x->work_dir, sizeof(x->work_dir), "%s/%s", s->settings.data_dir, XIAM_DIR_NAME);
    else snprintf(x->work_dir, sizeof(x->work_dir), "%s", XIAM_DIR_NAME);
    if (mkdir(x->work_dir, 0700) != 0 && errno != EEXIST) { snprintf(x->status,sizeof(x->status),"Cannot create XIAM workspace: %s",strerror(errno)); return 0; }
    return 1;
}

static int xiam_run(AppState *s, int fitting) {
    XiamState *x = &s->xiam;
    if (!x->loaded) { snprintf(x->status,sizeof(x->status),"Open a .xi input first (drop it onto the main window)."); return 0; }
    if (!s->settings.xiam_path[0] || access(s->settings.xiam_path, X_OK) != 0) { snprintf(x->status,sizeof(x->status),"Set the executable XIAM-2NQ v0.44 path in Settings."); return 0; }
    if (fitting) { int n = 0; for (int i=0;i<s->n_assignments;i++) if (s->assignments[i].fit_enabled && s->assignments[i].pred.backend == PRED_BACKEND_XIAM && s->assignments[i].hamiltonian_id == x->model_id) n++; if (!n) { snprintf(x->status,sizeof(x->status),"Select and assign at least one prediction from this .xi model before fitting."); return 0; } }
    if (!xiam_prepare_work_dir(s)) return 0;
    char in[700], out[700]; xiam_work_file(x,"model.xi",in,sizeof(in)); xiam_work_file(x,"model.xo",out,sizeof(out));
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
    snprintf(x->output_path, sizeof(x->output_path), "%s", out);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || xiam_fatal_output(out,x->diagnostics,sizeof(x->diagnostics))) {
        snprintf(x->status,sizeof(x->status),"XIAM did not complete; see retained model.xo diagnostics."); return 0;
    }
    return 1;
}

static void xiam_parse_fit_parameters(XiamState *x, const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return;
    char line[1024]; int in = 0;
    while (fgets(line,sizeof(line),fp)) {
        if (strstr(line,"Parameters") && strstr(line,"Errors")) { in = 1; continue; }
        if (!in) continue;
        char name[64]="", value[128]="", error[128]="";
        if (sscanf(line," %63s %127s { %127[^}]",name,value,error) < 3) continue;
        int pi = xiam_find_parameter(x,name); if (pi < 0) continue;
        x->params[pi].value_ghz = xiam_number(value,NULL); x->params[pi].error_ghz = xiam_number(error,NULL);
    }
    fclose(fp); xiam_update_axis_convention(x);
}

void xiam_init(AppState *s) { if (!s) return; s->xiam.edit_param = XIAM_EDIT_NONE; }

void xiam_close(AppState *s) {
    if (!s) return; XiamState *x=&s->xiam;
    if (x->renderer) SDL_DestroyRenderer(x->renderer); if (x->window) SDL_DestroyWindow(x->window);
    x->renderer=NULL; x->window=NULL; x->window_id=0; x->open=0; x->edit_param=XIAM_EDIT_NONE;
}
void xiam_dispose(AppState *s) { xiam_close(s); }

void xiam_open(AppState *s) {
    if (!s) return; XiamState *x=&s->xiam;
    if (x->window) { SDL_RaiseWindow(x->window); return; }
    x->window=SDL_CreateWindow("XIAM — one .xi Hamiltonian",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,980,700,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!x->window) { snprintf(x->status,sizeof(x->status),"Could not open XIAM window."); return; }
    SDL_SetWindowMinimumSize(x->window,780,480); x->renderer=SDL_CreateRenderer(x->window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!x->renderer) { SDL_DestroyWindow(x->window); x->window=NULL; snprintf(x->status,sizeof(x->status),"Could not create XIAM renderer."); return; }
    SDL_SetRenderDrawBlendMode(x->renderer,SDL_BLENDMODE_BLEND); x->window_id=SDL_GetWindowID(x->window); x->open=1;
}

int xiam_load_input_file(AppState *s, const char *path) {
    if (!s || !path || !path[0]) return 0; XiamState *x=&s->xiam;
    if (!xiam_read_source(x,path)) { snprintf(x->status,sizeof(x->status),"Cannot read XIAM input: %s",path); return 0; }
    snprintf(x->input_path,sizeof(x->input_path),"%s",path); x->loaded=1; xiam_parse_source(s);
    x->model_id = xiam_model_id_from_path(x->input_path);
    if (SDL_WasInit(SDL_INIT_VIDEO)) xiam_open(s);
    snprintf(x->status,sizeof(x->status),"Loaded %s: %d molecular parameters; input/output are displayed in MHz.",strrchr(path,'/')?strrchr(path,'/')+1:path,x->n_params);
    return 1;
}

int xiam_load_output_file(AppState *s, const char *path) {
    PredLine *rows=NULL; int n=0; double lo=0,hi=0,top=0;
    int model_id = s && s->xiam.loaded ? s->xiam.model_id : xiam_model_id_from_path(path);
    if (!s || !path || !xiam_read_output_records(path,model_id,&rows,&n,&lo,&hi,&top)) { if(s) snprintf(s->xiam.status,sizeof(s->xiam.status),"No readable native XIAM prediction records in %s.",path?path:"output"); return 0; }
    xiam_adopt_prediction(s,rows,n,lo,hi,top,path); snprintf(s->xiam.output_path,sizeof(s->xiam.output_path),"%s",path); if (SDL_WasInit(SDL_INIT_VIDEO)) xiam_open(s); return 1;
}

int xiam_calculate(AppState *s) {
    if (!xiam_run(s,0)) return 0;
    int ok=xiam_load_output_file(s,s->xiam.output_path);
    if (ok) snprintf(s->xiam.status,sizeof(s->xiam.status),"Prediction complete: %d lines; native SPCAT intensity is used unchanged.",s->n_pred);
    return ok;
}

int xiam_fit(AppState *s) {
    if (!xiam_run(s,1)) return 0;
    xiam_parse_fit_parameters(&s->xiam,s->xiam.output_path);
    snprintf(s->xiam.status,sizeof(s->xiam.status),"Fit completed; updated XIAM parameters are now used for the refreshed prediction.");
    return xiam_calculate(s);
}

static void xiam_edit_begin(XiamState *x, int what) {
    x->edit_param=what; x->edit_replace=1;
    if (what >= 0) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",xiam_display_value(&x->params[what]));
    else if (what==XIAM_EDIT_FMIN) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->fmin_mhz);
    else if (what==XIAM_EDIT_FMAX) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->fmax_mhz);
    else if (what==XIAM_EDIT_TEMP) snprintf(x->edit_buf,sizeof(x->edit_buf),"%.12g",x->temperature_k);
    else snprintf(x->edit_buf,sizeof(x->edit_buf),"%d",x->max_j);
    x->edit_caret=x->edit_anchor=(int)strlen(x->edit_buf); SDL_StartTextInput();
}

static void xiam_edit_commit(XiamState *x) {
    if (x->edit_param == XIAM_EDIT_NONE) return;
    double v=xiam_number(x->edit_buf,NULL);
    if (x->edit_param >= 0 && x->edit_param < x->n_params) x->params[x->edit_param].value_ghz=xiam_source_value(&x->params[x->edit_param],v);
    else if (x->edit_param==XIAM_EDIT_FMIN) x->fmin_mhz=v;
    else if (x->edit_param==XIAM_EDIT_FMAX) x->fmax_mhz=v;
    else if (x->edit_param==XIAM_EDIT_TEMP && v>0.0) x->temperature_k=v;
    else if (x->edit_param==XIAM_EDIT_JMAX) x->max_j=(int)lround(v);
    x->edit_param=XIAM_EDIT_NONE; SDL_StopTextInput();
}

static void xiam_edit_insert(XiamState *x, const char *text) {
    if (!text || !*text) return; int n=(int)strlen(x->edit_buf), add=(int)strlen(text);
    if (x->edit_replace) { x->edit_buf[0]='\0'; n=0; x->edit_caret=x->edit_anchor=0; x->edit_replace=0; }
    if (n+add >= (int)sizeof(x->edit_buf)) add=(int)sizeof(x->edit_buf)-n-1;
    if (add<=0) return; memcpy(x->edit_buf+x->edit_caret+add,x->edit_buf+x->edit_caret,(size_t)(n-x->edit_caret+1)); memcpy(x->edit_buf+x->edit_caret,text,(size_t)add); x->edit_caret+=add; x->edit_anchor=x->edit_caret;
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

int xiam_handle_event(AppState *s, const SDL_Event *e) {
    if (!s || !e || !s->xiam.open) return 0; XiamState *x=&s->xiam;
    Uint32 id = x->window_id;
    if (e->type==SDL_WINDOWEVENT && e->window.windowID==id && e->window.event==SDL_WINDOWEVENT_CLOSE) { xiam_close(s); return 1; }
    if (x->edit_param != XIAM_EDIT_NONE && e->type==SDL_TEXTINPUT && e->text.windowID==id) { xiam_edit_insert(x,e->text.text); return 1; }
    if (x->edit_param != XIAM_EDIT_NONE && e->type==SDL_KEYDOWN && e->key.windowID==id) {
        SDL_Keycode k=e->key.keysym.sym; if(k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_TAB){xiam_edit_commit(x);return 1;} if(k==SDLK_ESCAPE){x->edit_param=XIAM_EDIT_NONE;SDL_StopTextInput();return 1;}
        if(k==SDLK_BACKSPACE&&x->edit_caret>0){memmove(x->edit_buf+x->edit_caret-1,x->edit_buf+x->edit_caret,strlen(x->edit_buf+x->edit_caret)+1);x->edit_caret--;x->edit_anchor=x->edit_caret;} else if(k==SDLK_LEFT&&x->edit_caret>0)x->edit_caret--; else if(k==SDLK_RIGHT&&x->edit_caret<(int)strlen(x->edit_buf))x->edit_caret++; return 1;
    }
    if (e->type==SDL_KEYDOWN && e->key.windowID==id && e->key.keysym.sym==SDLK_ESCAPE) { xiam_close(s); return 1; }
    if (e->type==SDL_MOUSEWHEEL && e->wheel.windowID==id) { x->params_scroll-=e->wheel.y*3; if(x->params_scroll<0)x->params_scroll=0; if(x->params_scroll>x->n_params-1)x->params_scroll=x->n_params-1; return 1; }
    if (e->type!=SDL_MOUSEBUTTONDOWN || e->button.windowID!=id) return 0;
    int w=0,h=0; SDL_GetWindowSize(x->window,&w,&h); int px=e->button.x,py=e->button.y;
    SDL_Rect open={18,52,104,28},predict={130,52,112,28},fit={250,52,92,28},reload={350,52,100,28};
    if(point_in_rect(px,py,open)){char path[512];if(xiam_pick_input(path,sizeof(path)))xiam_load_input_file(s,path);else snprintf(x->status,sizeof(x->status),"Choose or drag a .xi input onto the main window.");return 1;} if(point_in_rect(px,py,predict)){xiam_edit_commit(x);xiam_calculate(s);return 1;} if(point_in_rect(px,py,fit)){xiam_edit_commit(x);xiam_fit(s);return 1;} if(point_in_rect(px,py,reload)){xiam_load_input_file(s,x->input_path);return 1;}
    SDL_Rect fmin={470,52,104,28},fmax={582,52,104,28},temp={694,52,82,28},jmax={784,52,65,28};
    if(point_in_rect(px,py,fmin)){xiam_edit_begin(x,XIAM_EDIT_FMIN);return 1;} if(point_in_rect(px,py,fmax)){xiam_edit_begin(x,XIAM_EDIT_FMAX);return 1;} if(point_in_rect(px,py,temp)){xiam_edit_begin(x,XIAM_EDIT_TEMP);return 1;} if(point_in_rect(px,py,jmax)){xiam_edit_begin(x,XIAM_EDIT_JMAX);return 1;}
    int first=x->params_scroll, row=(py-142)/24; if(py>=142 && row>=0){int p=first+row;if(p>=0&&p<x->n_params){xiam_edit_begin(x,p);return 1;}}
    (void)w; return 1;
}

void xiam_render(AppState *s) {
    if (!s || !s->xiam.open || !s->xiam.renderer) return; XiamState *x=&s->xiam; SDL_Renderer *r=x->renderer; int w=0,h=0,dw=0,dh=0; SDL_GetWindowSize(x->window,&w,&h);SDL_GetRendererOutputSize(r,&dw,&dh);if(w>0&&dw>0)SDL_RenderSetScale(r,(float)dw/w,(float)dw/w);
    SDL_SetRenderDrawColor(r,19,20,22,255);SDL_RenderClear(r);ui_fill(r,(SDL_Rect){0,0,w,42},UI_TITLEBAR);ui_text(r,UI_FONT_TITLE,"XIAM — one .xi Hamiltonian",18,12,UI_TEXT);
    int mx=0,my=0,down=(SDL_GetMouseState(&mx,&my)&SDL_BUTTON(SDL_BUTTON_LEFT))!=0;if(SDL_GetMouseFocus()!=x->window){mx=my=-999;}
    ui_button(r,(SDL_Rect){18,52,104,28},"Open .xi",-1,UI_BTN_QUIET,0,mx,my,down);ui_button(r,(SDL_Rect){130,52,112,28},"Predict",-1,UI_BTN_PRIMARY,0,mx,my,down);ui_button(r,(SDL_Rect){250,52,92,28},"Fit",-1,UI_BTN_PRIMARY,0,mx,my,down);ui_button(r,(SDL_Rect){350,52,100,28},"Reload .xi",-1,UI_BTN_QUIET,0,mx,my,down);
    char b[256];ui_text(r,UI_FONT_SANS_SM,"MHz",470,38,UI_FAINT);snprintf(b,sizeof(b),"%.8g",x->fmin_mhz);ui_field_ex(r,(SDL_Rect){470,52,104,28},NULL,x->edit_param==XIAM_EDIT_FMIN?x->edit_buf:b,NULL,x->edit_param==XIAM_EDIT_FMIN,x->edit_caret,x->edit_anchor);snprintf(b,sizeof(b),"%.8g",x->fmax_mhz);ui_field_ex(r,(SDL_Rect){582,52,104,28},NULL,x->edit_param==XIAM_EDIT_FMAX?x->edit_buf:b,NULL,x->edit_param==XIAM_EDIT_FMAX,x->edit_caret,x->edit_anchor);ui_text(r,UI_FONT_SANS_SM,"T / K",694,38,UI_FAINT);snprintf(b,sizeof(b),"%.6g",x->temperature_k);ui_field_ex(r,(SDL_Rect){694,52,82,28},NULL,x->edit_param==XIAM_EDIT_TEMP?x->edit_buf:b,NULL,x->edit_param==XIAM_EDIT_TEMP,x->edit_caret,x->edit_anchor);ui_text(r,UI_FONT_SANS_SM,"Jmax",784,38,UI_FAINT);snprintf(b,sizeof(b),"%d",x->max_j);ui_field_ex(r,(SDL_Rect){784,52,65,28},NULL,x->edit_param==XIAM_EDIT_JMAX?x->edit_buf:b,NULL,x->edit_param==XIAM_EDIT_JMAX,x->edit_caret,x->edit_anchor);
    ui_hline(r,18,w-18,96,UI_LINE);ui_text(r,UI_FONT_SANS_SM,"Parameter",24,113,UI_DIM);ui_text(r,UI_FONT_SANS_SM,"value shown in MHz",260,113,UI_DIM);ui_text(r,UI_FONT_SANS_SM,"error",500,113,UI_DIM);ui_text(r,UI_FONT_SANS_SM,"fit",660,113,UI_DIM);
    const char *axes=x->axis_convention==1?"Prolate mapping: XIAM x/y/z = b/c/a (z is A).":x->axis_convention==2?"Oblate mapping: XIAM x/y/z = a/b/c (z is C).":"Axis mapping needs BJ, BK and B-; verify before assigning labels.";ui_text(r,UI_FONT_SANS_SM,axes,24,h-48,UI_WARN);
    SDL_Rect rows={18,138,w-36,h-202};ui_fill(r,rows,UI_INPUT);ui_frame(r,rows,UI_LINE);SDL_RenderSetClipRect(r,&rows);int vis=rows.h/24;for(int row=0;row<vis;row++){int i=x->params_scroll+row;if(i>=x->n_params)break;int y=rows.y+row*24;XiamParameter*p=&x->params[i];if(i&1)ui_fill(r,(SDL_Rect){rows.x,y,rows.w,24},UI_CHROME);char label[96];xiam_parameter_label(x,p,label,sizeof(label));ui_text(r,UI_FONT_MONO_SM,label,rows.x+7,y+6,UI_TEXT);snprintf(b,sizeof(b),"%.12g%s",xiam_display_value(p),p->is_frequency?" MHz":"");if(x->edit_param==i)snprintf(b,sizeof(b),"%s",x->edit_buf);ui_text(r,UI_FONT_MONO_SM,b,rows.x+235,y+6,x->edit_param==i?UI_ACCENT_TEXT:UI_TEXT);if(p->error_ghz!=0.0){snprintf(b,sizeof(b),"%.5g",xiam_display_error(p));ui_text(r,UI_FONT_MONO_SM,b,rows.x+475,y+6,UI_DIM);}ui_text(r,UI_FONT_MONO_SM,p->fit_method==1?"fit":p->fit_method==2?"dqu":p->fit_method==3?"dqx":"—",rows.x+635,y+6,p->fit_method?UI_ACCENT_TEXT:UI_FAINT);}
    SDL_RenderSetClipRect(r,NULL);ui_hline(r,18,w-18,h-70,UI_LINE);ui_text(r,UI_FONT_SANS_SM,x->status[0]?x->status:"Drop a .xi input onto the main window. Drop a .xo to inspect a native prediction.",24,h-28,UI_TEXT);SDL_RenderPresent(r);
}
