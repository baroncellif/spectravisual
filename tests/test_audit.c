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

static void fill_peakfinder_trace(Point *pts, int n, double baseline) {
    for (int i = 0; i < n; i++) {
        double x = i * 0.01;
        double z = (x - 1.0) / 0.035;
        pts[i] = (Point){x, baseline + exp(-0.5 * z * z)};
    }
}

/* T-23, R-20: an arbitrary vertical baseline does not change the detected
   transition, because thresholding uses residual noise rather than raw RMS. */
static int test_peakfinder_baseline_invariant(void) {
    Point low[201], high[201]; Peak a[MAX_PEAKS], b[MAX_PEAKS];
    int na = 0, nb = 0;
    fill_peakfinder_trace(low, 201, 1.0);
    fill_peakfinder_trace(high, 201, 10.0);
    run_peak_finder(low, 201, 0.0, 2.0, 3, 25, 4.0, a, &na);
    run_peak_finder(high, 201, 0.0, 2.0, 3, 25, 4.0, b, &nb);
    CHECK_INT("picchi con baseline 1", na, 1);
    CHECK_INT("picchi con baseline 10", nb, na);
    if (na && nb) CHECK_DBL("frequenza invariata", b[0].x, a[0].x, 1e-6);
    DONE();
}

/* The noise span is a real input to both the estimator and the committed UI
   value: it is clamped to the points actually visible, never ignored. */
static int test_peakfinder_noise_window_used(void) {
    Point pts[41]; Peak out[MAX_PEAKS]; int n = 0;
    fill_peakfinder_trace(pts, 41, 3.0);
    run_peak_finder(pts, 41, 0.0, 0.4, 2, 3, 3.0, out, &n);
    CHECK(n >= 0, "il finder con finestra rumore corta non termina");
    AppState *s = new_state();
    s->current_pts = pts; s->n_pts = 41; s->vxmin = 0.0; s->vxmax = 0.4;
    s->input_state = INPUT_PF_NOISE;
    snprintf(s->text_input_buf, sizeof(s->text_input_buf), "999");
    commit_text_input(s);
    CHECK_INT("finestra rumore limitata alla vista", s->pf_noise_pts, 41);
    DONE();
}

/* T-24's memory-safety part: invalid width values cannot make i-k/i+k leave
   the visible data range, whether supplied through the field or API. */
static int test_peakfinder_width_bounds(void) {
    Point pts[9]; Peak out[MAX_PEAKS]; int n = -1;
    fill_peakfinder_trace(pts, 9, 1.0);
    run_peak_finder(pts, 9, 0.0, 0.08, 0, -4, 3.0, out, &n);
    CHECK(n >= 0 && n <= MAX_PEAKS, "numero picchi non valido: %d", n);
    AppState *s = new_state();
    s->current_pts = pts; s->n_pts = 9; s->vxmin = 0.0; s->vxmax = 0.08;
    s->input_state = INPUT_PF_SIG;
    snprintf(s->text_input_buf, sizeof(s->text_input_buf), "-4");
    commit_text_input(s);
    CHECK_INT("larghezza negativa rifiutata", s->pf_sig_pts, 1);
    s->input_state = INPUT_PF_SIG;
    snprintf(s->text_input_buf, sizeof(s->text_input_buf), "100");
    commit_text_input(s);
    CHECK_INT("larghezza limitata alla vista", s->pf_sig_pts, 9);
    DONE();
}

/* U-04: the Find button receives display coordinates, then translates them
   back to the raw spectrum coordinates before storing a peak. */
static int test_find_peaks_respects_offset(void) {
    Point pts[201];
    fill_peakfinder_trace(pts, 201, 1.0);
    AppState *s = new_state();
    s->current_pts = pts; s->n_pts = 201; s->data_loaded = 1;
    s->vxmin = 9.2; s->vxmax = 11.8; s->exp_offset = 10.0;
    s->pf_sig_pts = 3; s->pf_noise_pts = 25; s->pf_thresh = 4.0;
    run_visible_peak_finder(s);
    CHECK_INT("un picco con offset", s->n_peaks, 1);
    if (s->n_peaks) CHECK_DBL("picco in MHz grezzi", s->peaks[0].x, 1.0, 1e-6);
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

/* PF-01: one Hamiltonian owns exactly one .int control card.  Its Trot is
   common, while every included state contributes only its own Pickett dipole
   cards. Concentration deliberately never reaches the .int. */
static int test_multistate_writes_one_int_with_shared_trot(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    p->temp_k = 7.5;
    p->int_settings.temp_k = 123.0; /* obsolete cache must not drive output */
    add_species(s);
    p->species[0].mu[0] = 1.1;
    p->species[0].mu[1] = 2.2;
    p->species[0].mu[2] = 3.3;
    p->species[0].concentration = 0.25;
    p->species[1].mu[0] = 4.4;
    p->species[1].mu[1] = 5.5;
    p->species[1].mu[2] = 6.6;
    p->species[1].concentration = 9.0;
    memcpy(p->mu, p->species[1].mu, sizeof(p->mu)); /* active quick-panel state */
    type_option_line(p, "s 1 2 0");
    CHECK_INT("write shared multistate inputs", write_inputs(s, 0), 1);

    char int_path[900];
    snprintf(int_path, sizeof(int_path), "%s/.fit/model.int", g_work);
    FILE *fp = fopen(int_path, "r");
    CHECK(fp != NULL, "model.int non scritto");
    if (fp) {
        char title[256] = "", card[512] = "", all[2048] = "", line[256];
        fgets(title, sizeof(title), fp);
        fgets(card, sizeof(card), fp);
        while (fgets(line, sizeof(line), fp))
            strncat(all, line, sizeof(all) - strlen(all) - 1);
        fclose(fp);
        int flags, tag, fbegin, fend, maxv;
        double qrot, str0, str1, fqlim, temp;
        CHECK(sscanf(card, "%d %d %lf %d %d %lf %lf %lf %lf %d",
                     &flags, &tag, &qrot, &fbegin, &fend, &str0, &str1,
                     &fqlim, &temp, &maxv) == 10,
              "carta di controllo .int non leggibile: '%s'", card);
        CHECK_DBL("Trot unico scritto nella carta .int", temp, 7.5, 1e-12);
        CHECK(strstr(all, "1 1.1 /a dipole/") != NULL, "manca mu_a stato 0: %s", all);
        CHECK(strstr(all, "2 2.2 /b dipole/") != NULL, "manca mu_b stato 0: %s", all);
        CHECK(strstr(all, "3 3.3 /c dipole/") != NULL, "manca mu_c stato 0: %s", all);
        CHECK(strstr(all, "111 4.4 /a dipole/") != NULL, "manca mu_a stato 1: %s", all);
        CHECK(strstr(all, "112 5.5 /b dipole/") != NULL, "manca mu_b stato 1: %s", all);
        CHECK(strstr(all, "113 6.6 /c dipole/") != NULL, "manca mu_c stato 1: %s", all);
        CHECK(strstr(all, "0.25") == NULL && strstr(all, "9.0") == NULL,
              "la concentrazione esterna non deve comparire in model.int: %s", all);
    }
    CHECK(access(work_path(".fit/species_00.int"), F_OK) != 0,
          "non deve esistere uno .int per stato 0");
    CHECK(access(work_path(".fit/species_01.int"), F_OK) != 0,
          "non deve esistere uno .int per stato 1");
    DONE();
}

/* H-01: the active editor is a projection of one Hamiltonian at a time.
   Switching must preserve independent .int controls, states and constants;
   it must not merely relabel one mutable PredFitState. */
static int test_hamiltonian_switch_keeps_independent_models(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    p->temp_k = 19.0;
    p->int_settings.intensity_cutoff = -7.0;
    p->species[0].mu[0] = 4.0;
    p->mu[0] = 4.0; /* active state is projected into the quick panel */
    p->species[0].concentration = 0.3;
    CHECK_INT("duplica H1", predfit_duplicate_hamiltonian(s, "Tunneling pair"), 1);
    CHECK_INT("due Hamiltoniani", predfit_hamiltonian_count(s), 2);
    CHECK_INT("ID H2 stabile", predfit_active_hamiltonian_id(s), 2);
    p->temp_k = 41.0;
    p->int_settings.intensity_cutoff = -13.0;
    p->species[0].mu[0] = 8.0;
    p->mu[0] = 8.0;
    p->species[0].concentration = 2.5;

    CHECK_INT("seleziona H1", predfit_select_hamiltonian(s, 0), 1);
    CHECK_INT("ID H1 stabile", predfit_active_hamiltonian_id(s), 1);
    CHECK_DBL("Trot H1 indipendente", p->temp_k, 19.0, 1e-12);
    CHECK_DBL("cut H1 indipendente", p->int_settings.intensity_cutoff, -7.0, 1e-12);
    CHECK_DBL("dipolo stato H1 indipendente", p->species[0].mu[0], 4.0, 1e-12);
    CHECK_DBL("concentrazione stato H1 indipendente", p->species[0].concentration, 0.3, 1e-12);

    p->temp_k = 6.0;
    p->int_settings.intensity_cutoff = -21.0;
    p->species[0].concentration = 0.8;
    CHECK_INT("riseleziona H2", predfit_select_hamiltonian(s, 1), 1);
    CHECK_DBL("Trot H2 conservata", p->temp_k, 41.0, 1e-12);
    CHECK_DBL("cut H2 conservato", p->int_settings.intensity_cutoff, -13.0, 1e-12);
    CHECK_DBL("dipolo stato H2 conservato", p->species[0].mu[0], 8.0, 1e-12);
    CHECK_DBL("concentrazione stato H2 conservata", p->species[0].concentration, 2.5, 1e-12);

    CHECK_INT("torna H1", predfit_select_hamiltonian(s, 0), 1);
    CHECK_DBL("ultima Trot H1 conservata", p->temp_k, 6.0, 1e-12);
    CHECK_DBL("ultima concentrazione H1 conservata", p->species[0].concentration, 0.8, 1e-12);
    DONE();
}

/* H-02: h4 session records retain the whole project, not merely the active
   compatibility projection written at the start of the session file. */
static int test_multihamiltonian_session_roundtrip(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    p->temp_k = 12.0;
    p->int_settings.intensity_cutoff = -4.0;
    p->species[0].concentration = 0.4;
    CHECK_INT("crea H2", predfit_duplicate_hamiltonian(s, "Independent H"), 1);
    p->temp_k = 33.0;
    p->int_settings.intensity_cutoff = -16.0;
    p->species[0].mu[2] = 7.0;
    p->mu[2] = 7.0;
    p->species[0].concentration = 3.0;
    predfit_save_session(s);

    AppState *r = new_state();
    predfit_load_session(r);
    CHECK_INT("due H dopo restore", predfit_hamiltonian_count(r), 2);
    CHECK_INT("H2 attivo dopo restore", predfit_active_hamiltonian_id(r), 2);
    CHECK_DBL("Trot H2 dopo restore", r->predfit.temp_k, 33.0, 1e-12);
    CHECK_DBL("cut H2 dopo restore", r->predfit.int_settings.intensity_cutoff, -16.0, 1e-12);
    CHECK_DBL("mu_c H2 dopo restore", r->predfit.species[0].mu[2], 7.0, 1e-12);
    CHECK_DBL("conc H2 dopo restore", r->predfit.species[0].concentration, 3.0, 1e-12);
    CHECK_INT("seleziona H1 dopo restore", predfit_select_hamiltonian(r, 0), 1);
    CHECK_DBL("Trot H1 dopo restore", r->predfit.temp_k, 12.0, 1e-12);
    CHECK_DBL("cut H1 dopo restore", r->predfit.int_settings.intensity_cutoff, -4.0, 1e-12);
    CHECK_DBL("conc H1 dopo restore", r->predfit.species[0].concentration, 0.4, 1e-12);
    DONE();
}

static double int_temperature_of(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1.0;
    char title[256], card[512];
    if (!fgets(title, sizeof(title), fp) || !fgets(card, sizeof(card), fp)) { fclose(fp); return -1.0; }
    fclose(fp);
    int flags, tag, fbegin, fend, maxv;
    double qrot, str0, str1, fqlim, temp;
    return sscanf(card, "%d %d %lf %d %d %lf %lf %lf %lf %d",
                  &flags, &tag, &qrot, &fbegin, &fend, &str0, &str1,
                  &fqlim, &temp, &maxv) == 10 ? temp : -1.0;
}

/* H-03: every Hamiltonian writes its own Pickett files in .fit, named after
   it. Each H owns its own control card and cannot overwrite the other
   Hamiltonian's Trot/cut. */
static int test_two_hamiltonians_keep_independent_int_controls(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    p->temp_k = 5.0;
    p->int_settings.intensity_cutoff = -3.0;
    CHECK_INT("duplica H1", predfit_duplicate_hamiltonian(s, "H2"), 1);
    p->temp_k = 40.0;
    p->int_settings.intensity_cutoff = -18.0;
    CHECK_INT("scrive H2", write_inputs(s, 0), 1);
    CHECK(access(work_path(".fit/H2.int"), F_OK) == 0, "manca H2.int");
    CHECK_DBL("Trot H2 nel proprio .int", int_temperature_of(work_path(".fit/H2.int")), 40.0, 1e-12);

    CHECK_INT("seleziona H1", predfit_select_hamiltonian(s, 0), 1);
    CHECK_INT("scrive H1", write_inputs(s, 0), 1);
    CHECK(access(work_path(".fit/model.int"), F_OK) == 0, "manca model.int");
    CHECK_DBL("Trot H1 nel proprio .int", int_temperature_of(work_path(".fit/model.int")), 5.0, 1e-12);
    CHECK_DBL("H2 non viene sovrascritto", int_temperature_of(work_path(".fit/H2.int")), 40.0, 1e-12);
    DONE();
}

/* H-05: .fit/load is a drop box.  Every basename there is one Hamiltonian:
   the .par gives the option line and the parameters, the .int the states and
   the control card, the .lin the assignments that belong to it. */
/* Writes in load/ one model whose .lin holds a single transition, and - when
   with_cat - the catalogue that model comes with, holding that same
   transition at 3000.100 MHz. */
static void write_load_model(const char *stem, int with_cat) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.fit/load/%s.par", g_work, stem);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fputs("imported monomer\n"
          "   3    0   50    0  0.0000E+00  1.0000E+06  1.0000E+00 1.0000000000\n"
          "s   1  2  0\n"
          "       10000  1.15136041700E+03  1.00000000E+00 /A/\n"
          "       20000  3.16151112700E+02  0.00000000E+00 /B/\n"
          "       30000  3.13174236800E+02  1.00000000E+00 /C/\n", fp);
    fclose(fp);

    snprintf(path, sizeof(path), "%s/.fit/load/%s.lin", g_work, stem);
    fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "%3d%3d%3d%3d%3d%3d%18s%15.6f %10.6f 1.0\n", 3, 1, 2, 2, 0, 2, "", 3000.125, 0.02);
    fclose(fp);

    if (!with_cat) return;
    snprintf(path, sizeof(path), "%s/.fit/load/%s.cat", g_work, stem);
    fp = fopen(path, "w");
    if (!fp) return;
    /* One fixed-width .cat record: FREQ ERR LGINT DR ELO GUP TAG QNFMT, then
       six upper and six lower quantum numbers (calpgm/calcat.c:700). */
    fprintf(fp, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%2d%2d%2d%2s%2s%2s%2d%2d%2d%2s%2s%2s\n",
            3000.1000, 0.0020, -4.5000, 3, 1.2340, 7, 91, 303,
            3, 1, 2, "", "", "", 2, 0, 2, "", "", "");
    fclose(fp);
}

/* H-16: Import is idempotent.  Pressing it twice - or pressing it once in an
   app that already read assignments.txt at startup - must leave one copy of
   each model and one copy of each of its lines, not two. */
static int test_import_twice_keeps_one_copy(void) {
    AppState *s = new_state();
    mkdir(work_path(".fit"), 0700);
    mkdir(work_path(".fit/load"), 0700);
    write_load_model("mon", 1);

    CHECK_INT("primo import", predfit_import_load_dir(s), 1);
    int hamiltonians = predfit_hamiltonian_count(s);
    CHECK_INT("assignment dopo il primo import", s->n_assignments, 1);
    int owner = s->assignments[0].hamiltonian_id;

    CHECK_INT("secondo import", predfit_import_load_dir(s), 1);
    CHECK_INT("nessun modello duplicato", predfit_hamiltonian_count(s), hamiltonians);
    CHECK_INT("nessuna riga duplicata", s->n_assignments, 1);
    CHECK_INT("stesso proprietario", s->assignments[0].hamiltonian_id, owner);
    CHECK(strcmp(s->predfit.hamiltonian[s->predfit.active_hamiltonian].name, "mon") == 0,
          "H attivo: atteso mon, ottenuto %s",
          s->predfit.hamiltonian[s->predfit.active_hamiltonian].name);
    DONE();
}

/* H-17: a .lin carries no calculated frequency, so the catalogue that comes
   with the model in load/ fills the PREDICTED column at import time - without
   changing the identity of the assignment or its measured frequency. */
static int test_import_reads_predictions_from_the_catalog(void) {
    AppState *s = new_state();
    mkdir(work_path(".fit"), 0700);
    mkdir(work_path(".fit/load"), 0700);
    write_load_model("mon", 1);

    CHECK_INT("import", predfit_import_load_dir(s), 1);
    CHECK_INT("assignment importati", s->n_assignments, 1);
    if (s->n_assignments != 1) DONE();
    Assignment *a = &s->assignments[0];
    CHECK_DBL("frequenza predetta dal .cat", a->pred.freq_mhz, 3000.1000, 1e-4);
    CHECK_DBL("ELO dal .cat", a->pred.elo_cm, 1.2340, 1e-4);
    CHECK_DBL("frequenza misurata invariata", a->exp_freq, 3000.125, 1e-6);
    CHECK_INT("J superiore invariato", a->pred.Ju, 3);
    CHECK_INT("J inferiore invariato", a->pred.Jl, 2);
    CHECK_INT("proprietario invariato", a->pred.hamiltonian_id, a->hamiltonian_id);
    DONE();
}

static int test_import_load_dir_builds_hamiltonians(void) {
    AppState *s = new_state();
    mkdir(work_path(".fit"), 0700);
    mkdir(work_path(".fit/load"), 0700);

    FILE *fp = fopen(work_path(".fit/load/mon.par"), "w");
    CHECK(fp != NULL, "impossibile scrivere mon.par");
    if (!fp) DONE();
    fputs("imported monomer\n"
          "   3    0   50    0  0.0000E+00  1.0000E+06  1.0000E+00 1.0000000000\n"
          "s   1  2  0\n"
          "       10000  1.15136041700E+03  1.00000000E+00 /A/\n"
          "       20000  3.16151112700E+02  0.00000000E+00 /B/\n"
          "       30000  3.13174236800E+02  1.00000000E+00 /C/\n", fp);
    fclose(fp);

    fp = fopen(work_path(".fit/load/mon.int"), "w");
    CHECK(fp != NULL, "impossibile scrivere mon.int");
    if (!fp) DONE();
    fputs("imported monomer\n"
          "0 1 1000 0 40 -20 -20 8 7.5 1\n"
          "1 1.5 /a dipole/\n"
          "2 0.25 /b dipole/\n"
          "111 0.8 /a dipole/\n"
          "101 0.1 /interstate/\n", fp);
    fclose(fp);

    fp = fopen(work_path(".fit/load/mon.lin"), "w");
    CHECK(fp != NULL, "impossibile scrivere mon.lin");
    if (!fp) DONE();
    fprintf(fp, "%3d%3d%3d%3d%3d%3d%18s%15.6f %10.6f 1.0\n", 3, 1, 2, 2, 0, 2, "", 3000.125, 0.02);
    fclose(fp);

    /* A second model, with a .var alone: no states are declared, so it keeps
       the single default state and stays importable. */
    fp = fopen(work_path(".fit/load/dim.var"), "w");
    CHECK(fp != NULL, "impossibile scrivere dim.var");
    if (!fp) DONE();
    fputs("imported dimer\n"
          "   1    0    0    0  0.0000E+00  1.0000E+06  1.0000E+00 1.0000000000\n"
          "s   1  1  0\n"
          "       10000  2.00000000000E+03  1.00000000E+00 /A/\n", fp);
    fclose(fp);

    CHECK_INT("Hamiltoniani importati", predfit_import_load_dir(s), 2);
    PredFitState *p = &s->predfit;
    CHECK_INT("H nel progetto", predfit_hamiltonian_count(s), 3);
    /* load/ is read in name order, so mon is the last one imported. */
    CHECK(strcmp(p->hamiltonian[p->active_hamiltonian].name, "mon") == 0,
          "H attivo: atteso mon, ottenuto %s", p->hamiltonian[p->active_hamiltonian].name);
    CHECK(strcmp(p->hamiltonian[1].name, "dim") == 0,
          "secondo H: atteso dim, ottenuto %s", p->hamiltonian[1].name);
    CHECK(strcmp(p->hamiltonian_line, "s   1  2  0") == 0,
          "option line importata: attesa 's   1  2  0', ottenuta '%s'", p->hamiltonian_line);
    CHECK_INT("parametri importati", p->n_param, 3);
    CHECK_DBL("valore di A importato", p->a, 1151.360417, 1e-6);
    CHECK_DBL("B resta fissato come nel .par", p->param[1].error, 0.0, 1e-12);
    CHECK_INT("stati dal .int", p->n_species, 2);
    CHECK_INT("indice Pickett del secondo stato", p->species[1].state_index, 1);
    CHECK_DBL("mu a dello stato 0", p->species[0].mu[0], 1.5, 1e-12);
    CHECK_DBL("mu b dello stato 0", p->species[0].mu[1], 0.25, 1e-12);
    CHECK_DBL("mu a dello stato 1", p->species[1].mu[0], 0.8, 1e-12);
    CHECK_DBL("Trot dal .int", p->temp_k, 7.5, 1e-12);
    CHECK_INT("assignment importati dal .lin", s->n_assignments, 1);
    CHECK_INT("proprietario dell'assignment", s->assignments[0].hamiltonian_id,
              predfit_active_hamiltonian_id(s));
    CHECK_DBL("frequenza importata", s->assignments[0].exp_freq, 3000.125, 1e-6);
    /* Nothing in load/ is consumed, and the session is not written on its own. */
    CHECK(access(work_path(".fit/load/mon.par"), F_OK) == 0, "mon.par e' stato rimosso da load/");
    CHECK_INT("sessione da salvare esplicitamente", p->session_dirty, 1);
    CHECK(access(work_path(".fit/spectravisual.state"), F_OK) != 0,
          "l'import non deve scrivere la sessione");
    DONE();
}

/* H-06: the workspace is written only when the user asks for it. */
static int test_session_saved_only_on_request(void) {
    AppState *s = new_state();
    mono_model(&s->predfit);
    CHECK_INT("scrive gli input Pickett", write_inputs(s, 0), 1);
    CHECK(access(work_path(".fit/spectravisual.state"), F_OK) != 0,
          "Calculate non deve salvare la sessione");
    predfit_save_session(s);
    CHECK(access(work_path(".fit/spectravisual.state"), F_OK) == 0,
          "Save session non ha scritto spectravisual.state");
    CHECK_INT("sessione pulita dopo il salvataggio", s->predfit.session_dirty, 0);
    DONE();
}

/* H-07: the sidebar order is presentation.  Moving a Hamiltonian or a state
   keeps its identity, its selection and its Pickett state index. */
static int test_sidebar_reorder_keeps_identity(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    CHECK_INT("crea un secondo H", predfit_add_hamiltonian(s, "second"), 1);
    int active_id = predfit_active_hamiltonian_id(s);
    CHECK_INT("sposta l'H attivo in cima", predfit_move_hamiltonian(s, p->active_hamiltonian, -1), 1);
    CHECK_INT("resta attivo lo stesso H", predfit_active_hamiltonian_id(s), active_id);
    CHECK_INT("l'H attivo e' il primo", p->active_hamiltonian, 0);
    CHECK(strcmp(p->hamiltonian[0].name, "second") == 0,
          "ordine: atteso second in cima, ottenuto %s", p->hamiltonian[0].name);
    CHECK(strcmp(p->hamiltonian[1].name, "model") == 0,
          "ordine: atteso model in seconda posizione, ottenuto %s", p->hamiltonian[1].name);
    CHECK_INT("non si esce dalla lista", predfit_move_hamiltonian(s, 0, -1), 0);

    add_species(s);
    CHECK_INT("due stati", p->n_species, 2);
    int moved_state = p->species[1].state_index;
    CHECK_INT("sposta lo stato in cima", predfit_move_species(s, 1, -1), 1);
    CHECK_INT("lo stato conserva il suo indice Pickett", p->species[0].state_index, moved_state);
    CHECK_INT("lo stato resta selezionato", p->active_species, 0);
    CHECK_INT("non si esce dalla lista degli stati", predfit_move_species(s, 1, 1), 0);
    DONE();
}

/* H-08: Simulate runs SPCAT on every checked Hamiltonian and shows the
   catalogues in one plot; every row keeps the model that produced it. */
static int test_simulation_plots_every_checked_hamiltonian(void) {
    AppState *s = new_state();
    if (!have_program(s->settings.spcat_path)) SKIP("SPCAT non trovato da autodetect_program");
    PredFitState *p = &s->predfit;
    mono_model(p);
    CHECK_INT("crea il secondo H", predfit_duplicate_hamiltonian(s, "second"), 1);
    /* Two different rotors, so the two catalogues cannot coincide. */
    p->a = 2000.0; p->b = 800.0; p->c = 700.0;
    sync_basic_parameters(p);
    int id_second = predfit_active_hamiltonian_id(s);
    CHECK_INT("torna sul primo H", predfit_select_hamiltonian(s, 0), 1);
    int id_first = predfit_active_hamiltonian_id(s);

    CHECK_INT("simula due H", predfit_simulate(s), 2);
    CHECK_INT("l'H attivo non cambia", predfit_active_hamiltonian_id(s), id_first);
    pump(s);
    CHECK(s->n_pred > 0, "righe simulate: attese > 0, ottenute %d", s->n_pred);
    int from_first = 0, from_second = 0, orphan = 0;
    for (int i = 0; i < s->n_pred; i++) {
        if (s->pred_lines[i].hamiltonian_id == id_first) from_first++;
        else if (s->pred_lines[i].hamiltonian_id == id_second) from_second++;
        else orphan++;
    }
    CHECK(from_first > 0, "righe del primo H: attese > 0, ottenute %d", from_first);
    CHECK(from_second > 0, "righe del secondo H: attese > 0, ottenute %d", from_second);
    CHECK_INT("righe senza proprietario", orphan, 0);
    CHECK_INT("generated_catalog_active", s->predfit.generated_catalog_active, 1);
    /* One plot, one frequency order. */
    int sorted = 1;
    for (int i = 1; i < s->n_pred; i++)
        if (s->pred_lines[i].freq_mhz < s->pred_lines[i - 1].freq_mhz) sorted = 0;
    CHECK(sorted, "le righe unite non sono ordinate in frequenza");

    /* An assignment takes the owner from the row, not from the active H. */
    int row = -1;
    for (int i = 0; i < s->n_pred && row < 0; i++)
        if (s->pred_lines[i].hamiltonian_id == id_second) row = i;
    CHECK(row >= 0, "nessuna riga del secondo H da assegnare");
    if (row >= 0) {
        s->n_selected = 1;
        s->selected_indices[0] = row;
        assign_selected_predictions(s, s->pred_lines[row].freq_mhz + 0.01, 1.0);
        CHECK_INT("assignment creato", s->n_assignments, 1);
        if (s->n_assignments == 1)
            CHECK_INT("proprietario dell'assignment", s->assignments[0].hamiltonian_id, id_second);
    }

    /* Unchecking a Hamiltonian removes it from the next plot. */
    s->predfit.hamiltonian[1].simulate_excluded = 1;
    CHECK_INT("simula un solo H", predfit_simulate(s), 1);
    pump(s);
    int still_second = 0;
    for (int i = 0; i < s->n_pred; i++)
        if (s->pred_lines[i].hamiltonian_id == id_second) still_second++;
    CHECK_INT("righe dell'H escluso", still_second, 0);
    DONE();
}

/* H-09: a dipole unchecked in Simulation is written as zero in the .int, so
   SPCAT predicts no transition of that type. */
static int test_dipole_checkbox_zeroes_the_int(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    p->species[0].mu[0] = 1.5;
    p->species[0].mu[1] = 2.5;
    p->species[0].mu[2] = 3.5;
    memcpy(p->mu, p->species[0].mu, sizeof(p->mu));
    p->species[0].mu_excluded[1] = 1;
    CHECK_INT("scrive gli input", write_inputs(s, 0), 1);
    FILE *fp = fopen(work_path(".fit/model.int"), "r");
    CHECK(fp != NULL, "model.int non scritto");
    if (!fp) DONE();
    char line[256];
    double mu[3] = {-1.0, -1.0, -1.0};
    while (fgets(line, sizeof(line), fp)) {
        int id = 0; double value = 0.0;
        if (sscanf(line, "%d %lf", &id, &value) == 2 && id >= 1 && id <= 3) mu[id - 1] = value;
    }
    fclose(fp);
    CHECK_DBL("mu a resta nel .int", mu[0], 1.5, 1e-12);
    CHECK_DBL("mu b escluso e' zero", mu[1], 0.0, 1e-12);
    CHECK_DBL("mu c resta nel .int", mu[2], 3.5, 1e-12);
    CHECK_DBL("il valore in memoria non viene toccato", p->species[0].mu[1], 2.5, 1e-12);
    DONE();
}

/* H-10: in a simulated plot each row is rescaled with its own model, so the
   concentration of one Hamiltonian cannot restyle another's lines. */
static int test_simulated_intensities_follow_their_own_model(void) {
    PickettSpecies first = {"State 0", 0, 1, {1.0, 1.0, 1.0}, 1.0, {0, 0, 0}};
    PickettSpecies second = {"State 0", 0, 1, {1.0, 1.0, 1.0}, 0.25, {0, 0, 0}};
    PredIntensityModel models[2] = {
        {7, 5.0, 5.0, &first, 1},
        {9, 5.0, 5.0, &second, 1},
    };
    PredLine lines[2] = {
        {.freq_mhz = 10000.0, .cat_lgint = -4.0, .elo_cm = 1.0, .rot_dof = 3, .n_qn = 3,
         .hamiltonian_id = 7},
        {.freq_mhz = 10000.0, .cat_lgint = -4.0, .elo_cm = 1.0, .rot_dof = 3, .n_qn = 3,
         .hamiltonian_id = 9},
    };
    double max_int = 0.0;
    rescale_predicted_intensities_multi(lines, 2, models, 2, &max_int);
    CHECK(lines[0].linear_int > 0.0, "riga del primo modello non riscalata");
    CHECK_DBL("rapporto delle concentrazioni", lines[1].linear_int / lines[0].linear_int, 0.25, 1e-12);
    CHECK_DBL("il massimo e' quello del modello piu' intenso", max_int, lines[0].linear_int, 1e-12);

    /* A row nobody claims keeps the intensity its catalogue stated. */
    PredLine orphan = {.freq_mhz = 10000.0, .cat_lgint = -4.0, .elo_cm = 1.0, .rot_dof = 3,
                       .n_qn = 3, .hamiltonian_id = 42};
    rescale_predicted_intensities_multi(&orphan, 1, models, 2, NULL);
    CHECK_DBL("riga senza modello", orphan.linear_int, pow(10.0, -4.0), 1e-18);
    DONE();
}

/* H-11: a model that has never been calculated - one just imported from a
   .lin - is calculated by Fit itself instead of being refused, and the
   assignments of a .lin get their predicted line from that catalogue. */
static int test_fit_calculates_the_missing_catalogue(void) {
    AppState *s = new_state();
    if (!have_program(s->settings.spcat_path) || !have_program(s->settings.spfit_path))
        SKIP("SPCAT/SPFIT non trovati da autodetect_program");
    PredFitState *p = &s->predfit;
    mono_model(p);
    CHECK_INT("primo calcolo", predfit_calculate(s), 1);
    pump(s);
    CHECK(s->n_pred >= 2, "righe predette: attese >= 2, ottenute %d", s->n_pred);
    if (s->n_pred < 2) DONE();
    for (int k = 0; k < 2; k++) {
        s->n_selected = 1;
        s->selected_indices[0] = k;
        assign_selected_predictions(s, s->pred_lines[k].freq_mhz + 0.001, 1.0);
    }
    CHECK_INT("due assignment", s->n_assignments, 2);

    /* An import leaves exactly this: a model with assignments and no
       catalogue of its own. */
    unlink(work_path(".fit/model.cat"));
    CHECK_INT("Fit senza catalogo", predfit_fit(s), 1);
    CHECK(access(work_path(".fit/model.cat"), F_OK) == 0,
          "Fit non ha ricreato il catalogo del modello (stato: %s)", p->status);

    /* A .lin knows only quantum numbers: the predicted line comes back with
       the next catalogue, without touching the identity of the assignment. */
    int ju = s->assignments[0].pred.Ju, jl = s->assignments[0].pred.Jl;
    s->assignments[0].pred.freq_mhz = 0.0;
    s->assignments[0].pred.linear_int = 0.0;
    CHECK_INT("ricalcolo", predfit_calculate(s), 1);
    pump(s);
    CHECK(s->assignments[0].pred.freq_mhz > 0.0,
          "frequenza predetta non ripristinata: %.6f", s->assignments[0].pred.freq_mhz);
    CHECK_INT("J superiore invariato", s->assignments[0].pred.Ju, ju);
    CHECK_INT("J inferiore invariato", s->assignments[0].pred.Jl, jl);
    DONE();
}

/* H-12: a catalogue still queued must not load after the simulation that
   replaced it - it would wipe the models the merged plot is made of. */
static int test_simulation_supersedes_a_queued_catalog(void) {
    AppState *s = new_state();
    app_enqueue_pending_load(s, PENDING_LOAD_CATALOG, "/tmp/superseded.cat", 1);
    app_enqueue_pending_load(s, PENDING_LOAD_SPECTRUM, "/tmp/keep-me.txt", 0);
    app_enqueue_pending_load(s, PENDING_LOAD_SIMULATION, "/tmp/simulation.cat", 1);
    CHECK_INT("richieste rimaste in coda", s->pending_load_count, 2);
    int kind[2] = {-1, -1};
    for (int i = 0; i < s->pending_load_count && i < 2; i++)
        kind[i] = s->pending_loads[(s->pending_load_head + i) % MAX_PENDING_LOADS].kind;
    CHECK_INT("lo spettro resta in coda", kind[0], PENDING_LOAD_SPECTRUM);
    CHECK_INT("resta solo la simulazione", kind[1], PENDING_LOAD_SIMULATION);
    DONE();
}

/* H-13: with broadening on, every state also has a trace of its own: the
   per-state traces add up to exactly the total one, and a state's trace holds
   only the lines of that state. */
static int test_species_traces_split_the_broadened_profile(void) {
    AppState *s = new_state();
    s->pred_lines = calloc(3, sizeof(PredLine));
    CHECK(s->pred_lines != NULL, "allocazione delle righe predette");
    if (!s->pred_lines) DONE();
    s->n_pred = 3;
    /* H1 state 0, H1 state 1 (a multistate catalogue) and H2 state 0. */
    s->pred_lines[0] = (PredLine){.freq_mhz = 10000.0, .linear_int = 1e-3, .lgint = -3.0,
                                  .cat_lgint = -3.0, .n_qn = 4, .M1l = 0, .hamiltonian_id = 1,
                                  .mu = 'a', .branch = 'R'};
    s->pred_lines[1] = (PredLine){.freq_mhz = 10001.0, .linear_int = 2e-3, .lgint = -2.7,
                                  .cat_lgint = -2.7, .n_qn = 4, .M1l = 1, .hamiltonian_id = 1,
                                  .mu = 'a', .branch = 'R'};
    s->pred_lines[2] = (PredLine){.freq_mhz = 10002.0, .linear_int = 4e-3, .lgint = -2.4,
                                  .cat_lgint = -2.4, .n_qn = 4, .M1l = 0, .hamiltonian_id = 2,
                                  .mu = 'a', .branch = 'R'};
    s->pred_global_max = 4e-3;
    s->broadening_active = 1;
    s->broaden_mode = 0;                 /* analytic: the profile is additive */
    s->gauss_gamma = 0.8;
    s->lorentz_gamma = 0.0;

    const double at = 10001.0;
    double total = broadened_value_at(s, at, 0, 0);
    double h1s0  = broadened_value_at(s, at, 1, 0);
    double h1s1  = broadened_value_at(s, at, 1, 1);
    double h2s0  = broadened_value_at(s, at, 2, 0);
    CHECK(total > 0.0, "traccia totale nulla");
    CHECK(h1s0 > 0.0 && h1s1 > 0.0 && h2s0 > 0.0, "una sottotraccia e' nulla dove dovrebbe contribuire");
    CHECK_DBL("le sottotracce sommano alla traccia totale", h1s0 + h1s1 + h2s0, total, 1e-15);
    /* Each state's own line dominates its trace at its own centre. */
    CHECK(broadened_value_at(s, 10002.0, 2, 0) > broadened_value_at(s, 10002.0, 1, 0),
          "la sottotraccia di H2 non domina sulla propria riga");
    CHECK_DBL("uno stato senza righe non disegna nulla",
              broadened_value_at(s, at, 99, 0), 0.0, 1e-18);
    /* Without broadening there is nothing to split. */
    s->broadening_active = 0;
    CHECK_DBL("senza broadening non c'e' profilo", broadened_value_at(s, at, 1, 0), 0.0, 1e-18);
    DONE();
}

/* H-14: the per-state traces are off until asked for, every state starts with
   a colour of its own, and both survive a saved session. */
static int test_species_trace_colour_default_and_session(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    CHECK_INT("sottotracce spente di default", p->species_trace_mode, TRACE_SUM);
    add_species(s);
    CHECK_INT("due stati", p->n_species, 2);
    SDL_Color first = predfit_species_color(&p->species[0], 0);
    SDL_Color second = predfit_species_color(&p->species[1], 1);
    CHECK(first.a > 0 && second.a > 0, "colore di default non assegnato");
    CHECK(first.r != second.r || first.g != second.g || first.b != second.b,
          "due stati con lo stesso colore di default");

    /* Only the predicted states are drawn, and each is listed once. */
    PredfitSpeciesTrace list[8];
    p->generated_catalog_active = 1;
    CHECK_INT("stati nel plot", predfit_plot_species(s, list, 8), 2);
    p->species[1].predict_enabled = 0;
    CHECK_INT("uno stato escluso non ha traccia", predfit_plot_species(s, list, 8), 1);
    p->species[1].predict_enabled = 1;

    p->species_trace_mode = TRACE_STATES;
    p->species[1].trace_color = (SDL_Color){10, 20, 30, 235};
    predfit_save_session(s);

    AppState *r = new_state();
    predfit_load_session(r);
    CHECK_INT("modo delle tracce salvato", r->predfit.species_trace_mode, TRACE_STATES);
    CHECK_INT("colore salvato: rosso", r->predfit.species[1].trace_color.r, 10);
    CHECK_INT("colore salvato: verde", r->predfit.species[1].trace_color.g, 20);
    CHECK_INT("colore salvato: blu", r->predfit.species[1].trace_color.b, 30);
    DONE();
}

/* H-15: five Hamiltonians of one state each - what importing five models
   gives - must not end up sharing a default trace colour. */
static int test_default_species_colours_are_distinct(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    mono_model(p);
    for (int i = 0; i < 4; i++)
        CHECK_INT("crea un Hamiltoniano", predfit_add_hamiltonian(s, NULL), 1);
    CHECK_INT("cinque Hamiltoniani", p->n_hamiltonians, 5);
    store_active_hamiltonian(p);
    SDL_Color seen[8];
    int n = 0;
    for (int h = 0; h < p->n_hamiltonians; h++) {
        const PredFitSnapshot *m = &p->hamiltonian[h].model;
        for (int k = 0; k < m->n_species && n < 8; k++) {
            SDL_Color c = predfit_species_color(&m->species[k], n);
            for (int j = 0; j < n; j++)
                CHECK(!(seen[j].r == c.r && seen[j].g == c.g && seen[j].b == c.b),
                      "colore ripetuto #%02X%02X%02X fra lo stato %d e lo stato %d",
                      c.r, c.g, c.b, j, n);
            seen[n++] = c;
        }
    }
    CHECK_INT("colori raccolti", n, 5);
    DONE();
}

/* H-16: the Broadening panel chooses which traces are drawn - the total, the
   per-state ones, or both - and the three segments map to the three modes. */
static int test_broadening_panel_selects_the_traces(void) {
    AppState *s = new_state();
    Layout L;
    s->win_br.visible = 1;
    compute_layout(s, &L);
    update_sidebars(s, &L);
    SDL_Rect w = s->win_br.rect;
    CHECK(w.w > 0 && w.h > 0, "pannello Broadening senza geometria");
    if (w.w <= 0) DONE();
    SDL_Rect traces = ui_br_traces(w, s->broaden_mode == 1);
    CHECK_INT("modo iniziale", s->predfit.species_trace_mode, TRACE_SUM);

    int y = traces.y + traces.h / 2;
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, traces.x + traces.w / 2, y, SDL_BUTTON_LEFT);
    CHECK_INT("segmento centrale = somma + stati", s->predfit.species_trace_mode, TRACE_SUM_AND_STATES);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, traces.x + traces.w - 4, y, SDL_BUTTON_LEFT);
    CHECK_INT("terzo segmento = solo stati", s->predfit.species_trace_mode, TRACE_STATES);
    mouse_button(s, &L, SDL_MOUSEBUTTONDOWN, traces.x + 4, y, SDL_BUTTON_LEFT);
    CHECK_INT("primo segmento = solo somma", s->predfit.species_trace_mode, TRACE_SUM);

    /* The row exists in both broadening modes and never overlaps the toggle. */
    for (int kaiser = 0; kaiser <= 1; kaiser++) {
        SDL_Rect toggle = ui_br_toggle(w, kaiser);
        SDL_Rect row = ui_br_traces(w, kaiser);
        CHECK(row.y >= toggle.y + toggle.h,
              "la riga delle tracce si sovrappone al toggle (kaiser=%d)", kaiser);
    }
    DONE();
}

/* H-04: the same QN transition may legitimately appear in two independent
   Hamiltonians. Ownership is therefore part of assignment identity and
   survives assignments.txt format 2. */
static int test_assignment_owner_keeps_same_qn_in_two_hamiltonians(void) {
    AppState *s = new_state();
    PredLine line = {.freq_mhz = 3000.0, .linear_int = 1.0, .n_qn = 3,
                     .Ju = 3, .Kau = 1, .Kcu = 2, .Jl = 2, .Kal = 0, .Kcl = 2};
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 3000.01, 1.0, 1);
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 3000.02, 2.0, 2);
    CHECK_INT("stessa riga QN ammessa in H distinti", s->n_assignments, 2);
    CHECK_INT("owner primo assignment", s->assignments[0].hamiltonian_id, 1);
    CHECK_INT("owner secondo assignment", s->assignments[1].hamiltonian_id, 2);
    CHECK_INT("salva assignment con owner", save_assignments(s), 1);
    Assignment loaded[4] = {0}; int n_loaded = 0;
    CHECK_INT("ricarica assignment con owner", load_assignments_file(work_path("assignments.txt"), loaded, &n_loaded, NULL), 2);
    CHECK_INT("due righe dopo restore", n_loaded, 2);
    CHECK_INT("owner H1 dopo restore", loaded[0].hamiltonian_id, 1);
    CHECK_INT("owner H2 dopo restore", loaded[1].hamiltonian_id, 2);
    DONE();
}

/* H-05: the Project tab's hierarchy is deterministic: each Hamiltonian row
   is followed by exactly its states, rather than relying on a hidden current
   index or previous/next navigation. */
static int test_project_rows_are_hamiltonian_state_hierarchy(void) {
    AppState *s = new_state();
    add_species(s);                                      /* H1: header + states 0,1 */
    CHECK_INT("duplica H1", predfit_duplicate_hamiltonian(s, "H2"), 1);
    int h = -1, state = -2;
    CHECK_INT("sei righe progetto", project_row_count(&s->predfit), 6);
    CHECK_INT("riga 0 H1", project_row_info(&s->predfit, 0, &h, &state), 1);
    CHECK_INT("header H1", h, 0); CHECK_INT("header non è stato", state, -1);
    CHECK_INT("riga 1 stato H1", project_row_info(&s->predfit, 1, &h, &state), 1);
    CHECK_INT("stato H1 appartiene a H1", h, 0); CHECK_INT("indice stato H1", state, 0);
    CHECK_INT("riga 3 H2", project_row_info(&s->predfit, 3, &h, &state), 1);
    CHECK_INT("header H2", h, 1); CHECK_INT("header H2 non è stato", state, -1);
    CHECK_INT("riga 5 stato H2", project_row_info(&s->predfit, 5, &h, &state), 1);
    CHECK_INT("stato H2 appartiene a H2", h, 1); CHECK_INT("indice stato H2", state, 1);
    DONE();
}

/* H-06: Add Hamiltonian is intentionally different from Duplicate.  It is a
   clean Pickett model, while the model the user was editing is retained. */
static int test_add_hamiltonian_starts_fresh_and_keeps_source(void) {
    AppState *s = new_state();
    PredFitState *p = &s->predfit;
    p->a = 4321.0;
    p->temp_k = 42.0;
    p->mu[0] = 7.0; /* active-state editing is projected through p->mu */
    p->species[0].concentration = 0.25;

    CHECK_INT("aggiungi Hamiltoniano pulito", predfit_add_hamiltonian(s, "Fresh H"), 1);
    CHECK_INT("due Hamiltoniani", p->n_hamiltonians, 2);
    CHECK_INT("nuovo H è attivo", p->active_hamiltonian, 1);
    CHECK(strcmp(p->hamiltonian[1].name, "Fresh H") == 0, "nome H nuovo: '%s'", p->hamiltonian[1].name);
    CHECK_DBL("A del nuovo H è default", p->a, 10000.0, 1e-12);
    CHECK_DBL("Trot del nuovo H è default", p->temp_k, 5.0, 1e-12);
    CHECK_DBL("dipolo stato nuovo è default", p->species[0].mu[0], 1.0, 1e-12);
    CHECK_DBL("concentrazione stato nuovo è default", p->species[0].concentration, 1.0, 1e-12);

    CHECK_INT("ritorna a H1", predfit_select_hamiltonian(s, 0), 1);
    CHECK_DBL("A H1 conservata", p->a, 4321.0, 1e-12);
    CHECK_DBL("Trot H1 conservata", p->temp_k, 42.0, 1e-12);
    CHECK_DBL("dipolo H1 conservato", p->species[0].mu[0], 7.0, 1e-12);
    CHECK_DBL("concentrazione H1 conservata", p->species[0].concentration, 0.25, 1e-12);
    DONE();
}

/* H-07: deletion removes the whole ownership unit, not a model while leaving
   its assignments silently attached to an unrelated remaining Hamiltonian. */
static int test_delete_hamiltonian_removes_its_assignments_and_keeps_one(void) {
    AppState *s = new_state();
    PredLine line = {.freq_mhz = 3000.0, .linear_int = 1.0, .n_qn = 3,
                     .Ju = 3, .Kau = 1, .Kcu = 2, .Jl = 2, .Kal = 0, .Kcl = 2};
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 3000.01, 1.0, 1);
    CHECK_INT("crea H2", predfit_add_hamiltonian(s, "H2"), 1);
    line.Ju = 4;
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 4000.01, 1.0, 2);
    CHECK_INT("due assignment prima della rimozione", s->n_assignments, 2);

    CHECK_INT("elimina H2", predfit_delete_hamiltonian(s, 1), 1);
    CHECK_INT("rimane H1", s->predfit.n_hamiltonians, 1);
    CHECK_INT("H1 torna attivo", predfit_active_hamiltonian_id(s), 1);
    CHECK_INT("assignment H2 rimosso", s->n_assignments, 1);
    CHECK_INT("assignment rimasto è H1", s->assignments[0].hamiltonian_id, 1);
    CHECK_INT("ultimo H protetto", predfit_delete_hamiltonian(s, 0), 0);
    CHECK_INT("H1 ancora presente", s->predfit.n_hamiltonians, 1);
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

static int make_mock_program(const char *path, int exit_code) {
    FILE *fp = fopen(path, "w");
    if (!fp) return 0;
    fprintf(fp, "#!/bin/sh\nexit %d\n", exit_code);
    return fclose(fp) == 0 && chmod(path, 0700) == 0;
}

/* T-21, R-18: no shell is involved, so both the work directory and executable
   path may contain shell-sensitive characters. */
static int test_spaces_and_quotes_in_data_dir(void) {
    char data_dir[700];
    snprintf(data_dir, sizeof(data_dir), "%s/data dir with ' quote", g_work);
    CHECK_INT("crea data_dir", mkdir(data_dir, 0700), 0);
    AppState *s = new_state();
    mono_model(&s->predfit);
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", data_dir);
    char spcat[700], spfit[700];
    snprintf(spcat, sizeof(spcat), "%s/mock SPCAT", g_work);
    snprintf(spfit, sizeof(spfit), "%s/mock SPFIT", g_work);
    CHECK(make_mock_program(spcat, 0), "mock SPCAT non creato");
    CHECK(make_mock_program(spfit, 0), "mock SPFIT non creato");
    snprintf(s->settings.spcat_path, sizeof(s->settings.spcat_path), "%s", spcat);
    snprintf(s->settings.spfit_path, sizeof(s->settings.spfit_path), "%s", spfit);
    set_predictions(s, fx("cat3_303.cat"));
    assign_index(s, 0, 3000.01, 1.0);
    CHECK_INT("Calculate con percorso quotato", predfit_calculate(s), 1);
    char model_cat[800];
    snprintf(model_cat, sizeof(model_cat), "%s/.fit/model.cat", data_dir);
    CHECK(copy_path(fx("cat3_303.cat"), model_cat), "CAT corrente non copiato per Fit");
    CHECK_INT("Fit con percorso quotato", predfit_fit(s), 1);
    DONE();
}

/* M-01: report the program's real exit value, rather than system()'s encoded
   wait status (for example 256 for exit 1). */
static int test_process_exit_status_decoded(void) {
    char program[700];
    snprintf(program, sizeof(program), "%s/exit one", g_work);
    CHECK(make_mock_program(program, 1), "mock exit 1 non creato");
    PredFitState p = {0};
    CHECK_INT("processo fallisce", run_program(program, g_work, &p, "Mock program"), 0);
    CHECK(strstr(p.status, "exit 1") != NULL, "stato: %s", p.status);
    CHECK(strstr(p.status, "256") == NULL, "stato codificato: %s", p.status);
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
    add_or_update_assignment(s->assignments, &s->n_assignments, line, 3000.25, 2.0, 0);
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

/* T-17, T-25: Intensity analysis may refine its own displayed result, but a
   rejected fit must change nothing and a successful one must not write the
   active Pred&Fit species or mark its session dirty. */
static int test_intensity_fit_does_not_touch_predfit(void) {
    AppState *s = new_state();
    Point points[11];
    for (int i = 0; i < 11; i++) points[i] = (Point){(double)i, -1.0};
    PredLine lines[2] = {
        {.freq_mhz = 3.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 3, .Ju = 3, .mu = 'a'},
        {.freq_mhz = 7.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 3, .Ju = 7, .mu = 'a'},
    };
    s->current_pts = points; s->n_pts = 11;
    s->pred_lines = lines; s->n_pred = 2;
    s->assignments[0] = (Assignment){.pred = lines[0], .exp_freq = 3.0, .fit_enabled = 1};
    s->assignments[1] = (Assignment){.pred = lines[1], .exp_freq = 7.0, .fit_enabled = 1};
    s->n_assignments = 2;
    s->cat_temp_k = 10.0; s->rot_temp_k = 12.0;
    s->dipole_cat[0] = 2.0; s->dipole_red[0] = 0.0;
    s->intfit_fit_temperature = 0;
    s->intfit_fit_dipole[0] = 1;
    s->intfit_fit_dipole[1] = s->intfit_fit_dipole[2] = 0;
    s->predfit.temp_k = 77.0;
    s->predfit.mu[0] = 7.0; s->predfit.mu[1] = 8.0; s->predfit.mu[2] = 9.0;
    s->predfit.session_dirty = 0;

    CHECK_INT("fit con aree non positive rifiutato", intensity_fit_run(s), 0);
    CHECK_DBL("fit rifiutato non riempie mu red", s->dipole_red[0], 0.0, 1e-12);
    CHECK_DBL("fit rifiutato non tocca T Pred&Fit", s->predfit.temp_k, 77.0, 1e-12);
    CHECK_DBL("fit rifiutato non tocca mu Pred&Fit", s->predfit.mu[0], 7.0, 1e-12);
    CHECK_INT("fit rifiutato non sporca sessione", s->predfit.session_dirty, 0);

    for (int i = 0; i < 11; i++) points[i].y = 1.0;
    CHECK_INT("fit con due aree positive", intensity_fit_run(s), 1);
    CHECK_INT("risultato intensity disponibile", s->intfit_has_result, 1);
    CHECK_DBL("fit riuscito non tocca T Pred&Fit", s->predfit.temp_k, 77.0, 1e-12);
    CHECK_DBL("fit riuscito non tocca mu a Pred&Fit", s->predfit.mu[0], 7.0, 1e-12);
    CHECK_DBL("fit riuscito non tocca mu b Pred&Fit", s->predfit.mu[1], 8.0, 1e-12);
    CHECK_DBL("fit riuscito non tocca mu c Pred&Fit", s->predfit.mu[2], 9.0, 1e-12);
    CHECK_INT("fit riuscito non sporca sessione", s->predfit.session_dirty, 0);
    DONE();
}

/* T-18, R-12: the common display recalculation must keep every generated
   species' concentration.  Intensity-analysis controls are not allowed to
   collapse a generated multi-species catalogue into the active species. */
static int test_intensity_recompute_keeps_concentration(void) {
    AppState *s = new_state();
    PredLine lines[2] = {
        {.freq_mhz = 3000.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 4, .M1u = 0, .M1l = 0, .mu = 'a'},
        {.freq_mhz = 3001.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 4, .M1u = 1, .M1l = 1, .mu = 'a'},
    };
    s->pred_lines = lines; s->n_pred = 2;
    s->predfit.generated_catalog_active = 1;
    s->predfit.int_settings.temp_k = 10.0;
    s->predfit.n_species = 2;
    s->predfit.species[0] = (PickettSpecies){"A", 0, 1, {1.0, 1.0, 1.0}, 1.0};
    s->predfit.species[1] = (PickettSpecies){"B", 1, 1, {1.0, 1.0, 1.0}, 0.1};

    predfit_recompute_display_intensities(s);
    CHECK_DBL("rapporto iniziale delle concentrazioni", lines[1].linear_int / lines[0].linear_int, 0.1, 1e-9);

    /* These are the values edited by Intensity analysis for an external
       catalogue.  They must not route a generated multi-species catalogue
       through the old single-species rescaler. */
    s->rot_temp_k = 50.0;
    s->dipole_red[0] = 7.0;
    predfit_recompute_display_intensities(s);
    CHECK_DBL("rapporto conservato dopo T/mu intensity", lines[1].linear_int / lines[0].linear_int, 0.1, 1e-9);
    DONE();
}

/* PF-02: concentration is an external lower-state population multiplier.
   This matters only for a non-diagonal transition; diagonal rotational rows
   retain the expected state concentration. */
static int test_interstate_line_uses_lower_state_concentration(void) {
    AppState *s = new_state();
    PredLine lines[2] = {
        {.freq_mhz = 3000.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 4,
         .M1u = 1, .M1l = 0, .mu = 'a'},
        {.freq_mhz = 3000.0, .cat_lgint = 0.0, .rot_dof = 3, .n_qn = 4,
         .M1u = 1, .M1l = 1, .mu = 'a'},
    };
    s->pred_lines = lines; s->n_pred = 2;
    s->predfit.generated_catalog_active = 1;
    s->predfit.temp_k = 10.0;
    s->predfit.n_species = 2;
    s->predfit.species[0] = (PickettSpecies){"state 0", 0, 1, {1, 1, 1}, 0.25};
    s->predfit.species[1] = (PickettSpecies){"state 1", 1, 1, {1, 1, 1}, 9.0};
    predfit_recompute_display_intensities(s);
    CHECK_DBL("transizione inter-stato pesa lo stato inferiore",
              lines[0].linear_int / lines[1].linear_int, 0.25 / 9.0, 1e-12);
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
    {"test_peakfinder_baseline_invariant",    test_peakfinder_baseline_invariant},
    {"test_peakfinder_noise_window_used",     test_peakfinder_noise_window_used},
    {"test_peakfinder_width_bounds",          test_peakfinder_width_bounds},
    {"test_find_peaks_respects_offset",       test_find_peaks_respects_offset},
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
    {"test_multistate_writes_one_int_with_shared_trot", test_multistate_writes_one_int_with_shared_trot},
    {"test_hamiltonian_switch_keeps_independent_models", test_hamiltonian_switch_keeps_independent_models},
    {"test_multihamiltonian_session_roundtrip", test_multihamiltonian_session_roundtrip},
    {"test_two_hamiltonians_keep_independent_int_controls", test_two_hamiltonians_keep_independent_int_controls},
    {"test_import_load_dir_builds_hamiltonians", test_import_load_dir_builds_hamiltonians},
    {"test_import_twice_keeps_one_copy", test_import_twice_keeps_one_copy},
    {"test_import_reads_predictions_from_the_catalog", test_import_reads_predictions_from_the_catalog},
    {"test_simulation_plots_every_checked_hamiltonian", test_simulation_plots_every_checked_hamiltonian},
    {"test_dipole_checkbox_zeroes_the_int", test_dipole_checkbox_zeroes_the_int},
    {"test_simulated_intensities_follow_their_own_model", test_simulated_intensities_follow_their_own_model},
    {"test_fit_calculates_the_missing_catalogue", test_fit_calculates_the_missing_catalogue},
    {"test_simulation_supersedes_a_queued_catalog", test_simulation_supersedes_a_queued_catalog},
    {"test_species_traces_split_the_broadened_profile", test_species_traces_split_the_broadened_profile},
    {"test_species_trace_colour_default_and_session", test_species_trace_colour_default_and_session},
    {"test_default_species_colours_are_distinct", test_default_species_colours_are_distinct},
    {"test_broadening_panel_selects_the_traces", test_broadening_panel_selects_the_traces},
    {"test_session_saved_only_on_request", test_session_saved_only_on_request},
    {"test_sidebar_reorder_keeps_identity", test_sidebar_reorder_keeps_identity},
    {"test_assignment_owner_keeps_same_qn_in_two_hamiltonians", test_assignment_owner_keeps_same_qn_in_two_hamiltonians},
    {"test_project_rows_are_hamiltonian_state_hierarchy", test_project_rows_are_hamiltonian_state_hierarchy},
    {"test_add_hamiltonian_starts_fresh_and_keeps_source", test_add_hamiltonian_starts_fresh_and_keeps_source},
    {"test_delete_hamiltonian_removes_its_assignments_and_keeps_one", test_delete_hamiltonian_removes_its_assignments_and_keeps_one},
    {"test_nvib_too_small_rejected",           test_nvib_too_small_rejected},
    {"test_option_line_other_tokens_kept",     test_option_line_other_tokens_kept},
    {"test_param_id_zero_or_duplicate_rejected", test_param_id_zero_or_duplicate_rejected},
    {"test_calculate_requires_abc",            test_calculate_requires_abc},
    {"test_restore_defaults_keeps_paths",      test_restore_defaults_keeps_paths},
    {"test_line_error_persists",               test_line_error_persists},
    {"test_calculate_after_fit_keeps_fitted_var", test_calculate_after_fit_keeps_fitted_var},
    {"test_exports_go_to_data_dir",            test_exports_go_to_data_dir},
    {"test_spaces_and_quotes_in_data_dir",     test_spaces_and_quotes_in_data_dir},
    {"test_process_exit_status_decoded",       test_process_exit_status_decoded},
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
    {"test_intensity_fit_does_not_touch_predfit", test_intensity_fit_does_not_touch_predfit},
    {"test_intensity_recompute_keeps_concentration", test_intensity_recompute_keeps_concentration},
    {"test_interstate_line_uses_lower_state_concentration", test_interstate_line_uses_lower_state_concentration},
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
