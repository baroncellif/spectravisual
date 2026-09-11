/*
 * Audit reproduction harness (isolated, scratch only).
 *
 * The real application translation units are #included so that their static
 * functions (assign_selected_predictions, handle_mouse_down, write_inputs,
 * advanced_commit_edit, import_fit_lines, ...) run exactly as in the app.
 * Nothing is rendered.  Every scenario chdir()s into its own scratch work
 * directory and sets settings.data_dir there, so no repository file is read
 * for writing.  SPCAT/SPFIT are run only inside those scratch directories.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define main sv_app_main
#include "main.c"
#undef main
#include "controller.c"
#include "predfit.c"

static const char *ARGV0;
#define WIN_W 1400
#define WIN_H 884

/* ------------------------------------------------------------------ utils */
static void banner(const char *t) { printf("\n==================== %s ====================\n", t); }

static void cat_file(const char *path, int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("  (missing %s)\n", path); return; }
    char line[1024]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (max_lines > 0 && n >= max_lines) { printf("  | ...\n"); break; }
        printf("  | %s", line); n++;
        if (!strchr(line, '\n')) printf("\n");
    }
    fclose(f);
}

static void print_pred(const char *tag, const PredLine *p) {
    printf("%s f=%.4f n_qn=%d U=(%d %d %d %d %d %d) L=(%d %d %d %d %d %d) lin=%.4e\n", tag,
           p->freq_mhz, p->n_qn, p->Ju, p->Kau, p->Kcu, p->M1u, p->M2u, p->M3u,
           p->Jl, p->Kal, p->Kcl, p->M1l, p->M2l, p->M3l, p->linear_int);
}

static void print_assignments(const AppState *s, const char *title) {
    printf("-- %s: n_assignments=%d\n", title, s->n_assignments);
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        char tag[96];
        snprintf(tag, sizeof tag, "  [%d] exp=%.4f exp_int=%.3e fit=%d pred:", i, a->exp_freq, a->exp_int, a->fit_enabled);
        print_pred(tag, &a->pred);
    }
}

static AppState *new_state(const char *data_dir) {
    AppState *s = calloc(1, sizeof(AppState));
    init_app_defaults(s);
    settings_init(s, ARGV0);
    settings_apply_defaults(s);
    if (data_dir) snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", data_dir);
    return s;
}

/* Verbatim copy of main.c:469-511 (layout) — main() is not callable. */
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

/* Verbatim copy of the pending-load pump, main.c:514-522 and 530. */
static void pump(AppState *s) {
    if (s->pending_session_load) { s->pending_session_load = 0; reopen_predfit_session(s); }
    if (s->pending_load) {
        s->pending_load = 0;
        if (s->pending_pred_path[0]) { set_predictions(s, s->pending_pred_path); predfit_adopt_generated_catalog(s); s->pending_pred_path[0] = '\0'; }
        if (s->pending_spec_path[0]) { add_spectrum(s, s->pending_spec_path); s->pending_spec_path[0] = '\0'; predfit_save_session(s); }
    }
    commit_active(s);
}

static void mouse_down(AppState *s, Layout *L, int x, int y, int button) {
    SDL_MouseButtonEvent b; memset(&b, 0, sizeof b);
    b.type = SDL_MOUSEBUTTONDOWN; b.button = (Uint8)button; b.x = x; b.y = y;
    handle_mouse_down(s, L, &b);
}

/* Real click on the prediction pane (controller.c:669-694). */
static int click_select(AppState *s, double freq) {
    Layout L;
    s->pvxmin = freq - 0.5; s->pvxmax = freq + 0.5;
    compute_layout(s, &L);
    int mx = L.pred_x + (int)lround((freq - s->pvxmin) / (s->pvxmax - s->pvxmin) * L.pred_w);
    mouse_down(s, &L, mx, L.pred_y + L.pred_h / 2, SDL_BUTTON_LEFT);
    return s->n_selected;
}

/* Real "Save all" button (controller.c:283-319). */
static void click_save_all(AppState *s) {
    Layout L;
    s->win_as.visible = 1;
    compute_layout(s, &L);
    SDL_Rect r = ui_as_save(s, s->win_as.rect);
    mouse_down(s, &L, r.x + r.w / 2, r.y + r.h / 2, SDL_BUTTON_LEFT);
}

/* Gaussian peaks (sigma 0.03 MHz) on windows of +-1.5 MHz, step 0.002 MHz. */
static int cmpd(const void *a, const void *b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static void write_spectrum(const char *path, const double *pk, const double *ht, int n) {
    double *c = malloc(sizeof(double) * n); memcpy(c, pk, sizeof(double) * n);
    qsort(c, n, sizeof(double), cmpd);
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
    fclose(f); free(c);
}

static void enter(const char *work) {
    mkdir(work, 0700);
    if (chdir(work) != 0) { perror(work); exit(2); }
}

static int find_line(const AppState *s, int ju, int kau, int kcu, int jl, int kal, int kcl, int m1u) {
    for (int i = 0; i < s->n_pred; i++) {
        const PredLine *p = &s->pred_lines[i];
        if (p->Ju == ju && p->Kau == kau && p->Kcu == kcu && p->Jl == jl && p->Kal == kal && p->Kcl == kcl &&
            (m1u < 0 || p->M1u == m1u)) return i;
    }
    return -1;
}

static void assign_index(AppState *s, int idx, double exp_freq, double exp_int) {
    s->selected_indices[0] = idx; s->n_selected = 1;          /* = result of controller.c:681-693 */
    assign_selected_predictions(s, exp_freq, exp_int);        /* controller.c:836-852 */
}

static void set_param(PredFitState *p, int id, double v, double err) {
    for (int i = 0; i < p->n_param; i++) if (p->param[i].id == id) { p->param[i].value = v; p->param[i].error = err; return; }
    if (p->n_param < MAX_PICKETT_PARAMS) {
        p->param[p->n_param] = (PickettParameter){id, v, err, ""};
        parameter_label(&p->param[p->n_param]);
        p->n_param++;
    }
}

/* ------------------------------------------------------------ scenarios */
static int sc_qnfmt(int argc, char **argv) {
    for (int a = 0; a < argc; a++) {
        PredLine *L = NULL; double x0, x1, gm;
        int n = read_pred_cat_alloc(argv[a], &L, &x0, &x1, &gm);
        banner(argv[a]);
        printf("read_pred_cat_alloc -> %d lines\n", n);
        int hist[10][8]; memset(hist, 0, sizeof hist);
        for (int i = 0; i < n; i++) {
            int b = L[i].Ju / 10; if (b < 0) b = 0; if (b > 9) b = 9;
            int q = L[i].n_qn; if (q < 0) q = 0; if (q > 7) q = 7;
            hist[b][q]++;
        }
        printf("  Ju bucket |  n_qn=0   1    2    3    4    5    6\n");
        for (int b = 0; b < 10; b++) {
            int tot = 0; for (int q = 0; q < 8; q++) tot += hist[b][q];
            if (!tot) continue;
            printf("  %2d..%2d    | %5d %4d %4d %4d %4d %4d %4d\n", b * 10, b * 10 + 9,
                   hist[b][0], hist[b][1], hist[b][2], hist[b][3], hist[b][4], hist[b][5], hist[b][6]);
        }
        int show = n < 12 ? n : 0;
        for (int i = 0; i < show; i++) print_pred("  line", &L[i]);
        for (int i = 0; i < n; i++) if (fabs(L[i].freq_mhz - 5999.2867) < 1e-4) print_pred("  5999.2867:", &L[i]);
        free(L);
    }
    return 0;
}

static int sc_roundtrip(const char *cat, const char *work, int nf, double *freqs) {
    banner("ROUNDTRIP: CAT -> select -> peak -> assignment -> Save all -> restart");
    printf("catalogue: %s\nwork/data_dir: %s\n", cat, work);
    enter(work);
    char spec[800], asg[800];
    snprintf(spec, sizeof spec, "%s/spectrum.txt", work);
    snprintf(asg, sizeof asg, "%s/assignments.txt", work);
    unlink(asg);
    double pk[64];
    for (int i = 0; i < nf; i++) pk[i] = freqs[i] - 0.03;          /* obs - calc = -0.03 MHz */
    write_spectrum(spec, pk, NULL, nf);

    AppState *s = new_state(work);
    set_predictions(s, cat);
    add_spectrum(s, spec);
    for (int i = 0; i < nf; i++) {
        int n = click_select(s, freqs[i]);
        printf("click at %.4f MHz -> n_selected=%d\n", freqs[i], n);
        for (int k = 0; k < n; k++) print_pred("   selected", &s->pred_lines[s->selected_indices[k]]);
        run_right_click_peak_find(s, pk[i] - 0.2, pk[i] + 0.2);    /* controller.c:784-834 */
    }
    print_assignments(s, "in memory (before Save all)");
    click_save_all(s);
    printf("-- %s written by the real Save-all handler:\n", asg);
    cat_file(asg, 0);

    AppState *r = new_state(work);
    set_predictions(r, cat);                  /* -> ensure_aux_loaded -> load_existing_assignments */
    print_assignments(r, "after restart (set_predictions -> ensure_aux_loaded)");
    int lost = 0;
    for (int i = 0; i < s->n_assignments; i++) {
        const PredLine *a = &s->assignments[i].pred;
        int found = 0;
        for (int k = 0; k < r->n_assignments; k++) {
            const PredLine *b = &r->assignments[k].pred;
            if (a->Ju == b->Ju && a->Kau == b->Kau && a->Kcu == b->Kcu && a->M1u == b->M1u && a->M2u == b->M2u && a->M3u == b->M3u &&
                a->Jl == b->Jl && a->Kal == b->Kal && a->Kcl == b->Kcl && a->M1l == b->M1l && a->M2l == b->M2l && a->M3l == b->M3l) found = 1;
        }
        if (!found) { lost++; print_pred("   NOT RESTORED IDENTICALLY:", a); }
    }
    printf("RESULT: %d in memory, %d restored, %d transitions not restored identically\n",
           s->n_assignments, r->n_assignments, lost);
    return 0;
}

static int sc_nvib(const char *work) {
    banner("NVIB: third .par line forced to the species count");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    printf("init                     : '%s'  (state_count=%d)\n", p->hamiltonian_line, state_count(p));
    p->advanced_edit_param = -4; snprintf(p->advanced_edit_buf, sizeof p->advanced_edit_buf, "s 1 2 0");
    advanced_commit_edit(p);
    printf("1 species, typed 's 1 2 0': '%s'\n", p->hamiltonian_line);
    add_species(s); printf("after + species (2)      : '%s'  (state_count=%d)\n", p->hamiltonian_line, state_count(p));
    add_species(s); printf("after + species (3)      : '%s'  (state_count=%d)\n", p->hamiltonian_line, state_count(p));
    const char *tries[] = {"s 1 1 0", "s 1 5 0", "a 1 1 0", "s 1 1", "s 0 1 0 0 1", "s,1,1,0"};
    for (unsigned t = 0; t < sizeof tries / sizeof tries[0]; t++) {
        p->advanced_edit_param = -4;
        snprintf(p->advanced_edit_buf, sizeof p->advanced_edit_buf, "%s", tries[t]);
        advanced_commit_edit(p);
        printf("3 species, typed %-12s -> '%s'\n", tries[t], p->hamiltonian_line);
    }
    write_inputs(s, 0);
    char par[800]; snprintf(par, sizeof par, "%s/.fit/model.par", work);
    printf("-- model.par written by write_inputs (first 3 lines):\n"); cat_file(par, 3);

    /* the user edits the session / .par by hand, then restarts */
    char st[800]; snprintf(st, sizeof st, "%s/.fit/spectravisual.state", work);
    FILE *f = fopen(st, "r"); char buf[65536]; size_t nb = f ? fread(buf, 1, sizeof buf - 1, f) : 0; if (f) fclose(f); buf[nb] = 0;
    char *h = strstr(buf, "hamiltonian ");
    char out[70000]; out[0] = 0;
    if (h) {
        char *e = strchr(h, '\n');
        snprintf(out, sizeof out, "%.*shamiltonian s 1 1 0%s", (int)(h - buf), buf, e ? e : "");
        f = fopen(st, "w"); fputs(out, f); fclose(f);
    }
    printf("-- session edited by hand; hamiltonian line now:\n");
    { FILE *g = fopen(st, "r"); char l[512]; while (g && fgets(l, sizeof l, g)) if (!strncmp(l, "hamiltonian", 11)) printf("  | %s", l); if (g) fclose(g); }
    /* and the .par edited by hand (third line NVIB=1) */
    FILE *pf = fopen(par, "r"); char pb[65536]; size_t pn = pf ? fread(pb, 1, sizeof pb - 1, pf) : 0; if (pf) fclose(pf); pb[pn] = 0;
    char *l3 = pb; for (int k = 0; k < 2 && l3; k++) { l3 = strchr(l3, '\n'); if (l3) l3++; }
    if (l3) { char *e3 = strchr(l3, '\n'); char po[70000]; snprintf(po, sizeof po, "%.*ss 1 1 0%s", (int)(l3 - pb), pb, e3 ? e3 : ""); pf = fopen(par, "w"); fputs(po, pf); fclose(pf); }
    printf("-- model.par edited by hand (first 3 lines):\n"); cat_file(par, 3);
    AppState *r = new_state(work);
    predfit_load_session(r);
    printf("after predfit_load_session (session says NVIB=1, 3 molecule2 rows): '%s'\n", r->predfit.hamiltonian_line);
    /* a Calculate would now rewrite model.par from memory */
    write_inputs(r, 0);
    printf("-- model.par after the next Calculate/Fit (write_inputs):\n"); cat_file(par, 3);
    return 0;
}

/* Undo restores the fit flags by list index, not by transition. */
static int sc_undo(const char *work) {
    banner("UNDO: fit flags restored by index after the list changed");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368; p->fmax_ghz = 8.0; p->temp_k = 2.0;
    sync_basic_parameters(p);
    predfit_calculate(s); pump(s);
    int picks[][6] = {{4,1,4,3,1,3},{4,0,4,3,0,3},{5,1,5,4,1,4},{6,1,6,5,1,5},{7,1,7,6,1,6}};
    for (int k = 0; k < 5; k++) {
        int idx = find_line(s, picks[k][0], picks[k][1], picks[k][2], picks[k][3], picks[k][4], picks[k][5], -1);
        if (idx >= 0) assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    s->assignments[1].fit_enabled = 0;                    /* Lines tab: exclude 4 0 4 - 3 0 3 */
    print_assignments(s, "before Fit (row 1 excluded)");
    int ok = predfit_fit(s); pump(s);
    printf("Fit -> %d  history_count=%d  status: %s\n", ok, p->history_count, p->status);
    delete_assignment(s, 0);                              /* Delete selected on row 0 */
    print_assignments(s, "after deleting row 0");
    ok = predfit_undo_last_fit(s); pump(s);
    printf("Undo -> %d  history_count=%d  status: %s\n", ok, p->history_count, p->status);
    print_assignments(s, "after Undo");
    return 0;
}

/* The 90000 MHz exclusion relies on ERRTST=1e6 in the .par. */
static int sc_sentinel(const char *work, double line_error) {
    char title[128]; snprintf(title, sizeof title, "SENTINEL: excluded row with .lin uncertainty %.3f MHz", line_error);
    banner(title);
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368; p->fmax_ghz = 8.0; p->temp_k = 2.0;
    p->line_error_mhz = line_error;
    sync_basic_parameters(p);
    predfit_calculate(s); pump(s);
    int picks[][6] = {{4,1,4,3,1,3},{4,0,4,3,0,3},{5,1,5,4,1,4},{6,1,6,5,1,5},{7,1,7,6,1,6},{5,0,5,4,0,4}};
    for (int k = 0; k < 6; k++) {
        int idx = find_line(s, picks[k][0], picks[k][1], picks[k][2], picks[k][3], picks[k][4], picks[k][5], -1);
        if (idx >= 0) assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    s->assignments[2].fit_enabled = 0;
    int ok = predfit_fit(s);
    printf("Fit -> %d  status: %s\n", ok, p->status);
    char lin[800], fit[800];
    snprintf(lin, sizeof lin, "%s/.fit/model.lin", work);
    snprintf(fit, sizeof fit, "%s/.fit/model.fit", work);
    printf("-- model.lin:\n"); cat_file(lin, 0);
    FILE *ff = fopen(fit, "r"); char l2[512]; int last_block = 0;
    while (ff && fgets(l2, sizeof l2, ff)) {
        if (strstr(l2, "NOT USED") || strstr(l2, "MICROWAVE RMS") || strstr(l2, "END OF ITERATION")) printf("  ! %s", l2);
        int nn; if (sscanf(l2, " %d:", &nn) == 1 && nn == 3 && strlen(l2) > 40) { printf("  | %s", l2); last_block++; }
    }
    if (ff) fclose(ff);
    printf("A after fit: %.6f  (start 1151.360417)\n", p->a);
    return 0;
}

/* Restore clobbers the active species with the state-0 dipoles and Tcat. */
static int sc_restore_int(const char *work, const char *catfile) {
    banner("RESTORE of .int: auto fields and active-species values");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368; p->fmax_ghz = 8.0;
    p->temp_k = 2.0; p->mu[0] = 0.75; p->mu[1] = 0.21; p->mu[2] = 1.14;
    sync_basic_parameters(p);
    add_species(s);                                    /* active becomes species 1 */
    p->temp_k = 5.0; p->mu[0] = 0.4; p->mu[1] = 0.3; p->mu[2] = 0.5; store_active_species(p);
    p->int_settings.temp_k = 1.0;                      /* explicit TCAT = 1 K */
    printf("before: active=%d int{temp=%.3g fqlim=%.3g maxv=%d}\n", p->active_species,
           p->int_settings.temp_k, p->int_settings.fqlim_ghz, p->int_settings.maxv);
    for (int i = 0; i < p->n_species; i++)
        printf("  species %d '%s' v=%d T=%.3f mu=(%.3f %.3f %.3f) conc=%.3f\n", i, p->species[i].name, p->species[i].state_index,
               p->species[i].temp_k, p->species[i].mu[0], p->species[i].mu[1], p->species[i].mu[2], p->species[i].concentration);
    write_inputs(s, 0);
    char dst[800], cmd[2000]; snprintf(dst, sizeof dst, "%s/.fit/model.cat", work);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", catfile, dst); system(cmd);
    char in[800]; snprintf(in, sizeof in, "%s/.fit/model.int", work);
    printf("-- model.int:\n"); cat_file(in, 0);

    AppState *r = new_state(work);
    predfit_load_session(r);
    predfit_restore_latest(r);
    PredFitState *q = &r->predfit;
    printf("after restart: active=%d int{temp=%.3g fqlim=%.3g maxv=%d} p->temp=%.3f p->mu=(%.3f %.3f %.3f)\n", q->active_species,
           q->int_settings.temp_k, q->int_settings.fqlim_ghz, q->int_settings.maxv, q->temp_k, q->mu[0], q->mu[1], q->mu[2]);
    for (int i = 0; i < q->n_species; i++)
        printf("  species %d '%s' v=%d T=%.3f mu=(%.3f %.3f %.3f) conc=%.3f\n", i, q->species[i].name, q->species[i].state_index,
               q->species[i].temp_k, q->species[i].mu[0], q->species[i].mu[1], q->species[i].mu[2], q->species[i].concentration);
    add_species(r);
    printf("after adding a 3rd species: state_count=%d  MAXV written to .int = %d\n", state_count(q), int_maxv_at(q));
    return 0;
}

static int sc_predfit_mixed(const char *work, const char *extcat, const char *ext_hint) {
    banner("PRED&FIT consumes assignments from an external 3-QN CAT with NVIB forced to 3");
    (void)ext_hint;
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    printf("spcat=%s\nspfit=%s\n", s->settings.spcat_path, s->settings.spfit_path);
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368;
    p->fmin_ghz = 0; p->fmax_ghz = 8.0; p->temp_k = 2.0;
    p->mu[0] = 0.75; p->mu[1] = 0.21; p->mu[2] = 1.14;
    sync_basic_parameters(p);
    set_param(p, 10000, p->a, 1.0); set_param(p, 20000, p->b, 1.0); set_param(p, 30000, p->c, 1.0);
    add_species(s);
    set_param(p, 10011, 907.5172, 0.0); set_param(p, 20011, 272.5706, 0.0); set_param(p, 30011, 250.97429, 0.0);
    add_species(s);
    set_param(p, 10022, 1100.0, 0.0); set_param(p, 20022, 300.0, 0.0); set_param(p, 30022, 280.0, 0.0);
    select_species(s, 0);
    p->species[1].concentration = 0.1; p->species[2].concentration = 0.01;
    printf("hamiltonian line: '%s', species=%d, active=%d\n", p->hamiltonian_line, p->n_species, p->active_species);
    int ok = predfit_calculate(s);
    printf("Calculate -> %d, status: %s\n", ok, p->status);
    pump(s);
    printf("model.cat loaded: n_pred=%d, pred_path=%s, generated_active=%d\n", s->n_pred, s->pred_path, p->generated_catalog_active);
    int h4 = 0, h3 = 0, hx = 0;
    for (int i = 0; i < s->n_pred; i++) { if (s->pred_lines[i].n_qn == 4) h4++; else if (s->pred_lines[i].n_qn == 3) h3++; else hx++; }
    printf("model.cat n_qn: 4->%d 3->%d other->%d\n", h4, h3, hx);
    char mc[800]; snprintf(mc, sizeof mc, "%s/.fit/model.cat", work);
    cat_file(mc, 3);

    /* four assignments from model.cat, state 0, QNs present in pred.cat too */
    int picks[][6] = {{4,1,4,3,1,3},{4,0,4,3,0,3},{5,1,5,4,1,4},{6,1,6,5,1,5}};
    for (int k = 0; k < 4; k++) {
        int idx = find_line(s, picks[k][0], picks[k][1], picks[k][2], picks[k][3], picks[k][4], picks[k][5], 0);
        if (idx < 0) { printf("  model.cat line %d not found\n", k); continue; }
        assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    /* then the external catalogue (3 QN, QNFMT 303) */
    set_predictions(s, extcat);
    printf("external CAT loaded: n_pred=%d, generated_active=%d\n", s->n_pred, p->generated_catalog_active);
    int ext[][6] = {{8,2,7,7,2,6},{9,1,9,8,1,8},{11,1,11,10,1,10},{12,1,12,11,1,11}};
    for (int k = 0; k < 4; k++) {
        int idx = find_line(s, ext[k][0], ext[k][1], ext[k][2], ext[k][3], ext[k][4], ext[k][5], -1);
        if (idx < 0) { printf("  ext line %d not found\n", k); continue; }
        assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    print_assignments(s, "assignments handed to Fit");
    ok = predfit_fit(s);
    printf("Fit -> %d, status: %s\n", ok, p->status);
    char lin[800], fit[800];
    snprintf(lin, sizeof lin, "%s/.fit/model.lin", work);
    snprintf(fit, sizeof fit, "%s/.fit/model.fit", work);
    printf("-- model.lin written by write_inputs:\n"); cat_file(lin, 0);
    printf("-- model.fit observation echo (how SPFIT read each row):\n");
    FILE *ff = fopen(fit, "r"); char l2[512];
    while (ff && fgets(l2, sizeof l2, ff)) {
        int nn; if (sscanf(l2, " %d:", &nn) == 1 && strchr(l2, ':') && strlen(l2) > 40) printf("  | %s", l2);
        if (strstr(l2, "Bad") || strstr(l2, "ERROR") || strstr(l2, "error") || strstr(l2, "QUANTUM")) printf("  ! %s", l2);
    }
    if (ff) fclose(ff);
    pump(s);
    printf("after Fit: pred_path=%s n_pred=%d\n", s->pred_path, s->n_pred);
    return 0;
}

static int sc_restore(const char *work, const char *cwd, const char *cat) {
    banner("RESTORE: which file rebuilds the assignment list, by launch mode");
    mkdir(work, 0700);
    enter(cwd);
    char asg_cwd[800]; snprintf(asg_cwd, sizeof asg_cwd, "%s/assignments.txt", cwd); unlink(asg_cwd);
    AppState *s = new_state(work);
    set_predictions(s, cat);
    for (int i = 0; i < s->n_pred && i < 3; i++) assign_index(s, i, s->pred_lines[i].freq_mhz + 0.001, 1.0);
    s->assignments[1].fit_enabled = 0;                         /* excluded from the fit */
    click_save_all(s);
    write_inputs(s, 1);                                        /* model.lin + session, as Fit does */
    char dst[800], cmd[2000]; snprintf(dst, sizeof dst, "%s/.fit/model.cat", work);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", cat, dst); system(cmd);
    printf("data_dir=%s  CWD=%s\n", work, cwd);
    printf("-- data_dir/assignments.txt:\n"); { char a[800]; snprintf(a, sizeof a, "%s/assignments.txt", work); cat_file(a, 0); }
    printf("-- data_dir/.fit/model.lin:\n"); { char a[800]; snprintf(a, sizeof a, "%s/.fit/model.lin", work); cat_file(a, 0); }

    /* launch 1: `spectravisual` without a .cat (main.c:400-401) */
    AppState *r1 = new_state(work);
    predfit_load_session(r1);
    int restored = predfit_restore_latest(r1);
    pump(r1);
    printf("launch WITHOUT .cat: restore=%d, pred_path=%s\n", restored, r1->pred_path);
    print_assignments(r1, "  list rebuilt by import_fit_lines (data_dir assignments.txt + model.lin)");
    /* launch 2: `spectravisual <cat>` */
    AppState *r2 = new_state(work);
    predfit_load_session(r2);
    set_predictions(r2, cat);
    print_assignments(r2, "launch WITH .cat: list from data_dir/assignments.txt (ensure_aux_loaded)");
    return 0;
}

static int sc_intfit(const char *work) {
    banner("INTENSITY FIT side effects on a generated multi-species catalogue");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368;
    p->fmax_ghz = 8.0; p->temp_k = 2.0; p->mu[0] = 0.75; p->mu[1] = 0.21; p->mu[2] = 1.14;
    sync_basic_parameters(p);
    add_species(s);
    set_param(p, 10011, 907.5172, 0.0); set_param(p, 20011, 272.5706, 0.0); set_param(p, 30011, 250.97429, 0.0);
    select_species(s, 0);
    p->species[1].concentration = 0.1;
    p->species[1].mu[0] = 0.4; p->species[1].mu[1] = 0.3; p->species[1].mu[2] = 0.5;
    predfit_calculate(s); pump(s);
    printf("generated catalogue: n_pred=%d generated_active=%d cat_temp_k=%.3f rot_temp_k=%.3f\n",
           s->n_pred, p->generated_catalog_active, s->cat_temp_k, s->rot_temp_k);
    int i1 = -1;  /* a state-1 (species 1) line to watch */
    for (int i = 0; i < s->n_pred; i++) if (s->pred_lines[i].M1u == 1 && s->pred_lines[i].n_qn == 4) { i1 = i; break; }
    double f_watch = i1 >= 0 ? s->pred_lines[i1].freq_mhz : 0;
    printf("watch species-1 line f=%.4f: lin=%.4e (cat_lgint=%.4f, conc=%.2f)\n", f_watch,
           i1 >= 0 ? s->pred_lines[i1].linear_int : 0, i1 >= 0 ? s->pred_lines[i1].cat_lgint : 0, p->species[1].concentration);

    /* six assignments of species 0 with peaks following the model */
    double pk[8], ht[8]; int na = 0;
    for (int i = 0; i < s->n_pred && na < 6; i++) {
        PredLine *q = &s->pred_lines[i];
        if (q->M1u != 0 || q->freq_mhz < 2000 || q->lgint < -6) continue;
        pk[na] = q->freq_mhz + 0.002; ht[na] = q->linear_int / s->pred_global_max; na++;
    }
    char spec[800]; snprintf(spec, sizeof spec, "%s/spectrum.txt", work);
    write_spectrum(spec, pk, ht, na);
    add_spectrum(s, spec);
    for (int k = 0; k < na; k++) {
        for (int i = 0; i < s->n_pred; i++) if (fabs(s->pred_lines[i].freq_mhz - (pk[k] - 0.002)) < 1e-6) { assign_index(s, i, pk[k], ht[k]); break; }
    }
    printf("assignments: %d\n", s->n_assignments);
    printf("BEFORE Run fit: dipole_cat=(%.3f %.3f %.3f) dipole_red=(%.3f %.3f %.3f) rot=%.3f  species0 mu=(%.3f %.3f %.3f) T=%.3f\n",
           s->dipole_cat[0], s->dipole_cat[1], s->dipole_cat[2], s->dipole_red[0], s->dipole_red[1], s->dipole_red[2], s->rot_temp_k,
           p->species[0].mu[0], p->species[0].mu[1], p->species[0].mu[2], p->species[0].temp_k);
    /* real "Run fit" button, controller.c:438-446 */
    s->win_dip.visible = 1;
    Layout L; compute_layout(s, &L);
    SDL_Rect run = ui_fit_run(s->win_dip.rect);
    mouse_down(s, &L, run.x + run.w / 2, run.y + run.h / 2, SDL_BUTTON_LEFT);
    printf("intfit message: %s\n", s->intfit_message);
    printf("AFTER  Run fit: dipole_red=(%.3f %.3f %.3f) rot=%.3f  species0 mu=(%.3f %.3f %.3f) T=%.3f  p->temp_k=%.3f\n",
           s->dipole_red[0], s->dipole_red[1], s->dipole_red[2], s->rot_temp_k,
           p->species[0].mu[0], p->species[0].mu[1], p->species[0].mu[2], p->species[0].temp_k, p->temp_k);
    if (i1 >= 0) {
        int j = -1; for (int i = 0; i < s->n_pred; i++) if (fabs(s->pred_lines[i].freq_mhz - f_watch) < 1e-6 && s->pred_lines[i].M1u == 1) j = i;
        printf("watch species-1 line after Run fit: lin=%.4e  (concentration %.2f still applied? ratio to cat base=%.4e)\n",
               s->pred_lines[j].linear_int, p->species[1].concentration, s->pred_lines[j].linear_int / pow(10, s->pred_lines[j].cat_lgint));
    }
    /* T rot typed in the command bar, controller.c:922-933 */
    s->input_state = INPUT_ROT_TEMP; snprintf(s->text_input_buf, sizeof s->text_input_buf, "2.0");
    commit_text_input(s);
    if (i1 >= 0) {
        int j = -1; for (int i = 0; i < s->n_pred; i++) if (fabs(s->pred_lines[i].freq_mhz - f_watch) < 1e-6 && s->pred_lines[i].M1u == 1) j = i;
        printf("after typing T rot=2.0 in the top bar: species-1 line lin=%.4e ratio=%.4e\n",
               s->pred_lines[j].linear_int, s->pred_lines[j].linear_int / pow(10, s->pred_lines[j].cat_lgint));
    }
    /* the same temperature through the Pred&Fit panel, controller.c:952-960 */
    s->input_state = INPUT_PF_TEMP; snprintf(s->text_input_buf, sizeof s->text_input_buf, "2.0");
    commit_text_input(s);
    if (i1 >= 0) {
        int j = -1; for (int i = 0; i < s->n_pred; i++) if (fabs(s->pred_lines[i].freq_mhz - f_watch) < 1e-6 && s->pred_lines[i].M1u == 1) j = i;
        printf("after typing T rot=2.0 in Pred&Fit panel : species-1 line lin=%.4e ratio=%.4e\n",
               s->pred_lines[j].linear_int, s->pred_lines[j].linear_int / pow(10, s->pred_lines[j].cat_lgint));
    }
    return 0;
}

static int sc_formats(const char *work, const char *backup) {
    banner("FORMATS: what load_existing_assignments makes of each accepted layout");
    enter(work);
    struct { const char *name, *text; } cases[] = {
        {"new_nqn4", " 12  2 10  2 11  2  9  2                        6033.589400     6033.590000    1.000000E-03 4\n"},
        {"new_nqn1_bug", " 11 10                                  5999.246095     5999.286700    1.364897E-04 1\n"},
        {"legacy16_nqn3", "  2511.3375    4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   1.0000e-05 3\n"},
        {"legacy16_nqn6", "  3300.0000    2   1   1   3   4   5   1   1   0   2   3   4      3300.0100   2.0000e-05 6\n"},
        {"legacy14_ei1e-5", "  4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   1.0000e-05\n"},
        {"legacy14_ei5.2", "  4   1   4   0   0   0   3   1   3   0   0   0      2511.3375   5.2000e+00\n"},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        FILE *f = fopen("case.txt", "w"); fputs(cases[c].text, f); fclose(f);
        Assignment list[8]; int n = 0; memset(list, 0, sizeof list);
        load_existing_assignments("case.txt", list, &n);
        printf("%-16s -> n=%d", cases[c].name, n);
        if (n) { printf("  exp=%.4f exp_int=%.3e ", list[0].exp_freq, list[0].exp_int); print_pred("pred:", &list[0].pred); } else printf("\n");
    }
    Assignment *list = calloc(MAX_ASSIGNMENTS, sizeof(Assignment)); int n = 0;
    load_existing_assignments(backup, list, &n);
    printf("assignments_backup.txt (.lin + NQN, 422 rows) -> n=%d\n", n);
    for (int i = 0; i < n && i < 4; i++) { printf("  [%d] exp=%.4f exp_int=%.3e ", i, list[i].exp_freq, list[i].exp_int); print_pred("pred:", &list[i].pred); }
    int sentinel = 0; for (int i = 0; i < n; i++) if (list[i].exp_freq > 90000) sentinel++;
    printf("  rows kept with exp_freq > 90000 MHz (undecoded .lin sentinel): %d\n", sentinel);
    return 0;
}

static int sc_stale_sel(const char *work, const char *catA, const char *catB) {
    banner("STALE SELECTION across a catalogue reload");
    enter(work);
    AppState *s = new_state(work);
    set_predictions(s, catA);
    int n = click_select(s, 5999.2867);
    printf("selected in A (%s): n=%d idx=%d\n", catA, n, n ? s->selected_indices[0] : -1);
    if (n) print_pred("  A line:", &s->pred_lines[s->selected_indices[0]]);
    set_predictions(s, catB);               /* drop of another .cat, or model.cat after Calculate/Fit */
    printf("after loading B (%s): n_selected=%d idx=%d n_pred=%d\n", catB, s->n_selected, s->selected_indices[0], s->n_pred);
    if (s->selected_indices[0] < s->n_pred) print_pred("  now points to:", &s->pred_lines[s->selected_indices[0]]);
    else printf("  index out of range: view.c:1815 would read pred_lines[%d] beyond n_pred=%d\n", s->selected_indices[0], s->n_pred);
    assign_selected_predictions(s, 5999.25, 1.0);
    print_assignments(s, "assignment created after the reload");
    return 0;
}

/* ------------------------------------------------ second-pass scenarios */
static void mono_model(PredFitState *p) {
    p->a = 1151.360417; p->b = 316.1511127; p->c = 313.1742368; p->fmax_ghz = 8.0; p->temp_k = 2.0;
    sync_basic_parameters(p);
}

static int sc_spaces(const char *work) {
    banner("PATH WITH SPACES: data_dir containing a space");
    enter(work);
    AppState *s = new_state(work);
    mono_model(&s->predfit);
    int ok = predfit_calculate(s);
    printf("data_dir='%s'\nCalculate -> %d, status: %s\n", s->settings.data_dir, ok, s->predfit.status);
    char f1[900], f2[900];
    snprintf(f1, sizeof f1, "%s/.fit/model.var", work); snprintf(f2, sizeof f2, "%s/.fit/model.cat", work);
    printf("model.var written: %s; model.cat produced by SPCAT: %s\n",
           access(f1, F_OK) == 0 ? "yes" : "no", access(f2, F_OK) == 0 ? "yes" : "no");
    return 0;
}

static int sc_remove_active(const char *work) {
    banner("REMOVE SPECTRUM: which trace is active after removing an earlier one");
    enter(work);
    const char *names[3] = {"A.txt", "B.txt", "C.txt"};
    double pk[3] = {3000.0, 4000.0, 5000.0};
    AppState *s = new_state(work);
    for (int i = 0; i < 3; i++) {
        char path[900]; snprintf(path, sizeof path, "%s/%s", work, names[i]);
        write_spectrum(path, &pk[i], NULL, 1);
        add_spectrum(s, path);
    }
    select_spectrum(s, 1);
    s->exp_offset = 0.5; commit_active(s);            /* Alt-drag on B */
    printf("before: n=%d active=%d (%s) offset=%.2f\n", s->n_spectra, s->active_spec,
           s->spectra[s->active_spec].name, s->exp_offset);
    remove_spectrum(s, 0);                            /* the x of row 0 (A) in the Spectra panel */
    printf("after removing A: n=%d active=%d (%s), mirrored exp_path=%s, offset=%.2f\n", s->n_spectra,
           s->active_spec, s->spectra[s->active_spec].name, s->exp_path, s->exp_offset);
    return 0;
}

static int sc_peakfinder(void) {
    banner("PEAK FINDER: noise window unused, threshold on the raw signal, negative search width");
    int n = 2000;
    Point *pts = malloc(sizeof(Point) * n);
    Peak *peaks = malloc(sizeof(Peak) * MAX_PEAKS);
    int np = 0, a, b;
    for (int i = 0; i < n; i++) { pts[i].x = 1000.0 + i * 0.01; pts[i].y = 1.0 + ((i % 97) == 50 ? 5.0 : 0.0); }
    run_peak_finder(pts, n, 1000.0, 1019.99, 5, 10, 3.0, peaks, &np);   a = np;
    run_peak_finder(pts, n, 1000.0, 1019.99, 5, 1000, 3.0, peaks, &np); b = np;
    printf("baseline 1, lines +5: noise window 10 -> %d peaks, noise window 1000 -> %d peaks\n", a, b);
    for (int i = 0; i < n; i++) pts[i].y += 9.0;       /* same lines, noise-free, on a baseline of 10 */
    run_peak_finder(pts, n, 1000.0, 1019.99, 5, 10, 3.0, peaks, &np);
    printf("same lines on a baseline of 10 (still noise-free) -> %d peaks\n", np);
    printf("search width -5 (the panel accepts it, controller.c:890):\n");
    run_peak_finder(pts, n, 1000.0, 1019.99, -5, 50, 3.0, peaks, &np);
    printf("returned %d peaks\n", np);
    free(pts); free(peaks);
    return 0;
}

static int sc_divzero(const char *work, const char *cat) {
    banner("PREDICTION-ONLY LAYOUT: W divides by exp_h = 0");
    enter(work);
    AppState *s = new_state(work);
    set_predictions(s, cat);
    Layout L; compute_layout(s, &L);
    printf("exp_h=%d pred_h=%d vymin=%g vymax=%g\n", L.exp_h, L.pred_h, s->vymin, s->vymax);
    SDL_KeyboardEvent k; memset(&k, 0, sizeof k); k.type = SDL_KEYDOWN; k.keysym.sym = SDLK_w;
    handle_keydown(s, &L, &k);
    printf("after W: vymin=%g vymax=%g\n", s->vymin, s->vymax);
    char spec[900]; snprintf(spec, sizeof spec, "%s/spec.txt", work);
    double pk = 3000.1; write_spectrum(spec, &pk, NULL, 1);
    add_spectrum(s, spec);
    printf("after loading a spectrum: vymin=%g vymax=%g\n", s->vymin, s->vymax);
    return 0;
}

static double watch_ratio(const AppState *s, double f, int state) {
    for (int i = 0; i < s->n_pred; i++)
        if (fabs(s->pred_lines[i].freq_mhz - f) < 1e-6 && s->pred_lines[i].M1u == state)
            return s->pred_lines[i].linear_int / pow(10, s->pred_lines[i].cat_lgint);
    return NAN;
}

static int sc_fitfail(const char *work) {
    banner("RUN FIT that fails: side effects anyway");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p); p->mu[0] = 0.75; p->mu[1] = 0.21; p->mu[2] = 1.14;
    add_species(s);
    set_param(p, 10011, 907.5172, 0.0); set_param(p, 20011, 272.5706, 0.0); set_param(p, 30011, 250.97429, 0.0);
    select_species(s, 0);
    p->species[1].concentration = 0.1;
    predfit_calculate(s); pump(s);
    double fw = 0; for (int i = 0; i < s->n_pred; i++) if (s->pred_lines[i].M1u == 1 && s->pred_lines[i].freq_mhz > 2000) { fw = s->pred_lines[i].freq_mhz; break; }
    printf("species-1 line %.4f: ratio to catalogue = %.3f (concentration 0.1)\n", fw, watch_ratio(s, fw, 1));
    char spec[900]; snprintf(spec, sizeof spec, "%s/far.txt", work);
    double far = 90000.0; write_spectrum(spec, &far, NULL, 1);
    add_spectrum(s, spec);                            /* a trace that contains none of the lines */
    s->win_dip.visible = 1;
    Layout L; compute_layout(s, &L);
    SDL_Rect run = ui_fit_run(s->win_dip.rect);
    mouse_down(s, &L, run.x + run.w / 2, run.y + run.h / 2, SDL_BUTTON_LEFT);
    printf("Run fit with 0 assignments -> '%s'; ratio now %.3f\n", s->intfit_message, watch_ratio(s, fw, 1));
    predfit_publish_shared_state(s);                  /* back to the per-species intensities */
    for (int i = 0, k = 0; i < s->n_pred && k < 3; i++)
        if (s->pred_lines[i].M1u == 0 && s->pred_lines[i].freq_mhz > 3000) { assign_index(s, i, s->pred_lines[i].freq_mhz, 1.0); k++; }
    s->dipole_red[1] = 0.0;                           /* user cleared mu red b */
    printf("before: dipole_red=(%.3f %.3f %.3f)\n", s->dipole_red[0], s->dipole_red[1], s->dipole_red[2]);
    compute_layout(s, &L); run = ui_fit_run(s->win_dip.rect);
    mouse_down(s, &L, run.x + run.w / 2, run.y + run.h / 2, SDL_BUTTON_LEFT);
    printf("Run fit, lines outside the trace -> '%s'; dipole_red=(%.3f %.3f %.3f); ratio %.3f\n", s->intfit_message,
           s->dipole_red[0], s->dipole_red[1], s->dipole_red[2], watch_ratio(s, fw, 1));
    return 0;
}

static int sc_mu_input(const char *work) {
    banner("DIPOLE INPUT: one quantity, three validation rules");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    printf("start: predfit.mu=(%g %g %g)\n", p->mu[0], p->mu[1], p->mu[2]);
    s->input_state = INPUT_PF_MUB; snprintf(s->text_input_buf, sizeof s->text_input_buf, "0"); commit_text_input(s);
    printf("Pred&Fit panel, mu b = 0            -> predfit.mu[1] = %g\n", p->mu[1]);
    s->input_state = INPUT_PF_MUC; snprintf(s->text_input_buf, sizeof s->text_input_buf, "-1.141963"); commit_text_input(s);
    printf("Pred&Fit panel, mu c = -1.141963    -> predfit.mu[2] = %g\n", p->mu[2]);
    advanced_begin_species_edit(p, 0, 4);
    snprintf(p->advanced_edit_buf, sizeof p->advanced_edit_buf, "-1.141963"); advanced_commit_edit(p);
    printf("Advanced species, mu c = -1.141963  -> species[0].mu[2] = %g\n", p->species[0].mu[2]);
    s->input_state = INPUT_MURED_B; snprintf(s->text_input_buf, sizeof s->text_input_buf, "0"); commit_text_input(s);
    printf("Intensity analysis, mu red b = 0    -> dipole_red[1] = %g, predfit.mu[1] = %g\n", s->dipole_red[1], p->mu[1]);
    s->input_state = INPUT_MURED_C; snprintf(s->text_input_buf, sizeof s->text_input_buf, "-1.141963"); commit_text_input(s);
    printf("Intensity analysis, mu red c = -1.1 -> dipole_red[2] = %g, predfit.mu[2] = %g, species[0].mu[2] = %g\n",
           s->dipole_red[2], p->mu[2], p->species[0].mu[2]);
    return 0;
}

static int sc_multidrop(const char *work) {
    banner("DROP of several spectra in one gesture");
    enter(work);
    if (SDL_Init(SDL_INIT_EVENTS) != 0) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    AppState *s = new_state(work);
    const char *names[3] = {"s1.txt", "s2.txt", "s3.txt"};
    double pk[3] = {3000, 4000, 5000};
    for (int i = 0; i < 3; i++) {
        char path[900]; snprintf(path, sizeof path, "%s/%s", work, names[i]);
        write_spectrum(path, &pk[i], NULL, 1);
        SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_DROPFILE; e.drop.file = SDL_strdup(path);
        SDL_PushEvent(&e);
    }
    Layout L; compute_layout(s, &L); int running = 1;
    handle_app_events(s, &L, &running);              /* controller.c:150-237 */
    pump(s);                                          /* main.c:514-522 */
    printf("3 files dropped together -> n_spectra = %d:", s->n_spectra);
    for (int i = 0; i < s->n_spectra; i++) printf(" %s", s->spectra[i].name);
    printf("\n");
    SDL_Quit();
    return 0;
}

static int sc_workdir(const char *work) {
    banner("WORK DIR and Restore defaults");
    enter(work);
    AppState *s = calloc(1, sizeof(AppState));
    init_app_defaults(s);                             /* main.c:369 -> predfit_init: work_dir from an empty data_dir */
    settings_init(s, ARGV0);
    settings_apply_defaults(s);
    snprintf(s->settings.data_dir, sizeof(s->settings.data_dir), "%s", work);   /* as if read from spectravisual.settings */
    char root[700]; fit_root(s, root, sizeof root);
    printf("predfit.work_dir = '%s'   fit_root(data_dir) = '%s'\n", s->predfit.work_dir, root);
    printf("before Restore defaults: spcat='%s'\n", s->settings.spcat_path);
    settings_restore_defaults(s);                     /* Settings > Restore defaults, settings.c:968-972 */
    printf("after  Restore defaults: spcat='%s' spfit='%s' data_dir='%s'\n",
           s->settings.spcat_path, s->settings.spfit_path, s->settings.data_dir);
    return 0;
}

static int sc_uppercase(const char *work, const char *cat) {
    banner("UPPERCASE .CAT extension");
    enter(work);
    char up[900], cmd[2000];
    snprintf(up, sizeof up, "%s/PRED.CAT", work);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", cat, up); system(cmd);
    printf("path_looks_like_cat(PRED.CAT) = %d\n", path_looks_like_cat(up));
    AppState *s = new_state(work);
    int ok = add_spectrum(s, up);
    printf("dropped/passed as a spectrum -> add_spectrum = %d, n_pts = %d, first point (%.4f, %.4f)\n", ok, s->n_pts,
           s->n_pts ? s->raw_pts[0].x : 0.0, s->n_pts ? s->raw_pts[0].y : 0.0);
    return 0;
}

static int sc_perf(const char *work) {
    banner("LOAD TIME of assignments.txt versus number of rows");
    enter(work);
    int sizes[4] = {250, 500, 1000, 2000};
    for (int t = 0; t < 4; t++) {
        FILE *f = fopen("big.txt", "w");
        fprintf(f, "# Upper QNs, lower QNs (SPFIT .lin order), ObsFreq(MHz) CalcFreq(MHz) CalcIntensity NQN\n");
        for (int i = 0; i < sizes[t]; i++) {
            int J = 1 + i / 40, Ka = (i / 4) % 10, Kc = i % 40, v = i % 4;
            fprintf(f, "%3d%3d%3d%3d%3d%3d%3d%3d            %15.6f %15.6f %15.6E %d\n",
                    J, Ka, Kc, v, J - 1, Ka, Kc, v, 3000.0 + i, 3000.0 + i, 1e-4, 4);
        }
        fclose(f);
        Assignment *list = calloc(MAX_ASSIGNMENTS, sizeof(Assignment)); int n = 0;
        fflush(stdout); int keep = dup(1), nul = open("/dev/null", O_WRONLY); dup2(nul, 1);
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        load_existing_assignments("big.txt", list, &n);
        clock_gettime(CLOCK_MONOTONIC, &b);
        fflush(stdout); dup2(keep, 1); close(nul); close(keep);
        printf("%5d rows -> %5d assignments in %9.1f ms\n", sizes[t], n,
               (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
        free(list);
    }
    return 0;
}

static int sc_param(const char *work, int dup_id) {
    banner(dup_id ? "ADVANCED: a second row with ID 10000" : "ADVANCED: '+ parameter' then Esc");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p);
    predfit_calculate(s); pump(s);
    int picks[][6] = {{4,1,4,3,1,3},{4,0,4,3,0,3},{5,1,5,4,1,4},{6,1,6,5,1,5},{7,1,7,6,1,6},{5,0,5,4,0,4}};
    for (int k = 0; k < 6; k++) {
        int idx = find_line(s, picks[k][0], picks[k][1], picks[k][2], picks[k][3], picks[k][4], picks[k][5], -1);
        if (idx >= 0) assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    add_parameter(p);                                 /* predfit.c:1346-1351: row {0, 0.0, 1.0} in edit mode */
    if (dup_id) { snprintf(p->advanced_edit_buf, sizeof p->advanced_edit_buf, "10000"); advanced_commit_edit(p); }
    else { p->advanced_edit_param = -1; }             /* Esc, predfit.c:1340 */
    int ok = predfit_fit(s);
    printf("Fit -> %d  status: %s\n", ok, p->status);
    char par[900], fit[900];
    snprintf(par, sizeof par, "%s/.fit/model.par", work); snprintf(fit, sizeof fit, "%s/.fit/model.fit", work);
    printf("-- model.par:\n"); cat_file(par, 8);
    printf("-- model.fit (parameter block and messages):\n");
    FILE *ff = fopen(fit, "r"); char l[512]; int shown = 0;
    while (ff && fgets(l, sizeof l, ff) && shown < 40) {
        if (strstr(l, "10000") || strstr(l, "  0 ") || strstr(l, "rror") || strstr(l, "RMS") || strstr(l, "ignored") ||
            strstr(l, "same") || strstr(l, "Duplicate") || strstr(l, "duplicate") || strstr(l, "NEW PARAMETER")) { printf("  | %s", l); shown++; }
    }
    if (ff) fclose(ff);
    printf("A after fit: %.6f\n", p->a);
    return 0;
}

/* ------------------------------------------------ second-pass scenarios, part 2 */
static int sc_clicat(const char *work, const char *extcat) {
    banner("LAUNCH WITH A .cat: the Pred&Fit model is not restored");
    enter(work);
    char var[900], spec[900];
    snprintf(var, sizeof var, "%s/.fit/model.var", work);
    snprintf(spec, sizeof spec, "%s/spec.txt", work);
    double pk = 3000.1; write_spectrum(spec, &pk, NULL, 1);
    /* session 1: a model with a parameter the user fixed (error 0) */
    AppState *s = new_state(work);
    mono_model(&s->predfit);
    set_param(&s->predfit, 200, -7.0e-6, 0.0);
    predfit_calculate(s); pump(s);
    printf("-- session 1: model.var written by Calculate:\n"); cat_file(var, 7);
    /* session 2a: `spectravisual spec.txt pred.cat`, then quit (main.c:400, 429-450, 545) */
    AppState *a = new_state(work);
    predfit_load_session(a);
    set_predictions(a, extcat);
    add_spectrum(a, spec);
    predfit_save_session(a);
    printf("session 2a (launched with a .cat): quick A B C = %.4f %.4f %.4f, n_param = %d\n",
           a->predfit.a, a->predfit.b, a->predfit.c, a->predfit.n_param);
    /* session 3a: plain restart, restore from .fit */
    AppState *b = new_state(work);
    predfit_load_session(b); predfit_restore_latest(b);
    for (int i = 0; i < b->predfit.n_param; i++)
        printf("  session 3a: param %6d = %-14.9g error %g\n", b->predfit.param[i].id, b->predfit.param[i].value, b->predfit.param[i].error);
    /* session 2b: launched with a .cat again, the user presses Calculate */
    AppState *c = new_state(work);
    predfit_load_session(c);
    set_predictions(c, extcat);
    add_spectrum(c, spec);
    predfit_calculate(c);
    printf("-- session 2b: model.var after Calculate:\n"); cat_file(var, 7);
    return 0;
}

static int sc_saveloss(const char *work, const char *cwd, const char *cat) {
    banner("DATA LOSS: restart without .cat from another folder, then Save all");
    mkdir(work, 0700); enter(cwd);
    char asg[900], lin[900], dst[900], cmd[2000];
    snprintf(asg, sizeof asg, "%s/assignments.txt", work);
    snprintf(lin, sizeof lin, "%s/.fit/model.lin", work);
    snprintf(dst, sizeof dst, "%s/.fit/model.cat", work);
    unlink(asg);
    AppState *s = new_state(work);
    set_predictions(s, cat);
    double want[3] = {3000.1, 5999.2867, 7000.0};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < s->n_pred; i++)
            if (fabs(s->pred_lines[i].freq_mhz - want[k]) < 1e-4) { assign_index(s, i, want[k] - 0.02, 1.0); break; }
    click_save_all(s);
    write_inputs(s, 1);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", cat, dst); system(cmd);
    printf("-- session 1: data_dir/assignments.txt\n"); cat_file(asg, 0);
    printf("-- session 1: model.lin\n"); cat_file(lin, 0);
    AppState *r = new_state(work);
    predfit_load_session(r); predfit_restore_latest(r); pump(r);
    print_assignments(r, "session 2 (no .cat, CWD != data_dir)");
    click_save_all(r);
    printf("-- data_dir/assignments.txt after Save all in session 2:\n"); cat_file(asg, 0);
    return 0;
}

static int sc_exportstale(const char *work) {
    banner("INTENSITY FIT EXPORT after the assignment list changed");
    enter(work);
    AppState *s = new_state(work);
    mono_model(&s->predfit);
    predfit_calculate(s); pump(s);
    double pk[8], ht[8]; int idx[8], na = 0;
    for (int i = 0; i < s->n_pred && na < 6; i++) {
        PredLine *q = &s->pred_lines[i];
        if (q->freq_mhz < 2000 || q->lgint < -6) continue;
        idx[na] = i; pk[na] = q->freq_mhz + 0.002; ht[na] = q->linear_int / s->pred_global_max; na++;
    }
    char spec[900]; snprintf(spec, sizeof spec, "%s/spectrum.txt", work);
    write_spectrum(spec, pk, ht, na);
    add_spectrum(s, spec);
    for (int k = 0; k < na; k++) assign_index(s, idx[k], pk[k], ht[k]);
    intensity_fit_run(s);
    intensity_fit_export(s, "fit_before.ifit");
    delete_assignment(s, 0);                     /* Assignments > Delete selected, no new fit */
    intensity_fit_export(s, "fit_after_delete.ifit");
    printf("-- export right after the fit (last rows):\n"); system("tail -6 fit_before.ifit | sed 's/^/  | /'");
    printf("-- export after deleting assignment 0, without refitting:\n"); system("tail -6 fit_after_delete.ifit | sed 's/^/  | /'");
    return 0;
}

static int sc_descending(const char *work) {
    banner("SPECTRUM FILE in descending frequency order");
    enter(work);
    char path[900]; snprintf(path, sizeof path, "%s/desc.txt", work);
    FILE *f = fopen(path, "w");
    for (double x = 3001.5; x >= 2998.5; x -= 0.002) { double d = (x - 3000.0) / 0.03; fprintf(f, "%.6f %.8e\n", x, 1e-4 + exp(-0.5 * d * d)); }
    fclose(f);
    AppState *s = new_state(work);
    add_spectrum(s, path);
    printf("loaded %d points, first x = %.3f, last x = %.3f\n", s->n_pts, s->raw_pts[0].x, s->raw_pts[s->n_pts - 1].x);
    printf("binary_search_lower(2999.8) = %d, binary_search_upper(3000.2) = %d\n",
           binary_search_lower(s->current_pts, s->n_pts, 2999.8), binary_search_upper(s->current_pts, s->n_pts, 3000.2));
    run_right_click_peak_find(s, 2999.8, 3000.2);
    printf("right-drag over the 3000 MHz line -> n_peaks = %d%s\n", s->n_peaks,
           s->n_peaks ? "" : " (the line is visible but cannot be measured)");
    return 0;
}

static int sc_speciesdel(const char *work) {
    banner("ADVANCED > SPECIES: delete a row above the active one; re-add after deleting the last");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p);
    add_species(s);
    set_param(p, 10011, 907.5172, 0.0); set_param(p, 20011, 272.5706, 0.0); set_param(p, 30011, 250.97429, 0.0);
    add_species(s);
    set_param(p, 10022, 1100.0, 0.0); set_param(p, 20022, 300.0, 0.0); set_param(p, 30022, 280.0, 0.0);
    snprintf(p->species[0].name, 64, "Mono"); snprintf(p->species[1].name, 64, "Donor"); snprintf(p->species[2].name, 64, "Accep");
    select_species(s, 1);
    printf("before: n=%d active=%d (%s) quick A=%.4f\n", p->n_species, p->active_species, p->species[p->active_species].name, p->a);
    p->advanced_open = 1; p->advanced_tab = 3; p->advanced_window_id = 0;
    AdvUI u = adv_ui(s, 3);
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = 0; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = u.table.x + u.table.w - 12; e.button.y = u.rows.y + u.row_h / 2;          /* x of row 0 */
    predfit_handle_advanced_event(s, &e);
    printf("after deleting row 0 (Mono): n=%d active=%d (%s) quick A=%.4f, option line '%s'\n",
           p->n_species, p->active_species, p->species[p->active_species].name, p->a, p->hamiltonian_line);
    u = adv_ui(s, 3);
    e.button.y = u.rows.y + (p->n_species - 1) * u.row_h + u.row_h / 2;                      /* x of the last row */
    predfit_handle_advanced_event(s, &e);
    printf("after deleting the last row: n=%d active=%d (%s) quick A=%.4f\n", p->n_species, p->active_species,
           p->species[p->active_species].name, p->a);
    add_species(s);
    printf("'+ species' -> '%s' v=%d, quick A B C = %.4f %.4f %.4f (the seed is the active species' values)\n",
           p->species[p->active_species].name, p->species[p->active_species].state_index, p->a, p->b, p->c);
    return 0;
}

static int sc_lineerr(const char *work) {
    banner("PERSISTENCE of the .lin uncertainty typed in Advanced");
    enter(work);
    AppState *s = new_state(work);
    s->predfit.line_error_mhz = 0.05;             /* predfit.c:1263-1270 */
    predfit_save_session(s);
    AppState *r = new_state(work);
    predfit_load_session(r);
    printf("typed 0.05 MHz; after a restart: %.4f MHz\n", r->predfit.line_error_mhz);
    return 0;
}

static double max_cat_err(const char *path, double *at) {
    FILE *f = fopen(path, "r"); char l[512]; double best = -1; *at = 0;
    while (f && fgets(l, sizeof l, f)) {
        if (strlen(l) < 30) continue;
        char e[9]; memcpy(e, l + 13, 8); e[8] = 0;
        char fr[14]; memcpy(fr, l, 13); fr[13] = 0;
        double v = atof(e);
        if (v > best) { best = v; *at = atof(fr); }
    }
    if (f) fclose(f);
    return best;
}

static int sc_errcol(const char *work) {
    banner("ERR column of model.cat after Fit and after a later Calculate");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p);
    predfit_calculate(s); pump(s);
    char mc[900]; snprintf(mc, sizeof mc, "%s/.fit/model.cat", work);
    double at, e1 = max_cat_err(mc, &at); printf("after Calculate: max ERR = %9.4f MHz (at %.4f MHz)\n", e1, at);
    int picks[][6] = {{4,1,4,3,1,3},{4,0,4,3,0,3},{5,1,5,4,1,4},{6,1,6,5,1,5},{7,1,7,6,1,6},{5,0,5,4,0,4},{6,0,6,5,0,5},{3,1,3,2,1,2}};
    for (int k = 0; k < 8; k++) {
        int idx = find_line(s, picks[k][0], picks[k][1], picks[k][2], picks[k][3], picks[k][4], picks[k][5], -1);
        if (idx >= 0) assign_index(s, idx, s->pred_lines[idx].freq_mhz + 0.002, 1.0);
    }
    predfit_fit(s); pump(s);
    double e2 = max_cat_err(mc, &at); printf("after Fit (SPCAT on the .var written by SPFIT): max ERR = %9.4f MHz (at %.4f MHz)\n", e2, at);
    predfit_calculate(s); pump(s);
    double e3 = max_cat_err(mc, &at); printf("after Calculate with the same parameters: max ERR = %9.4f MHz (at %.4f MHz)\n", e3, at);
    char var[900]; snprintf(var, sizeof var, "%s/.fit/model.var", work);
    printf("-- model.var now:\n"); cat_file(var, 7);
    return 0;
}

static void write_spectrum_base(const char *path, const double *pk, const double *ht, int n, double base) {
    write_spectrum(path, pk, ht, n);
    if (base == 0.0) return;
    char tmp[1000]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w"); double x, y;
    while (fscanf(in, "%lf %lf", &x, &y) == 2) fprintf(out, "%.6f %.8e\n", x, y + base);
    fclose(in); fclose(out); rename(tmp, path);
}

static int sc_baseline(const char *work) {
    banner("INTENSITY FIT: fitted T rot versus a constant spectral baseline");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p); p->temp_k = 2.0;
    predfit_calculate(s); pump(s);
    char mc[900]; snprintf(mc, sizeof mc, "%s/.fit/model.cat", work);
    double fsel[16], ht[16]; int n = 0;
    for (int i = 0; i < s->n_pred && n < 10; i += 7) {
        PredLine *q = &s->pred_lines[i];
        if (q->freq_mhz < 2000 || q->lgint < -6.5) continue;
        fsel[n] = q->freq_mhz; ht[n] = q->linear_int / s->pred_global_max; n++;
    }
    double bases[4] = {0.0, 0.005, 0.02, 0.1};
    for (int b = 0; b < 4; b++) {
        char spec[900]; snprintf(spec, sizeof spec, "%s/base_%d.txt", work, b);
        double pk[16]; for (int k = 0; k < n; k++) pk[k] = fsel[k] + 0.002;
        write_spectrum_base(spec, pk, ht, n, bases[b]);
        AppState *r = new_state(work);
        set_predictions(r, mc);                      /* opened as a catalogue: Tcat typed by the user */
        r->cat_temp_k = 2.0; r->rot_temp_k = 2.0;
        r->intfit_fit_temperature = 1; r->intfit_fit_dipole[0] = r->intfit_fit_dipole[1] = r->intfit_fit_dipole[2] = 0;
        add_spectrum(r, spec);
        for (int k = 0; k < n; k++)
            for (int i = 0; i < r->n_pred; i++) if (fabs(r->pred_lines[i].freq_mhz - fsel[k]) < 1e-6) { assign_index(r, i, pk[k], ht[k]); break; }
        intensity_fit_run(r);
        printf("baseline %.3f (strongest line = 1): T rot fitted = %7.3f K  (true 2 K)  %s\n", bases[b], r->rot_temp_k, r->intfit_message);
    }
    return 0;
}

static int sc_reassign(const char *work, const char *cat) {
    banner("REASSIGNING an excluded transition");
    enter(work);
    AppState *s = new_state(work);
    set_predictions(s, cat);
    assign_index(s, 0, s->pred_lines[0].freq_mhz - 0.01, 1.0);
    s->assignments[0].fit_enabled = 0;           /* excluded in Advanced > Lines */
    assign_index(s, 0, s->pred_lines[0].freq_mhz + 0.01, 1.0);
    printf("fit_enabled after moving the assignment to another peak: %d\n", s->assignments[0].fit_enabled);
    return 0;
}

static void write_cat2(const char *path) {
    FILE *f = fopen(path, "w");
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3000.0, 0.001, -4.0, 3, 1.0, 11, 1, 303, " 5 1 5       4 1 4      ");
    fprintf(f, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d%s\n", 3001.0, 0.001, -3.3, 3, 1.0, 11, 1, 303, " 6 1 6       5 1 5      ");
    fclose(f);
}

static int sc_descending2(const char *work) {
    banner("SPECTRUM IN DESCENDING ORDER: right-drag and intensity areas");
    enter(work);
    char cat[900]; snprintf(cat, sizeof cat, "%s/two.cat", work); write_cat2(cat);
    for (int order = 0; order < 2; order++) {
        char path[900]; snprintf(path, sizeof path, "%s/%s.txt", work, order ? "desc" : "asc");
        FILE *f = fopen(path, "w");
        for (int i = 0; i <= 2000; i++) {
            double x = order ? 3002.0 - i * 0.002 : 2998.0 + i * 0.002;
            double d1 = (x - 3000.0) / 0.03, d2 = (x - 3001.0) / 0.03;
            fprintf(f, "%.6f %.8e\n", x, 1e-4 + 1.0 * exp(-0.5 * d1 * d1) + 5.0 * exp(-0.5 * d2 * d2));
        }
        fclose(f);
        AppState *s = new_state(work);
        set_predictions(s, cat);
        add_spectrum(s, path);
        run_right_click_peak_find(s, 2999.8, 3000.2);
        double got = s->n_peaks ? s->peaks[s->n_peaks - 1].x : 0.0;
        s->n_peaks = 0;
        for (int i = 0; i < s->n_pred; i++) assign_index(s, i, s->pred_lines[i].freq_mhz, 1.0);
        s->intfit_fit_temperature = 0;
        s->intfit_fit_dipole[0] = s->intfit_fit_dipole[1] = s->intfit_fit_dipole[2] = 0;
        intensity_fit_run(s);
        printf("%s file: right-drag over 2999.8-3000.2 MHz (weak line at 3000.0) -> measured %.4f MHz; intensity fit: %s\n",
               order ? "descending" : "ascending ", got, s->intfit_message);
    }
    return 0;
}

static int sc_textleak(const char *work) {
    banner("TYPING in the Advanced window while a main-window field has the focus");
    enter(work);
    if (SDL_Init(SDL_INIT_EVENTS) != 0) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    AppState *s = new_state(work);
    s->exp_offset = 0.0;
    s->input_state = INPUT_OFFSET;                                   /* Offset field focused, value selected */
    snprintf(s->text_input_buf, sizeof s->text_input_buf, "0.0000");
    s->input_anchor = 0; s->input_caret = (int)strlen(s->text_input_buf);
    s->predfit.advanced_open = 1; s->predfit.advanced_window_id = 7; s->predfit.advanced_edit_param = -1;
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT; e.text.windowID = 7; snprintf(e.text.text, sizeof e.text.text, "12");
    SDL_PushEvent(&e);
    SDL_Event k; memset(&k, 0, sizeof k);
    k.type = SDL_KEYDOWN; k.key.windowID = 7; k.key.keysym.sym = SDLK_RETURN;
    SDL_PushEvent(&k);
    Layout L; compute_layout(s, &L); int running = 1;
    handle_app_events(s, &L, &running);
    printf("'12' + Enter typed while the Advanced window has the keyboard (no cell in edit) -> main Offset = %.4f MHz\n", s->exp_offset);
    SDL_Quit();
    return 0;
}

static int sc_yrange(const char *work, const char *cat) {
    banner("CATALOGUE LOADED BEFORE THE SPECTRUM (the order of main.c:429-430)");
    enter(work);
    char spec[900]; snprintf(spec, sizeof spec, "%s/weak.txt", work);
    FILE *f = fopen(spec, "w");
    for (int i = 0; i <= 3000; i++) { double x = 2999.0 + i * 0.002, d = (x - 3000.0) / 0.03; fprintf(f, "%.6f %.8e\n", x, 1e-6 + 3e-5 * exp(-0.5 * d * d)); }
    fclose(f);
    for (int order = 0; order < 2; order++) {
        AppState *s = new_state(work);
        if (order == 0) { set_predictions(s, cat); add_spectrum(s, spec); }
        else            { add_spectrum(s, spec); set_predictions(s, cat); }
        printf("%s: spectrum y range %.3g..%.3g, shown Y range vymin=%g vymax=%g\n",
               order == 0 ? "CAT then spectrum (command line)" : "spectrum then CAT (drop)       ",
               s->spectra[0].ymin, s->spectra[0].ymax, s->vymin, s->vymax);
    }
    return 0;
}

static int sc_noA(const char *work) {
    banner("DELETING THE A ROW of the parameter table, then Calculate");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p);
    delete_parameter(p, 0);                         /* the x of row 0 (ID 10000) in Advanced > Parameters */
    printf("quick A B C still shown: %.4f %.4f %.4f, n_param = %d\n", p->a, p->b, p->c, p->n_param);
    int ok = predfit_calculate(s);
    printf("Calculate -> %d, status: %s\n", ok, p->status);
    char var[900], mc[900];
    snprintf(var, sizeof var, "%s/.fit/model.var", work); snprintf(mc, sizeof mc, "%s/.fit/model.cat", work);
    printf("-- model.var:\n"); cat_file(var, 6);
    printf("-- first lines of model.cat:\n"); cat_file(mc, 3);
    return 0;
}

static int sc_keyleak(const char *work) {
    banner("KEYS pressed in the Advanced / Settings window with no cell in edit");
    enter(work);
    if (SDL_Init(SDL_INIT_EVENTS) != 0) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    char spec[900]; snprintf(spec, sizeof spec, "%s/k.txt", work);
    FILE *f = fopen(spec, "w");
    for (int i = 0; i <= 1000; i++) { double x = 2999.0 + i * 0.002, d = (x - 3000.0) / 0.03; fprintf(f, "%.6f %.8e\n", x, 1e-3 + exp(-0.5 * d * d)); }
    fclose(f);
    for (int win = 0; win < 2; win++) {
        AppState *s = new_state(work);
        add_spectrum(s, spec);
        s->peaks[0].x = 3000.0; s->peaks[0].y = 1.0; s->peaks[1].x = 3000.5; s->peaks[1].y = 0.5; s->n_peaks = 2;
        s->vxmin = 2999.9; s->vxmax = 3000.1;                       /* zoomed in on the line */
        int dip_before = s->win_dip.visible;
        Uint32 wid;
        if (win == 0) { s->predfit.advanced_open = 1; s->predfit.advanced_window_id = 7; s->predfit.advanced_edit_param = -1; wid = 7; }
        else          { s->settings.open = 1; s->settings.window_id = 9; s->settings.edit_id = -1; wid = 9; }
        SDL_Keycode keys[3] = {SDLK_BACKSPACE, SDLK_d, SDLK_r};
        for (int k = 0; k < 3; k++) {
            SDL_Event e; memset(&e, 0, sizeof e);
            e.type = SDL_KEYDOWN; e.key.windowID = wid; e.key.keysym.sym = keys[k];
            SDL_PushEvent(&e);
        }
        Layout L; compute_layout(s, &L); int running = 1;
        handle_app_events(s, &L, &running);
        printf("%s window has the keyboard, keys Backspace D R: n_peaks 2 -> %d; Intensity panel visible %d -> %d; main view 2999.900-3000.100 -> %.3f-%.3f\n",
               win == 0 ? "Advanced" : "Settings", s->n_peaks, dip_before, s->win_dip.visible, s->vxmin, s->vxmax);
        s->predfit.advanced_open = 0; s->settings.open = 0;
    }
    SDL_Quit();
    return 0;
}

static int sc_noA2(const char *work) {
    banner("DELETING THE A ROW, then typing A in the Pred&Fit panel");
    enter(work);
    AppState *s = new_state(work);
    PredFitState *p = &s->predfit;
    mono_model(p);
    delete_parameter(p, 0);
    s->input_state = INPUT_PF_A;                    /* the A field of the Pred&Fit panel */
    snprintf(s->text_input_buf, sizeof s->text_input_buf, "1151.3604");
    commit_text_input(s);
    printf("Pred&Fit panel: A typed 1151.3604 -> p->a = %.4f, n_param = %d, row with ID 10000: %s\n",
           p->a, p->n_param, have_parameter_id(p, 10000) ? "yes" : "no");
    int ok = predfit_calculate(s);
    printf("Calculate -> %d, status: %s\n", ok, p->status);
    char var[900]; snprintf(var, sizeof var, "%s/.fit/model.var", work);
    printf("-- model.var:\n"); cat_file(var, 6);
    return 0;
}

int main(int argc, char **argv) {
    ARGV0 = argv[0];
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: harness <scenario> ...\n"); return 1; }
    const char *sc = argv[1];
    if (!strcmp(sc, "qnfmt")) return sc_qnfmt(argc - 2, argv + 2);
    if (!strcmp(sc, "roundtrip")) {
        double f[64]; int nf = 0;
        for (int i = 4; i < argc && nf < 64; i++) f[nf++] = atof(argv[i]);
        return sc_roundtrip(argv[2], argv[3], nf, f);
    }
    if (!strcmp(sc, "nvib")) return sc_nvib(argv[2]);
    if (!strcmp(sc, "predfit_mixed")) return sc_predfit_mixed(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    if (!strcmp(sc, "restore")) return sc_restore(argv[2], argv[3], argv[4]);
    if (!strcmp(sc, "intfit")) return sc_intfit(argv[2]);
    if (!strcmp(sc, "formats")) return sc_formats(argv[2], argv[3]);
    if (!strcmp(sc, "stale_sel")) return sc_stale_sel(argv[2], argv[3], argv[4]);
    if (!strcmp(sc, "undo")) return sc_undo(argv[2]);
    if (!strcmp(sc, "sentinel")) return sc_sentinel(argv[2], atof(argv[3]));
    if (!strcmp(sc, "restore_int")) return sc_restore_int(argv[2], argv[3]);
    if (!strcmp(sc, "spaces")) return sc_spaces(argv[2]);
    if (!strcmp(sc, "remove_active")) return sc_remove_active(argv[2]);
    if (!strcmp(sc, "peakfinder")) return sc_peakfinder();
    if (!strcmp(sc, "divzero")) return sc_divzero(argv[2], argv[3]);
    if (!strcmp(sc, "fitfail")) return sc_fitfail(argv[2]);
    if (!strcmp(sc, "mu_input")) return sc_mu_input(argv[2]);
    if (!strcmp(sc, "multidrop")) return sc_multidrop(argv[2]);
    if (!strcmp(sc, "workdir")) return sc_workdir(argv[2]);
    if (!strcmp(sc, "uppercase")) return sc_uppercase(argv[2], argv[3]);
    if (!strcmp(sc, "perf")) return sc_perf(argv[2]);
    if (!strcmp(sc, "param0")) return sc_param(argv[2], 0);
    if (!strcmp(sc, "paramdup")) return sc_param(argv[2], 1);
    if (!strcmp(sc, "clicat")) return sc_clicat(argv[2], argv[3]);
    if (!strcmp(sc, "saveloss")) return sc_saveloss(argv[2], argv[3], argv[4]);
    if (!strcmp(sc, "exportstale")) return sc_exportstale(argv[2]);
    if (!strcmp(sc, "descending")) return sc_descending(argv[2]);
    if (!strcmp(sc, "speciesdel")) return sc_speciesdel(argv[2]);
    if (!strcmp(sc, "lineerr")) return sc_lineerr(argv[2]);
    if (!strcmp(sc, "errcol")) return sc_errcol(argv[2]);
    if (!strcmp(sc, "baseline")) return sc_baseline(argv[2]);
    if (!strcmp(sc, "reassign")) return sc_reassign(argv[2], argv[3]);
    if (!strcmp(sc, "descending2")) return sc_descending2(argv[2]);
    if (!strcmp(sc, "textleak")) return sc_textleak(argv[2]);
    if (!strcmp(sc, "yrange")) return sc_yrange(argv[2], argv[3]);
    if (!strcmp(sc, "noA")) return sc_noA(argv[2]);
    if (!strcmp(sc, "noA2")) return sc_noA2(argv[2]);
    if (!strcmp(sc, "keyleak")) return sc_keyleak(argv[2]);
    fprintf(stderr, "unknown scenario %s\n", sc);
    return 1;
}
