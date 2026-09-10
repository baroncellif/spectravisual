#include "settings.h"
#include "layout.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define SETTINGS_FILE "spectravisual.settings"

static char g_path[PATH_MAX + 64];

/* ---------------------------------------------------------------------------
 *  Where the file lives
 * ------------------------------------------------------------------------- */
static void resolve_settings_path(const char *argv0) {
    char exe[PATH_MAX] = {0};
    int got = 0;

#ifdef __APPLE__
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) == 0 && realpath(raw, exe)) got = 1;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) { exe[n] = '\0'; got = 1; }
#endif
    if (!got && argv0 && argv0[0] && realpath(argv0, exe)) got = 1;

    if (got) {
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        snprintf(g_path, sizeof(g_path), "%s/%s", exe, SETTINGS_FILE);
    } else {
        snprintf(g_path, sizeof(g_path), "%s", SETTINGS_FILE);
    }
}

const char *settings_file_path(void) { return g_path; }

/* ---------------------------------------------------------------------------
 *  Defaults, load and save
 * ------------------------------------------------------------------------- */
static const SDL_Color DEFAULT_TRACE[MAX_SPECTRA] = {
    {205, 214, 225, 235}, { 90, 200, 250, 235}, {255, 170,  80, 235}, {130, 220, 130, 235},
    {235, 130, 200, 235}, {245, 220,  90, 235}, {170, 150, 245, 235}, {240, 110, 110, 235}
};

void settings_restore_defaults(AppState *s) {
    AppSettings *d = &s->settings;
    d->trace_width = 1;
    for (int i = 0; i < MAX_SPECTRA; i++) d->trace_color[i] = DEFAULT_TRACE[i];
    d->pred_width = 1;
    d->profile_width = 1;
    d->profile_color   = UI_ACCENT;
    d->profile_opacity = 65;
    d->trace_opacity   = 100;
    d->stick_opacity   = 100;
    d->peak_color      = UI_WARN;
    d->assigned_color  = UI_OK;
    d->bar_color       = UI_WARN;
    d->cursor_color    = UI_ACCENT;
    d->show_grid = 1;
    d->show_legend = 1;
    d->show_peak_labels = 1;
    d->show_blend_marks = 1;
    d->plot_bg = 0;

    d->nav_pan_px           = 100.0;
    d->nav_zoom_factor      = 1.10;
    d->nav_intensity_factor = 1.10;
    d->nav_bar_px           = 4.0;
    d->nav_fast_mult        = 3.0;
    d->nav_trace_shift      = 0.05;
    d->nav_wheel_scroll     = 45.0;

    d->def_pf_sig = 5;  d->def_pf_noise = 50;  d->def_pf_thresh = 3.0;
    d->def_avg_window = 10;
    d->def_lorentz = 0.0; d->def_gauss = 0.0;
    d->def_kaiser_beta = 1.0; d->def_kaiser_ceros = 6; d->def_kaiser_intrinsic = 0.0;
    d->def_line_error = 0.01;
    d->def_int_min = -10.0; d->def_int_max = 0.0;

    /* Left empty on purpose: an absolute path to someone else's home would be
       worse than nothing. Pred&Fit says what is missing and where to set it. */
    d->edit_id = -1;
    d->spcat_path[0] = '\0';
    d->spfit_path[0] = '\0';
    d->data_dir[0]   = '\0';
}

static void write_color(FILE *fp, const char *key, SDL_Color c) {
    fprintf(fp, "%s %d %d %d %d\n", key, c.r, c.g, c.b, c.a);
}

static int read_color(const char *value, SDL_Color *c) {
    int r = 0, g = 0, b = 0, a = 255;
    int n = sscanf(value, "%d %d %d %d", &r, &g, &b, &a);
    if (n < 3) return 0;
    c->r = (Uint8)r; c->g = (Uint8)g; c->b = (Uint8)b; c->a = (Uint8)(n >= 4 ? a : 255);
    return 1;
}

int settings_save(AppState *s) {
    AppSettings *d = &s->settings;
    FILE *fp = fopen(g_path, "w");
    if (!fp) {
        snprintf(d->status, sizeof(d->status), "Cannot write %s", g_path);
        return 0;
    }
    fputs("# SpectraVisual display settings\n", fp);
    fprintf(fp, "trace_width %d\n", d->trace_width);
    for (int i = 0; i < MAX_SPECTRA; i++) {
        char key[32]; snprintf(key, sizeof(key), "trace_color%d", i);
        write_color(fp, key, d->trace_color[i]);
    }
    fprintf(fp, "pred_width %d\n", d->pred_width);
    fprintf(fp, "profile_width %d\n", d->profile_width);
    fprintf(fp, "profile_opacity %d\n", d->profile_opacity);
    fprintf(fp, "trace_opacity %d\n", d->trace_opacity);
    fprintf(fp, "stick_opacity %d\n", d->stick_opacity);
    write_color(fp, "profile_color", d->profile_color);
    write_color(fp, "peak_color", d->peak_color);
    write_color(fp, "assigned_color", d->assigned_color);
    write_color(fp, "bar_color", d->bar_color);
    write_color(fp, "cursor_color", d->cursor_color);
    fprintf(fp, "show_grid %d\n", d->show_grid);
    fprintf(fp, "show_legend %d\n", d->show_legend);
    fprintf(fp, "show_peak_labels %d\n", d->show_peak_labels);
    fprintf(fp, "show_blend_marks %d\n", d->show_blend_marks);
    fprintf(fp, "plot_bg %d\n", d->plot_bg);

    fprintf(fp, "nav_pan_px %.6g\n", d->nav_pan_px);
    fprintf(fp, "nav_zoom_factor %.6g\n", d->nav_zoom_factor);
    fprintf(fp, "nav_intensity_factor %.6g\n", d->nav_intensity_factor);
    fprintf(fp, "nav_bar_px %.6g\n", d->nav_bar_px);
    fprintf(fp, "nav_fast_mult %.6g\n", d->nav_fast_mult);
    fprintf(fp, "nav_trace_shift %.6g\n", d->nav_trace_shift);
    fprintf(fp, "nav_wheel_scroll %.6g\n", d->nav_wheel_scroll);

    fprintf(fp, "def_pf_sig %d\n", d->def_pf_sig);
    fprintf(fp, "def_pf_noise %d\n", d->def_pf_noise);
    fprintf(fp, "def_pf_thresh %.6g\n", d->def_pf_thresh);
    fprintf(fp, "def_avg_window %d\n", d->def_avg_window);
    fprintf(fp, "def_lorentz %.6g\n", d->def_lorentz);
    fprintf(fp, "def_gauss %.6g\n", d->def_gauss);
    fprintf(fp, "def_kaiser_beta %.6g\n", d->def_kaiser_beta);
    fprintf(fp, "def_kaiser_ceros %d\n", d->def_kaiser_ceros);
    fprintf(fp, "def_kaiser_intrinsic %.6g\n", d->def_kaiser_intrinsic);
    fprintf(fp, "def_line_error %.6g\n", d->def_line_error);
    fprintf(fp, "def_int_min %.6g\n", d->def_int_min);
    fprintf(fp, "def_int_max %.6g\n", d->def_int_max);

    if (d->spcat_path[0]) fprintf(fp, "spcat_path %s\n", d->spcat_path);
    if (d->spfit_path[0]) fprintf(fp, "spfit_path %s\n", d->spfit_path);
    if (d->data_dir[0])   fprintf(fp, "data_dir %s\n", d->data_dir);
    fclose(fp);
    snprintf(d->status, sizeof(d->status), "Saved as default in %s", g_path);
    return 1;
}

static void settings_load(AppState *s) {
    AppSettings *d = &s->settings;
    FILE *fp = fopen(g_path, "r");
    if (!fp) return;
    char line[700];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;
        /* Trim the line ending: numbers ignore it, but a path would carry it
           into the stored value and into every message that prints it. */
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char key[64];
        if (sscanf(line, "%63s", key) != 1) continue;
        const char *value = line + strlen(key);
        while (*value == ' ') value++;

        if      (!strcmp(key, "trace_width"))      d->trace_width = atoi(value);
        else if (!strcmp(key, "pred_width"))       d->pred_width = atoi(value);
        else if (!strcmp(key, "profile_width"))    d->profile_width = atoi(value);
        else if (!strcmp(key, "profile_opacity"))  d->profile_opacity = atoi(value);
        else if (!strcmp(key, "trace_opacity"))    d->trace_opacity = atoi(value);
        else if (!strcmp(key, "stick_opacity"))    d->stick_opacity = atoi(value);
        else if (!strcmp(key, "show_grid"))        d->show_grid = atoi(value) != 0;
        else if (!strcmp(key, "show_legend"))      d->show_legend = atoi(value) != 0;
        else if (!strcmp(key, "show_peak_labels")) d->show_peak_labels = atoi(value) != 0;
        else if (!strcmp(key, "show_blend_marks")) d->show_blend_marks = atoi(value) != 0;
        else if (!strcmp(key, "plot_bg"))          d->plot_bg = atoi(value);
        else if (!strcmp(key, "profile_color"))    read_color(value, &d->profile_color);
        else if (!strcmp(key, "peak_color"))       read_color(value, &d->peak_color);
        else if (!strcmp(key, "assigned_color"))   read_color(value, &d->assigned_color);
        else if (!strcmp(key, "bar_color"))        read_color(value, &d->bar_color);
        else if (!strcmp(key, "cursor_color"))     read_color(value, &d->cursor_color);
        else if (!strcmp(key, "nav_pan_px"))           d->nav_pan_px = atof(value);
        else if (!strcmp(key, "nav_zoom_factor"))      d->nav_zoom_factor = atof(value);
        else if (!strcmp(key, "nav_intensity_factor")) d->nav_intensity_factor = atof(value);
        else if (!strcmp(key, "nav_bar_px"))           d->nav_bar_px = atof(value);
        else if (!strcmp(key, "nav_fast_mult"))        d->nav_fast_mult = atof(value);
        else if (!strcmp(key, "nav_trace_shift"))      d->nav_trace_shift = atof(value);
        else if (!strcmp(key, "nav_wheel_scroll"))     d->nav_wheel_scroll = atof(value);
        else if (!strcmp(key, "def_pf_sig"))           d->def_pf_sig = atoi(value);
        else if (!strcmp(key, "def_pf_noise"))         d->def_pf_noise = atoi(value);
        else if (!strcmp(key, "def_pf_thresh"))        d->def_pf_thresh = atof(value);
        else if (!strcmp(key, "def_avg_window"))       d->def_avg_window = atoi(value);
        else if (!strcmp(key, "def_lorentz"))          d->def_lorentz = atof(value);
        else if (!strcmp(key, "def_gauss"))            d->def_gauss = atof(value);
        else if (!strcmp(key, "def_kaiser_beta"))      d->def_kaiser_beta = atof(value);
        else if (!strcmp(key, "def_kaiser_ceros"))     d->def_kaiser_ceros = atoi(value);
        else if (!strcmp(key, "def_kaiser_intrinsic")) d->def_kaiser_intrinsic = atof(value);
        else if (!strcmp(key, "def_line_error"))       d->def_line_error = atof(value);
        else if (!strcmp(key, "def_int_min"))          d->def_int_min = atof(value);
        else if (!strcmp(key, "def_int_max"))          d->def_int_max = atof(value);
        else if (!strcmp(key, "spcat_path"))  snprintf(d->spcat_path, sizeof(d->spcat_path), "%s", value);
        else if (!strcmp(key, "spfit_path"))  snprintf(d->spfit_path, sizeof(d->spfit_path), "%s", value);
        else if (!strcmp(key, "data_dir"))    snprintf(d->data_dir, sizeof(d->data_dir), "%s", value);
        else if (!strncmp(key, "trace_color", 11)) {
            int idx = atoi(key + 11);
            if (idx >= 0 && idx < MAX_SPECTRA) read_color(value, &d->trace_color[idx]);
        }
    }
    fclose(fp);
    if (d->trace_width   < 1 || d->trace_width   > 5) d->trace_width = 1;
    if (d->pred_width    < 1 || d->pred_width    > 5) d->pred_width = 1;
    if (d->profile_width < 1 || d->profile_width > 5) d->profile_width = 1;
    if (d->plot_bg < 0 || d->plot_bg > 2) d->plot_bg = 0;
    if (d->profile_opacity < 10 || d->profile_opacity > 100) d->profile_opacity = 65;
    if (d->trace_opacity   < 10 || d->trace_opacity   > 100) d->trace_opacity = 100;
    if (d->stick_opacity   < 10 || d->stick_opacity   > 100) d->stick_opacity = 100;
    if (d->nav_pan_px < 1 || d->nav_pan_px > 1000) d->nav_pan_px = 100;
    if (d->nav_zoom_factor < 1.01 || d->nav_zoom_factor > 4.0) d->nav_zoom_factor = 1.10;
    if (d->nav_intensity_factor < 1.01 || d->nav_intensity_factor > 4.0) d->nav_intensity_factor = 1.10;
    if (d->nav_bar_px < 0.5 || d->nav_bar_px > 200) d->nav_bar_px = 4.0;
    if (d->nav_fast_mult < 1.0 || d->nav_fast_mult > 20.0) d->nav_fast_mult = 3.0;
    if (d->nav_trace_shift <= 0.0 || d->nav_trace_shift > 1.0) d->nav_trace_shift = 0.05;
    if (d->nav_wheel_scroll < 5 || d->nav_wheel_scroll > 400) d->nav_wheel_scroll = 45;
    snprintf(d->status, sizeof(d->status), "Loaded %s", g_path);
}

/* Looks for a Pickett program in the usual places, so an existing install is
   found without anybody having to type a path. */
static void autodetect_program(const char *name, char *out, size_t n) {
    if (out[0]) return;
    const char *home = getenv("HOME");
    char cand[PATH_MAX];
    const char *dirs[] = {"/usr/local/bin", "/opt/homebrew/bin", "/usr/bin"};
    for (int i = 0; i < 3; i++) {
        snprintf(cand, sizeof(cand), "%s/%s", dirs[i], name);
        if (access(cand, X_OK) == 0) { snprintf(out, n, "%s", cand); return; }
    }
    if (home) {
        const char *rel[] = {"Desktop/Programmi_SP/calpgm", "calpgm", "bin"};
        for (int i = 0; i < 3; i++) {
            snprintf(cand, sizeof(cand), "%s/%s/%s", home, rel[i], name);
            if (access(cand, X_OK) == 0) { snprintf(out, n, "%s", cand); return; }
        }
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char buf[PATH_MAX] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        char *nl = strpbrk(buf, "\r\n");
        if (nl) *nl = '\0';
        /* An interactive shell would answer an alias here, not a path. */
        if (buf[0] == '/' && access(buf, X_OK) == 0) snprintf(out, n, "%s", buf);
    }
    pclose(fp);
}

void settings_init(AppState *s, const char *argv0) {
    resolve_settings_path(argv0);
    settings_restore_defaults(s);
    settings_load(s);
    autodetect_program("spcat", s->settings.spcat_path, sizeof(s->settings.spcat_path));
    autodetect_program("spfit", s->settings.spfit_path, sizeof(s->settings.spfit_path));
}

void settings_apply_defaults(AppState *s) {
    const AppSettings *d = &s->settings;
    s->pf_sig_pts        = d->def_pf_sig;
    s->pf_noise_pts      = d->def_pf_noise;
    s->pf_thresh         = d->def_pf_thresh;
    s->rolling_avg_window= d->def_avg_window;
    s->lorentz_gamma     = d->def_lorentz;
    s->gauss_gamma       = d->def_gauss;
    s->kaiser_beta       = d->def_kaiser_beta;
    s->kaiser_ceros      = d->def_kaiser_ceros;
    s->kaiser_intrinsic  = d->def_kaiser_intrinsic;
    s->pred_min_log_int  = d->def_int_min;
    s->pred_max_log_int  = d->def_int_max;
    s->predfit.line_error_mhz = d->def_line_error;
}

void settings_data_file(const AppState *s, const char *name, char *out, size_t size) {
    if (s->settings.data_dir[0]) snprintf(out, size, "%s/%s", s->settings.data_dir, name);
    else                         snprintf(out, size, "%s", name);
}

SDL_Color settings_plot_bg(const AppState *s) {
    switch (s->settings.plot_bg) {
        case 1:  return (SDL_Color){0, 0, 0, 255};
        case 2:  return (SDL_Color){8, 13, 22, 255};
        default: return UI_PLOT;
    }
}

/* Traces already on screen follow the palette, so a colour change is visible
   without reloading the file. */
static void apply_trace_colors(AppState *s) {
    for (int i = 0; i < s->n_spectra; i++) s->spectra[i].color = s->settings.trace_color[i % MAX_SPECTRA];
}

/* ---------------------------------------------------------------------------
 *  Colour picking
 *  macOS has a colour panel and the user knows it, so that is what opens.
 *  Where it is not available the swatch cycles a built-in palette instead of
 *  doing nothing.
 * ------------------------------------------------------------------------- */
static const SDL_Color FALLBACK_PALETTE[12] = {
    {205, 214, 225, 235}, { 90, 200, 250, 235}, { 47, 212, 232, 235}, {130, 220, 130, 235},
    {245, 220,  90, 235}, {255, 170,  80, 235}, {240, 110, 110, 235}, {235, 130, 200, 235},
    {170, 150, 245, 235}, { 70, 196, 138, 235}, {227, 179,  65, 235}, {120, 130, 145, 235}
};

static int pick_color(SDL_Color *c) {
#ifdef __APPLE__
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "osascript -e 'set c to choose color default color {%d, %d, %d}' "
             "-e 'set text item delimiters to \",\"' -e '(c as text)' 2>/dev/null",
             c->r * 257, c->g * 257, c->b * 257);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char out[128] = {0};
        char *line = fgets(out, sizeof(out), fp);
        pclose(fp);
        int r16 = 0, g16 = 0, b16 = 0;
        if (line && sscanf(out, "%d,%d,%d", &r16, &g16, &b16) == 3) {
            c->r = (Uint8)(r16 / 257); c->g = (Uint8)(g16 / 257); c->b = (Uint8)(b16 / 257);
            return 1;   /* cancelling the panel prints nothing and keeps the colour */
        }
        return 0;
    }
#endif
    for (int i = 0; i < 12; i++) {
        if (FALLBACK_PALETTE[i].r == c->r && FALLBACK_PALETTE[i].g == c->g && FALLBACK_PALETTE[i].b == c->b) {
            SDL_Color next = FALLBACK_PALETTE[(i + 1) % 12];
            c->r = next.r; c->g = next.g; c->b = next.b;
            return 1;
        }
    }
    *c = FALLBACK_PALETTE[0];
    return 1;
}


/* Picks a file or a folder with the panel the user already knows. */
static int pick_path(const char *prompt, int folder, char *out, size_t n) {
#ifdef __APPLE__
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "osascript -e 'POSIX path of (choose %s with prompt \"%s\")' 2>/dev/null",
             folder ? "folder" : "file", prompt);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char buf[600] = {0};
    char *line = fgets(buf, sizeof(buf), fp);
    pclose(fp);
    if (!line) return 0;                       /* cancelled */
    char *nl = strpbrk(buf, "\r\n");
    if (nl) *nl = '\0';
    if (!buf[0]) return 0;
    snprintf(out, n, "%s", buf);
    return 1;
#else
    (void)prompt; (void)folder; (void)out; (void)n;
    return 0;
#endif
}

/* ---------------------------------------------------------------------------
 *  Pages
 *
 *  One list of controls, built for the page in view and walked by both the
 *  renderer and the event handler, so a control and its click target cannot
 *  drift apart.
 * ------------------------------------------------------------------------- */
typedef enum {
    PAGE_PLOT = 0, PAGE_NAV, PAGE_ANALYSIS, PAGE_PATHS, PAGE_KEYS, PAGE_COUNT
} SettingsPage;

static const char *PAGE_NAME[PAGE_COUNT] = {"Plot", "Navigation", "Analysis", "Paths", "Shortcuts"};

typedef enum {
    SC_HEADING, SC_STEP_I, SC_STEP_F, SC_TOGGLE, SC_COLOR, SC_SWATCHES,
    SC_SEGMENT, SC_PATH, SC_KEYROW, SC_NOTE, SC_BUTTON
} SCtlKind;

typedef enum {
    /* plot */
    ID_TRACE_WIDTH, ID_TRACE_OPACITY, ID_TRACE_PALETTE, ID_PRED_WIDTH, ID_STICK_OPACITY,
    ID_PROFILE_WIDTH, ID_PROFILE_OPACITY, ID_PROFILE_COLOR,
    ID_BLEND_MARKS, ID_PEAK_COLOR, ID_ASSIGNED_COLOR, ID_BAR_COLOR, ID_CURSOR_COLOR,
    ID_BG, ID_GRID, ID_LEGEND, ID_PEAK_LABELS,
    /* navigation */
    ID_PAN_PX, ID_ZOOM_FACTOR, ID_INT_FACTOR, ID_BAR_PX, ID_FAST_MULT,
    ID_TRACE_SHIFT, ID_WHEEL_SCROLL,
    /* analysis */
    ID_PF_SIG, ID_PF_NOISE, ID_PF_THRESH, ID_AVG_WINDOW,
    ID_LORENTZ, ID_GAUSS, ID_KBETA, ID_KCEROS, ID_KINTR, ID_LINE_ERROR,
    ID_INT_MIN, ID_INT_MAX,
    /* paths */
    ID_SPCAT, ID_SPFIT, ID_DATA_DIR,
    /* footer */
    ID_SAVE, ID_RESET
} SCtlId;

typedef struct {
    int kind, id;
    const char *label;
    const char *detail;      /* second column of a shortcut row, or a note */
    SDL_Rect row, control;
} SCtl;

#define SETTINGS_PAD   20
#define SETTINGS_ROW   30
#define SETTINGS_CTL_H 24
#define SETTINGS_HEAD  28
#define SETTINGS_KEY   22
#define SETTINGS_FOOT  56
#define SETTINGS_TAB_Y 48
#define SETTINGS_TAB_H 30
#define SETTINGS_TAB_W 100
#define SETTINGS_TOP   92

static int g_content_h = 0;   /* filled by build_controls, used to clamp scroll */

/* Value, range and presentation of every numeric control. */
static int *int_value(AppState *s, int id) {
    switch (id) {
        case ID_TRACE_WIDTH:     return &s->settings.trace_width;
        case ID_TRACE_OPACITY:   return &s->settings.trace_opacity;
        case ID_PRED_WIDTH:      return &s->settings.pred_width;
        case ID_STICK_OPACITY:   return &s->settings.stick_opacity;
        case ID_PROFILE_WIDTH:   return &s->settings.profile_width;
        case ID_PROFILE_OPACITY: return &s->settings.profile_opacity;
        case ID_PF_SIG:          return &s->settings.def_pf_sig;
        case ID_PF_NOISE:        return &s->settings.def_pf_noise;
        case ID_AVG_WINDOW:      return &s->settings.def_avg_window;
        case ID_KCEROS:          return &s->settings.def_kaiser_ceros;
        default:                 return NULL;
    }
}

static double *dbl_value(AppState *s, int id) {
    switch (id) {
        case ID_PAN_PX:       return &s->settings.nav_pan_px;
        case ID_ZOOM_FACTOR:  return &s->settings.nav_zoom_factor;
        case ID_INT_FACTOR:   return &s->settings.nav_intensity_factor;
        case ID_BAR_PX:       return &s->settings.nav_bar_px;
        case ID_FAST_MULT:    return &s->settings.nav_fast_mult;
        case ID_TRACE_SHIFT:  return &s->settings.nav_trace_shift;
        case ID_WHEEL_SCROLL: return &s->settings.nav_wheel_scroll;
        case ID_PF_THRESH:    return &s->settings.def_pf_thresh;
        case ID_LORENTZ:      return &s->settings.def_lorentz;
        case ID_GAUSS:        return &s->settings.def_gauss;
        case ID_KBETA:        return &s->settings.def_kaiser_beta;
        case ID_KINTR:        return &s->settings.def_kaiser_intrinsic;
        case ID_LINE_ERROR:   return &s->settings.def_line_error;
        case ID_INT_MIN:      return &s->settings.def_int_min;
        case ID_INT_MAX:      return &s->settings.def_int_max;
        default:              return NULL;
    }
}

typedef struct { double lo, hi, step; int decimals; const char *unit; } Range;

static Range range_of(int id) {
    switch (id) {
        case ID_TRACE_WIDTH: case ID_PRED_WIDTH: case ID_PROFILE_WIDTH:
                                return (Range){1, 5, 1, 0, "px"};
        case ID_TRACE_OPACITY: case ID_STICK_OPACITY: case ID_PROFILE_OPACITY:
                                return (Range){10, 100, 5, 0, "%"};
        case ID_PAN_PX:         return (Range){5, 600, 5, 0, "px"};
        case ID_BAR_PX:         return (Range){0.5, 50, 0.5, 1, "px"};
        case ID_ZOOM_FACTOR:
        case ID_INT_FACTOR:     return (Range){1.02, 2.5, 0.02, 2, "x"};
        case ID_FAST_MULT:      return (Range){1, 10, 0.5, 1, "x"};
        case ID_TRACE_SHIFT:    return (Range){0.01, 0.5, 0.01, 2, "pane"};
        case ID_WHEEL_SCROLL:   return (Range){10, 200, 5, 0, "px"};
        case ID_PF_SIG:         return (Range){1, 200, 1, 0, "pts"};
        case ID_PF_NOISE:       return (Range){5, 2000, 5, 0, "pts"};
        case ID_PF_THRESH:      return (Range){0.5, 20, 0.5, 1, "x sigma"};
        case ID_AVG_WINDOW:     return (Range){1, 500, 1, 0, "pts"};
        case ID_LORENTZ: case ID_GAUSS:
                                return (Range){0, 20, 0.05, 2, "MHz"};
        case ID_KBETA:          return (Range){0, 20, 0.25, 2, ""};
        case ID_KCEROS:         return (Range){1, 64, 1, 0, "x"};
        case ID_KINTR:          return (Range){0, 5, 0.005, 3, "MHz"};
        case ID_LINE_ERROR:     return (Range){0.0001, 5, 0.001, 4, "MHz"};
        case ID_INT_MIN: case ID_INT_MAX:
                                return (Range){-20, 5, 0.5, 1, "log I"};
        default:                return (Range){0, 100, 1, 0, ""};
    }
}

static int *toggle_value(AppState *s, int id) {
    switch (id) {
        case ID_BLEND_MARKS: return &s->settings.show_blend_marks;
        case ID_GRID:        return &s->settings.show_grid;
        case ID_LEGEND:      return &s->settings.show_legend;
        case ID_PEAK_LABELS: return &s->settings.show_peak_labels;
        default:             return NULL;
    }
}

static SDL_Color *color_value(AppState *s, int id) {
    switch (id) {
        case ID_PROFILE_COLOR:  return &s->settings.profile_color;
        case ID_PEAK_COLOR:     return &s->settings.peak_color;
        case ID_ASSIGNED_COLOR: return &s->settings.assigned_color;
        case ID_BAR_COLOR:      return &s->settings.bar_color;
        case ID_CURSOR_COLOR:   return &s->settings.cursor_color;
        default:                return NULL;
    }
}

static char *path_value(AppState *s, int id) {
    switch (id) {
        case ID_SPCAT:    return s->settings.spcat_path;
        case ID_SPFIT:    return s->settings.spfit_path;
        case ID_DATA_DIR: return s->settings.data_dir;
        default:          return NULL;
    }
}

/* The keyboard contract, listed for reference. */
typedef struct { const char *keys, *what; } KeyRow;
static const KeyRow KEY_ROWS[] = {
    {"Navigate",   NULL},
    {"Q  E",       "Zoom out / in on frequency"},
    {"A  S",       "Pan left / right"},
    {"W  Z",       "Scale experimental intensity"},
    {"Tab",        "Autoscale intensity to the view"},
    {"Shift Tab",  "Normalise the prediction in view"},
    {"R",          "Reset the view"},
    {"Up Down",    "Shift the spectrum vertically"},
    {"Caps Lock",  "Hold for the fast multiplier"},
    {"Measure and assign", NULL},
    {"K  L",       "Move the bar"},
    {"G",          "Distance between two points"},
    {"Delete",     "Remove the latest peak"},
    {"X",          "Export the view as BMP"},
    {"H  ?",       "Shortcut overlay"},
    {",",          "This window"},
    {"Tools",      NULL},
    {"N",          "Assignments"},
    {"P",          "Peak finder"},
    {"T",          "Rolling average"},
    {"M",          "Broadening"},
    {"D",          "Intensity analysis"},
    {"C",          "Intensity range"},
    {"F",          "Frequency jump"},
    {"B",          "Transition filter"},
    {"Mouse",      NULL},
    {"left drag",  "Zoom into a frequency range"},
    {"right drag", "Pick the peak in the range"},
    {"alt drag",   "Slide the spectrum onto the prediction"},
    {"click",      "Select a predicted transition"},
    {"cmd click",  "Add to the selection"},
    {"Without Sync", NULL},
    {"shift + key", "The same commands act on the prediction only"}
};
#define KEY_ROW_COUNT ((int)(sizeof(KEY_ROWS) / sizeof(KEY_ROWS[0])))

static int build_controls(AppState *s, SCtl *out, int max, int w, int h) {
    int n = 0, y = SETTINGS_TOP - s->settings.scroll;
    int content_w = w - 2 * SETTINGS_PAD;

    #define PUSH(k, i, lab, det, ctl_w, row_h) do { \
        if (n < max) { \
            out[n].kind = (k); out[n].id = (i); out[n].label = (lab); out[n].detail = (det); \
            out[n].row = (SDL_Rect){SETTINGS_PAD, y, content_w, (row_h)}; \
            out[n].control = (SDL_Rect){SETTINGS_PAD + content_w - (ctl_w), \
                                        y + ((row_h) - SETTINGS_CTL_H) / 2, (ctl_w), SETTINGS_CTL_H}; \
            y += (row_h); n++; \
        } \
    } while (0)
    #define NUM(i, lab)  PUSH(int_value(s, i) ? SC_STEP_I : SC_STEP_F, i, lab, NULL, 116, SETTINGS_ROW)
    #define HEAD(lab)    PUSH(SC_HEADING, 0, lab, NULL, 0, SETTINGS_HEAD)

    switch (s->settings.page) {
    case PAGE_PLOT:
        HEAD("Traces");
        NUM(ID_TRACE_WIDTH,   "Line width");
        NUM(ID_TRACE_OPACITY, "Opacity of a new trace");
        PUSH(SC_SWATCHES, ID_TRACE_PALETTE, "Colours", NULL, MAX_SPECTRA * 26, SETTINGS_ROW);
        y += 6;
        HEAD("Prediction");
        NUM(ID_PRED_WIDTH,      "Stick width");
        NUM(ID_STICK_OPACITY,   "Stick opacity");
        NUM(ID_PROFILE_WIDTH,   "Profile width");
        PUSH(SC_COLOR, ID_PROFILE_COLOR, "Profile colour", NULL, 56, SETTINGS_ROW);
        NUM(ID_PROFILE_OPACITY, "Profile opacity");
        PUSH(SC_TOGGLE, ID_BLEND_MARKS, "Mark blended lines", NULL, 30, SETTINGS_ROW);
        y += 6;
        HEAD("Markers");
        PUSH(SC_COLOR, ID_PEAK_COLOR,     "Found peaks",    NULL, 56, SETTINGS_ROW);
        PUSH(SC_COLOR, ID_ASSIGNED_COLOR, "Assigned lines", NULL, 56, SETTINGS_ROW);
        PUSH(SC_COLOR, ID_BAR_COLOR,      "Bar",            NULL, 56, SETTINGS_ROW);
        PUSH(SC_COLOR, ID_CURSOR_COLOR,   "Cursor readout", NULL, 56, SETTINGS_ROW);
        y += 6;
        HEAD("Plot");
        PUSH(SC_SEGMENT, ID_BG,          "Background",  NULL, 210, SETTINGS_ROW);
        PUSH(SC_TOGGLE,  ID_GRID,        "Grid",        NULL, 30, SETTINGS_ROW);
        PUSH(SC_TOGGLE,  ID_LEGEND,      "Legend",      NULL, 30, SETTINGS_ROW);
        PUSH(SC_TOGGLE,  ID_PEAK_LABELS, "Peak labels", NULL, 30, SETTINGS_ROW);
        break;

    case PAGE_NAV:
        HEAD("Keyboard steps");
        NUM(ID_PAN_PX,      "Pan step (A / S)");
        NUM(ID_ZOOM_FACTOR, "Zoom step (Q / E)");
        NUM(ID_INT_FACTOR,  "Intensity step (W / Z)");
        NUM(ID_BAR_PX,      "Bar step (K / L)");
        NUM(ID_FAST_MULT,   "Fast multiplier (Caps Lock)");
        PUSH(SC_NOTE, 0, NULL,
             "The pan and bar steps are in pixels of the pane, so they move the same\n"
             "distance on screen whatever the zoom.", 0, 44);
        y += 6;
        HEAD("Traces and panels");
        NUM(ID_TRACE_SHIFT,  "Vertical shift of a trace");
        NUM(ID_WHEEL_SCROLL, "Inspector scroll per notch");
        break;

    case PAGE_ANALYSIS:
        HEAD("Peak finder");
        NUM(ID_PF_SIG,    "Search width");
        NUM(ID_PF_NOISE,  "Noise window");
        NUM(ID_PF_THRESH, "Threshold");
        y += 6;
        HEAD("Smoothing");
        NUM(ID_AVG_WINDOW, "Rolling average window");
        y += 6;
        HEAD("Broadening");
        NUM(ID_LORENTZ, "Lorentz HWHM");
        NUM(ID_GAUSS,   "Gauss HWHM");
        NUM(ID_KBETA,   "Kaiser beta");
        NUM(ID_KCEROS,  "Zero-pad");
        NUM(ID_KINTR,   "Intrinsic FWHM");
        y += 6;
        HEAD("Prediction");
        NUM(ID_INT_MIN,    "Intensity range, min");
        NUM(ID_INT_MAX,    "Intensity range, max");
        NUM(ID_LINE_ERROR, "Uncertainty written to .lin");
        PUSH(SC_NOTE, 0, NULL,
             "These are the values a tool starts from. Changing one here does not\n"
             "touch the session in progress.", 0, 44);
        break;

    case PAGE_PATHS:
        HEAD("Pickett programs");
        PUSH(SC_PATH, ID_SPCAT, "SPCAT", NULL, 110, SETTINGS_ROW + 14);
        PUSH(SC_PATH, ID_SPFIT, "SPFIT", NULL, 110, SETTINGS_ROW + 14);
        PUSH(SC_NOTE, 0, NULL,
             "Pred&Fit runs these two. Without them Calculate and Fit report what is\n"
             "missing instead of failing silently.", 0, 44);
        y += 6;
        HEAD("Working files");
        PUSH(SC_PATH, ID_DATA_DIR, "Data folder", NULL, 110, SETTINGS_ROW + 14);
        PUSH(SC_NOTE, 0, NULL,
             "Where assignments.txt, linelist.csv and the .fit session are read and\n"
             "written. Empty means the folder the program was started from.", 0, 44);
        break;

    case PAGE_KEYS:
        for (int i = 0; i < KEY_ROW_COUNT; i++) {
            if (!KEY_ROWS[i].what) { if (i) y += 6; HEAD(KEY_ROWS[i].keys); }
            else PUSH(SC_KEYROW, 0, KEY_ROWS[i].keys, KEY_ROWS[i].what, 0, SETTINGS_KEY);
        }
        break;
    default: break;
    }
    #undef NUM
    #undef HEAD

    g_content_h = y + s->settings.scroll + SETTINGS_PAD;

    if (n + 2 <= max) {
        int fy = h - SETTINGS_FOOT + 6;
        out[n++] = (SCtl){SC_BUTTON, ID_SAVE, "Save as default", NULL,
                          (SDL_Rect){SETTINGS_PAD, fy, 150, 30}, (SDL_Rect){SETTINGS_PAD, fy, 150, 30}};
        out[n++] = (SCtl){SC_BUTTON, ID_RESET, "Restore defaults", NULL,
                          (SDL_Rect){SETTINGS_PAD + 160, fy, 150, 30}, (SDL_Rect){SETTINGS_PAD + 160, fy, 150, 30}};
    }
    #undef PUSH
    return n;
}

/* ---------------------------------------------------------------------------
 *  Typing a value
 * ------------------------------------------------------------------------- */
static int edit_len(const AppSettings *d) { return (int)strlen(d->edit_buf); }
static int edit_lo(const AppSettings *d)  { return d->edit_caret < d->edit_anchor ? d->edit_caret : d->edit_anchor; }
static int edit_hi(const AppSettings *d)  { return d->edit_caret > d->edit_anchor ? d->edit_caret : d->edit_anchor; }

static void edit_clamp(AppSettings *d) {
    int n = edit_len(d);
    if (d->edit_caret  < 0) d->edit_caret  = 0;
    if (d->edit_caret  > n) d->edit_caret  = n;
    if (d->edit_anchor < 0) d->edit_anchor = 0;
    if (d->edit_anchor > n) d->edit_anchor = n;
}

static int edit_delete_selection(AppSettings *d) {
    edit_clamp(d);
    int lo = edit_lo(d), hi = edit_hi(d);
    if (lo == hi) return 0;
    memmove(d->edit_buf + lo, d->edit_buf + hi, strlen(d->edit_buf + hi) + 1);
    d->edit_caret = d->edit_anchor = lo;
    return 1;
}

static void edit_insert(AppSettings *d, const char *text) {
    edit_delete_selection(d);
    int n = edit_len(d), add = (int)strlen(text);
    int room = (int)sizeof(d->edit_buf) - 1 - n;
    if (add > room) add = room;
    if (add <= 0) return;
    memmove(d->edit_buf + d->edit_caret + add, d->edit_buf + d->edit_caret,
            (size_t)(n - d->edit_caret) + 1);
    memcpy(d->edit_buf + d->edit_caret, text, (size_t)add);
    d->edit_caret += add;
    d->edit_anchor = d->edit_caret;
}

static void edit_begin(AppState *s, int id) {
    AppSettings *d = &s->settings;
    Range r = range_of(id);
    int *iv = int_value(s, id);
    double *dv = dbl_value(s, id);
    if (iv)      snprintf(d->edit_buf, sizeof(d->edit_buf), "%d", *iv);
    else if (dv) snprintf(d->edit_buf, sizeof(d->edit_buf), "%.*f", r.decimals, *dv);
    else return;
    d->edit_id = id;
    d->edit_anchor = 0;
    d->edit_caret = edit_len(d);      /* the value starts selected */
    SDL_StartTextInput();
}

static void edit_commit(AppState *s) {
    AppSettings *d = &s->settings;
    if (d->edit_id < 0) return;
    char *end = NULL;
    double v = strtod(d->edit_buf, &end);
    if (end != d->edit_buf && isfinite(v)) {
        Range r = range_of(d->edit_id);
        if (v < r.lo) v = r.lo;
        if (v > r.hi) v = r.hi;
        int *iv = int_value(s, d->edit_id);
        double *dv = dbl_value(s, d->edit_id);
        if (iv)      *iv = (int)lround(v);
        else if (dv) *dv = v;
        if (d->edit_id == ID_TRACE_OPACITY) apply_trace_colors(s);
    }
    d->edit_id = -1;
    SDL_StopTextInput();
}

static void edit_cancel(AppSettings *d) { d->edit_id = -1; SDL_StopTextInput(); }

/* Only what can appear in a number, so a stray keystroke cannot corrupt one. */
static int edit_accepts(const char *text) {
    for (const char *p = text; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+' || *p == 'e' || *p == 'E'))
            return 0;
    return 1;
}

/* The typing area of a stepper row: between the minus and the plus. */
static SDL_Rect stepper_field(SDL_Rect control) {
    return (SDL_Rect){control.x + 27, control.y, control.w - 54, control.h};
}

static SDL_Rect page_tab(int i) {
    return (SDL_Rect){16 + i * (SETTINGS_TAB_W + 4), SETTINGS_TAB_Y, SETTINGS_TAB_W, SETTINGS_TAB_H};
}

static void clamp_scroll(AppState *s, int h) {
    int view = h - SETTINGS_FOOT - SETTINGS_TOP;
    int max_scroll = g_content_h - SETTINGS_TOP - view;
    if (max_scroll < 0) max_scroll = 0;
    if (s->settings.scroll < 0) s->settings.scroll = 0;
    if (s->settings.scroll > max_scroll) s->settings.scroll = max_scroll;
}

/* ---------------------------------------------------------------------------
 *  Window
 * ------------------------------------------------------------------------- */
void settings_open(AppState *s) {
    AppSettings *d = &s->settings;
    if (d->window) { SDL_RaiseWindow(d->window); return; }
    d->window = SDL_CreateWindow("Settings", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 560, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!d->window) return;
    SDL_SetWindowMinimumSize(d->window, 460, 420);
    d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d->renderer) { SDL_DestroyWindow(d->window); d->window = NULL; return; }
    SDL_SetRenderDrawBlendMode(d->renderer, SDL_BLENDMODE_BLEND);
    d->window_id = SDL_GetWindowID(d->window);
    d->open = 1;
}

void settings_close(AppState *s) {
    AppSettings *d = &s->settings;
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window) SDL_DestroyWindow(d->window);
    d->renderer = NULL; d->window = NULL; d->open = 0; d->window_id = 0;
}

void settings_dispose(AppState *s) { settings_close(s); }

int settings_handle_event(AppState *s, const SDL_Event *e) {
    AppSettings *d = &s->settings;
    if (!d->open) return 0;
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == d->window_id &&
        e->window.event == SDL_WINDOWEVENT_CLOSE) { settings_close(s); return 1; }
    /* While a value is being typed the keyboard belongs to that field. */
    if (d->edit_id >= 0 && e->type == SDL_TEXTINPUT && e->text.windowID == d->window_id) {
        if (edit_accepts(e->text.text)) edit_insert(d, e->text.text);
        return 1;
    }
    if (d->edit_id >= 0 && e->type == SDL_KEYDOWN && e->key.windowID == d->window_id) {
        SDL_Keymod mod = SDL_GetModState();
        int cmd = (mod & (KMOD_GUI | KMOD_CTRL)) != 0, shift = (mod & KMOD_SHIFT) != 0;
        SDL_Keycode k = e->key.keysym.sym;
        if (cmd && k == SDLK_a) { d->edit_anchor = 0; d->edit_caret = edit_len(d); return 1; }
        if (cmd && k == SDLK_v) {
            char *clip = SDL_GetClipboardText();
            if (clip) { if (edit_accepts(clip)) edit_insert(d, clip); SDL_free(clip); }
            return 1;
        }
        if (cmd && (k == SDLK_c || k == SDLK_x)) {
            edit_clamp(d);
            int lo = edit_lo(d), hi = edit_hi(d);
            if (lo == hi) { lo = 0; hi = edit_len(d); }
            char tmp[64]; int m = hi - lo;
            if (m > (int)sizeof(tmp) - 1) m = (int)sizeof(tmp) - 1;
            memcpy(tmp, d->edit_buf + lo, (size_t)m); tmp[m] = '\0';
            SDL_SetClipboardText(tmp);
            if (k == SDLK_x) { d->edit_anchor = lo; d->edit_caret = hi; edit_delete_selection(d); }
            return 1;
        }
        switch (k) {
            case SDLK_LEFT:  d->edit_caret--; if (!shift) d->edit_anchor = d->edit_caret; edit_clamp(d); return 1;
            case SDLK_RIGHT: d->edit_caret++; if (!shift) d->edit_anchor = d->edit_caret; edit_clamp(d); return 1;
            case SDLK_HOME:  d->edit_caret = 0; if (!shift) d->edit_anchor = 0; return 1;
            case SDLK_END:   d->edit_caret = edit_len(d); if (!shift) d->edit_anchor = d->edit_caret; return 1;
            case SDLK_BACKSPACE:
                if (!edit_delete_selection(d) && d->edit_caret > 0) {
                    d->edit_anchor = d->edit_caret - 1; edit_delete_selection(d);
                }
                return 1;
            case SDLK_DELETE:
                if (!edit_delete_selection(d) && d->edit_caret < edit_len(d)) {
                    d->edit_anchor = d->edit_caret + 1; edit_delete_selection(d);
                }
                return 1;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_TAB: edit_commit(s); return 1;
            case SDLK_ESCAPE: edit_cancel(d); return 1;
            default: return 1;
        }
    }
    if (e->type == SDL_KEYDOWN && e->key.windowID == d->window_id &&
        e->key.keysym.sym == SDLK_ESCAPE) { settings_close(s); return 1; }

    int w = 0, h = 0;
    if (d->window) SDL_GetWindowSize(d->window, &w, &h);

    if (e->type == SDL_MOUSEWHEEL && e->wheel.windowID == d->window_id) {
        SCtl probe[96];
        build_controls(s, probe, 96, w, h);
        d->scroll -= e->wheel.y * 24;
        clamp_scroll(s, h);
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.windowID != d->window_id) return 0;

    /* A click anywhere else keeps what was typed, as in any other field. */
    if (d->edit_id >= 0) edit_commit(s);

    int x = e->button.x, y = e->button.y;
    for (int p = 0; p < PAGE_COUNT; p++) {
        if (point_in_rect(x, y, page_tab(p))) { d->page = p; d->scroll = 0; return 1; }
    }

    SCtl ctl[96];
    int n = build_controls(s, ctl, 96, w, h);
    for (int i = 0; i < n; i++) {
        if (!point_in_rect(x, y, ctl[i].control)) continue;
        switch (ctl[i].kind) {
            case SC_STEP_I: {
                if (point_in_rect(x, y, stepper_field(ctl[i].control))) { edit_begin(s, ctl[i].id); break; }
                int *v = int_value(s, ctl[i].id);
                if (!v) break;
                Range r = range_of(ctl[i].id);
                int mid = ctl[i].control.x + ctl[i].control.w / 2;
                *v += (x < mid) ? -(int)r.step : (int)r.step;
                if (*v < (int)r.lo) *v = (int)r.lo;
                if (*v > (int)r.hi) *v = (int)r.hi;
                break;
            }
            case SC_STEP_F: {
                if (point_in_rect(x, y, stepper_field(ctl[i].control))) { edit_begin(s, ctl[i].id); break; }
                double *v = dbl_value(s, ctl[i].id);
                if (!v) break;
                Range r = range_of(ctl[i].id);
                int mid = ctl[i].control.x + ctl[i].control.w / 2;
                *v += (x < mid) ? -r.step : r.step;
                if (*v < r.lo) *v = r.lo;
                if (*v > r.hi) *v = r.hi;
                break;
            }
            case SC_TOGGLE: { int *v = toggle_value(s, ctl[i].id); if (v) *v = !*v; break; }
            case SC_COLOR:  { SDL_Color *c = color_value(s, ctl[i].id); if (c) pick_color(c); break; }
            case SC_SWATCHES: {
                int idx = (x - ctl[i].control.x) / 26;
                if (idx >= 0 && idx < MAX_SPECTRA) {
                    pick_color(&s->settings.trace_color[idx]);
                    apply_trace_colors(s);
                }
                break;
            }
            case SC_SEGMENT: {
                int seg = (x - ctl[i].control.x) / (ctl[i].control.w / 3);
                if (seg < 0) seg = 0;
                if (seg > 2) seg = 2;
                s->settings.plot_bg = seg;
                break;
            }
            case SC_PATH: {
                char *p = path_value(s, ctl[i].id);
                if (!p) break;
                int clear_x = ctl[i].control.x + ctl[i].control.w - 26;
                if (x >= clear_x) { p[0] = '\0'; snprintf(d->status, sizeof(d->status), "Cleared."); break; }
                char picked[512];
                const char *prompt = ctl[i].id == ID_SPCAT ? "Select the SPCAT program"
                                   : ctl[i].id == ID_SPFIT ? "Select the SPFIT program"
                                                           : "Select the data folder";
                if (pick_path(prompt, ctl[i].id == ID_DATA_DIR, picked, sizeof(picked))) {
                    snprintf(p, 512, "%s", picked);
                    snprintf(d->status, sizeof(d->status), "Set. Save as default to keep it.");
                }
                break;
            }
            case SC_BUTTON:
                if (ctl[i].id == ID_SAVE) settings_save(s);
                else {
                    settings_restore_defaults(s);
                    apply_trace_colors(s);
                    snprintf(d->status, sizeof(d->status), "Back to the built-in defaults (not saved yet).");
                }
                break;
            default: break;
        }
        return 1;
    }
    return 1;
}

/* Shortens a path in the middle, which keeps both the program name and enough
   of the folder to tell two installations apart. */
static void elide_path(const char *path, char *out, size_t n, int max_chars) {
    int len = (int)strlen(path);
    if (len <= max_chars) { snprintf(out, n, "%s", path); return; }
    int keep_tail = max_chars - 12;
    if (keep_tail < 8) keep_tail = 8;
    snprintf(out, n, "%.9s...%s", path, path + len - keep_tail);
}

void settings_render(AppState *s) {
    AppSettings *d = &s->settings;
    if (!d->open || !d->renderer) return;
    SDL_Renderer *r = d->renderer;

    int w = 0, h = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(d->window, &w, &h);
    SDL_GetRendererOutputSize(r, &dw, &dh);
    if (w > 0 && dw > 0) SDL_RenderSetScale(r, (float)dw / w, (float)dw / w);

    int mx = 0, my = 0;
    int mdown = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (SDL_GetMouseFocus() != d->window) { mx = -100; my = -100; }

    SDL_SetRenderDrawColor(r, 19, 20, 22, 255);
    SDL_RenderClear(r);
    ui_fill(r, (SDL_Rect){0, 0, w, 42}, UI_TITLEBAR);
    ui_text(r, UI_FONT_TITLE, "Settings", SETTINGS_PAD, 12, UI_TEXT);
    for (int p = 0; p < PAGE_COUNT; p++)
        ui_button(r, page_tab(p), PAGE_NAME[p], -1, UI_BTN_QUIET, d->page == p, mx, my, mdown);
    ui_hline(r, 0, w, SETTINGS_TOP - 8, UI_LINE);

    SCtl ctl[96];
    int n = build_controls(s, ctl, 96, w, h);
    clamp_scroll(s, h);
    n = build_controls(s, ctl, 96, w, h);
    char b[256];

    SDL_Rect list = {0, SETTINGS_TOP - 8, w, h - SETTINGS_FOOT - (SETTINGS_TOP - 8)};
    for (int i = 0; i < n; i++) {
        SCtl *c = &ctl[i];
        if (c->kind != SC_BUTTON) SDL_RenderSetClipRect(r, &list);
        else                      SDL_RenderSetClipRect(r, NULL);

        switch (c->kind) {
            case SC_HEADING:
                ui_hline(r, c->row.x, c->row.x + c->row.w, c->row.y + 6, UI_LINE_SOFT);
                ui_text(r, UI_FONT_SANS_SM, c->label, c->row.x, c->row.y + 12, UI_FAINT);
                break;
            case SC_STEP_I:
            case SC_STEP_F: {
                Range rg = range_of(c->id);
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                ui_button(r, (SDL_Rect){c->control.x, c->control.y, 26, c->control.h}, "-", -1,
                          UI_BTN_QUIET, 0, mx, my, mdown);
                if (d->edit_id == c->id) {
                    ui_field_ex(r, stepper_field(c->control), NULL, d->edit_buf, rg.unit, 1,
                                d->edit_caret, d->edit_anchor);
                } else {
                    if (c->kind == SC_STEP_I) {
                        int *v = int_value(s, c->id);
                        snprintf(b, sizeof(b), "%d %s", v ? *v : 0, rg.unit);
                    } else {
                        double *v = dbl_value(s, c->id);
                        snprintf(b, sizeof(b), "%.*f %s", rg.decimals, v ? *v : 0.0, rg.unit);
                    }
                    int tw = ui_text_w(UI_FONT_MONO, b);
                    ui_text_v(r, UI_FONT_MONO, b, c->control.x + (c->control.w - tw) / 2, c->control, UI_TEXT);
                }
                ui_button(r, (SDL_Rect){c->control.x + c->control.w - 26, c->control.y, 26, c->control.h},
                          "+", -1, UI_BTN_QUIET, 0, mx, my, mdown);
                break;
            }
            case SC_TOGGLE: {
                int *v = toggle_value(s, c->id);
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                ui_switch(r, c->control, v ? *v : 0);
                break;
            }
            case SC_COLOR: {
                SDL_Color *col = color_value(s, c->id);
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                SDL_Rect sw = {c->control.x, c->control.y + 2, c->control.w, c->control.h - 4};
                fill_rounded_rect(r, sw, 4, col ? *col : UI_TEXT);
                ui_frame(r, sw, UI_LINE);
                break;
            }
            case SC_SWATCHES:
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                for (int k = 0; k < MAX_SPECTRA; k++) {
                    SDL_Rect sw = {c->control.x + k * 26, c->control.y + 2, 20, c->control.h - 4};
                    fill_rounded_rect(r, sw, 4, s->settings.trace_color[k]);
                    ui_frame(r, sw, k < s->n_spectra ? UI_TEXT : UI_LINE);
                }
                break;
            case SC_SEGMENT: {
                static const char *bg[3] = {"Graphite", "Black", "Navy"};
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                ui_segmented(r, c->control, bg, 3, s->settings.plot_bg);
                break;
            }
            case SC_PATH: {
                char *p = path_value(s, c->id);
                ui_text(r, UI_FONT_SANS, c->label, c->row.x, c->row.y + 4, UI_DIM);
                if (p && p[0]) {
                    elide_path(p, b, sizeof(b), (c->row.w - 130) / 7);
                    ui_text(r, UI_FONT_MONO_SM, b, c->row.x, c->row.y + 24, UI_TEXT);
                } else {
                    ui_text(r, UI_FONT_SANS_SM, "not set", c->row.x, c->row.y + 24, UI_WARN);
                }
                ui_button(r, (SDL_Rect){c->control.x, c->control.y, c->control.w - 28, c->control.h},
                          "Choose", -1, UI_BTN_QUIET, 0, mx, my, mdown);
                ui_button(r, (SDL_Rect){c->control.x + c->control.w - 24, c->control.y, 24, c->control.h},
                          "", UI_ICON_CLOSE, UI_BTN_QUIET, 0, mx, my, mdown);
                break;
            }
            case SC_KEYROW: {
                SDL_Rect kb = {c->row.x, c->row.y + 1, ui_text_w(UI_FONT_MONO_SM, c->label) + 14, 18};
                fill_rounded_rect(r, kb, 3, UI_RAISED);
                ui_text_v(r, UI_FONT_MONO_SM, c->label, kb.x + 7, kb, UI_TEXT);
                ui_text(r, UI_FONT_SANS_SM, c->detail, c->row.x + 118, c->row.y + 3, UI_DIM);
                break;
            }
            case SC_NOTE: {
                const char *t = c->detail;
                int line_y = c->row.y + 2;
                while (t && *t) {
                    const char *nl = strchr(t, '\n');
                    int len = nl ? (int)(nl - t) : (int)strlen(t);
                    snprintf(b, sizeof(b), "%.*s", len, t);
                    ui_text(r, UI_FONT_SANS_SM, b, c->row.x, line_y, UI_FAINT);
                    line_y += 16;
                    t = nl ? nl + 1 : NULL;
                }
                break;
            }
            case SC_BUTTON:
                ui_button(r, c->control, c->label, -1,
                          c->id == ID_SAVE ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, mx, my, mdown);
                break;
            default: break;
        }
    }
    SDL_RenderSetClipRect(r, NULL);

    ui_hline(r, 0, w, h - SETTINGS_FOOT, UI_LINE);
    const char *msg = d->status[0] ? d->status : settings_file_path();
    ui_text(r, UI_FONT_SANS_SM, msg, SETTINGS_PAD, h - 20, UI_FAINT);
    SDL_RenderPresent(r);
}
