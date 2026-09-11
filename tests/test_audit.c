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

/* Copy of the pending-load pump of main.c:514-522 and 530. */
static void pump(AppState *s) {
    if (s->pending_session_load) { s->pending_session_load = 0; reopen_predfit_session(s); }
    if (s->pending_load) {
        s->pending_load = 0;
        if (s->pending_pred_path[0]) { set_predictions(s, s->pending_pred_path); predfit_adopt_generated_catalog(s); s->pending_pred_path[0] = '\0'; }
        if (s->pending_spec_path[0]) { add_spectrum(s, s->pending_spec_path); s->pending_spec_path[0] = '\0'; predfit_save_session(s); }
    }
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

/* ================================================================ runner */
typedef struct { const char *name; int (*fn)(void); } Test;

static const Test TESTS[] = {
    {"test_baseline_cat1404_nqn4",             test_baseline_cat1404_nqn4},
    {"test_baseline_roundtrip_qnfmt1404",      test_baseline_roundtrip_qnfmt1404},
    {"test_baseline_reassign_updates_obsfreq", test_baseline_reassign_updates_obsfreq},
    {"test_baseline_right_drag_ascending",     test_baseline_right_drag_ascending},
    {"test_baseline_calculate_single_species", test_baseline_calculate_single_species},
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
