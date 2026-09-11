/*
 * Regression suite of the audit (docs/audit/PIANO-FIX.md).
 *
 * Built like docs/audit/repro/harness.c: the real translation units main.c,
 * controller.c and predfit.c are #included, so the app's static functions
 * (set_predictions, handle_mouse_down, assign_selected_predictions,
 * write_inputs, ...) run exactly as in the app.  The other .c files are linked
 * unchanged; only the ImGui line renderer is replaced (tests/plotgpu_stub.c).
 *
 *   ./tests/test_audit             every test
 *   ./tests/test_audit <name> ...  only the named tests
 *   ./tests/test_audit -v ...      also show what the app prints on stdout
 *
 * Every test runs in a child process, inside its own scratch folder (mkdtemp)
 * which is also settings.data_dir: the repository is only ever read.  Tests
 * that need SPCAT/SPFIT are SKIP when autodetect_program does not find them.
 * The exit code is non-zero when any test FAILs.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <math.h>

#define main sv_app_main
#include "main.c"
#undef main
#include "controller.c"
#include "predfit.c"

#ifndef SV_TESTS_DIR
#error "compile with -DSV_TESTS_DIR=\"<repository>/tests\" (makefile target test)"
#endif

#define WIN_W 1400
#define WIN_H 884

enum { T_PASS = 0, T_FAIL = 1, T_SKIP = 2 };

static const char *g_argv0;
static char  g_fx[600];          /* synthetic fixtures, generated once per run */
static char  g_work[600];        /* scratch folder of the running test         */
static FILE *g_tlog;           /* expected/obtained lines of the running test */
static int   g_failed;

#define CHECK(cond, ...) do { if (!(cond)) {                                  \
    fprintf(g_tlog, "    %s:%d: ", __FILE__, __LINE__);                     \
    fprintf(g_tlog, __VA_ARGS__); fputc('\n', g_tlog); g_failed = 1; } } while (0)
#define CHECK_INT(what, got, want) do { long got_ = (long)(got), want_ = (long)(want); \
    CHECK(got_ == want_, "%s: atteso %ld, ottenuto %ld", (what), want_, got_); } while (0)
#define CHECK_DBL(what, got, want, tol) do { double got_ = (got), want_ = (want);      \
    CHECK(fabs(got_ - want_) <= (tol), "%s: atteso %.6f, ottenuto %.6f", (what), want_, got_); } while (0)
#define SKIP(...) do { fprintf(g_tlog, "    "); fprintf(g_tlog, __VA_ARGS__); \
    fputc('\n', g_tlog); return T_SKIP; } while (0)
#define DONE() return g_failed ? T_FAIL : T_PASS

/* ------------------------------------------------------------------ paths */
static const char *path_in(const char *dir, const char *name) {
    static char buf[8][900];
    static int k;
    k = (k + 1) % 8;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", dir, name);
    return buf[k];
}
static const char *fx(const char *name)        { return path_in(g_fx, name); }
static const char *work_path(const char *name) { return path_in(g_work, name); }
static const char *fixture(const char *name)   { return path_in(SV_TESTS_DIR "/fixtures", name); }

/* ------------------------------------------------------------------ state */
static AppState *new_state(void) {
    AppState *s = calloc(1, sizeof(AppState));
    init_app_defaults(s);
    settings_init(s, g_argv0);
    settings_apply_defaults(s);
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", g_work);
    return s;
}

static void mono_model(PredFitState *p) {
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368; p->fmax_ghz = 8.0; p->temp_k = 2.0;
    sync_basic_parameters(p);
}

/* Copy of the layout of main.c:469-511: main() itself is not callable. */
static void compute_layout(AppState *s, Layout *L) {
    int w = WIN_W, h = WIN_H;
    L->win_w = w; L->win_h = h;
    L->plot_x = UI_RAIL_W + UI_PLOT_GUTTER;
    L->gap = UI_PANEL_HEADER_H;
    int content_top = UI_CONTENT_Y, content_bottom = h - UI_STATUS_H;
    update_sidebars(s, L);
    int has_exp = (s->n_spectra > 0), has_pred = (s->n_pred > 0);
    int avail = content_bottom - content_top; if (avail < 200) avail = 200;
    if (has_exp && has_pred) {
        int usable = avail - 2 * UI_PANEL_HEADER_H - UI_PRED_AXIS_H; if (usable < 120) usable = 120;
        L->exp_h = (int)(usable * 0.62); L->pred_h = usable - L->exp_h;
        L->exp_y = content_top + UI_PANEL_HEADER_H; L->pred_y = L->exp_y + L->exp_h + UI_PANEL_HEADER_H;
    } else if (has_pred) {
        L->exp_h = 0; L->pred_h = avail - UI_PANEL_HEADER_H - UI_PRED_AXIS_H;
        L->exp_y = content_top + UI_PANEL_HEADER_H; L->pred_y = content_top + UI_PANEL_HEADER_H;
    } else {
        L->exp_h = avail - UI_PANEL_HEADER_H - UI_PRED_AXIS_H; L->pred_h = 0;
        L->exp_y = content_top + UI_PANEL_HEADER_H; L->pred_y = L->exp_y;
    }
    if (L->exp_h < 0) L->exp_h = 0;
    if (L->pred_h < 0) L->pred_h = 0;
    L->exp_x = L->plot_x; L->exp_w = L->plot_right - L->exp_x - 16; if (L->exp_w < 240) L->exp_w = 240;
    L->pred_x = L->plot_x; L->pred_w = L->exp_w;
}

/* Same pending-load pump used by the application event loop. */
static void pump(AppState *s) {
    process_pending_loads(s);
    commit_active(s);
}

/* ------------------------------------------------------------------ input */
static void mouse_button(AppState *s, Layout *L, Uint32 type, int x, int y, int button) {
    SDL_MouseButtonEvent b;
    memset(&b, 0, sizeof(b));
    b.type = type; b.button = (Uint8)button; b.x = x; b.y = y;
    if (type == SDL_MOUSEBUTTONDOWN) handle_mouse_down(s, L, &b);
    else                             handle_mouse_up(s, L, &b);
}

static void mouse_move(AppState *s, Layout *L, int x, int y) {
    SDL_MouseMotionEvent m;
    memset(&m, 0, sizeof(m));
    m.type = SDL_MOUSEMOTION; m.x = x; m.y = y;
    handle_mouse_motion(s, L, &m);
}

/* Real click on the prediction pane (controller.c:669-694). */
static int click_select(AppState *s, double freq) {
    Layout L;
    s->pvxmin = freq - 0.5; s->pvxmax = freq + 0.5;
    compute_layout(s, &L);
    int mx = L.pred_x + (int)lround((freq - s->pvxmin) / (s->pvxmax - s->pvxmin) * L.pred_w);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, mx, L.pred_y + L.pred_h / 2, SDL_BUTTON_LEFT);
    return s->n_selected;
}

/* Real right drag over [f0, f1] (true frequencies) on the experimental pane:
   mouse down, motion and mouse up through handle_mouse_* (controller.c:663-667,
   771-780), which then runs run_right_click_peak_find. */
static void right_drag(AppState *s, double f0, double f1) {
    s->vxmin = f0 - 1.0 + s->exp_offset; s->vxmax = f1 + 1.0 + s->exp_offset;
    Layout L;
    compute_layout(s, &L);
    double span = s->vxmax - s->vxmin;
    int x0 = L.exp_x + (int)lround((f0 + s->exp_offset - s->vxmin) / span * L.exp_w);
    int x1 = L.exp_x + (int)lround((f1 + s->exp_offset - s->vxmin) / span * L.exp_w);
    int y = L.exp_y + L.exp_h / 2;
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, x0, y, SDL_BUTTON_RIGHT);
    mouse_move(s, &L, x1, y);
    mouse_button(s, &L, SDL_MOUSEBUTTONUP, x1, y, SDL_BUTTON_RIGHT);
}

/* Real "Save all" button (controller.c:283-319). */
static void click_save_all(AppState *s) {
    Layout L;
    s->win_as.visible = 1;
    compute_layout(s, &L);
    SDL_Rect r = ui_as_save(s, s->win_as.rect);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, r.x + r.w / 2, r.y + r.h / 2, SDL_BUTTON_LEFT);
}

/* ------------------------------------------------------------------ files */
/* Gaussian peaks (sigma 0.03 MHz) on windows of +-1.5 MHz, step 0.002 MHz. */
static int cmp_freq(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void write_spectrum(const char *path, const double *pk, const double *ht, int n) {
    double *c = malloc(sizeof(double) * n);
    memcpy(c, pk, sizeof(double) * n);
    qsort(c, n, sizeof(double), cmp_freq);
    FILE *f = fopen(path, "w");
    double x_done = -1e99;
    for (int i = 0; i < n; i++) {
        double a = c[i] - 1.5, b = c[i] + 1.5;
        while (i + 1 < n && c[i + 1] - 1.5 <= b) { b = c[i + 1] + 1.5; i++; }
        if (a < x_done) a = x_done + 0.002;
        for (double x = a; x <= b; x += 0.002) {
            double y = 1e-4;
            for (int k = 0; k < n; k++) {
                double d = (x - pk[k]) / 0.03;
                if (fabs(d) < 12) y += (ht ? ht[k] : 1.0) * exp(-0.5 * d * d);
            }
            fprintf(f, "%.6f %.8e\n", x, y);
        }
        x_done = b;
    }
    fclose(f);
    free(c);
}

static void qn_of(const PredLine *p, int out[12]) {
    const int q[12] = {p->Ju, p->Kau, p->Kcu, p->M1u, p->M2u, p->M3u,
                       p->Jl, p->Kal, p->Kcl, p->M1l, p->M2l, p->M3l};
    memcpy(out, q, sizeof(q));
}

static int same_identity(const PredLine *a, const PredLine *b) {
    int qa[12], qb[12];
    qn_of(a, qa); qn_of(b, qb);
    return a->n_qn == b->n_qn && memcmp(qa, qb, sizeof(qa)) == 0;
}

/* R-02..R-07: catalogue -> click -> right drag on the peak -> assignment ->
   Save all -> new AppState -> set_predictions (ensure_aux_loaded).  The peaks
   sit 0.03 MHz below the predictions.  Every assignment in memory must come
   back with the same NQN, QN, ObsFreq, CalcFreq and CalcIntensity. */
static void check_roundtrip(const char *cat, const double *freqs, int nf) {
    double pk[64];
    for (int i = 0; i < nf; i++) pk[i] = freqs[i] - 0.03;
    write_spectrum(work_path("spectrum.txt"), pk, NULL, nf);

    AppState *s = new_state();
    set_predictions(s, cat);
    add_spectrum(s, work_path("spectrum.txt"));
    for (int i = 0; i < nf; i++) {
        int n = click_select(s, freqs[i]);
        CHECK(n >= 1, "clic a %.4f MHz: attesa almeno una riga selezionata, ottenute %d", freqs[i], n);
        right_drag(s, pk[i] - 0.2, pk[i] + 0.2);
    }
    click_save_all(s);

    AppState *r = new_state();
    set_predictions(r, cat);
    CHECK_INT("assignment dopo il riavvio", r->n_assignments, s->n_assignments);
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        const Assignment *b = NULL;
        for (int k = 0; k < r->n_assignments; k++)
            if (same_identity(&a->pred, &r->assignments[k].pred)) { b = &r->assignments[k]; break; }
        CHECK(b != NULL, "assignment %d (%.4f MHz, NQN %d): atteso ripristinato con gli stessi QN, ottenuto assente",
              i, a->pred.freq_mhz, a->pred.n_qn);
        if (!b) continue;
        CHECK_DBL("ObsFreq", b->exp_freq, a->exp_freq, 1e-6);
        CHECK_DBL("CalcFreq", b->pred.freq_mhz, a->pred.freq_mhz, 1e-6);
        CHECK(fabs(b->pred.linear_int - a->pred.linear_int) <= 1e-6 * fabs(a->pred.linear_int),
              "CalcIntensity: atteso %.6e, ottenuto %.6e", a->pred.linear_int, b->pred.linear_int);
    }
}

/* ============================================================ #0 baseline */

/* T-02, 1404 part: a four-digit QNFMT is read correctly today. */
static int test_baseline_cat1404_nqn4(void) {
    static const int want[4][12] = {
        { 4, 1,  4, 0, 0, 0,  3, 1, 3, 0, 0, 0},
        { 4, 1,  4, 1, 0, 0,  3, 1, 3, 1, 0, 0},
        {12, 2, 10, 2, 0, 0, 11, 2, 9, 2, 0, 0},
        {12, 2, 10, 0, 0, 0, 11, 2, 9, 0, 0, 0},
    };
    PredLine *L = NULL;
    double x0, x1, gmax;
    int n = read_pred_cat_alloc(fx("cat4_1404.cat"), &L, &x0, &x1, &gmax);
    CHECK_INT("righe lette", n, 4);
    for (int i = 0; i < n && i < 4; i++) {
        char what[64];
        snprintf(what, sizeof(what), "riga %d n_qn", i);
        CHECK_INT(what, L[i].n_qn, 4);
        int q[12];
        qn_of(&L[i], q);
        for (int k = 0; k < 12; k++) {
            snprintf(what, sizeof(what), "riga %d QN %d", i, k);
            CHECK_INT(what, q[k], want[i][k]);
        }
    }
    free(L);
    DONE();
}

/* R-02..R-07 with cat4_1404.cat: identical list after Save all and restart. */
static int test_baseline_roundtrip_qnfmt1404(void) {
    const double f[4] = {2511.3375, 2511.9, 6033.5894, 6034.0};
    check_roundtrip(fx("cat4_1404.cat"), f, 4);
    DONE();
}

/* T-08: the same transition moved to a second peak changes ObsFreq only. */
static int test_baseline_reassign_updates_obsfreq(void) {
    const double pk[2] = {3000.00, 3000.30};
    write_spectrum(work_path("two_peaks.txt"), pk, NULL, 2);
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    add_spectrum(s, work_path("two_peaks.txt"));

    CHECK_INT("righe selezionate", click_select(s, 3000.1), 1);
    right_drag(s, 2999.8, 3000.2);
    CHECK_INT("assignment dopo il primo picco", s->n_assignments, 1);
    if (s->n_assignments != 1) DONE();
    CHECK_DBL("ObsFreq del primo picco", s->assignments[0].exp_freq, 3000.00, 1e-3);
    PredLine first = s->assignments[0].pred;

    CHECK_INT("righe selezionate", click_select(s, 3000.1), 1);
    right_drag(s, 3000.2, 3000.4);
    CHECK_INT("assignment dopo il secondo picco", s->n_assignments, 1);
    CHECK_DBL("ObsFreq dopo la riassegnazione", s->assignments[0].exp_freq, 3000.30, 1e-3);
    CHECK(same_identity(&s->assignments[0].pred, &first),
          "transizione: attesa invariata (J %d), ottenuta J %d", first.Ju, s->assignments[0].pred.Ju);
    CHECK_DBL("CalcFreq", s->assignments[0].pred.freq_mhz, first.freq_mhz, 1e-9);
    DONE();
}

/* T-22, R-19: deleting a trace before the active one keeps the same trace
   active after the compacting move. */
static int test_remove_spectrum_keeps_active(void) {
    const double p0[] = {3000.0}, p1[] = {3001.0}, p2[] = {3002.0};
    write_spectrum(work_path("first.txt"), p0, NULL, 1);
    write_spectrum(work_path("second.txt"), p1, NULL, 1);
    write_spectrum(work_path("third.txt"), p2, NULL, 1);
    AppState *s = new_state();
    CHECK_INT("prima traccia", add_spectrum(s, work_path("first.txt")), 1);
    CHECK_INT("seconda traccia", add_spectrum(s, work_path("second.txt")), 1);
    CHECK_INT("terza traccia", add_spectrum(s, work_path("third.txt")), 1);
    select_spectrum(s, 2);
    remove_spectrum(s, 0);
    CHECK_INT("indice attivo corretto", s->active_spec, 1);
    CHECK(strstr(s->spectra[s->active_spec].path, "third.txt") != NULL,
          "traccia attiva cambiata: '%s'", s->spectra[s->active_spec].path);
    DONE();
}

static void queue_drop(const char *path) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_DROPFILE;
    e.drop.file = SDL_strdup(path);
    SDL_PushEvent(&e);
}

/* T-27, R-24: every path received in one SDL batch remains queued, rather
   than leaving only the final drop in a single pending-path field. */
static int test_drop_many_files_loads_all(void) {
    const double p0[] = {3000.0}, p1[] = {3001.0}, p2[] = {3002.0};
    write_spectrum(work_path("drop-one.txt"), p0, NULL, 1);
    write_spectrum(work_path("drop-two.txt"), p1, NULL, 1);
    write_spectrum(work_path("drop-three.txt"), p2, NULL, 1);
    CHECK_INT("SDL eventi", SDL_Init(SDL_INIT_EVENTS), 0);
    AppState *s = new_state(); Layout L = {0}; int running = 1;
    queue_drop(work_path("drop-one.txt"));
    queue_drop(work_path("drop-two.txt"));
    queue_drop(work_path("drop-three.txt"));
    handle_app_events(s, &L, &running);
    CHECK_INT("tre richieste in FIFO", s->pending_load_count, 3);
    pump(s);
    CHECK_INT("tutte le tracce caricate", s->n_spectra, 3);
    CHECK(strstr(s->spectra[0].path, "drop-one.txt") != NULL, "ordine primo drop: %s", s->spectra[0].path);
    CHECK(strstr(s->spectra[1].path, "drop-two.txt") != NULL, "ordine secondo drop: %s", s->spectra[1].path);
    CHECK(strstr(s->spectra[2].path, "drop-three.txt") != NULL, "ordine terzo drop: %s", s->spectra[2].path);
    DONE();
}

/* A completed Calculate produces the same catalog request as this direct
   enqueue.  A simultaneous spectrum drop must not overwrite either request. */
static int test_calculate_and_drop_same_frame(void) {
    const double p[] = {3000.0};
    write_spectrum(work_path("calculation-drop.txt"), p, NULL, 1);
    CHECK_INT("SDL eventi", SDL_Init(SDL_INIT_EVENTS), 0);
    AppState *s = new_state(); Layout L = {0}; int running = 1;
    CHECK_INT("catalogo Calculate accodato",
              app_enqueue_pending_load(s, PENDING_LOAD_CATALOG, fx("cat3_303.cat"), 0), 1);
    queue_drop(work_path("calculation-drop.txt"));
    handle_app_events(s, &L, &running);
    CHECK_INT("catalogo e drop in FIFO", s->pending_load_count, 2);
    pump(s);
    CHECK(s->n_pred > 0, "catalogo della Calculate perso");
    CHECK_INT("spettro del drop presente", s->n_spectra, 1);
    CHECK(strstr(s->spectra[0].path, "calculation-drop.txt") != NULL,
          "drop perso: %s", s->spectra[0].path);
    DONE();
}

/* R-31, ascending part: the right drag measures the weak line under the
   pointer, not the strong one 1 MHz away. */
static int test_baseline_right_drag_ascending(void) {
    FILE *f = fopen(work_path("two.cat"), "w");
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3000.0, 0.001, -4.0, 3, 1.0, 11, 1, 303, " 5 1 5       4 1 4      ");
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3001.0, 0.001, -3.3, 3, 1.0, 11, 1, 303, " 6 1 6       5 1 5      ");
    fclose(f);
    f = fopen(work_path("asc.txt"), "w");
    for (int i = 0; i <= 2000; i++) {
        double x = 2998.0 + i * 0.002;
        double d1 = (x - 3000.0) / 0.03, d2 = (x - 3001.0) / 0.03;
        fprintf(f, "%.6f %.8e\n", x, 1e-4 + 1.0 * exp(-0.5 * d1 * d1) + 5.0 * exp(-0.5 * d2 * d2));
    }
    fclose(f);

    AppState *s = new_state();
    set_predictions(s, work_path("two.cat"));
    add_spectrum(s, work_path("asc.txt"));
    right_drag(s, 2999.8, 3000.2);
    CHECK_INT("picchi misurati", s->n_peaks, 1);
    if (s->n_peaks > 0) CHECK_DBL("frequenza misurata", s->peaks[s->n_peaks - 1].x, 3000.0, 5e-4);
    DONE();
}

static void write_r31_spectrum(const char *path, int descending) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int k = 0; k <= 2000; k++) {
        int i = descending ? 2000 - k : k;
        double x = 2998.0 + i * 0.002;
        double d1 = (x - 3000.0) / 0.03, d2 = (x - 3001.0) / 0.03;
        fprintf(f, "%.6f %.8e\n", x, 1e-4 + exp(-0.5 * d1 * d1) + 5.0 * exp(-0.5 * d2 * d2));
    }
    fclose(f);
}

/* T-35, R-31: a descending trace is normalised at input, so all binary-search
   consumers measure the same local peak and area as its ascending twin. */
static int test_descending_spectrum_same_results(void) {
    FILE *f = fopen(work_path("two.cat"), "w");
    CHECK(f != NULL, "impossibile creare il catalogo R-31");
    if (!f) DONE();
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3000.0, 0.001, -4.0, 3, 1.0, 11, 1, 303, " 5 1 5       4 1 4      ");
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3001.0, 0.001, -3.3, 3, 1.0, 11, 1, 303, " 6 1 6       5 1 5      ");
    fclose(f);
    write_r31_spectrum(work_path("ascending.txt"), 0);
    write_r31_spectrum(work_path("descending.txt"), 1);

    AppState *ascending = new_state(), *descending = new_state();
    set_predictions(ascending, work_path("two.cat"));
    set_predictions(descending, work_path("two.cat"));
    CHECK_INT("carica crescente", add_spectrum(ascending, work_path("ascending.txt")), 1);
    CHECK_INT("carica decrescente", add_spectrum(descending, work_path("descending.txt")), 1);
    CHECK(strstr(descending->status_message, "reordered from descending") != NULL,
          "stato del file decrescente: '%s'", descending->status_message);
    click_select(ascending, 3000.0); click_select(descending, 3000.0);
    right_drag(ascending, 2999.8, 3000.2); right_drag(descending, 2999.8, 3000.2);
    CHECK_INT("picco crescente", ascending->n_peaks, 1);
    CHECK_INT("picco decrescente", descending->n_peaks, 1);
    if (ascending->n_peaks && descending->n_peaks)
        CHECK_DBL("stessa frequenza misurata", descending->peaks[0].x, ascending->peaks[0].x, 5e-4);
    double area_up = 0.0, area_down = 0.0;
    CHECK_INT("area crescente", intensity_fit_integrate_area(ascending, 3000.0, 0.2, &area_up), 1);
    CHECK_INT("area decrescente", intensity_fit_integrate_area(descending, 3000.0, 0.2, &area_down), 1);
    CHECK_DBL("stessa area", area_down, area_up, 1e-9);
    DONE();
}

static int test_nonmonotonic_spectrum_rejected(void) {
    const char *cases[] = {"1 1\n2 2\n1.5 3\n", "1 1\n2 2\n2 3\n"};
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(work_path(i ? "duplicate.txt" : "mixed.txt"), "w");
        CHECK(f != NULL, "impossibile creare il caso non monotono");
        if (!f) continue;
        fputs(cases[i], f); fclose(f);
        AppState *s = new_state();
        CHECK_INT("file non monotono rifiutato", add_spectrum(s, work_path(i ? "duplicate.txt" : "mixed.txt")), 0);
        CHECK(strstr(s->error_message, "strictly monotonic") != NULL,
              "errore del file non monotono: '%s'", s->error_message);
    }
    DONE();
}

/* Calculate with a one-species model produces model.cat and loads it. */
static int test_baseline_calculate_single_species(void) {
    AppState *s = new_state();
    if (!have_program(s->settings.spcat_path)) SKIP("SPCAT non trovato da autodetect_program");
    mono_model(&s->predfit);
    CHECK_INT("predfit_calculate", predfit_calculate(s), 1);
    struct stat st;
    int produced = stat(work_path(".fit/model.cat"), &st) == 0 && st.st_size > 0;
    CHECK(produced, "model.cat: atteso prodotto da SPCAT, ottenuto assente (stato: %s)", s->predfit.status);
    pump(s);
    CHECK(s->n_pred > 0, "righe di model.cat caricate: attese > 0, ottenute %d", s->n_pred);
    CHECK_INT("generated_catalog_active", s->predfit.generated_catalog_active, 1);
    DONE();
}

/* ============================================================== #1 parser */

typedef struct { double freq; int n_qn; int qn[12]; } CatRow;

static const char *fmt_qn(const int q[12]) {
    static char buf[4][96];
    static int k;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "(%d %d %d %d %d %d ; %d %d %d %d %d %d)",
             q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
    return buf[k];
}

/* Reads `path` and compares every row (frequency order) with `want`. */
static void check_cat_rows(const char *label, const char *path, const CatRow *want, int n_want) {
    PredLine *L = NULL;
    double x0, x1, gmax;
    int n = read_pred_cat_alloc(path, &L, &x0, &x1, &gmax);
    char what[160];
    snprintf(what, sizeof(what), "%s: righe lette", label);
    CHECK_INT(what, n, n_want);
    for (int i = 0; i < n && i < n_want; i++) {
        snprintf(what, sizeof(what), "%s %.4f MHz: frequenza", label, want[i].freq);
        CHECK_DBL(what, L[i].freq_mhz, want[i].freq, 1e-9);
        snprintf(what, sizeof(what), "%s %.4f MHz: n_qn", label, want[i].freq);
        CHECK_INT(what, L[i].n_qn, want[i].n_qn);
        int q[12];
        qn_of(&L[i], q);
        CHECK(memcmp(q, want[i].qn, sizeof(q)) == 0, "%s %.4f MHz: QN attesi %s, ottenuti %s",
              label, want[i].freq, fmt_qn(want[i].qn), fmt_qn(q));
    }
    free(L);
}

/* T-01: NQN is QNFMT % 10 on every row, whatever the number of digits of J. */
static int test_cat_nqn_from_qnfmt_3qn(void) {
    static const CatRow rows[] = {
        {3000.1000, 3, { 5, 1,  5, 0, 0, 0,   4, 1,  4, 0, 0, 0}},
        {5980.0000, 3, {11, 1, 11, 0, 0, 0,  10, 1, 10, 0, 0, 0}},
        {5999.2867, 3, {11, 0, 11, 0, 0, 0,  10, 1,  9, 0, 0, 0}},
        {7000.0000, 3, {25, 3, 22, 0, 0, 0,  24, 3, 21, 0, 0, 0}},
        {8000.0000, 3, {45, 2, 43, 0, 0, 0,  44, 2, 42, 0, 0, 0}},
        {9000.0000, 3, {72, 1, 71, 0, 0, 0,  71, 1, 70, 0, 0, 0}},
    };
    check_cat_rows("cat3_303", fx("cat3_303.cat"), rows, 6);

    PredLine *L = NULL;
    double x0, x1, gmax;
    int n = read_pred_cat_alloc(fixture("pred_reference.cat"), &L, &x0, &x1, &gmax);
    CHECK_INT("pred_reference: righe lette", n, 2312);
    int wrong = 0, first = -1;
    for (int i = 0; i < n; i++) if (L[i].n_qn != 3) { if (first < 0) first = i; wrong++; }
    CHECK(wrong == 0, "pred_reference: attese 0 righe con n_qn != 3, ottenute %d (la prima a %.4f MHz con n_qn %d)",
          wrong, first >= 0 ? L[first].freq_mhz : 0.0, first >= 0 ? L[first].n_qn : 0);
    static const int want_5999[12] = {11, 0, 11, 0, 0, 0, 10, 1, 9, 0, 0, 0};
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (fabs(L[i].freq_mhz - 5999.2867) > 1e-6) continue;
        int q[12];
        qn_of(&L[i], q);
        found = 1;
        CHECK_INT("pred_reference 5999.2867 MHz: n_qn", L[i].n_qn, 3);
        CHECK(memcmp(q, want_5999, sizeof(q)) == 0, "pred_reference 5999.2867 MHz: QN attesi %s, ottenuti %s",
              fmt_qn(want_5999), fmt_qn(q));
    }
    CHECK(found, "pred_reference: attesa la riga a 5999.2867 MHz, non trovata");
    free(L);
    DONE();
}

/* T-02, T-03: four QN with F (304) or v (1404) in the fourth field, five, six. */
static int test_cat_nqn_4_5_6(void) {
    static const CatRow c304[] = {
        {3100.0000, 4, { 3, 1,  2,  4, 0, 0,   2, 1,  1,  3, 0, 0}},
        {3100.3000, 4, { 3, 1,  2,  3, 0, 0,   2, 1,  1,  2, 0, 0}},
        {6100.0000, 4, {12, 1, 11, 13, 0, 0,  11, 1, 10, 12, 0, 0}},
        {6100.3000, 4, {12, 1, 11, 12, 0, 0,  11, 1, 10, 11, 0, 0}},
    };
    static const CatRow c1404[] = {
        {2511.3375, 4, { 4, 1,  4, 0, 0, 0,   3, 1, 3, 0, 0, 0}},
        {2511.9000, 4, { 4, 1,  4, 1, 0, 0,   3, 1, 3, 1, 0, 0}},
        {6033.5894, 4, {12, 2, 10, 2, 0, 0,  11, 2, 9, 2, 0, 0}},
        {6034.0000, 4, {12, 2, 10, 0, 0, 0,  11, 2, 9, 0, 0, 0}},
    };
    static const CatRow c305[] = {
        {3200.0000, 5, { 4, 0,  4,  5,  5, 0,   3, 0,  3,  4,  4, 0}},
        {3200.3000, 5, { 4, 0,  4,  5,  6, 0,   3, 0,  3,  4,  5, 0}},
        {6200.0000, 5, {15, 1, 14, 16, 16, 0,  14, 1, 13, 15, 15, 0}},
    };
    static const CatRow c306[] = {
        {3300.0000, 6, { 2, 1,  1,  3,  4,  5,   1, 1,  0,  2,  3,  4}},
        {3300.3000, 6, { 2, 1,  1,  3,  4,  4,   1, 1,  0,  2,  3,  3}},
        {6300.0000, 6, {13, 1, 12, 14, 15, 16,  12, 1, 11, 13, 14, 15}},
    };
    check_cat_rows("cat4_304", fx("cat4_304.cat"), c304, 4);
    check_cat_rows("cat4_1404", fx("cat4_1404.cat"), c1404, 4);
    check_cat_rows("cat5_305", fx("cat5_305.cat"), c305, 3);
    check_cat_rows("cat6_306", fx("cat6_306.cat"), c306, 3);
    DONE();
}

/* T-04: Pickett's letter codes (A5 = 105, a1 = -11) and "-d" (-5). */
static int test_cat_letter_and_negative_qn(void) {
    static const CatRow letter[] = {
        {3400.0000, 3, {105, 3, 102, 0, 0, 0,  104, 3, 101, 0, 0, 0}},
        {3401.0000, 3, {  6, 3,   3, 0, 0, 0,    5, 3,   2, 0, 0, 0}},
    };
    static const CatRow negative[] = {
        {3402.0000, 3, {7, -11, -5, 0, 0, 0,  6, -10, 3, 0, 0, 0}},
    };
    check_cat_rows("cat_letter", fx("cat_letter.cat"), letter, 2);
    check_cat_rows("cat_negative", fx("cat_negative.cat"), negative, 1);
    DONE();
}

/* T-05: the same records with and without trailing blanks. */
static int test_cat_trailing_spaces_irrelevant(void) {
    FILE *src = fopen(fixture("pred_reference.cat"), "r");
    FILE *dst = fopen(work_path("untrimmed.cat"), "w");
    char line[512];
    for (int i = 0; i < 5 && src && dst && fgets(line, sizeof(line), src); i++) fputs(line, dst);
    if (src) fclose(src);
    if (dst) fclose(dst);

    PredLine *a = NULL, *b = NULL;
    double x0, x1, gmax;
    int na = read_pred_cat_alloc(work_path("untrimmed.cat"), &a, &x0, &x1, &gmax);
    int nb = read_pred_cat_alloc(fx("cat_trim.cat"), &b, &x0, &x1, &gmax);
    CHECK_INT("righe lette con gli spazi finali", na, 5);
    CHECK_INT("righe lette senza spazi finali", nb, na);
    for (int i = 0; i < na && i < nb; i++) {
        int qa[12], qb[12];
        qn_of(&a[i], qa); qn_of(&b[i], qb);
        CHECK_DBL("frequenza", b[i].freq_mhz, a[i].freq_mhz, 0.0);
        CHECK_DBL("LGINT", b[i].cat_lgint, a[i].cat_lgint, 0.0);
        CHECK_DBL("ELO", b[i].elo_cm, a[i].elo_cm, 0.0);
        CHECK_INT("DR", b[i].rot_dof, a[i].rot_dof);
        CHECK_INT("n_qn", b[i].n_qn, a[i].n_qn);
        CHECK(memcmp(qa, qb, sizeof(qa)) == 0, "riga %d: QN attesi %s, ottenuti %s", i, fmt_qn(qa), fmt_qn(qb));
    }
    free(a); free(b);
    DONE();
}

/* B-25: the numeric fields are fixed width (calpgm/calcat.c:700-709), so
   FREQ/ERR and ELO/GUP that touch are still read apart. */
static int test_cat_fixed_width_numbers(void) {
    char line[512] = "";
    FILE *f = fopen(fx("cat_freq_err.cat"), "r");
    if (f) { if (!fgets(line, sizeof(line), f)) line[0] = '\0'; fclose(f); }
    CHECK(strstr(line, "6348.1049158.2229") != NULL, "fixture: attesi FREQ ed ERR a contatto, riga '%s'", line);

    PredLine *L = NULL;
    double x0, x1, gmax;
    int n = read_pred_cat_alloc(fx("cat_freq_err.cat"), &L, &x0, &x1, &gmax);
    CHECK_INT("righe lette", n, 1);
    if (n == 1) {
        CHECK_DBL("FREQ", L[0].freq_mhz, 6348.1049, 1e-9);
        CHECK_DBL("LGINT", L[0].cat_lgint, -5.1234, 1e-9);
        CHECK_DBL("ELO", L[0].elo_cm, 12.3456, 1e-9);
        CHECK_INT("DR", L[0].rot_dof, 3);
    }
    free(L);
    /* ERR is not kept in PredLine: parse_cat_record returns it. */
    PredLine rec;
    double err = 0.0;
    CHECK_INT("parse_cat_record", parse_cat_record(line, &rec, &err), 1);
    CHECK_DBL("ERR", err, 158.2229, 1e-9);
    DONE();
}

/* D6: NQN 0 (10 QN per state) and NQN > 6 are not loaded and are counted in
   the message shown to the user. */
static int test_cat_invalid_nqn_reported(void) {
    AppState *s = new_state();
    set_predictions(s, fx("cat_nqn_invalid.cat"));
    CHECK_INT("righe caricate", s->n_pred, 1);
    if (s->n_pred >= 1) CHECK_DBL("riga caricata", s->pred_lines[0].freq_mhz, 3500.0, 1e-9);
    CHECK(strstr(s->status_message, "2 skipped") != NULL,
          "messaggio di stato: atteso il conteggio '2 skipped', ottenuto '%s'", s->status_message);
    CHECK(strstr(s->error_message, "2 skipped") != NULL,
          "messaggio nella barra del titolo: atteso il conteggio '2 skipped', ottenuto '%s'", s->error_message);
    DONE();
}

/* ============================================== #2 no assignment is lost */

static void assign_index(AppState *s, int idx, double exp_freq, double exp_int) {
    s->selected_indices[0] = idx; s->n_selected = 1;          /* = controller.c:681-693 */
    assign_selected_predictions(s, exp_freq, exp_int);        /* controller.c:836-852 */
}

static int index_of_freq(const AppState *s, double f) {
    for (int i = 0; i < s->n_pred; i++) if (fabs(s->pred_lines[i].freq_mhz - f) < 1e-4) return i;
    return -1;
}

static int copy_path(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    char buf[4096];
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    return ok;
}

/* Whole file as a string, or NULL. */
static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* QN block of assignments.txt and of the .lin: NQN upper, NQN lower, blanks
   up to 36 columns. */
static void put_qn_fields(FILE *f, const int *upper, const int *lower, int nq) {
    for (int q = 0; q < nq; q++) fprintf(f, "%3d", upper[q]);
    for (int q = 0; q < nq; q++) fprintf(f, "%3d", lower[q]);
    for (int q = 2 * nq; q < 12; q++) fputs("   ", f);
}
static void write_list_row(FILE *f, const int *u, const int *l, int nq, double obs, double calc, double inten) {
    put_qn_fields(f, u, l, nq);
    fprintf(f, "%15.6f %15.6f %15.6E %d\n", obs, calc, inten, nq);
}
static void write_lin_row(FILE *f, const int *u, const int *l, int nq, double obs) {
    put_qn_fields(f, u, l, nq);
    fprintf(f, "%15.6f %10.6f 1.0\n", obs, 0.01);
}

static void click_panel_button(AppState *s, DraggableWindow *win, SDL_Rect (*rect_of)(SDL_Rect)) {
    Layout L;
    win->visible = 1;
    compute_layout(s, &L);
    SDL_Rect r = rect_of(win->rect);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, r.x + r.w / 2, r.y + r.h / 2, SDL_BUTTON_LEFT);
}

/* Real "Delete selected" (controller.c:320) on row k of the list. */
static void click_delete_selected(AppState *s, int k) {
    Layout L;
    s->selected_assignment = k;
    s->win_as.visible = 1;
    compute_layout(s, &L);
    SDL_Rect r = ui_as_delete(s, s->win_as.rect);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, r.x + r.w / 2, r.y + r.h / 2, SDL_BUTTON_LEFT);
}

/* The list on disk (data_dir/assignments.txt) must hold exactly the list in
   memory: same transitions, same observed frequencies. */
static void check_file_matches_list(const AppState *s, const char *when) {
    char path[700];
    settings_data_file(s, "assignments.txt", path, sizeof(path));
    Assignment *disk = calloc(MAX_ASSIGNMENTS, sizeof(Assignment));
    int n = 0;
    load_existing_assignments(path, disk, &n);
    char what[160];
    snprintf(what, sizeof(what), "%s: righe in assignments.txt", when);
    CHECK_INT(what, n, s->n_assignments);
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i], *b = NULL;
        for (int k = 0; k < n; k++) if (same_identity(&a->pred, &disk[k].pred)) b = &disk[k];
        CHECK(b != NULL, "%s: attesa su disco la transizione J %d <- %d, assente", when, a->pred.Ju, a->pred.Jl);
        if (b) CHECK_DBL("ObsFreq su disco", b->exp_freq, a->exp_freq, 1e-6);
    }
    free(disk);
}

/* A data folder inside the scratch folder, different from the CWD. */
static void make_data_dir(char *out, size_t n) {
    snprintf(out, n, "%s", work_path("data"));
    mkdir(out, 0700);
    char fit[800];
    snprintf(fit, sizeof(fit), "%s/.fit", out);
    mkdir(fit, 0700);
}

/* T-14: a launch without a .cat, from a folder that is not data_dir, rebuilds
   the same list (count, QN, frequencies) as a launch with the .cat. */
static int test_restore_reads_data_dir_list(void) {
    char data[700];
    make_data_dir(data, sizeof(data));
    const double want[3] = {3000.1, 5999.2867, 7000.0};

    AppState *s = new_state();
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", data);
    set_predictions(s, fx("cat3_303.cat"));
    for (int k = 0; k < 3; k++) {
        int i = index_of_freq(s, want[k]);
        CHECK(i >= 0, "riga a %.4f MHz non trovata nel catalogo", want[k]);
        if (i >= 0) assign_index(s, i, want[k] - 0.02, 1.0);
    }
    click_save_all(s);
    copy_path(fx("cat3_303.cat"), path_in(data, ".fit/model.cat"));
    write_inputs(s, 1);                                   /* model.lin, as a Fit writes it */

    AppState *a = new_state();                            /* no .cat, CWD != data_dir */
    snprintf(a->settings.data_dir, sizeof(a->settings.data_dir), "%s", data);
    predfit_load_session(a);
    CHECK_INT("predfit_restore_latest", predfit_restore_latest(a), 1);
    pump(a);
    AppState *b = new_state();                            /* with the .cat */
    snprintf(b->settings.data_dir, sizeof(b->settings.data_dir), "%s", data);
    predfit_load_session(b);
    set_predictions(b, fx("cat3_303.cat"));

    CHECK_INT("assignment all'avvio con il .cat", b->n_assignments, 3);
    CHECK_INT("assignment all'avvio senza .cat", a->n_assignments, b->n_assignments);
    for (int i = 0; i < b->n_assignments; i++) {
        const Assignment *x = &b->assignments[i], *y = NULL;
        for (int k = 0; k < a->n_assignments; k++) if (same_identity(&x->pred, &a->assignments[k].pred)) y = &a->assignments[k];
        CHECK(y != NULL, "senza .cat: attesa la transizione J %d <- %d, assente", x->pred.Ju, x->pred.Jl);
        if (!y) continue;
        CHECK_DBL("ObsFreq senza .cat", y->exp_freq, x->exp_freq, 1e-6);
        CHECK_DBL("CalcFreq senza .cat", y->pred.freq_mhz, x->pred.freq_mhz, 1e-6);
    }
    DONE();
}

/* The list and the .lin as commit 1f4df65 wrote them from cat3_303.cat: the
   J = 11 and J = 25 rows were truncated by B-01 to 1 and 2 QN per state. */
static const int LEG_U0[3] = {5, 1, 5}, LEG_L0[3] = {4, 1, 4};
static const int LEG_U1[1] = {11},      LEG_L1[1] = {10};
static const int LEG_U2[2] = {25, 3},   LEG_L2[2] = {24, 3};

static void write_legacy_session(const char *data, int with_list) {
    copy_path(fx("cat3_303.cat"), path_in(data, ".fit/model.cat"));
    if (with_list) {
        FILE *f = fopen(path_in(data, "assignments.txt"), "w");
        fprintf(f, "# Upper QNs, lower QNs (SPFIT .lin order), ObsFreq(MHz) CalcFreq(MHz) CalcIntensity NQN\n");
        write_list_row(f, LEG_U0, LEG_L0, 3, 3000.08, 3000.1, 1.0e-4);
        write_list_row(f, LEG_U1, LEG_L1, 1, 5999.2667, 5999.2867, 1.364897e-4);
        write_list_row(f, LEG_U2, LEG_L2, 2, 6999.98, 7000.0, 6.309573e-5);
        fclose(f);
    }
    FILE *f = fopen(path_in(data, ".fit/model.lin"), "w");
    write_lin_row(f, LEG_U0, LEG_L0, 3, 3000.08);
    write_lin_row(f, LEG_U1, LEG_L1, 1, 5999.2667);
    write_lin_row(f, LEG_U2, LEG_L2, 2, 6999.98);
    fclose(f);
}

static const Assignment *find_exp(const AppState *s, double f) {
    for (int i = 0; i < s->n_assignments; i++) if (fabs(s->assignments[i].exp_freq - f) < 1e-6) return &s->assignments[i];
    return NULL;
}

/* model.lin rows with 2 and 4 QN fields are kept, marked and counted. */
static int test_restore_keeps_short_lin_rows(void) {
    char data[700];
    make_data_dir(data, sizeof(data));
    write_legacy_session(data, 0);                        /* only model.lin, no list */

    AppState *r = new_state();
    snprintf(r->settings.data_dir, sizeof(r->settings.data_dir), "%s", data);
    predfit_load_session(r);
    CHECK_INT("predfit_restore_latest", predfit_restore_latest(r), 1);
    CHECK_INT("assignment ripristinati da model.lin", r->n_assignments, 3);
    const Assignment *a1 = find_exp(r, 5999.2667), *a2 = find_exp(r, 6999.98);
    CHECK(a1 != NULL, "riga .lin a 2 campi (5999.2667 MHz): attesa, assente");
    CHECK(a2 != NULL, "riga .lin a 4 campi (6999.98 MHz): attesa, assente");
    if (a1) {
        int q[12]; qn_of(&a1->pred, q);
        static const int w1[12] = {11, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0};
        CHECK_INT("n_qn della riga a 2 campi", a1->pred.n_qn, 1);
        CHECK(memcmp(q, w1, sizeof(q)) == 0, "QN attesi %s, ottenuti %s", fmt_qn(w1), fmt_qn(q));
    }
    if (a2) {
        int q[12]; qn_of(&a2->pred, q);
        static const int w2[12] = {25, 3, 0, 0, 0, 0, 24, 3, 0, 0, 0, 0};
        CHECK_INT("n_qn della riga a 4 campi", a2->pred.n_qn, 2);
        CHECK(memcmp(q, w2, sizeof(q)) == 0, "QN attesi %s, ottenuti %s", fmt_qn(w2), fmt_qn(q));
    }
    const Assignment *a0 = find_exp(r, 3000.08);
    CHECK(a0 != NULL && a0->needs_reassign == 0, "riga .lin a 6 campi: attesa presente e non marcata");
    if (a1) CHECK_INT("riga a 2 campi marcata da riassegnare", a1->needs_reassign, 1);
    if (a2) CHECK_INT("riga a 4 campi marcata da riassegnare", a2->needs_reassign, 1);
    CHECK(strstr(r->predfit.status, "2 assignments from model.lin") != NULL,
          "stato: atteso il conteggio '2 assignments from model.lin', ottenuto '%s'", r->predfit.status);
    DONE();
}

/* T-19, R-16: model.int is a disposable generated input.  On restore, the
   saved species and its automatic INT values must survive even when that file
   contains the default 1 K / fixed FQLIM values of another run. */
static int test_restore_int_keeps_species_and_auto_fields(void) {
    AppState *saved = new_state();
    PredFitState *p = &saved->predfit;
    mono_model(p);
    add_species(saved);                                  /* active state 1 */
    p->temp_k = 5.0;
    p->mu[0] = 0.4; p->mu[1] = 0.3; p->mu[2] = 0.5;
    store_active_species(p);
    p->int_settings.fqlim_ghz = 0.0;                     /* automatic */
    p->int_settings.maxv = -1;                           /* automatic */
    predfit_save_session(saved);
    copy_path(fx("cat3_303.cat"), work_path(".fit/model.cat"));

    FILE *fp = fopen(work_path(".fit/model.int"), "w");
    CHECK(fp != NULL, "impossibile preparare model.int conflittuale");
    if (fp) {
        fputs("stale generated input\n", fp);
        fputs("0 1 123 7 40 -20 -20 8 1 1\n", fp);
        fputs("001 .75\n002 .21\n003 1.14\n", fp);
        fclose(fp);
    }

    AppState *restored = new_state();
    CHECK_INT("ripristino", predfit_restore_latest(restored), 1);
    PredFitState *r = &restored->predfit;
    CHECK_INT("numero specie", r->n_species, 2);
    CHECK_INT("specie attiva", r->active_species, 1);
    CHECK_DBL("T della specie attiva", r->temp_k, 5.0, 1e-9);
    CHECK_DBL("mu_a della specie attiva", r->mu[0], 0.4, 1e-9);
    CHECK_DBL("mu_b della specie attiva", r->mu[1], 0.3, 1e-9);
    CHECK_DBL("mu_c della specie attiva", r->mu[2], 0.5, 1e-9);
    CHECK_DBL("FQLIM automatico", r->int_settings.fqlim_ghz, 0.0, 1e-9);
    CHECK_INT("MAXV automatico", r->int_settings.maxv, -1);
    add_species(restored);
    CHECK_INT("MAXV resta automatico dopo + specie", r->int_settings.maxv, -1);
    DONE();
}

/* T-33, R-29: restart from another folder, then Save all: no row is lost and
   the previous file is kept as assignments.txt.bak. */
static int test_save_all_after_restore_no_loss(void) {
    char data[700];
    make_data_dir(data, sizeof(data));
    write_legacy_session(data, 1);
    char *before = read_all(path_in(data, "assignments.txt"));

    AppState *r = new_state();                            /* CWD != data_dir, no .cat */
    snprintf(r->settings.data_dir, sizeof(r->settings.data_dir), "%s", data);
    predfit_load_session(r);
    CHECK_INT("predfit_restore_latest", predfit_restore_latest(r), 1);
    pump(r);
    CHECK_INT("assignment ripristinati", r->n_assignments, 3);
    click_save_all(r);

    FILE *f = fopen(work_path("before.txt"), "w");
    fputs(before ? before : "", f);
    fclose(f);
    Assignment *was = calloc(MAX_ASSIGNMENTS, sizeof(Assignment)), *now = calloc(MAX_ASSIGNMENTS, sizeof(Assignment));
    int n_was = 0, n_now = 0;
    load_existing_assignments(work_path("before.txt"), was, &n_was);
    load_existing_assignments(path_in(data, "assignments.txt"), now, &n_now);
    CHECK_INT("righe di assignments.txt prima", n_was, 3);
    CHECK_INT("righe di assignments.txt dopo Save all", n_now, n_was);
    for (int i = 0; i < n_was; i++) {
        const Assignment *b = NULL;
        for (int k = 0; k < n_now; k++) if (same_identity(&was[i].pred, &now[k].pred)) b = &now[k];
        CHECK(b != NULL, "dopo Save all: attesa la riga J %d <- %d (%.4f MHz), persa", was[i].pred.Ju, was[i].pred.Jl, was[i].exp_freq);
        if (b) CHECK_DBL("ObsFreq", b->exp_freq, was[i].exp_freq, 1e-6);
    }
    char *bak = read_all(path_in(data, "assignments.txt.bak"));
    CHECK(bak && before && strcmp(bak, before) == 0,
          "assignments.txt.bak: atteso uguale al file precedente, ottenuto %s", bak ? "un contenuto diverso" : "file assente");
    free(was); free(now); free(before); free(bak);
    DONE();
}

/* U-06: Save all and Export list report a failed write, and a folder that
   cannot be written keeps the previous list intact. */
static int test_save_all_reports_write_error(void) {
    AppState *s = new_state();
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", work_path("missing"));
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, 3000.08, 1.0);
    s->error_message[0] = '\0';
    click_save_all(s);
    CHECK(s->error_message[0] != '\0', "Save all in una cartella inesistente: atteso un errore in error_message, ottenuto nessun messaggio");
    s->error_message[0] = '\0';
    s->peaks[0] = (Peak){3000.08, 1.0};
    s->n_peaks = 1;
    click_panel_button(s, &s->win_pf, ui_pf_export);
    CHECK(s->error_message[0] != '\0', "Export list in una cartella inesistente: atteso un errore in error_message, ottenuto nessun messaggio");

    char ro[700];
    snprintf(ro, sizeof(ro), "%s", work_path("readonly"));
    mkdir(ro, 0700);
    FILE *f = fopen(path_in(ro, "assignments.txt"), "w");
    fprintf(f, "# Upper QNs, lower QNs (SPFIT .lin order), ObsFreq(MHz) CalcFreq(MHz) CalcIntensity NQN\n");
    write_list_row(f, LEG_U0, LEG_L0, 3, 3000.05, 3000.1, 1.0e-4);
    fclose(f);
    char *previous = read_all(path_in(ro, "assignments.txt"));
    chmod(ro, 0500);
    AppState *t = new_state();
    snprintf(t->settings.data_dir, sizeof(t->settings.data_dir), "%s", ro);
    set_predictions(t, fx("cat3_303.cat"));
    assign_index(t, 1, 5979.98, 1.0);
    assign_index(t, 3, 6999.98, 1.0);
    t->error_message[0] = '\0';
    click_save_all(t);
    chmod(ro, 0700);
    CHECK(t->error_message[0] != '\0', "Save all in una cartella non scrivibile: atteso un errore in error_message, ottenuto nessun messaggio");
    char *now = read_all(path_in(ro, "assignments.txt"));
    CHECK(now && previous && strcmp(now, previous) == 0, "assignments.txt nella cartella non scrivibile: atteso intatto, ottenuto %s",
          now ? "riscritto" : "assente");
    free(previous); free(now);
    DONE();
}

/* B-23: every assignment, reassignment and deletion is on disk at once. */
static int test_autosave_on_assign_update_delete(void) {
    const double pk[3] = {3000.07, 3000.30, 5979.97};
    write_spectrum(work_path("peaks.txt"), pk, NULL, 3);
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    add_spectrum(s, work_path("peaks.txt"));

    CHECK_INT("righe selezionate", click_select(s, 3000.1), 1);
    right_drag(s, 2999.97, 3000.17);
    CHECK_INT("assignment dopo la prima assegnazione", s->n_assignments, 1);
    check_file_matches_list(s, "dopo l'assegnazione");

    CHECK_INT("righe selezionate", click_select(s, 3000.1), 1);
    right_drag(s, 3000.2, 3000.4);
    CHECK_INT("assignment dopo la riassegnazione", s->n_assignments, 1);
    if (s->n_assignments == 1) CHECK_DBL("ObsFreq riassegnata", s->assignments[0].exp_freq, 3000.30, 1e-3);
    check_file_matches_list(s, "dopo la riassegnazione");

    CHECK_INT("righe selezionate", click_select(s, 5980.0), 1);
    right_drag(s, 5979.87, 5980.07);
    CHECK_INT("assignment dopo la seconda assegnazione", s->n_assignments, 2);
    check_file_matches_list(s, "dopo la seconda assegnazione");

    click_delete_selected(s, 0);
    CHECK_INT("assignment dopo la cancellazione", s->n_assignments, 1);
    check_file_matches_list(s, "dopo la cancellazione");
    DONE();
}

/* ================================= #3 selection cleared on catalogue change */

/* T-10, R-10: a line selected on one catalogue must not assign a row of the
   catalogue loaded after it (drop of a .cat, or model.cat after Calculate/Fit). */
static int test_selection_cleared_on_catalog_change(void) {
    const double pk[1] = {5999.25};
    write_spectrum(work_path("peak.txt"), pk, NULL, 1);
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    add_spectrum(s, work_path("peak.txt"));
    CHECK_INT("righe selezionate in cat3_303", click_select(s, 5999.2867), 1);
    CHECK_INT("indice selezionato in cat3_303", s->selected_indices[0], 2);
    set_predictions(s, fx("cat4_1404.cat"));
    CHECK_INT("righe selezionate dopo il cambio di catalogo", s->n_selected, 0);
    right_drag(s, 5999.15, 5999.35);
    CHECK_INT("assignment creati dal picco dopo il cambio di catalogo", s->n_assignments, 0);
    if (s->n_assignments > 0)
        CHECK(0, "assegnata la transizione di cat4_1404 J %d <- %d a %.4f MHz", s->assignments[0].pred.Ju,
              s->assignments[0].pred.Jl, s->assignments[0].pred.freq_mhz);
    DONE();
}

/* No selected index outlives the catalogue it points into. */
static int test_selection_indices_in_bounds(void) {
    AppState *s = new_state();
    set_predictions(s, fixture("pred_reference.cat"));
    const int picked[3] = {5, 2000, 2311};                /* = a Cmd-click selection, controller.c:681-693 */
    for (int k = 0; k < 3; k++) s->selected_indices[k] = picked[k];
    s->n_selected = 3;
    const char *next[3] = {"cat3_303.cat", "cat4_1404.cat", "cat6_306.cat"};
    for (int c = 0; c < 3; c++) {
        set_predictions(s, fx(next[c]));
        for (int k = 0; k < s->n_selected; k++)
            CHECK(s->selected_indices[k] >= 0 && s->selected_indices[k] < s->n_pred,
                  "dopo %s: indice selezionato %d, atteso minore di n_pred = %d", next[c], s->selected_indices[k], s->n_pred);
    }
    DONE();
}

/* ========================================= #4 integrity of assignments.txt */

/* The header that commit 1f4df65 wrote on the first line of assignments.txt. */
static const char *ASG_HEADER_1F4DF65 =
    "# Upper QNs, lower QNs (SPFIT .lin order), ObsFreq(MHz) CalcFreq(MHz) CalcIntensity NQN\n";

/* T-06, T-07: R-02..R-07 for every QNFMT and for pred_reference.cat: after Save
   all and a restart the same transitions come back, none merged. */
static int test_roundtrip_every_qnfmt(void) {
    static const double f303[6] = {3000.1, 5980.0, 5999.2867, 7000.0, 8000.0, 9000.0};
    static const double f304[4] = {3100.0, 3100.3, 6100.0, 6100.3};
    static const double f305[3] = {3200.0, 3200.3, 6200.0};
    static const double f306[3] = {3300.0, 3300.3, 6300.0};
    static const double f1404[4] = {2511.3375, 2511.9, 6033.5894, 6034.0};
    static const double fpred[3] = {221.5761, 392.7959, 5999.2867};
    struct { const char *name; const char *path; const double *f; int n; } cases[6] = {
        {"cat3_303", NULL, f303, 6}, {"cat4_304", NULL, f304, 4}, {"cat5_305", NULL, f305, 3},
        {"cat6_306", NULL, f306, 3}, {"cat4_1404", NULL, f1404, 4}, {"pred_reference", NULL, fpred, 3},
    };
    char paths[6][900];
    for (int c = 0; c < 5; c++) snprintf(paths[c], sizeof(paths[c]), "%s/%s.cat", g_fx, cases[c].name);
    snprintf(paths[5], sizeof(paths[5]), "%s", fixture("pred_reference.cat"));
    int failed_before = g_failed;
    for (int c = 0; c < 6; c++) {
        unlink(work_path("assignments.txt"));
        unlink(work_path("assignments.txt.bak"));
        g_failed = 0;
        check_roundtrip(paths[c], cases[c].f, cases[c].n);
        if (g_failed) fprintf(g_tlog, "    (le righe sopra riguardano %s)\n", cases[c].name);
        failed_before |= g_failed;
    }
    g_failed = failed_before;
    DONE();
}

/* T-09: the blended pair at 392.7959 MHz of pred.cat, assigned with one click to
   one peak, comes back as two assignments. */
static int test_blend_pair_survives_reload(void) {
    const double pk[1] = {392.7659};
    write_spectrum(work_path("blend.txt"), pk, NULL, 1);
    AppState *s = new_state();
    set_predictions(s, fixture("pred_reference.cat"));
    add_spectrum(s, work_path("blend.txt"));
    CHECK_INT("righe selezionate dal clic a 392.7959 MHz", click_select(s, 392.7959), 2);
    right_drag(s, 392.5659, 392.9659);
    CHECK_INT("assignment dopo il picco", s->n_assignments, 2);
    if (s->n_assignments == 2) CHECK_DBL("stessa ObsFreq", s->assignments[1].exp_freq, s->assignments[0].exp_freq, 0.0);
    click_save_all(s);
    AppState *r = new_state();
    set_predictions(r, fixture("pred_reference.cat"));
    CHECK_INT("assignment dopo il riavvio", r->n_assignments, 2);
    for (int i = 0; i < s->n_assignments; i++) {
        int found = 0;
        for (int k = 0; k < r->n_assignments; k++) if (same_identity(&s->assignments[i].pred, &r->assignments[k].pred)) found = 1;
        CHECK(found, "dopo il riavvio: attesa la transizione %s", fmt_qn((int[12]){s->assignments[i].pred.Ju, s->assignments[i].pred.Kau,
              s->assignments[i].pred.Kcu, 0, 0, 0, s->assignments[i].pred.Jl, s->assignments[i].pred.Kal, s->assignments[i].pred.Kcl, 0, 0, 0}));
    }
    DONE();
}

/* B-05: an assignment without a valid NQN is not written with an invented one;
   the message says how many were left out. */
static int test_save_skips_invalid_nqn(void) {
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, 3000.08, 1.0);                     /* NQN 3, from the catalogue */
    Assignment legacy = s->assignments[0];                /* a row of a legacy file: NQN unknown */
    legacy.pred.Ju = 6; legacy.pred.Jl = 5; legacy.pred.n_qn = 0; legacy.exp_freq = 3100.0;
    s->assignments[s->n_assignments++] = legacy;
    s->error_message[0] = '\0';
    click_save_all(s);
    Assignment *disk = calloc(MAX_ASSIGNMENTS, sizeof(Assignment));
    int n = 0;
    load_existing_assignments(work_path("assignments.txt"), disk, &n);
    CHECK_INT("righe scritte in assignments.txt", n, 1);
    if (n >= 1) CHECK_INT("NQN della riga scritta", disk[0].pred.n_qn, 3);
    CHECK(strstr(s->error_message, "without 1 row") != NULL,
          "messaggio: atteso il conteggio 'without 1 row', ottenuto '%s'", s->error_message);
    free(disk);
    DONE();
}

/* B-06: two rows of the same transition are not merged in silence. */
static int test_reload_reports_collisions(void) {
    static const int u5[3] = {11, 0, 11}, l5[3] = {10, 1, 9};
    FILE *f = fopen(work_path("assignments.txt"), "w");
    fputs(ASG_HEADER_1F4DF65, f);
    write_list_row(f, LEG_U0, LEG_L0, 3, 3000.05, 3000.1, 1.0e-4);
    write_list_row(f, LEG_U0, LEG_L0, 3, 3000.08, 3000.1, 1.0e-4);   /* same transition, other peak */
    write_list_row(f, u5, l5, 3, 5999.27, 5999.2867, 1.364897e-4);
    fclose(f);
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    CHECK_INT("assignment", s->n_assignments, 2);
    CHECK(find_exp(s, 3000.08) != NULL, "attesa l'ultima occorrenza (3000.08 MHz) della transizione ripetuta");
    CHECK(strstr(s->error_message, "1 duplicate") != NULL,
          "messaggio: atteso il conteggio '1 duplicate', ottenuto '%s'", s->error_message);
    DONE();
}

/* B-07: the observed intensity is not in the file, so a restored row has
   exp_int = 0, never CalcIntensity. */
static int test_reload_exp_int_zero(void) {
    FILE *f = fopen(work_path("assignments.txt"), "w");
    fputs(ASG_HEADER_1F4DF65, f);
    write_list_row(f, LEG_U0, LEG_L0, 3, 3000.08, 3000.1, 1.234e-4);
    fclose(f);
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    CHECK_INT("assignment", s->n_assignments, 1);
    if (s->n_assignments == 1) {
        CHECK_DBL("exp_int", s->assignments[0].exp_int, 0.0, 0.0);
        CHECK(fabs(s->assignments[0].pred.linear_int - 1.234e-4) < 1e-9, "CalcIntensity: attesa 1.234e-04, ottenuta %.6e",
              s->assignments[0].pred.linear_int);
    }
    DONE();
}

/* T-20, B-08: a .lin (uncertainty and weight where CalcFreq and CalcIntensity
   should be, 9xxxx sentinels) is not read as a list of assignments. */
static int test_reader_rejects_lin_file(void) {
    copy_path(fixture("lin_with_nqn.txt"), work_path("assignments.txt"));
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    CHECK_INT("assignment letti da un .lin", s->n_assignments, 0);
    CHECK(strstr(s->error_message, "ignored") != NULL,
          "messaggio: atteso che le righe ignorate siano segnalate, ottenuto '%s'", s->error_message);
    DONE();
}

/* T-20: the layouts written before 1f4df65 (no header): 12 QN + ExpFreq ExpInt
   (14 fields) and PredFreq + 12 QN + ExpFreq ExpInt [NQN] (15/16 fields). */
static int test_reader_legacy_14_and_16(void) {
    static const struct { const char *name, *text; int n_qn; double pred_f, exp_f, exp_i; int qn[12]; } cases[4] = {
        {"legacy16_nqn3", "  2511.3375    4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   1.0000e-05 3\n",
         3, 2511.3375, 2511.3375, 1.0e-5, {4, 1, 4, 0, 0, 0, 3, 1, 3, 0, 0, 0}},
        {"legacy16_nqn6", "  3300.0000    2   1   1   3   4   5   1   1   0   2   3   4      3300.0100   2.0000e-05 6\n",
         6, 3300.0, 3300.01, 2.0e-5, {2, 1, 1, 3, 4, 5, 1, 1, 0, 2, 3, 4}},
        {"legacy14_ei1e-5", "  4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   1.0000e-05\n",
         0, 2511.3375, 2511.3375, 1.0e-5, {4, 1, 4, 0, 0, 0, 3, 1, 3, 0, 0, 0}},
        {"legacy14_ei5.2", "  4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   5.2000e+00\n",
         0, 2511.3375, 2511.3375, 5.2, {4, 1, 4, 0, 0, 0, 3, 1, 3, 0, 0, 0}},
    };
    for (int c = 0; c < 4; c++) {
        const char *path = work_path(cases[c].name);
        FILE *f = fopen(path, "w");
        fputs(cases[c].text, f);
        fclose(f);
        Assignment list[4];
        int n = 0;
        memset(list, 0, sizeof(list));
        load_existing_assignments(path, list, &n);
        char what[120];
        snprintf(what, sizeof(what), "%s: assignment letti", cases[c].name);
        CHECK_INT(what, n, 1);
        if (n != 1) continue;
        int q[12];
        qn_of(&list[0].pred, q);
        CHECK(memcmp(q, cases[c].qn, sizeof(q)) == 0, "%s: QN attesi %s, ottenuti %s", cases[c].name, fmt_qn(cases[c].qn), fmt_qn(q));
        snprintf(what, sizeof(what), "%s: n_qn", cases[c].name);
        CHECK_INT(what, list[0].pred.n_qn, cases[c].n_qn);
        snprintf(what, sizeof(what), "%s: frequenza calcolata", cases[c].name);
        CHECK_DBL(what, list[0].pred.freq_mhz, cases[c].pred_f, 1e-9);
        snprintf(what, sizeof(what), "%s: ObsFreq", cases[c].name);
        CHECK_DBL(what, list[0].exp_freq, cases[c].exp_f, 1e-9);
        CHECK(fabs(list[0].exp_int - cases[c].exp_i) <= 1e-9 * fabs(cases[c].exp_i), "%s: ExpInt atteso %.6e, ottenuto %.6e",
              cases[c].name, cases[c].exp_i, list[0].exp_int);
    }
    DONE();
}

/* =============================================== #5 NVIB is not rewritten */

/* The PAR option line typed in Advanced > Parameters (predfit.c:1625). */
static void type_option_line(PredFitState *p, const char *line) {
    p->advanced_edit_param = -4;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%s", line);
    advanced_commit_edit(p);
}

/* Third line of a .par/.var, or "" when there is none. */
static void option_line_of(const char *path, char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    for (int k = 0; k < 3 && fgets(line, sizeof(line), f); k++)
        if (k == 2) { line[strcspn(line, "\r\n")] = '\0'; snprintf(out, n, "%s", line); }
    fclose(f);
}

static void check_option_line_files(const char *dir, const char *want) {
    const char *files[2] = {"model.par", "model.var"};
    for (int k = 0; k < 2; k++) {
        char path[900], got[512];
        snprintf(path, sizeof(path), "%s/.fit/%s", dir, files[k]);
        option_line_of(path, got, sizeof(got));
        CHECK(strcmp(got, want) == 0, "%s, riga opzioni: attesa '%s', ottenuta '%s'", files[k], want, got);
    }
}

/* T-11, R-08: the NVIB the user typed stays, with 1 and with 3 species, through
   the commit, write_inputs, a session save and a session load. */
static int test_nvib_typed_value_kept(void) {
    static const struct { int species; const char *line; } cases[2] = {{1, "s 1 2 0"}, {3, "s 1 5 0"}};
    for (int c = 0; c < 2; c++) {
        char dir[700];
        snprintf(dir, sizeof(dir), "%s/case%d", g_work, c);
        mkdir(dir, 0700);
        AppState *s = new_state();
        snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", dir);
        PredFitState *p = &s->predfit;
        mono_model(p);
        while (p->n_species < cases[c].species) add_species(s);
        type_option_line(p, cases[c].line);
        CHECK(strcmp(p->hamiltonian_line, cases[c].line) == 0, "%d specie, digitato '%s': in memoria '%s'",
              cases[c].species, cases[c].line, p->hamiltonian_line);
        CHECK_INT("write_inputs", write_inputs(s, 0), 1);
        CHECK(strcmp(p->hamiltonian_line, cases[c].line) == 0, "dopo write_inputs: attesa '%s', ottenuta '%s'",
              cases[c].line, p->hamiltonian_line);
        check_option_line_files(dir, cases[c].line);
        predfit_save_session(s);
        AppState *r = new_state();
        snprintf(r->settings.data_dir, sizeof(r->settings.data_dir), "%s", dir);
        predfit_load_session(r);
        CHECK(strcmp(r->predfit.hamiltonian_line, cases[c].line) == 0, "dopo il caricamento della sessione: attesa '%s', ottenuta '%s'",
              cases[c].line, r->predfit.hamiltonian_line);
    }
    /* adding a species does not touch the line either */
    AppState *a = new_state();
    mono_model(&a->predfit);
    type_option_line(&a->predfit, "s 1 4 0");
    add_species(a);
    CHECK(strcmp(a->predfit.hamiltonian_line, "s 1 4 0") == 0, "dopo + species: attesa 's 1 4 0', ottenuta '%s'",
          a->predfit.hamiltonian_line);
    DONE();
}

/* D1 answer: NVIB smaller than the states of the included species rejects
   Calculate and Fit, says the minimum and writes no Pickett file. */
static int test_nvib_too_small_rejected(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    add_species(s);
    add_species(s);                                       /* states 0, 1, 2 */
    type_option_line(p, "s 1 1 0");
    CHECK(strcmp(p->hamiltonian_line, "s 1 1 0") == 0, "riga opzioni: attesa 's 1 1 0', ottenuta '%s'", p->hamiltonian_line);
    CHECK_INT("Calculate", predfit_calculate(s), 0);
    CHECK(strstr(p->status, "at least 3") != NULL, "stato dopo Calculate: atteso il minimo 'at least 3', ottenuto '%s'", p->status);
    const char *files[4] = {"model.var", "model.par", "model.int", "model.lin"};
    for (int k = 0; k < 4; k++)
        CHECK(access(path_in(work_path(".fit"), files[k]), F_OK) != 0, "%s scritto nonostante il rifiuto", files[k]);
    set_predictions(s, fx("cat4_1404.cat"));
    assign_index(s, 0, 2511.34, 1.0);
    p->status[0] = '\0';
    CHECK_INT("Fit", predfit_fit(s), 0);
    CHECK(strstr(p->status, "at least 3") != NULL, "stato dopo Fit: atteso il minimo 'at least 3', ottenuto '%s'", p->status);
    CHECK_INT("storia di Undo dopo il Fit rifiutato", p->history_count, 0);
    for (int k = 0; k < 4; k++)
        CHECK(access(path_in(work_path(".fit"), files[k]), F_OK) != 0, "%s scritto dal Fit rifiutato", files[k]);
    DONE();
}

/* CHR, SPIND, KNMIN and the comma form stay exactly as typed. */
static int test_option_line_other_tokens_kept(void) {
    static const char *lines[3] = {"s 1 3", "s,1,5,0", "a 2 4 0 1 0"};
    for (int c = 0; c < 3; c++) {
        char dir[700];
        snprintf(dir, sizeof(dir), "%s/case%d", g_work, c);
        mkdir(dir, 0700);
        AppState *s = new_state();
        snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", dir);
        PredFitState *p = &s->predfit;
        mono_model(p);
        add_species(s);
        add_species(s);
        type_option_line(p, lines[c]);
        CHECK(strcmp(p->hamiltonian_line, lines[c]) == 0, "digitato '%s': in memoria '%s'", lines[c], p->hamiltonian_line);
        CHECK_INT("write_inputs", write_inputs(s, 0), 1);
        check_option_line_files(dir, lines[c]);
    }
    DONE();
}

static int test_param_id_zero_or_duplicate_rejected(void) {
    AppState *s = new_state();
    mono_model(&s->predfit);
    add_parameter(&s->predfit);
    CHECK_INT("ID zero rifiutato prima dei file", write_inputs(s, 0), 0);
    CHECK(strstr(s->predfit.status, "positive ID") != NULL, "stato ID zero: '%s'", s->predfit.status);
    s->predfit.advanced_edit_param = s->predfit.n_param - 1;
    s->predfit.advanced_edit_col = 0;
    snprintf(s->predfit.advanced_edit_buf, sizeof(s->predfit.advanced_edit_buf), "10000");
    advanced_commit_edit(&s->predfit);
    CHECK_INT("ID duplicato non inserito", s->predfit.param[s->predfit.n_param - 1].id, 0);
    CHECK(strstr(s->predfit.status, "unique") != NULL, "stato ID duplicato: '%s'", s->predfit.status);
    DONE();
}

static int test_calculate_requires_abc(void) {
    AppState *s = new_state();
    mono_model(&s->predfit);
    delete_parameter(&s->predfit, 0);                    /* A / 10000 */
    CHECK_INT("Calculate bloccato senza A", write_inputs(s, 0), 0);
    CHECK(strstr(s->predfit.status, "missing A") != NULL, "stato senza A: '%s'", s->predfit.status);
    DONE();
}

/* T-29, R-25: Restore defaults is a visual/defaults reset, never a request
   to forget the local executables and project directory. */
static int test_restore_defaults_keeps_paths(void) {
    AppState *s = new_state();
    snprintf(s->settings.spcat_path, sizeof(s->settings.spcat_path), "/tools/SPCAT");
    snprintf(s->settings.spfit_path, sizeof(s->settings.spfit_path), "/tools/SPFIT");
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", work_path("saved data"));
    settings_restore_defaults(s);
    CHECK(strcmp(s->settings.spcat_path, "/tools/SPCAT") == 0, "SPCAT cancellato dal reset");
    CHECK(strcmp(s->settings.spfit_path, "/tools/SPFIT") == 0, "SPFIT cancellato dal reset");
    CHECK(strstr(s->settings.data_dir, "saved data") != NULL, "cartella dati cancellata dal reset");
    DONE();
}

/* T-37, R-33: the one uncertainty written on every .lin observation belongs
   to the fit session, not to a transient application default. */
static int test_line_error_persists(void) {
    AppState *s = new_state();
    s->predfit.line_error_mhz = 0.00375;
    predfit_save_session(s);
    AppState *restored = new_state();
    predfit_load_session(restored);
    CHECK_DBL("incertezza .lin ripristinata", restored->predfit.line_error_mhz, 0.00375, 1e-12);
    DONE();
}

static double var_parameter_error(const char *path, int id) {
    FILE *fp = fopen(path, "r");
    char line[512];
    double error = -1.0;
    while (fp && fgets(line, sizeof(line), fp)) {
        int got_id = 0; double value = 0.0, got_error = 0.0;
        if (sscanf(line, "%d %lf %lf", &got_id, &value, &got_error) == 3 && got_id == id) {
            error = got_error;
            break;
        }
    }
    if (fp) fclose(fp);
    return error;
}

/* T-38, R-34: Calculate reuses a fitted model.var when the model values have
   not changed, so the SPFIT estimated uncertainty is not silently replaced. */
static int test_calculate_after_fit_keeps_fitted_var(void) {
    AppState *s = new_state();
    mono_model(&s->predfit);
    CHECK_INT("prepara modello", write_inputs(s, 0), 1);
    char var_path[700];
    snprintf(var_path, sizeof(var_path), "%s/.fit/model.var", g_work);
    FILE *fp = fopen(var_path, "w");
    CHECK(fp != NULL, "impossibile preparare model.var da SPFIT");
    if (!fp) DONE();
    PredFitState *p = &s->predfit;
    fprintf(fp, "SpectraVisual Pred&Fit quick model\n%4d%5d%5d%5d %15.4E %15.4E %15.4E %.10f\n%s\n",
            p->n_param, 0, 0, 0, 0.0, 1e6, 1.0, 1.0, p->hamiltonian_line);
    for (int i = 0; i < p->n_param; i++)
        fprintf(fp, "%12d % .15E % .8E /%s/\n", p->param[i].id, p->param[i].value,
                0.12345 + i, p->param[i].label);
    fclose(fp);
    CHECK_INT("Calculate riscrive gli altri input", write_inputs(s, 0), 1);
    for (int i = 0; i < p->n_param; i++) {
        char what[64]; snprintf(what, sizeof(what), "errore SPFIT parametro %d", i + 1);
        CHECK_DBL(what, var_parameter_error(var_path, p->param[i].id), 0.12345 + i, 1e-7);
    }
    DONE();
}

/* M-06: both export callers derive their destination from data_dir. */
static int test_exports_go_to_data_dir(void) {
    AppState *s = new_state();
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", work_path("exports"));
    char ifit[700], screenshot[700];
    settings_data_file(s, "intensity_fit.ifit", ifit, sizeof(ifit));
    settings_data_file(s, "spectravisual_export.bmp", screenshot, sizeof(screenshot));
    CHECK(strstr(ifit, "/exports/intensity_fit.ifit") != NULL, "export IFIT fuori data_dir: %s", ifit);
    CHECK(strstr(screenshot, "/exports/spectravisual_export.bmp") != NULL,
          "screenshot fuori data_dir: %s", screenshot);
    DONE();
}

static void queue_secondary_text(Uint32 window_id, const char *text) {
    SDL_Event e; memset(&e, 0, sizeof(e));
    e.type = SDL_TEXTINPUT; e.text.windowID = window_id;
    snprintf(e.text.text, sizeof(e.text.text), "%s", text);
    SDL_PushEvent(&e);
}

static void queue_secondary_key(Uint32 window_id, SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event e; memset(&e, 0, sizeof(e));
    e.type = SDL_KEYDOWN; e.key.windowID = window_id; e.key.keysym.sym = key; e.key.keysym.mod = mod;
    SDL_PushEvent(&e);
}

static int test_text_from_secondary_window_ignored(void) {
    CHECK_INT("SDL eventi", SDL_Init(SDL_INIT_EVENTS), 0);
    AppState *s = new_state(); Layout L = {0}; int running = 1;
    s->predfit.advanced_window_id = 9001;
    s->input_state = INPUT_OFFSET;
    snprintf(s->text_input_buf, sizeof(s->text_input_buf), "12");
    queue_secondary_text(9001, "9");
    handle_app_events(s, &L, &running);
    CHECK(strcmp(s->text_input_buf, "12") == 0, "testo secondario inoltrato: '%s'", s->text_input_buf);
    CHECK_INT("campo principale ancora attivo", s->input_state, INPUT_OFFSET);
    DONE();
}

static int test_keys_from_secondary_window_ignored(void) {
    CHECK_INT("SDL eventi", SDL_Init(SDL_INIT_EVENTS), 0);
    AppState *s = new_state(); Layout L = {0}; int running = 1;
    s->settings.window_id = 9002; s->data_loaded = 1;
    queue_secondary_key(9002, SDLK_x, KMOD_NONE);
    handle_app_events(s, &L, &running);
    CHECK_INT("X secondaria non esporta", s->export_requested, 0);
    DONE();
}

static int test_cmd_modified_keys_not_plain_actions(void) {
    CHECK_INT("SDL eventi", SDL_Init(SDL_INIT_EVENTS), 0);
    AppState *s = new_state(); Layout L = {0}; int running = 1;
    s->settings.window_id = 9003; s->data_loaded = 1;
    queue_secondary_key(9003, SDLK_x, KMOD_CTRL);
    handle_app_events(s, &L, &running);
    CHECK_INT("Cmd/Ctrl+X secondaria non esporta", s->export_requested, 0);
    DONE();
}

/* ========================================================= #6 Fit and NQN */

/* T-16, R-11: the current model catalogue is the authority for the SPFIT
   record shape.  A three-state model yields QNFMT 1404, while these external
   assignments have three QN per state; reject before opening model.lin. */
static int test_fit_rejects_nqn_mismatch(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    add_species(s); add_species(s);
    type_option_line(p, "s 1 3 0");
    CHECK_INT("write model inputs", write_inputs(s, 0), 1);
    copy_path(fx("cat4_1404.cat"), work_path(".fit/model.cat"));
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, 3000.01, 1.0);
    assign_index(s, 1, 3000.11, 1.0);
    CHECK_INT("NQN assignment 1", s->assignments[0].pred.n_qn, 3);
    CHECK_INT("NQN assignment 2", s->assignments[1].pred.n_qn, 3);
    unlink(work_path(".fit/model.lin"));
    CHECK_INT("Fit", predfit_fit(s), 0);
    CHECK(strstr(p->status, "NQN 4") != NULL, "stato: atteso NQN del modello, ottenuto '%s'", p->status);
    CHECK(strstr(p->status, "1") != NULL && strstr(p->status, "2") != NULL,
          "stato: attese le righe 1 e 2, ottenuto '%s'", p->status);
    CHECK(access(work_path(".fit/model.lin"), F_OK) != 0, "model.lin scritto nonostante il mismatch");
    CHECK_INT("storia di Undo dopo il Fit rifiutato", p->history_count, 0);
    DONE();
}

/* B-22: all four diagnostics that make a numerical result unsafe remain
   visible in the status, instead of being hidden by the final RMS line. */
static int test_fit_status_counts_spfit_diagnostics(void) {
    AppState *s = new_state();
    char fitdir[800], path[900];
    snprintf(fitdir, sizeof(fitdir), "%s/.fit", g_work);
    mkdir(fitdir, 0700);
    snprintf(s->predfit.work_dir, sizeof(s->predfit.work_dir), "%s", fitdir);
    snprintf(path, sizeof(path), "%s/model.fit", fitdir);
    copy_path(fixture("spfit_diagnostics.fit"), path);
    fit_summary(&s->predfit, s->predfit.status, sizeof(s->predfit.status));
    CHECK(strstr(s->predfit.status, "2 bad lines") != NULL, "stato: '%s'", s->predfit.status);
    CHECK(strstr(s->predfit.status, "3 rejected") != NULL, "stato: '%s'", s->predfit.status);
    CHECK(strstr(s->predfit.status, "1 not used") != NULL, "stato: '%s'", s->predfit.status);
    CHECK(strstr(s->predfit.status, "1 diverging") != NULL, "stato: '%s'", s->predfit.status);
    DONE();
}

/* U-08: Fitting distinguishes an excluded assignment, a SPFIT rejection and
   a line actually used.  The helper is the same one consumed by the renderer. */
static int test_fitting_tab_row_states(void) {
    AppState *s = new_state();
    report_invalidate();
    g_report.loaded = 1;
    s->n_assignments = 3;
    s->assignments[0].fit_enabled = 0;
    s->assignments[1].fit_enabled = 1;
    s->assignments[2].fit_enabled = 1;
    s->assignments[2].exp_freq = 3000.0;
    g_report.bad_line[1] = 1;
    g_report.obs[2] = (FitObservation){1, 1, 3000.0, 3000.0, 0.0, 0.01};
    CHECK_INT("riga esclusa", fitting_row_state(&s->assignments[0], 0), FIT_ROW_EXCLUDED);
    CHECK_INT("riga rifiutata", fitting_row_state(&s->assignments[1], 1), FIT_ROW_REJECTED);
    CHECK_INT("riga usata", fitting_row_state(&s->assignments[2], 2), FIT_ROW_USED);
    DONE();
}

/* =================================================== #7 Startup and model */

static PickettParameter *parameter_by_id(PredFitState *p, int id) {
    for (int i = 0; i < p->n_param; i++) if (p->param[i].id == id) return &p->param[i];
    return NULL;
}

/* T-32, R-28: an explicit external CAT must not make a persisted model fall
   back to defaults.  The next Calculate writes the restored values. */
static int test_launch_with_cat_keeps_model(void) {
    AppState *saved = new_state();
    mono_model(&saved->predfit);
    PickettParameter *dj = &saved->predfit.param[saved->predfit.n_param++];
    *dj = (PickettParameter){200, -7.0e-6, 0.0, "DJ"};
    predfit_save_session(saved);

    AppState *launch = new_state();
    predfit_load_session(launch);                       /* then main opens pred.cat */
    set_predictions(launch, fx("cat3_303.cat"));
    CHECK_DBL("A ripristinata", launch->predfit.a, 1151.360417, 1e-9);
    CHECK_DBL("B ripristinata", launch->predfit.b, 316.1511127, 1e-9);
    CHECK_DBL("C ripristinata", launch->predfit.c, 313.1742368, 1e-9);
    dj = parameter_by_id(&launch->predfit, 200);
    CHECK(dj != NULL, "DJ assente dopo avvio con .cat");
    if (dj) {
        CHECK_DBL("DJ ripristinato", dj->value, -7.0e-6, 1e-14);
        CHECK_DBL("incertezza DJ ripristinata", dj->error, 0.0, 1e-14);
    }
    CHECK_INT("write_inputs dopo .cat", write_inputs(launch, 0), 1);
    CHECK_DBL("A non sovrascritta da Calculate", launch->predfit.a, 1151.360417, 1e-9);
    dj = parameter_by_id(&launch->predfit, 200);
    if (dj) CHECK_DBL("DJ non sovrascritto da Calculate", dj->value, -7.0e-6, 1e-14);
    DONE();
}

/* A modern param record is self-contained: loading it must add a term that
   the initial quick model does not already carry. */
static int test_session_load_adds_missing_param_rows(void) {
    char fitdir[800];
    snprintf(fitdir, sizeof(fitdir), "%s/.fit", g_work);
    mkdir(fitdir, 0700);
    FILE *fp = fopen(work_path(".fit/spectravisual.state"), "w");
    CHECK(fp != NULL, "impossibile creare sessione di prova");
    if (!fp) DONE();
    fputs("# SpectraVisual session v3\nparam 910001 42.25 0.125\n", fp);
    fclose(fp);

    AppState *s = new_state();
    predfit_load_session(s);
    PickettParameter *x = parameter_by_id(&s->predfit, 910001);
    CHECK(x != NULL, "parametro assente non aggiunto dalla sessione");
    if (x) {
        CHECK_DBL("valore parametro aggiunto", x->value, 42.25, 1e-12);
        CHECK_DBL("errore parametro aggiunto", x->error, 0.125, 1e-12);
    }
    DONE();
}

/* A passive start/exit leaves a v3 session byte-for-byte unchanged.  This is
   the same guard main() uses at shutdown; a real edit sets session_dirty. */
static int test_startup_does_not_rewrite_session(void) {
    AppState *saved = new_state();
    mono_model(&saved->predfit);
    predfit_save_session(saved);
    char *before = read_all(work_path(".fit/spectravisual.state"));
    CHECK(before != NULL, "sessione iniziale assente");
    if (!before) DONE();

    AppState *start = new_state();
    predfit_load_session(start);
    CHECK_INT("sessione passiva non sporca", start->predfit.session_dirty, 0);
    if (start->predfit.session_dirty) predfit_save_session(start);
    char *after = read_all(work_path(".fit/spectravisual.state"));
    CHECK(after != NULL && strcmp(before, after) == 0,
          "sessione riscritta durante avvio/uscita passivi");
    free(before); free(after);
    DONE();
}

/* T-28, R-25: after Settings choose a data directory, the cached work path
   must agree immediately, before Calculate or Restore happen. */
static int test_workdir_after_settings(void) {
    AppState *s = new_state();
    predfit_refresh_work_dir(s);
    char want[800];
    snprintf(want, sizeof(want), "%s/.fit", g_work);
    CHECK(strcmp(s->predfit.work_dir, want) == 0,
          "work_dir: atteso '%s', ottenuto '%s'", want, s->predfit.work_dir);
    DONE();
}

/* ================================================= #8 Fit exclusions */

static Assignment *assignment_for(AppState *s, const PredLine *pred) {
    for (int i = 0; i < s->n_assignments; i++)
        if (same_identity(&s->assignments[i].pred, pred)) return &s->assignments[i];
    return NULL;
}

/* Prepare a real 3-QN model catalogue and two live assignments without
   requiring local SPFIT/SPCAT binaries. */
static void prepare_exclusion_case(AppState *s) {
    mono_model(&s->predfit);
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, s->pred_lines[0].freq_mhz - 0.02, 1.0);
    assign_index(s, 1, s->pred_lines[1].freq_mhz - 0.02, 1.0);
    write_inputs(s, 0);
    copy_path(fx("cat3_303.cat"), work_path(".fit/model.cat"));
}

static int lin_nline(const char *path) {
    FILE *fp = fopen(path, "r");
    char title[256], header[256];
    int n_param = 0, nline = -1;
    if (fp && fgets(title, sizeof(title), fp) && fgets(header, sizeof(header), fp))
        sscanf(header, "%d%d", &n_param, &nline);
    if (fp) fclose(fp);
    return nline;
}

/* T-15, R-15: the exclusion is not a numeric sentinel.  It is absent from
   the actual SPFIT input for both small and very large line uncertainties. */
static int test_excluded_rows_absent_from_lin(void) {
    AppState *s = new_state();
    prepare_exclusion_case(s);
    CHECK_INT("due assignment", s->n_assignments, 2);
    s->assignments[0].fit_enabled = 0;
    CHECK_INT("salva esclusioni", predfit_save_exclusions(s), 1);
    const double errors[] = {0.001, 5.0};
    for (int i = 0; i < 2; i++) {
        s->predfit.line_error_mhz = errors[i];
        CHECK_INT("write_inputs Fit", write_inputs(s, 1), 1);
        CHECK_INT("NLINE solo incluse", lin_nline(work_path(".fit/model.par")), 1);
        CHECK_INT("righe .lin", read_lin_rows(&s->predfit), 1);
        if (g_lin_rows[0].freq > 0.0)
            CHECK_DBL("frequenza inclusa", g_lin_rows[0].freq, s->assignments[1].exp_freq, 1e-6);
        FILE *fp = fopen(work_path(".fit/model.lin"), "r");
        char line[256] = "";
        if (fp) { fgets(line, sizeof(line), fp); fclose(fp); }
        CHECK(strstr(line, "90000") == NULL, "sentinella rimasta nel .lin: '%s'", line);
    }
    DONE();
}

/* T-12, T-14: direct CAT load and Pred&Fit restore apply exactly the same
   identity-keyed exclusion, regardless of assignment-list position. */
static int test_exclusions_persist_by_identity(void) {
    AppState *s = new_state();
    prepare_exclusion_case(s);
    PredLine excluded = s->assignments[1].pred;
    s->assignments[1].fit_enabled = 0;
    CHECK_INT("salva esclusioni", predfit_save_exclusions(s), 1);
    CHECK_INT("write Fit inputs", write_inputs(s, 1), 1);

    AppState *with_cat = new_state();
    set_predictions(with_cat, fx("cat3_303.cat"));
    Assignment *a = assignment_for(with_cat, &excluded);
    CHECK(a != NULL, "avvio con CAT: transizione esclusa assente");
    if (a) CHECK_INT("avvio con CAT: esclusa", a->fit_enabled, 0);

    AppState *without_cat = new_state();
    CHECK_INT("restore senza CAT", predfit_restore_latest(without_cat), 1);
    a = assignment_for(without_cat, &excluded);
    CHECK(a != NULL, "avvio senza CAT: transizione esclusa assente");
    if (a) CHECK_INT("avvio senza CAT: esclusa", a->fit_enabled, 0);
    DONE();
}

/* T-12, R-14: Undo restores only the Fit state.  After deletion, it neither
   re-inserts the deleted assignment nor moves the exclusion to index zero. */
static int test_undo_exclusions_by_identity(void) {
    AppState *s = new_state();
    prepare_exclusion_case(s);
    PredLine deleted = s->assignments[0].pred;
    PredLine excluded = s->assignments[1].pred;
    s->assignments[1].fit_enabled = 0;
    CHECK_INT("salva esclusioni prima dello snapshot", predfit_save_exclusions(s), 1);
    CHECK_INT("snapshot Fit", push_fit_snapshot(s), 1);
    delete_assignment(s, 0);
    CHECK_INT("lista dopo delete", s->n_assignments, 1);
    s->assignments[0].fit_enabled = 1; /* prove that Undo reapplies its key */
    restore_fit_snapshot(s, &s->predfit.history[0]);
    CHECK(assignment_for(s, &deleted) == NULL, "Undo ha ricreato un assignment cancellato");
    Assignment *a = assignment_for(s, &excluded);
    CHECK(a != NULL, "transizione esclusa assente dopo Undo");
    if (a) CHECK_INT("esclusione resta sulla transizione", a->fit_enabled, 0);
    DONE();
}

/* T-40, R-36: moving an already assigned transition to another experimental
   peak refreshes the values but retains the user's Fit choice. */
static int test_reassign_keeps_exclusion(void) {
    AppState *s = new_state();
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, 3000.0, 1.0);
    s->assignments[0].fit_enabled = 0;
    PredLine line = s->assignments[0].pred;
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 3000.25, 2.0);
    CHECK_INT("una sola transizione", s->n_assignments, 1);
    CHECK_INT("esclusione conservata", s->assignments[0].fit_enabled, 0);
    CHECK_DBL("nuova frequenza osservata", s->assignments[0].exp_freq, 3000.25, 1e-9);
    DONE();
}

/* The sidecar is optional input, never an authority on the assignment list:
   a hand-edited/wrong file may lose its own records, but cannot lose data. */
static int test_malformed_exclusions_are_safe(void) {
    AppState *s = new_state();
    prepare_exclusion_case(s);
    FILE *fp = fopen(work_path(".fit/exclusions.txt"), "w");
    if (fp) {
        fputs("# SpectraVisual fit exclusions, format 1\n", fp);
        fputs("not a transition\n", fp);
        fputs("9 1 2 3\n", fp);
        fclose(fp);
    }
    AppState *r = new_state();
    set_predictions(r, fx("cat3_303.cat"));
    CHECK_INT("assignment conservati", r->n_assignments, 2);
    for (int i = 0; i < r->n_assignments; i++)
        CHECK_INT("file malformato non esclude", r->assignments[i].fit_enabled, 1);
    DONE();
}

/* ================================================================ runner */
typedef struct { const char *name; int (*fn)(void); } Test;

static const Test TESTS[] = {
    {"test_baseline_cat1404_nqn4",             test_baseline_cat1404_nqn4},
    {"test_baseline_roundtrip_qnfmt1404",      test_baseline_roundtrip_qnfmt1404},
    {"test_baseline_reassign_updates_obsfreq", test_baseline_reassign_updates_obsfreq},
    {"test_remove_spectrum_keeps_active",     test_remove_spectrum_keeps_active},
    {"test_drop_many_files_loads_all",        test_drop_many_files_loads_all},
    {"test_calculate_and_drop_same_frame",    test_calculate_and_drop_same_frame},
    {"test_baseline_right_drag_ascending",     test_baseline_right_drag_ascending},
    {"test_descending_spectrum_same_results",  test_descending_spectrum_same_results},
    {"test_nonmonotonic_spectrum_rejected",    test_nonmonotonic_spectrum_rejected},
    {"test_baseline_calculate_single_species", test_baseline_calculate_single_species},
    {"test_cat_nqn_from_qnfmt_3qn",            test_cat_nqn_from_qnfmt_3qn},
    {"test_cat_nqn_4_5_6",                     test_cat_nqn_4_5_6},
    {"test_cat_letter_and_negative_qn",        test_cat_letter_and_negative_qn},
    {"test_cat_trailing_spaces_irrelevant",    test_cat_trailing_spaces_irrelevant},
    {"test_cat_fixed_width_numbers",           test_cat_fixed_width_numbers},
    {"test_cat_invalid_nqn_reported",          test_cat_invalid_nqn_reported},
    {"test_restore_reads_data_dir_list",       test_restore_reads_data_dir_list},
    {"test_restore_keeps_short_lin_rows",      test_restore_keeps_short_lin_rows},
    {"test_restore_int_keeps_species_and_auto_fields", test_restore_int_keeps_species_and_auto_fields},
    {"test_save_all_after_restore_no_loss",    test_save_all_after_restore_no_loss},
    {"test_save_all_reports_write_error",      test_save_all_reports_write_error},
    {"test_autosave_on_assign_update_delete",  test_autosave_on_assign_update_delete},
    {"test_selection_cleared_on_catalog_change", test_selection_cleared_on_catalog_change},
    {"test_selection_indices_in_bounds",       test_selection_indices_in_bounds},
    {"test_roundtrip_every_qnfmt",             test_roundtrip_every_qnfmt},
    {"test_blend_pair_survives_reload",        test_blend_pair_survives_reload},
    {"test_save_skips_invalid_nqn",            test_save_skips_invalid_nqn},
    {"test_reload_reports_collisions",         test_reload_reports_collisions},
    {"test_reload_exp_int_zero",               test_reload_exp_int_zero},
    {"test_reader_rejects_lin_file",           test_reader_rejects_lin_file},
    {"test_reader_legacy_14_and_16",           test_reader_legacy_14_and_16},
    {"test_nvib_typed_value_kept",             test_nvib_typed_value_kept},
    {"test_nvib_too_small_rejected",           test_nvib_too_small_rejected},
    {"test_option_line_other_tokens_kept",     test_option_line_other_tokens_kept},
    {"test_param_id_zero_or_duplicate_rejected", test_param_id_zero_or_duplicate_rejected},
    {"test_calculate_requires_abc",            test_calculate_requires_abc},
    {"test_restore_defaults_keeps_paths",      test_restore_defaults_keeps_paths},
    {"test_line_error_persists",               test_line_error_persists},
    {"test_calculate_after_fit_keeps_fitted_var", test_calculate_after_fit_keeps_fitted_var},
    {"test_exports_go_to_data_dir",            test_exports_go_to_data_dir},
    {"test_text_from_secondary_window_ignored", test_text_from_secondary_window_ignored},
    {"test_keys_from_secondary_window_ignored", test_keys_from_secondary_window_ignored},
    {"test_cmd_modified_keys_not_plain_actions", test_cmd_modified_keys_not_plain_actions},
    {"test_fit_rejects_nqn_mismatch",          test_fit_rejects_nqn_mismatch},
    {"test_fit_status_counts_spfit_diagnostics", test_fit_status_counts_spfit_diagnostics},
    {"test_fitting_tab_row_states",            test_fitting_tab_row_states},
    {"test_launch_with_cat_keeps_model",       test_launch_with_cat_keeps_model},
    {"test_session_load_adds_missing_param_rows", test_session_load_adds_missing_param_rows},
    {"test_startup_does_not_rewrite_session",   test_startup_does_not_rewrite_session},
    {"test_workdir_after_settings",            test_workdir_after_settings},
    {"test_excluded_rows_absent_from_lin",     test_excluded_rows_absent_from_lin},
    {"test_exclusions_persist_by_identity",    test_exclusions_persist_by_identity},
    {"test_undo_exclusions_by_identity",       test_undo_exclusions_by_identity},
    {"test_reassign_keeps_exclusion",          test_reassign_keeps_exclusion},
    {"test_malformed_exclusions_are_safe",     test_malformed_exclusions_are_safe},
};
#define N_TESTS ((int)(sizeof(TESTS) / sizeof(TESTS[0])))

static const char *tmp_root(void) {
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
}

static void remove_tree(const char *dir) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
}

static int run_one(const Test *t, int verbose) {
    char dir[700];
    snprintf(dir, sizeof(dir), "%s/sv-test.XXXXXX", tmp_root());
    if (!mkdtemp(dir)) { printf("FAIL %s\n    mkdtemp: %s\n", t->name, strerror(errno)); return T_FAIL; }
    char report[800];
    snprintf(report, sizeof(report), "%s/.report", dir);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        snprintf(g_work, sizeof(g_work), "%s", dir);
        g_tlog = fopen(report, "w");
        if (!g_tlog || chdir(g_work) != 0) _exit(T_FAIL);
        if (!verbose && !freopen("/dev/null", "w", stdout)) _exit(T_FAIL);
        alarm(600);
        g_failed = 0;
        int r = t->fn();
        fflush(stdout);
        fclose(g_tlog);
        _exit(r);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int r = (WIFEXITED(st) && WEXITSTATUS(st) <= T_SKIP) ? WEXITSTATUS(st) : T_FAIL;
    printf("%s %s\n", r == T_PASS ? "PASS" : r == T_SKIP ? "SKIP" : "FAIL", t->name);
    FILE *rep = fopen(report, "r");
    if (rep) {
        char line[1024];
        while (fgets(line, sizeof(line), rep)) fputs(line, stdout);
        fclose(rep);
    }
    if (WIFSIGNALED(st)) printf("    terminato dal segnale %d\n", WTERMSIG(st));
    if (r == T_FAIL) printf("    cartella del test conservata: %s\n", dir);
    else             remove_tree(dir);
    return r;
}

int main(int argc, char **argv) {
    g_argv0 = argv[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    int verbose = 0, first = 1;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) { verbose = 1; first = 2; }
    for (int a = first; a < argc; a++) {
        int known = 0;
        for (int i = 0; i < N_TESTS; i++) if (strcmp(argv[a], TESTS[i].name) == 0) known = 1;
        if (!known) { fprintf(stderr, "test sconosciuto: %s\n", argv[a]); return 2; }
    }

    snprintf(g_fx, sizeof(g_fx), "%s/sv-fixtures.XXXXXX", tmp_root());
    if (!mkdtemp(g_fx)) { perror("mkdtemp"); return 2; }
    char cmd[2000];
    snprintf(cmd, sizeof(cmd), "python3 '%s/gen_fixtures.py' '%s' '%s' > /dev/null",
             SV_TESTS_DIR, g_fx, fixture("pred_reference.cat"));
    if (system(cmd) != 0) { fprintf(stderr, "gen_fixtures.py failed\n"); return 2; }

    int pass = 0, fail = 0, skip = 0;
    for (int i = 0; i < N_TESTS; i++) {
        int selected = (first >= argc);
        for (int a = first; a < argc; a++) if (strcmp(argv[a], TESTS[i].name) == 0) selected = 1;
        if (!selected) continue;
        int r = run_one(&TESTS[i], verbose);
        if (r == T_PASS) pass++; else if (r == T_SKIP) skip++; else fail++;
    }
    remove_tree(g_fx);
    printf("\n%d PASS, %d FAIL, %d SKIP\n", pass, fail, skip);
    return fail ? 1 : 0;
}
