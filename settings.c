#include "settings.h"
#include "layout.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
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
    DisplaySettings *d = &s->settings;
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
    DisplaySettings *d = &s->settings;
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
    fclose(fp);
    snprintf(d->status, sizeof(d->status), "Saved as default in %s", g_path);
    return 1;
}

static void settings_load(AppState *s) {
    DisplaySettings *d = &s->settings;
    FILE *fp = fopen(g_path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;
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
    snprintf(d->status, sizeof(d->status), "Loaded %s", g_path);
}

void settings_init(AppState *s, const char *argv0) {
    resolve_settings_path(argv0);
    settings_restore_defaults(s);
    settings_load(s);
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

/* ---------------------------------------------------------------------------
 *  Layout: one list of controls, walked by both the renderer and the events
 * ------------------------------------------------------------------------- */
typedef enum {
    SC_HEADING, SC_STEPPER, SC_TOGGLE, SC_COLOR, SC_SWATCHES, SC_SEGMENT, SC_BUTTON
} SCtlKind;

typedef enum {
    ID_TRACE_WIDTH, ID_TRACE_OPACITY, ID_TRACE_PALETTE, ID_PRED_WIDTH, ID_STICK_OPACITY,
    ID_PROFILE_WIDTH, ID_PROFILE_OPACITY, ID_PROFILE_COLOR,
    ID_BLEND_MARKS, ID_PEAK_COLOR, ID_ASSIGNED_COLOR, ID_BAR_COLOR, ID_CURSOR_COLOR,
    ID_BG, ID_GRID, ID_LEGEND, ID_PEAK_LABELS, ID_SAVE, ID_RESET
} SCtlId;

typedef struct {
    int kind, id;
    const char *label;
    SDL_Rect row;      /* the whole row            */
    SDL_Rect control;  /* the interactive part     */
} SCtl;

#define SETTINGS_PAD   20
#define SETTINGS_ROW   30
#define SETTINGS_CTL_H 24
#define SETTINGS_HEAD  28
#define SETTINGS_FOOT  56

static int g_content_h = 0;   /* filled by build_controls, used to clamp scroll */

static int build_controls(AppState *s, SCtl *out, int max, int w, int h) {
    int n = 0, y = 64 - s->settings.scroll;
    int content_w = w - 2 * SETTINGS_PAD;

    #define PUSH(k, i, lab, ctl_w) do { \
        if (n < max) { \
            out[n].kind = (k); out[n].id = (i); out[n].label = (lab); \
            out[n].row = (SDL_Rect){SETTINGS_PAD, y, content_w, (k) == SC_HEADING ? SETTINGS_HEAD : SETTINGS_ROW}; \
            out[n].control = (SDL_Rect){SETTINGS_PAD + content_w - (ctl_w), \
                                        y + (((k) == SC_HEADING ? SETTINGS_HEAD : SETTINGS_ROW) - SETTINGS_CTL_H) / 2, \
                                        (ctl_w), SETTINGS_CTL_H}; \
            y += out[n].row.h; n++; \
        } \
    } while (0)

    PUSH(SC_HEADING, 0, "Traces", 0);
    PUSH(SC_STEPPER,  ID_TRACE_WIDTH,   "Line width",        96);
    PUSH(SC_STEPPER,  ID_TRACE_OPACITY, "Opacity (new traces)", 96);
    PUSH(SC_SWATCHES, ID_TRACE_PALETTE, "Colours",           MAX_SPECTRA * 26);
    y += 6;
    PUSH(SC_HEADING, 0, "Prediction", 0);
    PUSH(SC_STEPPER, ID_PRED_WIDTH,    "Stick width",        96);
    PUSH(SC_STEPPER, ID_STICK_OPACITY, "Stick opacity",      96);
    PUSH(SC_STEPPER, ID_PROFILE_WIDTH,   "Profile width",     96);
    PUSH(SC_COLOR,   ID_PROFILE_COLOR,   "Profile colour",    56);
    PUSH(SC_STEPPER, ID_PROFILE_OPACITY, "Profile opacity",   96);
    PUSH(SC_TOGGLE,  ID_BLEND_MARKS,   "Mark blended lines", 30);
    y += 6;
    PUSH(SC_HEADING, 0, "Markers", 0);
    PUSH(SC_COLOR, ID_PEAK_COLOR,     "Found peaks",     56);
    PUSH(SC_COLOR, ID_ASSIGNED_COLOR, "Assigned lines",  56);
    PUSH(SC_COLOR, ID_BAR_COLOR,      "Bar",             56);
    PUSH(SC_COLOR, ID_CURSOR_COLOR,   "Cursor readout",  56);
    y += 6;
    PUSH(SC_HEADING, 0, "Plot", 0);
    PUSH(SC_SEGMENT, ID_BG,          "Background",   210);
    PUSH(SC_TOGGLE,  ID_GRID,        "Grid",          30);
    PUSH(SC_TOGGLE,  ID_LEGEND,      "Legend",        30);
    PUSH(SC_TOGGLE,  ID_PEAK_LABELS, "Peak labels",   30);
    #undef PUSH

    /* Height the list needs, so the scroll can be clamped to it. */
    s->settings.status[sizeof(s->settings.status) - 1] = '\0';
    g_content_h = y + s->settings.scroll + SETTINGS_PAD;

    /* the footer keeps its place at the bottom whatever the window height */
    if (n + 2 <= max) {
        int fy = h - SETTINGS_FOOT + 6;
        out[n++] = (SCtl){SC_BUTTON, ID_SAVE,  "Save as default",
                          (SDL_Rect){SETTINGS_PAD, fy, 150, 30}, (SDL_Rect){SETTINGS_PAD, fy, 150, 30}};
        out[n++] = (SCtl){SC_BUTTON, ID_RESET, "Restore defaults",
                          (SDL_Rect){SETTINGS_PAD + 160, fy, 150, 30}, (SDL_Rect){SETTINGS_PAD + 160, fy, 150, 30}};
    }
    return n;
}

static void clamp_scroll(AppState *s, int h) {
    int view = h - SETTINGS_FOOT - 64;
    int max_scroll = g_content_h - 64 - view;
    if (max_scroll < 0) max_scroll = 0;
    if (s->settings.scroll < 0) s->settings.scroll = 0;
    if (s->settings.scroll > max_scroll) s->settings.scroll = max_scroll;
}

static int *stepper_value(AppState *s, int id) {
    switch (id) {
        case ID_TRACE_WIDTH:     return &s->settings.trace_width;
        case ID_PRED_WIDTH:      return &s->settings.pred_width;
        case ID_PROFILE_WIDTH:   return &s->settings.profile_width;
        case ID_PROFILE_OPACITY: return &s->settings.profile_opacity;
        case ID_TRACE_OPACITY:   return &s->settings.trace_opacity;
        case ID_STICK_OPACITY:   return &s->settings.stick_opacity;
        default:                 return NULL;
    }
}

/* Range and step of each stepper, and the unit shown next to the value. */
static void stepper_range(int id, int *lo, int *hi, int *step, const char **unit) {
    if (id == ID_PROFILE_OPACITY || id == ID_TRACE_OPACITY || id == ID_STICK_OPACITY) {
        *lo = 10; *hi = 100; *step = 5; *unit = "%";
    }
    else                          { *lo = 1;  *hi = 5;   *step = 1;  *unit = "px"; }
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

/* ---------------------------------------------------------------------------
 *  Window
 * ------------------------------------------------------------------------- */
void settings_open(AppState *s) {
    DisplaySettings *d = &s->settings;
    if (d->window) { SDL_RaiseWindow(d->window); return; }
    d->window = SDL_CreateWindow("Display settings", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 520, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!d->window) return;
    SDL_SetWindowMinimumSize(d->window, 420, 480);
    d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d->renderer) { SDL_DestroyWindow(d->window); d->window = NULL; return; }
    SDL_SetRenderDrawBlendMode(d->renderer, SDL_BLENDMODE_BLEND);
    d->window_id = SDL_GetWindowID(d->window);
    d->open = 1;
}

void settings_close(AppState *s) {
    DisplaySettings *d = &s->settings;
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window) SDL_DestroyWindow(d->window);
    d->renderer = NULL; d->window = NULL; d->open = 0; d->window_id = 0;
}

void settings_dispose(AppState *s) { settings_close(s); }

int settings_handle_event(AppState *s, const SDL_Event *e) {
    DisplaySettings *d = &s->settings;
    if (!d->open) return 0;
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == d->window_id &&
        e->window.event == SDL_WINDOWEVENT_CLOSE) { settings_close(s); return 1; }
    if (e->type == SDL_KEYDOWN && e->key.windowID == d->window_id &&
        e->key.keysym.sym == SDLK_ESCAPE) { settings_close(s); return 1; }
    if (e->type == SDL_MOUSEWHEEL && e->wheel.windowID == d->window_id) {
        int w = 0, h = 0;
        SDL_GetWindowSize(d->window, &w, &h);
        SCtl probe[48];
        build_controls(s, probe, 48, w, h);
        d->scroll -= e->wheel.y * 24;
        clamp_scroll(s, h);
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.windowID != d->window_id) return 0;

    int w = 0, h = 0;
    SDL_GetWindowSize(d->window, &w, &h);
    SCtl ctl[48];
    int n = build_controls(s, ctl, 48, w, h);
    int x = e->button.x, y = e->button.y;

    for (int i = 0; i < n; i++) {
        if (!point_in_rect(x, y, ctl[i].control)) continue;
        switch (ctl[i].kind) {
            case SC_STEPPER: {
                int *v = stepper_value(s, ctl[i].id);
                if (!v) break;
                int lo, hi, step; const char *unit;
                stepper_range(ctl[i].id, &lo, &hi, &step, &unit);
                int mid = ctl[i].control.x + ctl[i].control.w / 2;
                *v += (x < mid) ? -step : step;
                if (*v < lo) *v = lo;
                if (*v > hi) *v = hi;
                break;
            }
            case SC_TOGGLE: {
                int *v = toggle_value(s, ctl[i].id);
                if (v) *v = !*v;
                break;
            }
            case SC_COLOR: {
                SDL_Color *c = color_value(s, ctl[i].id);
                if (c) pick_color(c);
                break;
            }
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
            case SC_BUTTON:
                if (ctl[i].id == ID_SAVE) settings_save(s);
                else { settings_restore_defaults(s); apply_trace_colors(s); 
                       snprintf(d->status, sizeof(d->status), "Back to the built-in defaults (not saved yet)."); }
                break;
            default: break;
        }
        return 1;
    }
    return 1;
}

void settings_render(AppState *s) {
    DisplaySettings *d = &s->settings;
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
    ui_text(r, UI_FONT_TITLE, "Display settings", SETTINGS_PAD, 12, UI_TEXT);

    SCtl ctl[48];
    int n = build_controls(s, ctl, 48, w, h);
    clamp_scroll(s, h);
    n = build_controls(s, ctl, 48, w, h);
    char b[64];

    SDL_Rect list = {0, 42, w, h - SETTINGS_FOOT - 42};
    for (int i = 0; i < n; i++) {
        if (ctl[i].kind != SC_BUTTON) SDL_RenderSetClipRect(r, &list);
        else                          SDL_RenderSetClipRect(r, NULL);
        SCtl *c = &ctl[i];
        switch (c->kind) {
            case SC_HEADING:
                ui_hline(r, c->row.x, c->row.x + c->row.w, c->row.y + 6, UI_LINE_SOFT);
                ui_text(r, UI_FONT_SANS_SM, c->label, c->row.x, c->row.y + 12, UI_FAINT);
                break;
            case SC_STEPPER: {
                int *v = stepper_value(s, c->id);
                ui_text_v(r, UI_FONT_SANS, c->label, c->row.x, c->row, UI_DIM);
                ui_button(r, (SDL_Rect){c->control.x, c->control.y, 26, c->control.h}, "-", -1,
                          UI_BTN_QUIET, 0, mx, my, mdown);
                int lo, hi, step; const char *unit;
                stepper_range(c->id, &lo, &hi, &step, &unit);
                snprintf(b, sizeof(b), "%d %s", v ? *v : lo, unit);
                ui_text_v(r, UI_FONT_MONO, b, c->control.x + 34, c->control, UI_TEXT);
                ui_button(r, (SDL_Rect){c->control.x + c->control.w - 26, c->control.y, 26, c->control.h}, "+", -1,
                          UI_BTN_QUIET, 0, mx, my, mdown);
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
