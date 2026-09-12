#include "predfit.h"
#include "controller.h"
#include "layout.h"
#include "loader.h"
#include "ui_theme.h"
#include "ui_chrome.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "settings.h"
#include <limits.h>
#include <time.h>
#include <dirent.h>

#define FIT_DIR_NAME ".fit"
/* Drop box inside .fit: Pickett files put here are read by Import, never
   written to.  Every generated file keeps living one level up, in .fit. */
#define LOAD_DIR_NAME "load"

static void work_file(const PredFitState *p, const char *name, char *out, size_t size);

/* Pickett takes a basename rather than an arbitrary path.  The visible H name
   is therefore converted to a portable filename stem; spaces are made `_`,
   while ordinary letters, digits, `_` and `-` stay readable. */
static void name_file_stem(const char *name, int id, char *out, size_t size) {
    size_t used = 0;
    for (const unsigned char *c = (const unsigned char *)(name ? name : ""); *c && used + 1 < size; c++) {
        if (isalnum(*c) || *c == '_' || *c == '-') out[used++] = (char)*c;
        else if (used && out[used - 1] != '_') out[used++] = '_';
    }
    while (used && out[used - 1] == '_') used--;
    if (!used) used = (size_t)snprintf(out, size, "Hamiltonian_%d", id);
    out[used < size ? used : size - 1] = '\0';
}

static void hamiltonian_file_stem(const PredFitState *p, char *out, size_t size) {
    const char *name = "model";
    int id = 1;
    if (p && p->active_hamiltonian >= 0 && p->active_hamiltonian < p->n_hamiltonians) {
        const HamiltonianModel *h = &p->hamiltonian[p->active_hamiltonian];
        if (h->name[0]) name = h->name;
        id = h->id;
    }
    name_file_stem(name, id, out, size);
}

/* Where the Pickett working files live, and which programs run them: both come
   from the settings now.  They used to be absolute paths into one developer's
   home directory, which meant Pred&Fit could not work on anybody else's
   machine. */
static void fit_root(const AppState *s, char *out, size_t n) {
    if (s->settings.data_dir[0]) snprintf(out, n, "%s/%s", s->settings.data_dir, FIT_DIR_NAME);
    else                         snprintf(out, n, "%s", FIT_DIR_NAME);
}

void predfit_refresh_work_dir(AppState *s) {
    fit_root(s, s->predfit.work_dir, sizeof(s->predfit.work_dir));
}

int predfit_is_generated_catalog(const AppState *s, const char *path) {
    if (!s || !path || !path[0]) return 0;
    char expected[700];
    work_file(&s->predfit, "model.cat", expected, sizeof(expected));
    if (strcmp(path, expected) == 0) return 1;

    /* Command-line paths and restored sessions can differ only in their
       spelling (relative versus absolute).  Compare canonical paths when
       possible, without making an inaccessible path an error. */
    char canonical_path[PATH_MAX], canonical_expected[PATH_MAX];
    if (realpath(path, canonical_path) && realpath(expected, canonical_expected))
        return strcmp(canonical_path, canonical_expected) == 0;
    return 0;
}

static int have_program(const char *path) {
    return path && path[0] && access(path, X_OK) == 0;
}

static int state_count(const PredFitState *p);
static void parameter_label(PickettParameter *x);
static void sync_basic_from_parameters(PredFitState *p);
static void store_active_hamiltonian(PredFitState *p);
static void restore_active_model(PredFitState *p, const PredFitSnapshot *snap);
static void snapshot_active_model(const PredFitState *p, PredFitSnapshot *snap);
static int assignment_belongs_to_active_hamiltonian(const AppState *s, const Assignment *a);
static void refresh_assignment_predictions(AppState *s);

static void exclusion_key_from_pred(FitExclusionKey *key, const PredLine *line) {
    const int qn[12] = {line->Ju, line->Kau, line->Kcu, line->M1u, line->M2u, line->M3u,
                        line->Jl, line->Kal, line->Kcl, line->M1l, line->M2l, line->M3l};
    key->n_qn = line->n_qn;
    memcpy(key->qn, qn, sizeof(key->qn));
}

static int exclusion_key_matches(const FitExclusionKey *key, const PredLine *line) {
    FitExclusionKey candidate;
    exclusion_key_from_pred(&candidate, line);
    return key->n_qn == candidate.n_qn &&
           memcmp(key->qn, candidate.qn, sizeof(key->qn)) == 0;
}

static void exclusions_path(const AppState *s, char *out, size_t size) {
    work_file(&s->predfit, "model.exclusions.txt", out, size);
}

/* One small, versioned sidecar holds the transient fitting choice.  The
   write is atomic: a failed save leaves the previous good set untouched. */
int predfit_save_exclusions(AppState *s) {
    PredFitState *p = &s->predfit;
    char path[700], tmp[740];
    int excluded = 0;
    for (int i = 0; i < s->n_assignments; i++)
        if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i]) &&
            !s->assignments[i].fit_enabled) excluded++;
    exclusions_path(s, path, sizeof(path));
    /* No stale file to clear and no workspace yet: avoid creating .fit only
       because every assignment happens to be included. */
    if (!excluded && access(path, F_OK) != 0) return 1;
    if (mkdir(p->work_dir, 0700) != 0 && errno != EEXIST) {
        snprintf(p->status, sizeof(p->status), "Cannot create %s for fit exclusions.", p->work_dir);
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        snprintf(p->status, sizeof(p->status), "Cannot save fit exclusions.");
        return 0;
    }
    fprintf(fp, "# SpectraVisual fit exclusions, format 1: NQN then upper/lower QNs\n");
    for (int i = 0; i < s->n_assignments; i++) {
        if (s->assignments[i].fit_enabled ||
            !assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) continue;
        FitExclusionKey key;
        exclusion_key_from_pred(&key, &s->assignments[i].pred);
        fprintf(fp, "%d", key.n_qn);
        for (int q = 0; q < 12; q++) fprintf(fp, " %d", key.qn[q]);
        fputc('\n', fp);
    }
    int failed = ferror(fp);
    if (fclose(fp) != 0 || failed || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(p->status, sizeof(p->status), "Cannot save fit exclusions.");
        return 0;
    }
    return 1;
}

static int parse_exclusion_key(const char *line, FitExclusionKey *out) {
    long values[13];
    const char *at = line;
    for (int i = 0; i < 13; i++) {
        char *end = NULL;
        errno = 0;
        values[i] = strtol(at, &end, 10);
        if (end == at || errno == ERANGE || values[i] < INT_MIN || values[i] > INT_MAX) return 0;
        at = end;
    }
    while (isspace((unsigned char)*at)) at++;
    if (*at && *at != '#') return 0;
    if (values[0] < 1 || values[0] > 6) return 0;
    out->n_qn = (int)values[0];
    for (int i = 0; i < 12; i++) out->qn[i] = (int)values[i + 1];
    return 1;
}

/* Absence is a valid empty exclusion set.  Malformed records are ignored,
   never interpreted as a request to remove assignments. */
static int load_fit_exclusions(AppState *s) {
    char path[700];
    exclusions_path(s, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    for (int i = 0; i < s->n_assignments; i++)
        if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) s->assignments[i].fit_enabled = 1;
    int ignored = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        const char *at = line;
        while (isspace((unsigned char)*at)) at++;
        if (!*at || *at == '#') continue;
        FitExclusionKey key;
        if (!parse_exclusion_key(at, &key)) { ignored++; continue; }
        for (int i = 0; i < s->n_assignments; i++)
            if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i]) &&
                exclusion_key_matches(&key, &s->assignments[i].pred)) s->assignments[i].fit_enabled = 0;
    }
    fclose(fp);
    return ignored;
}

int predfit_load_exclusions(AppState *s) {
    return load_fit_exclusions(s);
}

static int fit_exclusions_file_exists(const AppState *s) {
    char path[700];
    exclusions_path(s, path, sizeof(path));
    return access(path, F_OK) == 0;
}

static double qrot_at(const PredFitState *p, double temp_k) {
    if (!(p->a > 0 && p->b > 0 && p->c > 0 && temp_k > 0)) return 0.0;
    double sigma = p->int_settings.sigma > 0.0 ? p->int_settings.sigma : 1.0;
    return 5.3311e6 * sqrt((temp_k * temp_k * temp_k) / (p->a * p->b * p->c)) / sigma;
}

/* TEMP is a single field in one SPCAT .int.  It is therefore the rotational
   temperature of the Hamiltonian, never a per-state setting. */
static double int_temp_at(const PredFitState *p, double fallback) {
    (void)fallback;
    return p->temp_k;
}

static double int_fqlim_at(const PredFitState *p) {
    return p->int_settings.fqlim_ghz > 0.0 ? p->int_settings.fqlim_ghz : p->fmax_ghz;
}

static int int_maxv_at(const PredFitState *p) {
    return p->int_settings.maxv >= 0 ? p->int_settings.maxv : state_count(p) - 1;
}

static void write_int_header(FILE *fp, const PredFitState *p, const char *title, double fallback_temp) {
    const PickettIntSettings *x = &p->int_settings;
    double temp = int_temp_at(p, fallback_temp);
    fprintf(fp, "%s\n%d %d %.12g %d %d %.8g %.8g %.8g %.8g %d\n",
            title, x->flags, x->tag, qrot_at(p, temp), x->fbegin, x->fend,
            x->intensity_cutoff, x->intensity_cutoff, int_fqlim_at(p), temp, int_maxv_at(p));
}

static int prepare_fit_dir(AppState *s) {
    PredFitState *p = &s->predfit;
    char root[600];
    fit_root(s, root, sizeof(root));
    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        snprintf(p->status, sizeof(p->status), "Cannot create %s.", root);
        return 0;
    }
    predfit_refresh_work_dir(s);
    if (mkdir(p->work_dir, 0700) != 0 && errno != EEXIST) {
        snprintf(p->status, sizeof(p->status), "Cannot create %s.", p->work_dir);
        return 0;
    }
    return 1;
}

static void work_file(const PredFitState *p, const char *name, char *out, size_t size) {
    char stem[128];
    hamiltonian_file_stem(p, stem, sizeof(stem));
    if (strncmp(name, "model", 5) == 0)
        snprintf(out, size, "%s/%s%s", p->work_dir, stem, name + 5);
    else
        snprintf(out, size, "%s/%s", p->work_dir, name);
}

static PickettSpecies *active_species(PredFitState *p) {
    if (p->active_species < 0 || p->active_species >= p->n_species) return NULL;
    return &p->species[p->active_species];
}

static int active_state_suffix(const PredFitState *p) {
    if (p->active_species < 0 || p->active_species >= p->n_species) return 0;
    return 11 * p->species[p->active_species].state_index;
}

static int state_count(const PredFitState *p) {
    int count = 1;
    for (int i = 0; i < p->n_species; i++)
        if (p->species[i].state_index + 1 > count) count = p->species[i].state_index + 1;
    return count;
}

/* The third Pickett option-line field after CHR and SPIND is NVIB.  It is a
   user-owned setting, not a mirror of the rows in species[].  Parse just
   enough of the line to validate it, accepting both blank and comma fields
   without changing its spelling or any trailing Pickett options. */
static int hamiltonian_nvib(const char *line, int *out) {
    if (!line || !line[0] || !out) return 0;
    const char *q = line;
    while (*q == ' ' || *q == '\t') q++;
    if (!*q) return 0;
    q++; /* CHR */
    while (*q == ' ' || *q == '\t' || *q == ',') q++;
    char *end = NULL;
    strtol(q, &end, 10); /* SPIND */
    if (end == q) return 0;
    q = end;
    while (*q == ' ' || *q == '\t' || *q == ',') q++;
    errno = 0;
    long nvib = strtol(q, &end, 10);
    if (end == q || errno == ERANGE || nvib < 1 || nvib > INT_MAX) return 0;
    *out = (int)nvib;
    return 1;
}

static int included_state_count(const PredFitState *p) {
    int count = 1;
    for (int i = 0; i < p->n_species; i++)
        if (p->species[i].predict_enabled && p->species[i].state_index + 1 > count)
            count = p->species[i].state_index + 1;
    return count;
}

/* SPFIT applies one QN layout to the whole .lin, selected by the .par that
   generated its model.cat.  The catalogue itself is the authoritative source
   for that layout: NVIB alone is not enough for every Pickett option line.
   The saved .par must still contain the current line, otherwise the catalogue
   predates an edit and Calculate has to make a new one first. */
static int current_model_nqn(const AppState *s, int *out) {
    const PredFitState *p = &s->predfit;
    char par_path[700], cat_path[700], line[512];
    work_file(p, "model.par", par_path, sizeof(par_path));
    work_file(p, "model.cat", cat_path, sizeof(cat_path));
    FILE *fp = fopen(par_path, "r");
    if (!fp) return 0;
    for (int i = 0; i < 3; i++)
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    fclose(fp);
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, p->hamiltonian_line) != 0) return 0;

    PredLine *rows = NULL;
    double xmin, xmax, max_int;
    int n = read_pred_cat_alloc(cat_path, &rows, &xmin, &xmax, &max_int);
    if (n < 1) { free(rows); return 0; }
    int nqn = rows[0].n_qn;
    free(rows);
    if (nqn < 1 || nqn > 6) return 0;
    *out = nqn;
    return 1;
}

/* The quick controls remain a view of the selected species.  The global
   parameter list is deliberately retained because SPFIT needs one .par/.var
   for all states. */
/* Default colours of the per-state broadened traces.  Chosen to stay apart on
   the dark plot and from the experimental trace; the user can replace any of
   them from the Simulation page. */
static const SDL_Color SPECIES_PALETTE[8] = {
    { 90, 200, 250, 235}, {130, 220, 130, 235}, {245, 220,  90, 235}, {240, 110, 110, 235},
    {235, 130, 200, 235}, {170, 150, 245, 235}, { 47, 212, 232, 235}, {255, 170,  80, 235}
};

static int species_color_taken(const PredFitState *p, SDL_Color c) {
    for (int h = 0; h < p->n_hamiltonians; h++) {
        const PredFitSnapshot *m = &p->hamiltonian[h].model;
        for (int k = 0; k < m->n_species; k++) {
            SDL_Color o = m->species[k].trace_color;
            if (o.a > 0 && o.r == c.r && o.g == c.g && o.b == c.b) return 1;
        }
    }
    /* The active Hamiltonian's live states are not in its snapshot yet. */
    for (int k = 0; k < p->n_species; k++) {
        SDL_Color o = p->species[k].trace_color;
        if (o.a > 0 && o.r == c.r && o.g == c.g && o.b == c.b) return 1;
    }
    return 0;
}

/* The next colour of the project.  A state is given one when it is born, so
   the swatch in Simulation, the plot and a saved session always agree.  A
   colour already on screen is skipped: a plain counter can wrap onto one -
   five Hamiltonians of one state each consume the palette unevenly - and two
   traces of the same colour are exactly what this is for. */
static void assign_default_species_color(PredFitState *p, PickettSpecies *sp) {
    if (!p || !sp) return;
    for (int step = 0; step < 8; step++) {
        SDL_Color c = SPECIES_PALETTE[(p->species_color_next + step) % 8];
        if (species_color_taken(p, c)) continue;
        sp->trace_color = c;
        p->species_color_next += step + 1;
        return;
    }
    sp->trace_color = SPECIES_PALETTE[(p->species_color_next++) % 8];
}

SDL_Color predfit_species_color(const PickettSpecies *sp, int fallback_index) {
    if (sp && sp->trace_color.a > 0) return sp->trace_color;
    if (fallback_index < 0) fallback_index = 0;
    return SPECIES_PALETTE[fallback_index % 8];
}

static void store_active_species(PredFitState *p) {
    PickettSpecies *sp = active_species(p);
    if (!sp) return;
    memcpy(sp->mu, p->mu, sizeof(sp->mu));
}

static void load_active_species(PredFitState *p) {
    PickettSpecies *sp = active_species(p);
    if (!sp) return;
    memcpy(p->mu, sp->mu, sizeof(p->mu));
}

/* SPFIT overwrites the parameter uncertainties in .var/.par with estimated
   errors.  Our table's uncertainty is instead the user's fit-control value:
   1.0 by default, or 0/fixed/a custom prior set in Advanced.  The same file
   also records the experimental spectra of the session: a prediction alone is
   not a session, and the app used to reopen with the catalogue restored and no
   trace to compare it against. */
static void session_path(const AppState *s, char *out, size_t size) {
    char root[600];
    /* The configured data directory is authoritative.  work_dir may still
       refer to the preceding location immediately after Settings is edited. */
    fit_root(s, root, sizeof(root));
    snprintf(out, size, "%s/spectravisual.state", root);
}

int predfit_is_session_file(const AppState *s, const char *path) {
    if (!path || !path[0]) return 0;
    char state_path[600], state_real[PATH_MAX], candidate_real[PATH_MAX];
    session_path(s, state_path, sizeof(state_path));
    if (realpath(state_path, state_real) && realpath(path, candidate_real))
        return strcmp(state_real, candidate_real) == 0;
    return strcmp(state_path, path) == 0;
}

void predfit_save_session(AppState *s) {
    PredFitState *p = &s->predfit;
    store_active_hamiltonian(p);
    char root[600];
    fit_root(s, root, sizeof(root));
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return;
    char path[600], tmp_path[640];
    session_path(s, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return;
    fputs("# SpectraVisual session v5\n", fp);
    fputs("# Pickett parameter ID, value and user-selected fit uncertainty\n", fp);
    for (int i = 0; i < p->n_param; i++)
        fprintf(fp, "param %d %.17g %.17g\n", p->param[i].id, p->param[i].value, p->param[i].error);
    fprintf(fp, "line_error %.17g\n", p->line_error_mhz);
    fprintf(fp, "hamiltonian %s\n", p->hamiltonian_line);
    {
        const PickettIntSettings *x = &p->int_settings;
        fprintf(fp, "int2 %d %d %d %d %.17g %.17g %.17g %d %.17g\n",
                x->flags, x->tag, x->fbegin, x->fend, x->intensity_cutoff,
                x->fqlim_ghz, p->temp_k, x->maxv, x->sigma);
    }
    fprintf(fp, "trot %.17g\n", p->temp_k);
    for (int i = 0; i < p->n_species; i++) {
        const PickettSpecies *sp = &p->species[i];
        const double *mu = i == p->active_species ? p->mu : sp->mu;
        fprintf(fp, "state3 %d %d %.17g %.17g %.17g %.17g %s\n",
                sp->state_index, sp->predict_enabled != 0, mu[0], mu[1], mu[2],
                sp->concentration, sp->name);
    }
    fprintf(fp, "active_molecule %d\n", p->active_species);
    /* The unprefixed records above are a readable compatibility projection of
       the active model.  h4 records are the authoritative project list. */
    fprintf(fp, "species_traces %d\n", p->species_trace_mode);
    fprintf(fp, "h4project %d %d %d\n", p->n_hamiltonians,
            p->active_hamiltonian, p->next_hamiltonian_id);
    for (int h = 0; h < p->n_hamiltonians; h++) {
        const HamiltonianModel *model = &p->hamiltonian[h];
        const PredFitSnapshot *m = &model->model;
        fprintf(fp, "h4begin %d %s\n", model->id, model->name);
        if (model->simulate_excluded) fputs("h4sim 0\n", fp);
        for (int i = 0; i < m->n_param; i++)
            fprintf(fp, "h4param %d %.17g %.17g\n", m->param[i].id,
                    m->param[i].value, m->param[i].error);
        fprintf(fp, "h4line_error %.17g\n", m->line_error_mhz);
        fprintf(fp, "h4line %s\n", m->hamiltonian_line);
        fprintf(fp, "h4int %d %d %d %d %.17g %.17g %d %.17g\n",
                m->int_settings.flags, m->int_settings.tag, m->int_settings.fbegin,
                m->int_settings.fend, m->int_settings.intensity_cutoff,
                m->int_settings.fqlim_ghz, m->int_settings.maxv, m->int_settings.sigma);
        fprintf(fp, "h4trot %.17g\n", m->temp_k);
        for (int i = 0; i < m->n_species; i++) {
            const PickettSpecies *sp = &m->species[i];
            fprintf(fp, "h4state %d %d %.17g %.17g %.17g %.17g %s\n",
                    sp->state_index, sp->predict_enabled != 0, sp->mu[0], sp->mu[1],
                    sp->mu[2], sp->concentration, sp->name);
            /* Written only when a dipole was switched off, so a session that
               says nothing simulates with every component. */
            if (sp->mu_excluded[0] || sp->mu_excluded[1] || sp->mu_excluded[2])
                fprintf(fp, "h4statemu %d %d %d %d\n", i, sp->mu_excluded[0] != 0,
                        sp->mu_excluded[1] != 0, sp->mu_excluded[2] != 0);
            if (sp->trace_color.a > 0)
                fprintf(fp, "h4statecolor %d %d %d %d %d\n", i, sp->trace_color.r,
                        sp->trace_color.g, sp->trace_color.b, sp->trace_color.a);
        }
        fprintf(fp, "h4active_state %d\n", m->active_species);
        fputs("h4end\n", fp);
    }
    fprintf(fp, "view %.17g %.17g %.17g %.17g %.17g %.17g %d %d\n",
            s->vxmin, s->vxmax, s->vymin, s->vymax, s->pvxmin, s->pvxmax,
            s->sync_active != 0, s->rolling_avg_window);
    int saved_active = -1;
    int saved_count = 0;
    for (int i = 0; i < s->n_spectra; i++) {
        const Spectrum *sp = &s->spectra[i];
        /* Repair old contaminated sessions while saving.  The state file has
           numeric rows and must never be accepted as experimental data. */
        if (predfit_is_session_file(s, sp->path)) continue;
        char full[PATH_MAX];
        const char *stored = realpath(sp->path, full) ? full : sp->path;
        /* The active trace is mirrored in AppState until the end of a frame.
           Saving from a Fit click must therefore take those live values. */
        double offset = i == s->active_spec ? s->exp_offset : sp->exp_offset;
        int smoothing = i == s->active_spec ? s->rolling_avg_active : sp->rolling_avg_active;
        fprintf(fp, "spectrum2 %d %d %d %.17g %.17g %.17g %s\n",
                sp->visible != 0, smoothing != 0, sp->opacity, sp->vscale,
                offset, sp->voffset, stored);
        if (i == s->active_spec) saved_active = saved_count;
        saved_count++;
    }
    if (saved_active >= 0) fprintf(fp, "active %d\n", saved_active);
    if (fclose(fp) == 0 && rename(tmp_path, path) == 0) p->session_dirty = 0;
}

static void session_set_parameter(PredFitState *p, int id, double value, double error) {
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id != id) continue;
        p->param[i].value = value;
        p->param[i].error = error;
        parameter_label(&p->param[i]);
        return;
    }
    if (p->n_param >= MAX_PICKETT_PARAMS) return;
    PickettParameter *x = &p->param[p->n_param++];
    *x = (PickettParameter){id, value, error, ""};
    parameter_label(x);
}

static void snapshot_set_parameter(PredFitSnapshot *m, int id, double value, double error) {
    for (int i = 0; i < m->n_param; i++) {
        if (m->param[i].id != id) continue;
        m->param[i].value = value;
        m->param[i].error = error;
        parameter_label(&m->param[i]);
        return;
    }
    if (m->n_param >= MAX_PICKETT_PARAMS) return;
    PickettParameter *x = &m->param[m->n_param++];
    *x = (PickettParameter){id, value, error, ""};
    parameter_label(x);
}

typedef struct {
    int has_int_settings;
} SessionRestoreInfo;

/* The public entry point is used during normal startup.  Restore also needs
   to know whether a modern session supplied the complete INT settings, so its
   private path receives that small bit of provenance. */
static void load_session(AppState *s, SessionRestoreInfo *restore_info) {
    PredFitState *p = &s->predfit;
    if (restore_info) memset(restore_info, 0, sizeof(*restore_info));
    char path[600]; session_path(s, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    s->n_session_spec = 0;
    s->session_active_spec = -1;
    s->session_has_view = 0;
    p->n_species = 0;
    p->active_species = 0;
    double legacy_state_temp[MAX_PICKETT_SPECIES] = {0.0};
    int legacy_state_temp_count = 0;
    double legacy_int_temp = 0.0;
    int has_trot = 0;
    HamiltonianModel loaded_h[MAX_PICKETT_HAMILTONIANS] = {0};
    int n_loaded_h = 0, loaded_active_h = 0, loaded_next_h_id = 0;
    HamiltonianModel *h4 = NULL;
    PredFitSnapshot h4_defaults = p->hamiltonian[0].model;
    char line[700];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "species_traces ", 15) == 0) {
            int mode = atoi(line + 15);
            p->species_trace_mode = (mode >= 0 && mode <= 2) ? mode : TRACE_SUM;
            continue;
        }
        if (strncmp(line, "h4project ", 10) == 0) {
            int ignored_count = 0;
            sscanf(line + 10, "%d %d %d", &ignored_count, &loaded_active_h, &loaded_next_h_id);
            continue;
        }
        if (strncmp(line, "h4begin ", 8) == 0) {
            int id = 0, consumed = 0;
            if (sscanf(line + 8, "%d %n", &id, &consumed) == 1 && id > 0 &&
                n_loaded_h < MAX_PICKETT_HAMILTONIANS) {
                h4 = &loaded_h[n_loaded_h++];
                h4->id = id;
                h4->model = h4_defaults;
                h4->model.n_species = 0;
                h4->model.active_species = 0;
                const char *name = line + 8 + consumed;
                while (*name == ' ' || *name == '\t') name++;
                snprintf(h4->name, sizeof(h4->name), "%s", *name ? name : "Hamiltonian");
            }
            continue;
        }
        if (strcmp(line, "h4end") == 0) { h4 = NULL; continue; }
        if (h4 && strncmp(line, "h4param ", 8) == 0) {
            int id = 0; double value = 0.0, error = 0.0;
            if (sscanf(line + 8, "%d %lf %lf", &id, &value, &error) == 3 && id > 0)
                snapshot_set_parameter(&h4->model, id, value, error);
            continue;
        }
        if (h4 && strncmp(line, "h4line_error ", 13) == 0) {
            double value = strtod(line + 13, NULL);
            if (isfinite(value) && value > 0.0) h4->model.line_error_mhz = value;
            continue;
        }
        if (h4 && strncmp(line, "h4line ", 7) == 0) {
            if (line[7]) snprintf(h4->model.hamiltonian_line, sizeof(h4->model.hamiltonian_line), "%s", line + 7);
            continue;
        }
        if (h4 && strncmp(line, "h4int ", 6) == 0) {
            PickettIntSettings x = h4->model.int_settings;
            if (sscanf(line + 6, "%d %d %d %d %lf %lf %d %lf",
                       &x.flags, &x.tag, &x.fbegin, &x.fend, &x.intensity_cutoff,
                       &x.fqlim_ghz, &x.maxv, &x.sigma) == 8) {
                h4->model.int_settings = x;
                if (restore_info) restore_info->has_int_settings = 1;
            }
            continue;
        }
        if (h4 && strncmp(line, "h4trot ", 7) == 0) {
            double value = strtod(line + 7, NULL);
            if (isfinite(value) && value > 0.0) {
                h4->model.temp_k = value;
                h4->model.int_settings.temp_k = value;
            }
            continue;
        }
        if (h4 && strncmp(line, "h4state ", 8) == 0) {
            PickettSpecies sp = {0};
            int consumed = 0;
            int got = sscanf(line + 8, "%d %d %lf %lf %lf %lf %n",
                             &sp.state_index, &sp.predict_enabled, &sp.mu[0], &sp.mu[1],
                             &sp.mu[2], &sp.concentration, &consumed);
            const char *name = line + 8 + consumed;
            while (*name == ' ' || *name == '\t') name++;
            if (got == 6 && h4->model.n_species < MAX_PICKETT_SPECIES) {
                snprintf(sp.name, sizeof(sp.name), "%s", *name ? name : "State");
                sp.predict_enabled = sp.predict_enabled != 0;
                h4->model.species[h4->model.n_species++] = sp;
            }
            continue;
        }
        if (h4 && strncmp(line, "h4sim ", 6) == 0) {
            h4->simulate_excluded = atoi(line + 6) == 0;
            continue;
        }
        if (h4 && strncmp(line, "h4statemu ", 10) == 0) {
            int index = -1, a = 0, b = 0, c = 0;
            if (sscanf(line + 10, "%d %d %d %d", &index, &a, &b, &c) == 4 &&
                index >= 0 && index < h4->model.n_species) {
                h4->model.species[index].mu_excluded[0] = a != 0;
                h4->model.species[index].mu_excluded[1] = b != 0;
                h4->model.species[index].mu_excluded[2] = c != 0;
            }
            continue;
        }
        if (h4 && strncmp(line, "h4statecolor ", 13) == 0) {
            int index = -1, cr = 0, cg = 0, cb = 0, ca = 235;
            if (sscanf(line + 13, "%d %d %d %d %d", &index, &cr, &cg, &cb, &ca) >= 4 &&
                index >= 0 && index < h4->model.n_species) {
                SDL_Color c = {(Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)(ca > 0 ? ca : 235)};
                h4->model.species[index].trace_color = c;
            }
            continue;
        }
        if (h4 && strncmp(line, "h4active_state ", 15) == 0) {
            h4->model.active_species = atoi(line + 15);
            continue;
        }
        if (strncmp(line, "param ", 6) == 0) {
            int id = 0; double value = 0.0, error = 0.0;
            if (sscanf(line + 6, "%d %lf %lf", &id, &value, &error) == 3 && id > 0)
                session_set_parameter(p, id, value, error);
            continue;
        }
        if (strncmp(line, "line_error ", 11) == 0) {
            char *end = NULL;
            errno = 0;
            double value = strtod(line + 11, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end != line + 11 && end && !*end && errno != ERANGE &&
                isfinite(value) && value > 0.0)
                p->line_error_mhz = value;
            continue;
        }
        if (strncmp(line, "view ", 5) == 0) {
            int sync = 1, average_window = s->rolling_avg_window;
            if (sscanf(line + 5, "%lf %lf %lf %lf %lf %lf %d %d",
                       &s->session_vxmin, &s->session_vxmax,
                       &s->session_vymin, &s->session_vymax,
                       &s->session_pvxmin, &s->session_pvxmax,
                       &sync, &average_window) == 8) {
                s->session_has_view = 1;
                s->session_sync_active = sync != 0;
                s->session_rolling_avg_window = average_window;
            }
            continue;
        }
        if (strncmp(line, "hamiltonian ", 12) == 0) {
            if (line[12]) snprintf(p->hamiltonian_line, sizeof(p->hamiltonian_line), "%s", line + 12);
            continue;
        }
        if (strncmp(line, "int2 ", 5) == 0) {
            PickettIntSettings x = p->int_settings;
            if (sscanf(line + 5, "%d %d %d %d %lf %lf %lf %d %lf",
                       &x.flags, &x.tag, &x.fbegin, &x.fend, &x.intensity_cutoff,
                       &x.fqlim_ghz, &x.temp_k, &x.maxv, &x.sigma) == 9) {
                p->int_settings = x;
                if (x.temp_k > 0.0) legacy_int_temp = x.temp_k;
                if (restore_info) restore_info->has_int_settings = 1;
            }
            continue;
        }
        if (strncmp(line, "trot ", 5) == 0) {
            char *end = NULL;
            double value = strtod(line + 5, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end != line + 5 && end && !*end && isfinite(value) && value > 0.0)
                p->temp_k = value;
                has_trot = 1;
            continue;
        }
        if (strncmp(line, "state3 ", 7) == 0) {
            PickettSpecies sp = {0};
            int consumed = 0;
            int got = sscanf(line + 7, "%d %d %lf %lf %lf %lf %n",
                             &sp.state_index, &sp.predict_enabled, &sp.mu[0],
                             &sp.mu[1], &sp.mu[2], &sp.concentration, &consumed);
            const char *name = line + 7 + consumed;
            while (*name == ' ' || *name == '\t') name++;
            if (got == 6 && p->n_species < MAX_PICKETT_SPECIES) {
                snprintf(sp.name, sizeof(sp.name), "%s", *name ? name : "State");
                sp.predict_enabled = sp.predict_enabled != 0;
                p->species[p->n_species++] = sp;
            }
            continue;
        }
        if (strncmp(line, "molecule2 ", 10) == 0) {
            PickettSpecies sp = {0};
            double old_temp = 0.0;
            int consumed = 0;
            int got = sscanf(line + 10, "%d %d %lf %lf %lf %lf %lf %n",
                             &sp.state_index, &sp.predict_enabled, &old_temp, &sp.mu[0],
                             &sp.mu[1], &sp.mu[2], &sp.concentration, &consumed);
            const char *name = line + 10 + consumed;
            while (*name == ' ' || *name == '\t') name++;
            if (got == 7 && p->n_species < MAX_PICKETT_SPECIES) {
                snprintf(sp.name, sizeof(sp.name), "%s", *name ? name : "Species");
                sp.predict_enabled = sp.predict_enabled != 0;
                p->species[p->n_species++] = sp;
                legacy_state_temp[legacy_state_temp_count++] = old_temp;
            }
            continue;
        }
        /* v2 stored the same fields but had no prediction checkbox: those
           species remain included on restore. */
        if (strncmp(line, "molecule ", 9) == 0) {
            PickettSpecies sp = {0};
            double old_temp = 0.0;
            int consumed = 0;
            int got = sscanf(line + 9, "%d %lf %lf %lf %lf %lf %n",
                             &sp.state_index, &old_temp, &sp.mu[0], &sp.mu[1],
                             &sp.mu[2], &sp.concentration, &consumed);
            const char *name = line + 9 + consumed;
            while (*name == ' ' || *name == '\t') name++;
            if (got == 6 && p->n_species < MAX_PICKETT_SPECIES) {
                snprintf(sp.name, sizeof(sp.name), "%s", *name ? name : "Species");
                sp.predict_enabled = 1;
                p->species[p->n_species++] = sp;
                legacy_state_temp[legacy_state_temp_count++] = old_temp;
            }
            continue;
        }
        if (strncmp(line, "active_molecule ", 16) == 0) {
            p->active_species = atoi(line + 16);
            continue;
        }
        if (strncmp(line, "spectrum2 ", 10) == 0) {
            SessionSpectrum saved = {0};
            int consumed = 0;
            int got = sscanf(line + 10, "%d %d %d %lf %lf %lf %n",
                             &saved.visible, &saved.rolling_avg_active,
                             &saved.opacity, &saved.vscale, &saved.exp_offset,
                             &saved.voffset, &consumed);
            const char *stored = line + 10 + consumed;
            while (*stored == ' ' || *stored == '\t') stored++;
            if (got == 6 && s->n_session_spec < MAX_SPECTRA && *stored &&
                !predfit_is_session_file(s, stored)) {
                snprintf(saved.path, sizeof(saved.path), "%s", stored);
                s->session_spectrum[s->n_session_spec++] = saved;
            }
            continue;
        }
        /* v1 sessions only had the path.  Preserve backward compatibility
           and give their trace the same defaults as a freshly opened file. */
        if (strncmp(line, "spectrum ", 9) == 0) {
            if (s->n_session_spec < MAX_SPECTRA && line[9] &&
                !predfit_is_session_file(s, line + 9)) {
                SessionSpectrum saved = {0};
                saved.visible = 1; saved.opacity = s->settings.trace_opacity; saved.vscale = 1.0;
                snprintf(saved.path, sizeof(saved.path), "%s", line + 9);
                s->session_spectrum[s->n_session_spec++] = saved;
            }
            continue;
        }
        if (strncmp(line, "active ", 7) == 0) { s->session_active_spec = atoi(line + 7); continue; }
        /* v1-v3 stored just "ID uncertainty".  Keep reading it, but only
           modern param rows can reconstruct a term absent from defaults. */
        int id = 0; double error = 0;
        if (sscanf(line, "%d %lf", &id, &error) != 2) continue;
        for (int i = 0; i < p->n_param; i++) if (p->param[i].id == id) { p->param[i].error = error; break; }
    }
    fclose(fp);
    if (n_loaded_h > 0) {
        for (int i = 0; i < n_loaded_h; i++) {
            if (loaded_h[i].model.n_species == 0) {
                loaded_h[i].model.species[0] = (PickettSpecies){"State 0", 0, 1, {1.0, 1.0, 1.0}, 1.0};
                loaded_h[i].model.n_species = 1;
            }
            if (loaded_h[i].model.active_species < 0 ||
                loaded_h[i].model.active_species >= loaded_h[i].model.n_species)
                loaded_h[i].model.active_species = 0;
        }
        memcpy(p->hamiltonian, loaded_h, (size_t)n_loaded_h * sizeof(loaded_h[0]));
        p->n_hamiltonians = n_loaded_h;
        /* Restart the palette past everything the session already holds, and
           give a colour to the states of a session written before they had
           one, so no two states of the project share a default. */
        p->species_color_next = 0;
        for (int i = 0; i < n_loaded_h; i++)
            for (int k = 0; k < p->hamiltonian[i].model.n_species; k++) {
                PickettSpecies *sp = &p->hamiltonian[i].model.species[k];
                if (sp->trace_color.a > 0) p->species_color_next++;
                else assign_default_species_color(p, sp);
            }
        if (loaded_active_h < 0 || loaded_active_h >= n_loaded_h) loaded_active_h = 0;
        p->active_hamiltonian = loaded_active_h;
        int max_id = 0;
        for (int i = 0; i < n_loaded_h; i++) if (p->hamiltonian[i].id > max_id) max_id = p->hamiltonian[i].id;
        p->next_hamiltonian_id = loaded_next_h_id > max_id ? loaded_next_h_id : max_id + 1;
        restore_active_model(p, &p->hamiltonian[p->active_hamiltonian].model);
        sync_basic_from_parameters(p);
        return;
    }
    if (p->n_species == 0) {
        p->species[0] = (PickettSpecies){"Species 1", 0, 1, {p->mu[0], p->mu[1], p->mu[2]}, 1.0};
        p->n_species = 1;
    }
    if (p->active_species < 0 || p->active_species >= p->n_species) p->active_species = 0;
    /* Old sessions had an independent Tred per species.  Migrate their
       selected state's value to the one Hamiltonian-wide Trot. */
    if (p->active_species < legacy_state_temp_count && legacy_state_temp[p->active_species] > 0.0)
        p->temp_k = legacy_state_temp[p->active_species];
    else if (!has_trot && legacy_int_temp > 0.0)
        p->temp_k = legacy_int_temp;
    load_active_species(p);
    sync_basic_from_parameters(p);
}

void predfit_load_session(AppState *s) {
    load_session(s, NULL);
}

static int hamiltonian_index_by_id(const PredFitState *p, int id) {
    for (int h = 0; h < p->n_hamiltonians; h++)
        if (p->hamiltonian[h].id == id) return h;
    return -1;
}

void predfit_recompute_display_intensities(AppState *s) {
    if (!s || !s->pred_lines || s->n_pred <= 0) return;
    PredFitState *p = &s->predfit;
    if (p->generated_catalog_active && p->n_simulated > 0) {
        /* A simulated plot holds the catalogues of several Hamiltonians: each
           row is rescaled with the model that produced it.  Trot and the state
           concentrations are read live, so editing them restyles the plot
           without running SPCAT again; only Tcat is frozen at its run. */
        static PredIntensityModel models[MAX_PICKETT_HAMILTONIANS];
        int n = 0;
        for (int i = 0; i < p->n_simulated; i++) {
            int h = hamiltonian_index_by_id(p, p->simulated[i].hamiltonian_id);
            if (h < 0) continue;
            const PredFitSnapshot *m = h == p->active_hamiltonian ? NULL : &p->hamiltonian[h].model;
            models[n].hamiltonian_id = p->simulated[i].hamiltonian_id;
            models[n].cat_temp_k = p->simulated[i].cat_temp_k;
            models[n].rot_temp_k = m ? m->temp_k : p->temp_k;
            models[n].species = m ? m->species : p->species;
            models[n].n_species = m ? m->n_species : p->n_species;
            n++;
        }
        if (n > 0) {
            rescale_predicted_intensities_multi(s->pred_lines, s->n_pred, models, n,
                                                &s->pred_global_max);
            return;
        }
    }
    if (s->pred_lines && s->n_pred > 0 && p->generated_catalog_active) {
        double tcat = int_temp_at(p, p->temp_k);
        rescale_predicted_intensities_by_species(s->pred_lines, s->n_pred, tcat, p->temp_k,
                                                  p->species, p->n_species,
                                                  &s->pred_global_max);
    } else {
        rescale_predicted_intensities(s->pred_lines,s->n_pred,s->cat_temp_k,s->rot_temp_k,
                                      s->dipole_cat,s->dipole_red,&s->pred_global_max);
    }
}

/* Pred&Fit deliberately publishes its active species only when a Pred&Fit
   action changes it.  Intensity analysis must not call this reverse route. */
void predfit_publish_shared_state(AppState *s) {
    PredFitState *p=&s->predfit;
    store_active_species(p);
    s->rot_temp_k=p->temp_k;
    memcpy(s->dipole_red,p->mu,sizeof(p->mu));
    if (p->generated_catalog_active) s->cat_temp_k=int_temp_at(p, p->temp_k);
    predfit_recompute_display_intensities(s);
}

/* SPCAT receives one .int beside the shared multi-state .var. model.int encodes the
   diagonal a/b/c dipoles with V2=V1=v so that SPCAT emits transitions carrying
   the vibrational/state quantum number.  For states 0..9 the Pickett IDs are
   001/002/003, 111/112/113, 221/222/223, ... respectively. */
static int write_multi_state_int(FILE *fp, const PredFitState *p) {
    if (!fp) return 0;
    /* A dipole the user unchecked in Simulation is written as zero: SPCAT
       then predicts no transition of that type, which is exactly what "show
       only the a-type lines" means. */
    int included = 0;
    for (int i = 0; i < p->n_species; i++) {
        const PickettSpecies *sp = &p->species[i];
        if (!sp->predict_enabled) continue;
        for (int k = 0; k < 3; k++) if (!sp->mu_excluded[k] && sp->mu[k] != 0.0) included++;
    }
    if (!included) return 0;
    write_int_header(fp, p, "SpectraVisual multi-state prediction", p->temp_k);
    for (int i = 0; i < p->n_species; i++) {
        const PickettSpecies *sp = &p->species[i];
        if (!sp->predict_enabled) continue;
        if (sp->state_index < 0 || sp->state_index > 9) return 0;
        int id = 110 * sp->state_index;
        static const char *axis[3] = {"a", "b", "c"};
        for (int k = 0; k < 3; k++)
            fprintf(fp, "%d %.10g /%s dipole/\n", id + 1 + k,
                    sp->mu_excluded[k] ? 0.0 : sp->mu[k], axis[k]);
    }
    return 1;
}

void predfit_adopt_shared_state(AppState *s) {
    PredFitState *p=&s->predfit;
    if (s->rot_temp_k > 0.0) p->temp_k=s->rot_temp_k;
    for (int c=0;c<3;c++) if (s->dipole_red[c] != 0.0) p->mu[c]=s->dipole_red[c];
    p->session_dirty = 1;
    store_active_species(p);
}

void predfit_adopt_generated_catalog(AppState *s) {
    PredFitState *p=&s->predfit;
    if (!p->generated_catalog_pending) return;
    p->generated_catalog_pending=0;
    p->generated_catalog_active=1;
    s->cat_temp_k=int_temp_at(p, p->temp_k);
    memcpy(s->dipole_cat,p->mu,sizeof(p->mu));
    s->rot_temp_k=p->temp_k;
    int hamiltonian_id = predfit_active_hamiltonian_id(s);
    for (int i = 0; i < s->n_pred; i++) s->pred_lines[i].hamiltonian_id = hamiltonian_id;
    predfit_recompute_display_intensities(s);
    refresh_assignment_predictions(s);
}

static int same_transition(const PredLine *a, const PredLine *b) {
    return a->n_qn == b->n_qn &&
           a->Ju == b->Ju && a->Kau == b->Kau && a->Kcu == b->Kcu &&
           a->M1u == b->M1u && a->M2u == b->M2u && a->M3u == b->M3u &&
           a->Jl == b->Jl && a->Kal == b->Kal && a->Kcl == b->Kcl &&
           a->M1l == b->M1l && a->M2l == b->M2l && a->M3l == b->M3l;
}

/* An assignment read from a .lin knows only its quantum numbers: a .lin holds
   no calculated frequency and no intensity.  As soon as a catalogue contains
   that exact transition of that exact model, the assignment takes its
   predicted line from it.  Identity - owner and quantum numbers - is what the
   match is made on and is therefore never changed by the refresh. */
static void refresh_assignment_predictions(AppState *s) {
    PredFitState *p = &s->predfit;
    if (!s->pred_lines || s->n_pred <= 0 || !p->generated_catalog_active) return;
    for (int i = 0; i < s->n_assignments; i++) {
        Assignment *a = &s->assignments[i];
        for (int k = 0; k < s->n_pred; k++) {
            const PredLine *q = &s->pred_lines[k];
            /* Pre-v2 assignments have no owner and belong to H1 by definition. */
            if (!(q->hamiltonian_id == a->hamiltonian_id ||
                  (a->hamiltonian_id == 0 && q->hamiltonian_id == 1))) continue;
            if (!same_transition(q, &a->pred)) continue;
            int owner = a->pred.hamiltonian_id;
            a->pred = *q;
            a->pred.hamiltonian_id = owner ? owner : q->hamiltonian_id;
            break;
        }
    }
}

/* Gives every assignment of one model the catalogue row of the same
   transition.  The match is made on identity - owner and quantum numbers -
   which the fill therefore never changes. */
static int fill_predictions_from_catalog(AppState *s, int owner_id,
                                         const PredLine *rows, int n) {
    int filled = 0;
    for (int i = 0; i < s->n_assignments; i++) {
        Assignment *a = &s->assignments[i];
        if (a->hamiltonian_id != owner_id) continue;
        for (int k = 0; k < n; k++) {
            if (!same_transition(&rows[k], &a->pred)) continue;
            a->pred = rows[k];
            a->pred.hamiltonian_id = owner_id;
            filled++;
            break;
        }
    }
    return filled;
}

/* Counterpart of predfit_adopt_generated_catalog for a simulated plot: the
   Hamiltonian id of every row is set while merging the catalogues, so here
   only the shared display state is refreshed. */
void predfit_adopt_simulation(AppState *s) {
    PredFitState *p = &s->predfit;
    p->generated_catalog_pending = 0;
    p->generated_catalog_active = 1;
    /* The single-model fields keep describing the active Hamiltonian: the top
       bar and Intensity analysis still read them. */
    s->cat_temp_k = int_temp_at(p, p->temp_k);
    memcpy(s->dipole_cat, p->mu, sizeof(p->mu));
    s->rot_temp_k = p->temp_k;
    predfit_recompute_display_intensities(s);
    refresh_assignment_predictions(s);
}

static void snapshot_active_model(const PredFitState *p, PredFitSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->a=p->a; snap->b=p->b; snap->c=p->c;
    memcpy(snap->mu, p->mu, sizeof(snap->mu));
    snap->temp_k=p->temp_k; snap->fmin_ghz=p->fmin_ghz; snap->fmax_ghz=p->fmax_ghz;
    snap->line_error_mhz=p->line_error_mhz;
    snap->int_settings=p->int_settings;
    snap->n_param=p->n_param;
    memcpy(snap->param, p->param, sizeof(snap->param));
    memcpy(snap->hamiltonian_line, p->hamiltonian_line, sizeof(snap->hamiltonian_line));
    snap->n_species=p->n_species;
    snap->active_species=p->active_species;
    memcpy(snap->species,p->species,sizeof(snap->species));
    if (snap->active_species >= 0 && snap->active_species < snap->n_species)
        memcpy(snap->species[snap->active_species].mu, p->mu, sizeof(snap->species[0].mu));
}

static void restore_active_model(PredFitState *p, const PredFitSnapshot *snap) {
    p->a=snap->a; p->b=snap->b; p->c=snap->c;
    memcpy(p->mu, snap->mu, sizeof(p->mu));
    p->temp_k=snap->temp_k; p->fmin_ghz=snap->fmin_ghz; p->fmax_ghz=snap->fmax_ghz;
    p->line_error_mhz=snap->line_error_mhz;
    p->int_settings=snap->int_settings;
    p->n_param=snap->n_param;
    memcpy(p->param, snap->param, sizeof(p->param));
    memcpy(p->hamiltonian_line, snap->hamiltonian_line, sizeof(p->hamiltonian_line));
    p->n_species=snap->n_species;
    p->active_species=snap->active_species;
    memcpy(p->species,snap->species,sizeof(p->species));
    if (p->n_species <= 0) p->n_species = 1;
    if (p->active_species < 0 || p->active_species >= p->n_species) p->active_species = 0;
    load_active_species(p);
}

static HamiltonianModel *active_hamiltonian(PredFitState *p) {
    if (p->active_hamiltonian < 0 || p->active_hamiltonian >= p->n_hamiltonians) return NULL;
    return &p->hamiltonian[p->active_hamiltonian];
}

static void store_active_hamiltonian(PredFitState *p) {
    HamiltonianModel *h = active_hamiltonian(p);
    if (h) snapshot_active_model(p, &h->model);
}

static int push_fit_snapshot(AppState *s) {
    PredFitState *p = &s->predfit;
    if (p->history_count == p->history_capacity) {
        int capacity = p->history_capacity ? p->history_capacity * 2 : 16;
        PredFitSnapshot *history = realloc(p->history, (size_t)capacity * sizeof(*history));
        if (!history) {
            snprintf(p->status, sizeof(p->status), "Cannot reserve fit-history memory.");
            return 0;
        }
        p->history = history;
        p->history_capacity = capacity;
    }
    PredFitSnapshot *snap = &p->history[p->history_count++];
    snapshot_active_model(p, snap);
    snap->n_exclusions = 0;
    for (int i = 0; i < s->n_assignments && snap->n_exclusions < MAX_ASSIGNMENTS; i++) {
        if (s->assignments[i].fit_enabled ||
            !assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) continue;
        exclusion_key_from_pred(&snap->exclusions[snap->n_exclusions++],
                                &s->assignments[i].pred);
    }
    return 1;
}

static void restore_fit_snapshot(AppState *s, const PredFitSnapshot *snap) {
    PredFitState *p = &s->predfit;
    restore_active_model(p, snap);
    /* Undo concerns the model and its temporary Fit choices, never the
       assignment list.  Match the saved exclusions by identity so deleting
       or reordering a row cannot move the exclusion to another transition. */
    for (int i = 0; i < s->n_assignments; i++)
        if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) s->assignments[i].fit_enabled = 1;
    for (int i = 0; i < s->n_assignments; i++)
        if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i]))
        for (int k = 0; k < snap->n_exclusions; k++)
            if (exclusion_key_matches(&snap->exclusions[k], &s->assignments[i].pred)) {
                s->assignments[i].fit_enabled = 0;
                break;
            }
    predfit_save_exclusions(s);
    store_active_hamiltonian(p);
}

void predfit_init(AppState *s) {
    PredFitState *p = &s->predfit;
    p->a = 10000.0; p->b = 1000.0; p->c = 900.0;
    p->mu[0] = p->mu[1] = p->mu[2] = 1.0;
    p->temp_k = 5.0; p->fmin_ghz = 0.0; p->fmax_ghz = 20.0;
    p->line_error_mhz = 0.01;
    p->int_settings = (PickettIntSettings){0, 1, 0, 40, -20.0, 0.0, 0.0, -1, 1.0};
    p->n_param = 3;
    /* A non-zero a-priori error makes the three supplied constants float in
       SPFIT.  The Advanced table can set an error to zero to keep one fixed. */
    p->param[0] = (PickettParameter){10000, p->a, 1.0, "A"};
    p->param[1] = (PickettParameter){20000, p->b, 1.0, "B"};
    p->param[2] = (PickettParameter){30000, p->c, 1.0, "C"};
    snprintf(p->hamiltonian_line, sizeof(p->hamiltonian_line), "s 1 1 0");
    p->species[0] = (PickettSpecies){"State 0", 0, 1, {1.0, 1.0, 1.0}, 1.0};
    p->n_species = 1;
    p->active_species = 0;
    p->n_hamiltonians = 1;
    p->active_hamiltonian = 0;
    p->next_hamiltonian_id = 2;
    p->hamiltonian[0].id = 1;
    /* `model` is the familiar initial Pickett basename; users can rename it
       and its next Calculate/Fit will use that visible name as the basename. */
    snprintf(p->hamiltonian[0].name, sizeof(p->hamiltonian[0].name), "model");
    snapshot_active_model(p, &p->hamiltonian[0].model);
    p->species_color_next = 1;   /* state 0 of H1 already holds the first colour */
    p->advanced_edit_param = -1;
    p->advanced_edit_species = -1;
    p->advanced_edit_hamiltonian = -1;
    p->advanced_nav_state = -1;
    p->advanced_delete_hamiltonian_id = 0;
    p->advanced_hover_line = -1;
    /* Point at the working directory straight away, so the Fitting tab shows
       the last run even before this session calculates or fits anything. */
    fit_root(s, p->work_dir, sizeof(p->work_dir));
}

int predfit_hamiltonian_count(const AppState *s) {
    return s ? s->predfit.n_hamiltonians : 0;
}

int predfit_active_hamiltonian_id(const AppState *s) {
    if (!s) return 0;
    const PredFitState *p = &s->predfit;
    if (p->active_hamiltonian < 0 || p->active_hamiltonian >= p->n_hamiltonians) return 0;
    return p->hamiltonian[p->active_hamiltonian].id;
}

static void default_hamiltonian_model(PredFitSnapshot *m) {
    memset(m, 0, sizeof(*m));
    m->a = 10000.0; m->b = 1000.0; m->c = 900.0;
    m->mu[0] = m->mu[1] = m->mu[2] = 1.0;
    m->temp_k = 5.0; m->fmin_ghz = 0.0; m->fmax_ghz = 20.0;
    m->line_error_mhz = 0.01;
    m->int_settings = (PickettIntSettings){0, 1, 0, 40, -20.0, 0.0, 5.0, -1, 1.0};
    m->n_param = 3;
    m->param[0] = (PickettParameter){10000, m->a, 1.0, "A"};
    m->param[1] = (PickettParameter){20000, m->b, 1.0, "B"};
    m->param[2] = (PickettParameter){30000, m->c, 1.0, "C"};
    snprintf(m->hamiltonian_line, sizeof(m->hamiltonian_line), "s 1 1 0");
    m->species[0] = (PickettSpecies){"State 0", 0, 1, {1.0, 1.0, 1.0}, 1.0, {0, 0, 0}, SPECIES_PALETTE[0]};
    m->n_species = 1;
    m->active_species = 0;
}

int predfit_add_hamiltonian(AppState *s, const char *name) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    if (p->n_hamiltonians >= MAX_PICKETT_HAMILTONIANS) {
        snprintf(p->status, sizeof(p->status), "At most %d Hamiltonians are supported in one project.",
                 MAX_PICKETT_HAMILTONIANS);
        return 0;
    }
    store_active_hamiltonian(p);
    HamiltonianModel *dst = &p->hamiltonian[p->n_hamiltonians++];
    memset(dst, 0, sizeof(*dst));
    dst->id = p->next_hamiltonian_id++;
    if (name && name[0]) snprintf(dst->name, sizeof(dst->name), "%s", name);
    else snprintf(dst->name, sizeof(dst->name), "Hamiltonian %d", dst->id);
    default_hamiltonian_model(&dst->model);
    for (int i = 0; i < dst->model.n_species; i++)
        assign_default_species_color(p, &dst->model.species[i]);
    p->active_hamiltonian = p->n_hamiltonians - 1;
    restore_active_model(p, &dst->model);
    predfit_refresh_work_dir(s);
    p->history_count = 0;
    p->generated_catalog_active = 0;
    p->generated_catalog_pending = 0;
    p->session_dirty = 1;
    snprintf(p->status, sizeof(p->status), "%s created as a new empty Hamiltonian.", dst->name);
    return 1;
}

int predfit_duplicate_hamiltonian(AppState *s, const char *name) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    if (p->n_hamiltonians >= MAX_PICKETT_HAMILTONIANS) {
        snprintf(p->status, sizeof(p->status), "At most %d Hamiltonians are supported in one project.",
                 MAX_PICKETT_HAMILTONIANS);
        return 0;
    }
    store_active_hamiltonian(p);
    HamiltonianModel *src = active_hamiltonian(p);
    HamiltonianModel *dst = &p->hamiltonian[p->n_hamiltonians++];
    *dst = *src;
    dst->id = p->next_hamiltonian_id++;
    if (name && name[0]) snprintf(dst->name, sizeof(dst->name), "%s", name);
    else snprintf(dst->name, sizeof(dst->name), "Hamiltonian %d", dst->id);
    /* A copy is a different model in the same plot: give it its own colours. */
    for (int i = 0; i < dst->model.n_species; i++)
        assign_default_species_color(p, &dst->model.species[i]);
    p->active_hamiltonian = p->n_hamiltonians - 1;
    restore_active_model(p, &dst->model);
    predfit_refresh_work_dir(s);
    p->history_count = 0; /* a fit history belongs to the source Hamiltonian */
    p->generated_catalog_active = 0;
    p->generated_catalog_pending = 0;
    p->session_dirty = 1;
    snprintf(p->status, sizeof(p->status), "%s created from the active Hamiltonian; calculate it before fitting.", dst->name);
    return 1;
}

int predfit_delete_hamiltonian(AppState *s, int index) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    if (p->n_hamiltonians <= 1) {
        snprintf(p->status, sizeof(p->status), "A project must retain at least one Hamiltonian.");
        return 0;
    }
    if (index < 0 || index >= p->n_hamiltonians) return 0;

    store_active_hamiltonian(p);
    const int removed_id = p->hamiltonian[index].id;
    char removed_name[sizeof(p->hamiltonian[index].name)];
    snprintf(removed_name, sizeof(removed_name), "%s", p->hamiltonian[index].name);

    /* The H is the ownership boundary for assignments.  Legacy owner 0 is
       H1 by definition, so it disappears together with H1 rather than being
       silently reassigned to another model. */
    int removed_assignments = 0, write = 0;
    for (int read = 0; read < s->n_assignments; read++) {
        Assignment *a = &s->assignments[read];
        if (a->hamiltonian_id == removed_id ||
            (removed_id == 1 && a->hamiltonian_id == 0)) {
            removed_assignments++;
            continue;
        }
        if (write != read) s->assignments[write] = *a;
        write++;
    }
    s->n_assignments = write;
    if (s->selected_assignment >= s->n_assignments)
        s->selected_assignment = s->n_assignments ? s->n_assignments - 1 : -1;
    if (s->assignments_scroll < 0) s->assignments_scroll = 0;
    if (s->assignments_scroll > s->n_assignments - 13)
        s->assignments_scroll = s->n_assignments > 13 ? s->n_assignments - 13 : 0;

    /* A generated catalogue belongs to the active H only.  Do not leave a
       now-orphaned prediction selectable after deleting that H. */
    if (p->generated_catalog_active && index == p->active_hamiltonian) {
        free(s->pred_lines);
        s->pred_lines = NULL;
        s->n_pred = 0;
        s->n_selected = 0;
        p->generated_catalog_active = 0;
        p->generated_catalog_pending = 0;
    }

    for (int i = index; i < p->n_hamiltonians - 1; i++)
        p->hamiltonian[i] = p->hamiltonian[i + 1];
    p->n_hamiltonians--;
    memset(&p->hamiltonian[p->n_hamiltonians], 0, sizeof(p->hamiltonian[0]));
    p->active_hamiltonian = index < p->n_hamiltonians ? index : p->n_hamiltonians - 1;
    restore_active_model(p, &p->hamiltonian[p->active_hamiltonian].model);
    predfit_refresh_work_dir(s);
    p->history_count = 0;
    p->advanced_delete_hamiltonian_id = 0;
    p->session_dirty = 1;
    save_assignments(s);
    predfit_save_exclusions(s);
    snprintf(p->status, sizeof(p->status), "%s deleted (%d assignment%s removed).",
             removed_name, removed_assignments, removed_assignments == 1 ? "" : "s");
    return 1;
}

int predfit_select_hamiltonian(AppState *s, int index) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    if (index < 0 || index >= p->n_hamiltonians) return 0;
    if (index == p->active_hamiltonian) return 1;
    store_active_hamiltonian(p);
    p->active_hamiltonian = index;
    restore_active_model(p, &p->hamiltonian[index].model);
    predfit_refresh_work_dir(s);
    p->history_count = 0;
    p->generated_catalog_active = 0;
    p->generated_catalog_pending = 0;
    p->advanced_delete_hamiltonian_id = 0;
    p->session_dirty = 1;
    snprintf(p->status, sizeof(p->status), "Selected %s; calculate it before fitting.", p->hamiltonian[index].name);
    return 1;
}

/* The sidebar shows the project in a user-chosen order.  Moving an entry
   changes that order and nothing else: a Hamiltonian is identified by its id
   and a state by its Pickett state index, so no assignment, catalogue row or
   working file follows the move. */
int predfit_move_hamiltonian(AppState *s, int index, int delta) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    int to = index + delta;
    if (index < 0 || index >= p->n_hamiltonians || to < 0 || to >= p->n_hamiltonians) return 0;
    store_active_hamiltonian(p);
    HamiltonianModel moved = p->hamiltonian[index];
    p->hamiltonian[index] = p->hamiltonian[to];
    p->hamiltonian[to] = moved;
    if (p->active_hamiltonian == index)      p->active_hamiltonian = to;
    else if (p->active_hamiltonian == to)    p->active_hamiltonian = index;
    p->session_dirty = 1;
    return 1;
}

int predfit_move_species(AppState *s, int index, int delta) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    int to = index + delta;
    if (index < 0 || index >= p->n_species || to < 0 || to >= p->n_species) return 0;
    store_active_species(p);
    PickettSpecies moved = p->species[index];
    p->species[index] = p->species[to];
    p->species[to] = moved;
    if (p->active_species == index)      p->active_species = to;
    else if (p->active_species == to)    p->active_species = index;
    load_active_species(p);
    p->session_dirty = 1;
    return 1;
}

static int assignment_belongs_to_active_hamiltonian(const AppState *s, const Assignment *a) {
    int active_id = predfit_active_hamiltonian_id(s);
    /* Pre-v2 assignment files have no owner. Their one-model semantics maps
       unambiguously to H1 and never to a later duplicated Hamiltonian. */
    return a->hamiltonian_id == active_id ||
           (a->hamiltonian_id == 0 && active_id == 1);
}

static void sync_basic_parameters(PredFitState *p) {
    int suffix = active_state_suffix(p);
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id == 10000 + suffix) p->param[i].value = p->a;
        if (p->param[i].id == 20000 + suffix) p->param[i].value = p->b;
        if (p->param[i].id == 30000 + suffix) p->param[i].value = p->c;
    }
}

static void sync_basic_from_parameters(PredFitState *p) {
    int suffix = active_state_suffix(p);
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id == 10000 + suffix) p->a = p->param[i].value;
        if (p->param[i].id == 20000 + suffix) p->b = p->param[i].value;
        if (p->param[i].id == 30000 + suffix) p->c = p->param[i].value;
    }
}

typedef struct { int id; const char *watson_a, *watson_s, *other; } ParameterName;

/* Pickett's identifiers are the actual interface; a typed ID therefore gets
   a physical label immediately, without relying on a separate lookup file. */
static const ParameterName PARAMETER_NAMES[] = {
    /* Watson A-reduction                 Watson S-reduction */
    {10000,"A","A",NULL},{20000,"B","B",NULL},{30000,"C","C",NULL},
    {200,"DeltaJ","DJ",NULL},{1100,"DeltaJK","DJK",NULL},{2000,"DeltaK","DK",NULL},
    {40100,"deltaJ","d1",NULL},{41000,"deltaK","d2",NULL},
    {300,"PhiJ","HJ",NULL},{1200,"PhiJK","HJK",NULL},{2100,"PhiKJ","HKJ",NULL},{3000,"PhiK","HK",NULL},
    {40200,"phiJ","h1",NULL},{41100,"phiJK","h2",NULL},{42000,"phiK","h3",NULL},
    {400,"LJ","LJ",NULL},{1300,"LJJK","LJJK",NULL},{2200,"LJK","LJK",NULL},{3100,"LKKJ","LKKJ",NULL},{4000,"LK","LK",NULL},
    {40300,"lJ","l1",NULL},{41200,"lJK","l2",NULL},{42100,"lKJ","l3",NULL},{43000,"lK","l4",NULL},
    {500,"PJ","PJ",NULL},{1400,"PJJK","PJJK",NULL},{2300,"PJK","PJK",NULL},{3200,"PKJ","PKJ",NULL},{4100,"PKKJ","PKKJ",NULL},{5000,"PK","PK",NULL},
    {40400,"pJ","p1",NULL},{41300,"pJJK","p2",NULL},{42200,"pJK","p3",NULL},{43100,"pKKJ","p4",NULL},{44000,"pK","p5",NULL},

    /* Nuclear quadrupole coupling: spin 1 */
    {110010000,NULL,NULL,"chi.aa*3/2"},{110020000,NULL,NULL,"chi.bb*3/2"},{110030000,NULL,NULL,"chi.cc*3/2"},{110040000,NULL,NULL,"chi(b-c)/4"},
    {110610000,NULL,NULL,"chi.ab"},{110210000,NULL,NULL,"chi.bc"},{110410000,NULL,NULL,"chi.ac"},{110011000,NULL,NULL,"chi.k*3/2"},{110010100,NULL,NULL,"chi.J*3/2"},

    /* Nuclear quadrupole coupling: spin 2 */
    {220010000,NULL,NULL,"chi.aa*3/2"},{220020000,NULL,NULL,"chi.bb*3/2"},{220030000,NULL,NULL,"chi.cc*3/2"},{220040000,NULL,NULL,"chi(b-c)/4"},
    {220610000,NULL,NULL,"chi.ab"},{220210000,NULL,NULL,"chi.bc"},{220410000,NULL,NULL,"chi.ac"},{220011000,NULL,NULL,"chi.k*3/2"},{220010100,NULL,NULL,"chi.J*3/2"},

    /* Spin-spin and spin-rotation (nucleus 1) */
    {120010000,NULL,NULL,"D.aa*3/2"},{120020000,NULL,NULL,"D.bb*3/2"},{120030000,NULL,NULL,"D.cc*3/2"},{120040000,NULL,NULL,"D(b-c)/4"},
    {120610000,NULL,NULL,"Dab"},{120210000,NULL,NULL,"Dbc"},{120410000,NULL,NULL,"Dac"},
    {10010000,NULL,NULL,"M.aa {+IMJ}"},{10020000,NULL,NULL,"M.bb {+IMJ}"},{10030000,NULL,NULL,"M.cc {+IMJ}"},

    /* Coriolis (0 <-> 1) and Fermi (0 <-> 1) */
    {11,NULL,NULL,"E1"},{200001,NULL,NULL,"Ga"},{210001,NULL,NULL,"Fbc"},{400001,NULL,NULL,"Gb"},{410001,NULL,NULL,"Fca"},{600001,NULL,NULL,"Gc"},{610001,NULL,NULL,"Fab"},
    {1,NULL,NULL,"F"},

    /* Internal rotation */
    {200000,NULL,NULL,"D_a"},{200100,NULL,NULL,"D_aJ"},{201000,NULL,NULL,"D_aK"},
    {400000,NULL,NULL,"D_b"},{600000,NULL,NULL,"D_c"}
};

static const ParameterName *parameter_name(int id) {
    for (size_t i=0;i<sizeof(PARAMETER_NAMES)/sizeof(PARAMETER_NAMES[0]);i++) {
        if (PARAMETER_NAMES[i].id==id) return &PARAMETER_NAMES[i];
        /* The two trailing state digits are 00,11,...,88; 99 marks a shared
           parameter.  Strip them only for display-name lookup. */
        for (int suffix = 11; suffix <= 99; suffix += 11)
            if (PARAMETER_NAMES[i].id == id - suffix) return &PARAMETER_NAMES[i];
    }
    return NULL;
}

static void parameter_label(PickettParameter *x) {
    const ParameterName *name=parameter_name(x->id);
    const char *label=name ? (name->watson_s ? name->watson_s : name->other) : NULL;
    if (label) snprintf(x->label,sizeof(x->label),"%s",label);
    else snprintf(x->label,sizeof(x->label),"SPFIT parameter %d", x->id);
}

static int have_parameter_id(const PredFitState *p, int id) {
    for (int i = 0; i < p->n_param; i++) if (p->param[i].id == id) return 1;
    return 0;
}

static int validate_model_parameters(PredFitState *p) {
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id <= 0) {
            snprintf(p->status, sizeof(p->status), "Parameter row %d has no valid positive ID.", i + 1);
            return 0;
        }
        for (int j = 0; j < i; j++) if (p->param[j].id == p->param[i].id) {
            snprintf(p->status, sizeof(p->status), "Parameter ID %d is duplicated (rows %d and %d).",
                     p->param[i].id, j + 1, i + 1);
            return 0;
        }
    }
    const int base[3] = {10000, 20000, 30000};
    const char *name[3] = {"A", "B", "C"};
    for (int i = 0; i < p->n_species; i++) {
        if (!p->species[i].predict_enabled) continue;
        int suffix = 11 * p->species[i].state_index;
        for (int k = 0; k < 3; k++) if (!have_parameter_id(p, base[k] + suffix)) {
            snprintf(p->status, sizeof(p->status), "Calculate/Fit refused: species %d is missing %s.", i + 1, name[k]);
            return 0;
        }
    }
    return 1;
}

static void select_species(AppState *s, int index) {
    PredFitState *p = &s->predfit;
    if (index < 0 || index >= p->n_species || index == p->active_species) return;
    store_active_species(p);
    p->active_species = index;
    load_active_species(p);
    sync_basic_from_parameters(p);
    predfit_publish_shared_state(s);
    p->session_dirty = 1;
}

static void add_species(AppState *s) {
    PredFitState *p = &s->predfit;
    if (p->n_species >= MAX_PICKETT_SPECIES) return;
    store_active_species(p);
    int state = 0;
    for (int i = 0; i < p->n_species; i++) if (p->species[i].state_index >= state) state = p->species[i].state_index + 1;
    PickettSpecies *sp = &p->species[p->n_species];
    *sp = (PickettSpecies){0};
    snprintf(sp->name, sizeof(sp->name), "Species %d", state + 1);
    sp->state_index = state;
    sp->predict_enabled = 1;
    memcpy(sp->mu, p->mu, sizeof(sp->mu));
    sp->concentration = 1.0;
    assign_default_species_color(p, sp);

    /* Seed the independent A/B/C card for the new state.  All other terms
       can be added manually, including shared xx99 ones. */
    int suffix = 11 * state;
    const int base[3] = {10000, 20000, 30000};
    const double value[3] = {p->a, p->b, p->c};
    for (int k = 0; k < 3 && p->n_param < MAX_PICKETT_PARAMS; k++) {
        int id = base[k] + suffix;
        if (have_parameter_id(p, id)) continue;
        PickettParameter *x = &p->param[p->n_param++];
        *x = (PickettParameter){id, value[k], 1.0, ""};
        parameter_label(x);
    }
    p->active_species = p->n_species++;
    load_active_species(p);
    sync_basic_from_parameters(p);
    predfit_publish_shared_state(s);
    p->session_dirty = 1;
}

/* A Calculate immediately after SPFIT must leave SPFIT's estimated errors in
   model.var intact.  It is safe to reuse that file only when it describes the
   current Hamiltonian and every current parameter value; otherwise write a
   fresh input model as usual. */
static int existing_var_matches_model(const char *path, const PredFitState *p) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    int seen[MAX_PICKETT_PARAMS] = {0};
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp) ||
        !fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, p->hamiltonian_line) != 0) {
        fclose(fp);
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        int id = 0;
        double value = 0.0, ignored_error = 0.0;
        if (!strchr(line, '/') || sscanf(line, "%d %lf %lf", &id, &value, &ignored_error) != 3) continue;
        int found = 0;
        for (int i = 0; i < p->n_param; i++) {
            if (p->param[i].id != id) continue;
            found = 1;
            double scale = fmax(1.0, fabs(p->param[i].value));
            if (fabs(value - p->param[i].value) > 1e-11 * scale) {
                fclose(fp);
                return 0;
            }
            seen[i] = 1;
            break;
        }
        if (!found) { fclose(fp); return 0; }
    }
    fclose(fp);
    for (int i = 0; i < p->n_param; i++) if (!seen[i]) return 0;
    return 1;
}

static int write_inputs(AppState *s, int for_fit) {
    PredFitState *p = &s->predfit;
    int nvib = 0;
    int required_nvib = included_state_count(p);
    if (!hamiltonian_nvib(p->hamiltonian_line, &nvib)) {
        snprintf(p->status, sizeof(p->status),
                 "Hamiltonian option line has no valid NVIB (third field). ");
        return 0;
    }
    if (nvib < required_nvib) {
        snprintf(p->status, sizeof(p->status),
                 "Hamiltonian NVIB %d is too small: at least %d is required for the included species.",
                 nvib, required_nvib);
        return 0;
    }
    if (!validate_model_parameters(p)) return 0;
    store_active_species(p);
    /* A .lin must contain one observation per quantum-number transition.
       This also repairs any duplicate rows produced by older app versions. */
    if (for_fit) {
        deduplicate_assignments(s->assignments, &s->n_assignments);
        if (!predfit_save_exclusions(s)) return 0;
    }
    if (for_fit) {
        for (int i = 0; i < s->n_assignments; i++) {
            if (!assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) continue;
            int n = s->assignments[i].pred.n_qn;
            if (n < 1 || n > 6) {
                snprintf(p->status, sizeof(p->status),
                         "Assignment %d has no valid CAT QNFMT/NQN; recalculate and assign that CAT again.", i + 1);
                return 0;
            }
        }
        int model_nqn = 0;
        if (!current_model_nqn(s, &model_nqn)) {
            char cat_name[700];
            work_file(p, "model.cat", cat_name, sizeof(cat_name));
            snprintf(p->status, sizeof(p->status),
                     "Simulate %s before Fit: %s is missing or predates the option line.",
                     p->hamiltonian[p->active_hamiltonian].name, cat_name);
            return 0;
        }
        char rows[128] = "";
        int mismatch_count = 0;
        for (int i = 0; i < s->n_assignments; i++) {
            const Assignment *a = &s->assignments[i];
            if (!assignment_belongs_to_active_hamiltonian(s, a)) continue;
            if (!a->fit_enabled || a->pred.n_qn == model_nqn) continue;
            mismatch_count++;
            if (mismatch_count <= 6) {
                size_t used = strlen(rows);
                snprintf(rows + used, sizeof(rows) - used, "%s%d", used ? ", " : "", i + 1);
            }
        }
        if (mismatch_count) {
            snprintf(p->status, sizeof(p->status),
                     "Fit refused: model.cat uses NQN %d; assignment row%s %s use%s a different NQN.",
                     model_nqn, mismatch_count == 1 ? "" : "s", rows,
                     mismatch_count == 1 ? "s" : "");
            return 0;
        }
    }
    if (!prepare_fit_dir(s)) return 0;
    sync_basic_parameters(p);
    char var_path[600], int_path[600], par_path[600], lin_path[600];
    work_file(p,"model.var",var_path,sizeof(var_path)); work_file(p,"model.int",int_path,sizeof(int_path));
    work_file(p,"model.par",par_path,sizeof(par_path)); work_file(p,"model.lin",lin_path,sizeof(lin_path));
    int preserve_fitted_var = !for_fit && existing_var_matches_model(var_path, p);
    FILE *var = preserve_fitted_var ? NULL : fopen(var_path, "w");
    FILE *in = fopen(int_path, "w");
    /* SPCAT reads .var and SPFIT reads .par.  A Calculate-only pass can retain
       an unchanged fitted .var; otherwise both files describe the same model. */
    FILE *par = fopen(par_path, "w");
    FILE *lin = for_fit ? fopen(lin_path, "w") : NULL;
    if ((!preserve_fitted_var && !var) || !in || !par || (for_fit && !lin)) {
        if (var) fclose(var); if (in) fclose(in); if (par) fclose(par); if (lin) fclose(lin);
        snprintf(p->status, sizeof(p->status), "Cannot write Pickett working files.");
        return 0;
    }
    /* NLINE and model.lin contain only observations SPFIT may actually use.
       Temporary exclusions live in .fit/exclusions.txt, keyed by transition. */
    int nline = 0;
    if (for_fit) for (int i = 0; i < s->n_assignments; i++)
        if (s->assignments[i].fit_enabled && assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) nline++;
    if (var) fprintf(var, "SpectraVisual Pred&Fit quick model\n%4d%5d%5d%5d %15.4E %15.4E %15.4E %.10f\n%s\n",
                     p->n_param, nline, 0, 0, 0.0, 1e6, 1.0, 1.0, p->hamiltonian_line);
    if (par) fprintf(par, "SpectraVisual Pred&Fit quick model\n%4d%5d%5d%5d %15.4E %15.4E %15.4E %.10f\n%s\n",
                     p->n_param, nline, 50, 0, 0.0, 1e6, 1.0, 1.0, p->hamiltonian_line);
    for (int i = 0; i < p->n_param; i++) {
        PickettParameter *x = &p->param[i];
        if (var) fprintf(var, "%12d % .15E % .8E /%s/\n", x->id, x->value, x->error, x->label);
        if (par) fprintf(par, "%12d % .15E % .8E /%s/\n", x->id, x->value, x->error, x->label);
    }
    if (!write_multi_state_int(in, p)) {
        if (var) fclose(var); fclose(in); if (par) fclose(par); if (lin) fclose(lin);
        snprintf(p->status, sizeof(p->status), "Select a prediction species (states 0 through 9 are supported).");
        return 0;
    }
    if (lin) for (int i = 0; i < s->n_assignments; i++) {
        Assignment *a = &s->assignments[i]; PredLine *q = &a->pred;
        if (!a->fit_enabled || !assignment_belongs_to_active_hamiltonian(s, a)) continue;
        /* SPFIT's basic, spin-free record is three upper followed immediately
           by three lower QNs.  Do not emit the zero-valued M placeholders:
           they turn a no-spin record into a malformed 12-QN one.  If an
           assigned catalogue genuinely carries extra QNs, retain as many
           paired fields as it uses. */
        int u[6] = {q->Ju,q->Kau,q->Kcu,q->M1u,q->M2u,q->M3u};
        int l[6] = {q->Jl,q->Kal,q->Kcl,q->M1l,q->M2l,q->M3l};
        /* NQN comes from QNFMT in the SPCAT catalogue used for this exact
           assignment.  Do not infer it from Hamiltonian settings or zeroes. */
        int nq = q->n_qn;
        for (int k = 0; k < nq; k++) fprintf(lin, "%3d", u[k]);
        for (int k = 0; k < nq; k++) fprintf(lin, "%3d", l[k]);
        /* The first 36 characters are the complete 12-I3 QN field, even
           when the molecule only uses six of those slots. */
        for (int k = 2 * nq; k < 12; k++) fputs("   ", lin);
        fprintf(lin, "%15.6f %10.6f 1.0\n", a->exp_freq, p->line_error_mhz);
    }
    if (var) fclose(var); fclose(in); if (par) fclose(par); if (lin) fclose(lin);
    return 1;
}

/* Implemented beside the cached SPFIT report below.  A second SPFIT run may
   replace model.fit within the same filesystem timestamp second, so the cache
   must be explicitly invalidated after every successful run. */
static void report_invalidate(void);

/* Run Pickett directly: command strings route paths through a shell, which
   breaks a perfectly valid work directory containing spaces or apostrophes. */
static int run_program(const char *program, const char *work_dir,
                       PredFitState *p, const char *what) {
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(p->status, sizeof(p->status), "%s could not start: %s.", what, strerror(errno));
        return 0;
    }
    if (pid == 0) {
        if (chdir(work_dir) != 0) _exit(127);
        char stem[128];
        hamiltonian_file_stem(p, stem, sizeof(stem));
        char *const argv[] = {(char *)program, stem, NULL};
        execv(program, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        snprintf(p->status, sizeof(p->status), "%s could not be waited for: %s.", what, strerror(errno));
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    if (WIFEXITED(status))
        snprintf(p->status, sizeof(p->status), "%s failed (exit %d).", what, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(p->status, sizeof(p->status), "%s failed (signal %d).", what, WTERMSIG(status));
    else
        snprintf(p->status, sizeof(p->status), "%s failed.", what);
    return 0;
}

static void import_fitted_parameters(PredFitState *p) {
    char path[600]; work_file(p,"model.var",path,sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int id; double value, error;
        if (!strchr(line, '/') || sscanf(line, "%d %lf %lf", &id, &value, &error) != 3) continue;
        int i=0;
        for (; i < p->n_param; i++) if (p->param[i].id == id) break;
        if (i == p->n_param && p->n_param < MAX_PICKETT_PARAMS) {
            p->param[i]=(PickettParameter){id,value,1.0,"Pickett parameter"};
            p->n_param++;
        }
        if (i < p->n_param) { p->param[i].value=value; parameter_label(&p->param[i]); }
    }
    fclose(fp);
    sync_basic_from_parameters(p);
}

static void fit_summary(PredFitState *p, char *out, size_t outsz) {
    /* SPFIT performs the iteration loop internally.  NITR=50 in model.par is
       the hard ceiling; the last END OF ITERATION reports where it stopped. */
    char path[600]; work_file(p,"model.fit",path,sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256], last_rms[128]="";
    int bad_lines = 0, rejected = 0, not_used = 0, diverging = 0;
    p->last_fit_iterations=0;
    while (fgets(line, sizeof(line), fp)) {
        int iteration=0;
        if (sscanf(line, " END OF ITERATION %d", &iteration) == 1) p->last_fit_iterations=iteration;
        char *rms = strstr(line, "MICROWAVE RMS =");
        if (rms) {
            char *end = strchr(rms, ',');
            if (end) *end = '\0';
            snprintf(last_rms,sizeof(last_rms),"%s",rms);
        }
        if (strstr(line, "Bad Line(")) bad_lines++;
        int count = 0;
        if (sscanf(line, " %d Lines rejected from fit", &count) == 1) rejected = count;
        if (strstr(line, "NEXT LINE NOT USED IN FIT")) not_used++;
        if (strstr(line, "Fit Diverging")) diverging++;
    }
    fclose(fp);
    if (last_rms[0]) {
        snprintf(out, outsz,
                 "SPFIT stopped after %d/50 iterations; %s; diagnostics: %d bad lines, %d rejected, %d not used, %d diverging.",
                 p->last_fit_iterations, last_rms, bad_lines, rejected, not_used, diverging);
    }
}

/* `used` is 0 for a line SPFIT read but left out of the fit: it writes the
   999.99999 placeholder for those, which is not a residual to colour. */
typedef struct { int found, used; double obs, calc, diff, unc; } FitObservation;

/* model.int is a generated work file, not the session's source of truth.
   It can seed INT settings for old/no-session restores, but it must never
   replace the active species' temperature or dipoles. */
static void import_int_settings(PredFitState *p, int session_has_int_settings) {
    char path[600]; work_file(p,"model.int",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return;
    char line[256];
    if (!fgets(line,sizeof(line),fp) || !fgets(line,sizeof(line),fp)) { fclose(fp); return; }
    int flags=0, tag=0;
    double ignored_qrot=0, fbegin_raw=0, fend_raw=0, s0=0, s1=0, limit=0, temp=0, maxv_raw=-1;
    if (!session_has_int_settings &&
        sscanf(line,"%d %d %lf %lf %lf %lf %lf %lf %lf %lf",
               &flags,&tag,&ignored_qrot,&fbegin_raw,&fend_raw,&s0,&s1,&limit,&temp,&maxv_raw)==10) {
        p->int_settings.flags = flags;
        p->int_settings.tag = tag;
        p->int_settings.fbegin = (int)lround(fbegin_raw);
        /* Older SpectraVisual versions mistakenly put FQLIM in FEND.  FEND
           is an integer quantum-number bound, so a fractional value can only
           be that old malformed output; migrate it to the normal default. */
        p->int_settings.fend = fabs(fend_raw - round(fend_raw)) > 1e-9 ? 40 : (int)lround(fend_raw);
        p->int_settings.intensity_cutoff = s0;
        p->int_settings.fqlim_ghz = limit;
        p->int_settings.temp_k = temp;
        p->int_settings.maxv = (int)lround(maxv_raw);
    }
    fclose(fp);
}

/* One row of .fit/model.lin.  SPFIT's record is a fixed 12 x I3 quantum-number
   field followed by the frequency, so it is read by column: reading it with a
   plain "%d %d %d %d %d %d" mistakes the upper-state hyperfine numbers of a
   12-QN record for the lower state. */
typedef struct {
    int    qn[12];
    int    nq;        /* quantum numbers actually written per state */
    double freq;      /* measured frequency, sentinel removed        */
    int    enabled;   /* 0 when SPFIT was told to skip the line      */
} LinRow;

static LinRow g_lin_rows[MAX_ASSIGNMENTS];

static int read_lin_rows_path(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char line[256];
    while (n < MAX_ASSIGNMENTS && fgets(line, sizeof(line), fp)) {
        if ((int)strlen(line) < 37) continue;
        LinRow row;
        memset(&row, 0, sizeof(row));
        int slots = 0;
        for (int k = 0; k < 12; k++) {
            char field[4] = {line[k * 3], line[k * 3 + 1], line[k * 3 + 2], '\0'};
            if (field[0] == ' ' && field[1] == ' ' && field[2] == ' ') continue;
            row.qn[k] = atoi(field);
            slots = k + 1;
        }
        /* Keep every row with at least one QN per state.  Fewer than three per
           state is not a rotational record - an older build truncated it
           (B-01) - so import_fit_lines keeps it marked for reassignment. */
        if (slots < 2) continue;
        row.nq = slots / 2;
        double freq = atof(line + 36);
        if (!(freq > 0.0)) continue;
        row.enabled = freq < 90000.0;
        row.freq = row.enabled ? freq : freq - 90000.0;
        g_lin_rows[n++] = row;
    }
    fclose(fp);
    return n;
}

static int read_lin_rows(const PredFitState *p) {
    char path[600];
    work_file(p, "model.lin", path, sizeof(path));
    return read_lin_rows_path(path);
}

/* Derived rather than stored: .lin carries no branch or dipole type. */
static void set_branch_and_dipole(PredLine *q) {
    int dJ = q->Ju - q->Jl;
    q->branch = (dJ == 1) ? 'R' : (dJ == -1) ? 'P' : (dJ == 0) ? 'Q' : '?';
    int even_ka = (abs(q->Kau - q->Kal) % 2) == 0;
    int even_kc = (abs(q->Kcu - q->Kcl) % 2) == 0;
    q->mu = (even_ka && !even_kc) ? 'a' : (!even_ka && !even_kc) ? 'b' : (!even_ka && even_kc) ? 'c' : '?';
}

/* The QN order and its length are not UI policy.  They are exactly the NQN
   fields that SPCAT printed in QNFMT for the selected CAT transition.  Each
   displayed number keeps the right-aligned three-character .lin field. */
static void format_assignment_qn(char *out, size_t size, const PredLine *q) {
    int n = q->n_qn;
    if (n < 1 || n > 6) n = 3; /* display-only compatibility for old sessions */
    const int upper[6] = {q->Ju, q->Kau, q->Kcu, q->M1u, q->M2u, q->M3u};
    const int lower[6] = {q->Jl, q->Kal, q->Kcl, q->M1l, q->M2l, q->M3l};
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n && used < size; i++) {
        int wrote = snprintf(out + used, size - used, "%3d", upper[i]);
        if (wrote < 0 || (size_t)wrote >= size - used) { out[size - 1] = '\0'; return; }
        used += (size_t)wrote;
    }
    if (used < size) {
        int wrote = snprintf(out + used, size - used, " - ");
        if (wrote < 0 || (size_t)wrote >= size - used) { out[size - 1] = '\0'; return; }
        used += (size_t)wrote;
    }
    for (int i = 0; i < n && used < size; i++) {
        int wrote = snprintf(out + used, size - used, "%3d", lower[i]);
        if (wrote < 0 || (size_t)wrote >= size - used) { out[size - 1] = '\0'; return; }
        used += (size_t)wrote;
    }
}

/* Restores the assignment list of the saved session. assignments.txt is the
 * source of its rich CAT data; model.lin can only contribute old observations
 * which were never saved.  Fit inclusion is read from exclusions.txt, never
 * inferred from a special observed-frequency value in model.lin. */
/* The QN of a .lin row as two six-field states; fields past the row's own NQN
   are 0, like the unused slots of a catalogue row. */
static void lin_row_states(const LinRow *row, int upper[6], int lower[6]) {
    for (int k = 0; k < 6; k++) {
        upper[k] = k < row->nq ? row->qn[k] : 0;
        lower[k] = k < row->nq ? row->qn[row->nq + k] : 0;
    }
}

/* A .lin row as the transition it stands for.  The two layouts are not the
   same and must never be compared field by field: a .lin packs the quantum
   numbers of the two states one after the other, NQN of each, while a
   PredLine - and every key derived from one - keeps six fixed slots per
   state and pads the unused ones with zero. */
static void lin_row_pred(const LinRow *row, PredLine *q) {
    int u[6], l[6];
    lin_row_states(row, u, l);
    memset(q, 0, sizeof(*q));
    q->Ju = u[0]; q->Kau = u[1]; q->Kcu = u[2];
    q->M1u = u[3]; q->M2u = u[4]; q->M3u = u[5];
    q->Jl = l[0]; q->Kal = l[1]; q->Kcl = l[2];
    q->M1l = l[3]; q->M2l = l[4]; q->M3l = l[5];
    q->n_qn = row->nq;
    set_branch_and_dipole(q);
}

/* Returns the number of assignments marked for reassignment because their
   .lin row has fewer than three QN per state. */
static int import_fit_lines(AppState *s, int *ignored_exclusions) {
    PredFitState *p = &s->predfit;
    int n_rows = read_lin_rows(p);
    static unsigned char used[MAX_ASSIGNMENTS];
    memset(used, 0, sizeof(used));

    /* The same file Save all writes and a launch with a .cat reads: in the
       data folder, not in whichever folder the program was started from. */
    char path[600];
    settings_data_file(s, "assignments.txt", path, sizeof(path));
    s->n_assignments = 0;
    AssignmentFileReport report;
    char note[512];
    load_assignments_file(path, s->assignments, &s->n_assignments, &report);
    assignment_file_message(&report, path, note, sizeof(note));
    if (note[0]) snprintf(s->error_message, sizeof(s->error_message), "%s", note);

    int marked = 0;
    int legacy_exclusions = !fit_exclusions_file_exists(s);
    int saw_legacy_exclusion = 0;
    /* The two files can disagree - lines assigned after the last save are
       only in the .lin, lines saved and never fitted only in assignments.txt
       - so each row is matched once, on its quantum numbers when available
       and on frequency otherwise.  Old sentinel rows are read only to migrate
       an existing workspace that predates exclusions.txt. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->n_assignments; i++) {
            Assignment *a = &s->assignments[i];
            if (!assignment_belongs_to_active_hamiltonian(s, a)) continue;
            if (pass == 0) a->fit_enabled = 1;
            const int au[6] = {a->pred.Ju, a->pred.Kau, a->pred.Kcu, a->pred.M1u, a->pred.M2u, a->pred.M3u};
            const int al[6] = {a->pred.Jl, a->pred.Kal, a->pred.Kcl, a->pred.M1l, a->pred.M2l, a->pred.M3l};
            for (int k = 0; k < n_rows; k++) {
                if (used[k]) continue;
                LinRow *row = &g_lin_rows[k];
                if (fabs(row->freq - a->exp_freq) >= 1e-4) continue;
                int u[6], l[6];
                lin_row_states(row, u, l);
                int compare = row->nq > 3 ? row->nq : 3;
                int same_qn = 1;
                for (int q = 0; q < compare; q++)
                    if (u[q] != au[q] || l[q] != al[q]) same_qn = 0;
                if (pass == 0 && !same_qn) continue;   /* exact match first */
                if (legacy_exclusions && !row->enabled) {
                    a->fit_enabled = 0;
                    saw_legacy_exclusion = 1;
                }
                /* Never promote the QN length from an old .lin: versions
                   before CAT/QNFMT tracking could have written an incorrect
                   number of fields there.  New assignments persist NQN in
                   assignments.txt; legacy ones deliberately remain invalid
                   until selected again from a freshly calculated CAT. */
                if (row->nq < 3 && !a->needs_reassign) { a->needs_reassign = 1; marked++; }
                used[k] = 1;
                break;
            }
        }
    }

    /* Anything the .lin has and assignments.txt does not is still part of the
       session: keep it, with the little the .lin can say about it. */
    for (int k = 0; k < n_rows && s->n_assignments < MAX_ASSIGNMENTS; k++) {
        if (used[k]) continue;
        LinRow *row = &g_lin_rows[k];
        Assignment *a = &s->assignments[s->n_assignments++];
        memset(a, 0, sizeof(*a));
        lin_row_pred(row, &a->pred);
        a->exp_freq = row->freq;
        a->fit_enabled = legacy_exclusions ? row->enabled : 1;
        a->hamiltonian_id = predfit_active_hamiltonian_id(s);
        if (legacy_exclusions && !row->enabled) saw_legacy_exclusion = 1;
        /* Fewer than three QN per state is not a rotational record: the row
           was truncated by an older build (B-01).  Keep it rather than drop
           it, and say that it must be assigned again from the catalogue. */
        if (row->nq < 3) { a->needs_reassign = 1; marked++; }
    }
    if (legacy_exclusions && saw_legacy_exclusion)
        predfit_save_exclusions(s); /* one-way migration from the old sentinel */
    if (!legacy_exclusions && ignored_exclusions)
        *ignored_exclusions = predfit_load_exclusions(s);
    return marked;
}

/* ---------------------------------------------------------------------- *
 *  Importing Pickett files written elsewhere
 *
 *  .fit/load is a drop box.  Files named after the model they describe -
 *  mon.par (or mon.var), mon.int, mon.lin - become the Hamiltonian "mon"
 *  with every state its .int declares.  Several models can wait there at
 *  once: each distinct basename is one Hamiltonian.  Nothing in load/ is
 *  modified or deleted, and the workspace files keep being generated from
 *  the imported model like any other Hamiltonian's.
 * ---------------------------------------------------------------------- */

#define MAX_LOAD_MODELS 32

typedef struct {
    char stem[64];
    int has_par, has_var, has_int, has_lin;
} LoadCandidate;

static void load_dir(const AppState *s, char *out, size_t size) {
    char root[600];
    fit_root(s, root, sizeof(root));
    snprintf(out, size, "%s/%s", root, LOAD_DIR_NAME);
}

/* Pickett extensions are matched without regard to case: a file saved as
   MON.PAR on another machine is the same model as mon.par. */
static int load_candidate_slot(LoadCandidate *list, int *n, const char *stem) {
    for (int i = 0; i < *n; i++)
        if (strcasecmp(list[i].stem, stem) == 0) return i;
    if (*n >= MAX_LOAD_MODELS) return -1;
    memset(&list[*n], 0, sizeof(list[0]));
    snprintf(list[*n].stem, sizeof(list[0].stem), "%s", stem);
    return (*n)++;
}

static int load_candidate_cmp(const void *a, const void *b) {
    return strcasecmp(((const LoadCandidate *)a)->stem, ((const LoadCandidate *)b)->stem);
}

/* Two header records, the option line, then one record per parameter.  The
   third column is the uncertainty exactly as the file states it: in a .par
   that is the fit control its author wrote, in a .var whatever SPFIT last
   estimated.  Neither is reinterpreted here. */
static int read_pickett_model_file(const char *path, PredFitSnapshot *m) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp) ||
        !fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    line[strcspn(line, "\r\n")] = '\0';
    int nvib = 0;
    if (!hamiltonian_nvib(line, &nvib)) { fclose(fp); return 0; }
    snprintf(m->hamiltonian_line, sizeof(m->hamiltonian_line), "%s", line);
    m->n_param = 0;
    while (fgets(line, sizeof(line), fp) && m->n_param < MAX_PICKETT_PARAMS) {
        int id = 0;
        double value = 0.0, error = 1.0;
        int fields = sscanf(line, "%d %lf %lf", &id, &value, &error);
        if (fields < 2 || id <= 0 || !isfinite(value)) continue;
        if (fields < 3 || !isfinite(error) || error < 0.0) error = 1.0;
        PickettParameter *x = &m->param[m->n_param++];
        *x = (PickettParameter){id, value, error, ""};
        parameter_label(x);
    }
    fclose(fp);
    return m->n_param > 0;
}

/* The .int gives the control card and one dipole record per state: its
   identifier carries the two vibrational indices and the axis, so V1 == V2
   is a state of this model and V1 != V2 an interstate dipole SpectraVisual
   cannot represent yet.  Those are reported rather than silently dropped. */
static int read_pickett_int_file(const char *path, PredFitSnapshot *m, int *interstate) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char title[512], line[512];
    if (!fgets(title, sizeof(title), fp) || !fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    int flags = 0, tag = 0, maxv = -1;
    double qrot = 0.0, fbegin_raw = 0.0, fend_raw = 40.0, s0 = -20.0, s1 = -20.0, fqlim = 0.0, temp = 0.0;
    if (sscanf(line, "%d %d %lf %lf %lf %lf %lf %lf %lf %d",
               &flags, &tag, &qrot, &fbegin_raw, &fend_raw, &s0, &s1, &fqlim, &temp, &maxv) >= 9) {
        m->int_settings.flags = flags;
        m->int_settings.tag = tag;
        m->int_settings.fbegin = (int)lround(fbegin_raw);
        /* FEND is a quantum-number bound; a fractional value can only come
           from the old build that wrote FQLIM there. */
        m->int_settings.fend = fabs(fend_raw - round(fend_raw)) > 1e-9 ? 40 : (int)lround(fend_raw);
        if (isfinite(s0)) m->int_settings.intensity_cutoff = s0;
        if (fqlim > 0.0) m->int_settings.fqlim_ghz = fqlim;
        if (temp > 0.0) { m->temp_k = temp; m->int_settings.temp_k = temp; }
        m->int_settings.maxv = maxv;
    }
    double mu[MAX_PICKETT_SPECIES][3];
    int seen[MAX_PICKETT_SPECIES];
    memset(mu, 0, sizeof(mu));
    memset(seen, 0, sizeof(seen));
    while (fgets(line, sizeof(line), fp)) {
        int id = 0;
        double value = 0.0;
        if (sscanf(line, "%d %lf", &id, &value) != 2 || id <= 0 || !isfinite(value)) continue;
        int axis = id % 10, v1 = (id / 10) % 10, v2 = (id / 100) % 10;
        if (axis < 1 || axis > 3) continue;
        if (v1 != v2) { if (interstate) (*interstate)++; continue; }
        if (v1 >= MAX_PICKETT_SPECIES) continue;
        mu[v1][axis - 1] = value;
        seen[v1] = 1;
    }
    fclose(fp);
    int n = 0;
    for (int v = 0; v < MAX_PICKETT_SPECIES; v++) {
        if (!seen[v]) continue;
        PickettSpecies *sp = &m->species[n++];
        memset(sp, 0, sizeof(*sp));
        snprintf(sp->name, sizeof(sp->name), "State %d", v);
        sp->state_index = v;
        sp->predict_enabled = 1;
        memcpy(sp->mu, mu[v], sizeof(sp->mu));
        sp->concentration = 1.0;
    }
    if (n > 0) {
        m->n_species = n;
        m->active_species = 0;
        memcpy(m->mu, m->species[0].mu, sizeof(m->mu));
    }
    return 1;
}

/* Two Hamiltonians may not share a Pickett basename, so an imported model
   whose name is taken is imported beside it as "mon-2" rather than writing
   over the files of the model already in the project. */
/* Two Hamiltonians whose names reduce to the same file stem would share every
   Pickett working file, which is why unique_hamiltonian_name compares stems.
   An import asks the same question the other way round: which model of this
   project, if any, is the one this folder holds. */
static int hamiltonian_index_by_file_stem(const PredFitState *p, const char *stem) {
    char want[128], existing[128];
    name_file_stem(stem, 0, want, sizeof(want));
    for (int h = 0; h < p->n_hamiltonians; h++) {
        name_file_stem(p->hamiltonian[h].name, p->hamiltonian[h].id, existing, sizeof(existing));
        if (strcmp(want, existing) == 0) return h;
    }
    return -1;
}

static void unique_hamiltonian_name(const PredFitState *p, const char *wanted,
                                    char *out, size_t size) {
    for (int attempt = 1; attempt <= MAX_PICKETT_HAMILTONIANS + 1; attempt++) {
        char candidate[64], stem[128], existing[128];
        if (attempt == 1) snprintf(candidate, sizeof(candidate), "%s", wanted);
        else snprintf(candidate, sizeof(candidate), "%s-%d", wanted, attempt);
        name_file_stem(candidate, 0, stem, sizeof(stem));
        int taken = 0;
        for (int h = 0; h < p->n_hamiltonians && !taken; h++) {
            name_file_stem(p->hamiltonian[h].name, p->hamiltonian[h].id, existing, sizeof(existing));
            taken = strcmp(stem, existing) == 0;
        }
        if (!taken) { snprintf(out, size, "%s", candidate); return; }
    }
    snprintf(out, size, "%s", wanted);
}

/* The list can already hold this very line: assignments.txt is read when the
   spectrum opens, and its rows carry the owner they were saved with, so an
   import that ran before this one is in the list under the same id.  The
   measured frequency is part of the identity, which keeps the two rows of a
   blend apart. */
static int assignment_already_present(const AppState *s, int owner, const PredLine *qn, double obs) {
    for (int i = 0; i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        if (a->hamiltonian_id != owner) continue;
        if (!same_transition(&a->pred, qn)) continue;
        if (fabs(a->exp_freq - obs) < 1e-5) return 1;
    }
    return 0;
}

/* A .lin says what was measured and nothing about what the model calculates,
   so an imported model would show an empty PREDICTED column until the user
   calculated it.  The catalogue that comes with it in the load folder - or,
   when the folder has none, one SPCAT run of the model just read - fills that
   column at import time.  Lines the catalogue does not contain (below the
   .int cutoff, outside its frequency window) simply stay empty. */
static void import_fill_predictions(AppState *s, const char *dir, const char *stem) {
    PredFitState *p = &s->predfit;
    char path[900];
    snprintf(path, sizeof(path), "%s/%s.cat", dir, stem);
    if (access(path, R_OK) != 0) {
        if (!have_program(s->settings.spcat_path)) return;
        if (!write_inputs(s, 0)) return;
        if (!run_program(s->settings.spcat_path, p->work_dir, p, "SPCAT")) return;
        work_file(p, "model.cat", path, sizeof(path));
    }
    PredLine *rows = NULL;
    double xmin = 0, xmax = 0, max_int = 0;
    int n = read_pred_cat_alloc(path, &rows, &xmin, &xmax, &max_int);
    if (n > 0) fill_predictions_from_catalog(s, predfit_active_hamiltonian_id(s), rows, n);
    free(rows);
}

/* The .lin rows of an imported model become assignments owned by it.  They
   carry only what a .lin can say - quantum numbers and a measured frequency
   - so rows with fewer than three QN per state are kept and marked, exactly
   as a restored workspace .lin is. */
static int import_load_lines(AppState *s, const char *path, int *marked, int *already) {
    int n_rows = read_lin_rows_path(path);
    int owner = predfit_active_hamiltonian_id(s);
    int added = 0;
    for (int k = 0; k < n_rows && s->n_assignments < MAX_ASSIGNMENTS; k++) {
        LinRow *row = &g_lin_rows[k];
        PredLine qn;
        lin_row_pred(row, &qn);
        if (assignment_already_present(s, owner, &qn, row->freq)) {
            if (already) (*already)++;
            continue;
        }
        Assignment *a = &s->assignments[s->n_assignments++];
        memset(a, 0, sizeof(*a));
        a->pred = qn;
        a->exp_freq = row->freq;
        a->fit_enabled = row->enabled;
        a->hamiltonian_id = owner;
        if (row->nq < 3) { a->needs_reassign = 1; if (marked) (*marked)++; }
        added++;
    }
    return added;
}

static int import_one_model(AppState *s, const char *dir, const LoadCandidate *c,
                            char *note, size_t note_size) {
    PredFitState *p = &s->predfit;
    PredFitSnapshot m;
    default_hamiltonian_model(&m);
    char path[900];
    /* .par is the fitting input and states the uncertainties its author
       chose; .var is read only when no .par accompanies it. */
    snprintf(path, sizeof(path), "%s/%s.%s", dir, c->stem, c->has_par ? "par" : "var");
    if (!read_pickett_model_file(path, &m)) {
        snprintf(note, note_size, "%s: the %s has no readable option line or parameters.",
                 c->stem, c->has_par ? ".par" : ".var");
        return 0;
    }
    int interstate = 0, states_from_int = 0;
    if (c->has_int) {
        snprintf(path, sizeof(path), "%s/%s.int", dir, c->stem);
        states_from_int = read_pickett_int_file(path, &m, &interstate);
    }
    /* Pressing Import twice must not leave the project with two copies of
       every model: a folder whose stem already names a Hamiltonian here is
       imported over it, parameters, states and lines. */
    int over = hamiltonian_index_by_file_stem(p, c->stem);
    if (over >= 0) {
        store_active_hamiltonian(p);
        p->active_hamiltonian = over;
        p->history_count = 0;
        p->generated_catalog_pending = 0;
        p->session_dirty = 1;
    } else {
        char name[64];
        unique_hamiltonian_name(p, c->stem, name, sizeof(name));
        if (!predfit_add_hamiltonian(s, name)) {
            snprintf(note, note_size, "%s: no room for another Hamiltonian.", c->stem);
            return 0;
        }
    }
    restore_active_model(p, &m);
    for (int i = 0; i < p->n_species; i++) assign_default_species_color(p, &p->species[i]);
    sync_basic_from_parameters(p);
    store_active_hamiltonian(p);
    predfit_refresh_work_dir(s);
    int lines = 0, marked = 0, already = 0;
    if (c->has_lin) {
        snprintf(path, sizeof(path), "%s/%s.lin", dir, c->stem);
        lines = import_load_lines(s, path, &marked, &already);
    }
    import_fill_predictions(s, dir, c->stem);
    size_t used = (size_t)snprintf(note, note_size, "%s: %d parameters, %d state%s, %d line%s",
                                   p->hamiltonian[p->active_hamiltonian].name,
                                   p->n_param, p->n_species, p->n_species == 1 ? "" : "s",
                                   lines, lines == 1 ? "" : "s");
    if (already > 0 && used < note_size)
        used += (size_t)snprintf(note + used, note_size - used, " (%d already assigned)", already);
    if (!states_from_int && used < note_size)
        snprintf(note + used, note_size - used, " (no .int: one default state)");
    else if (interstate > 0 && used < note_size)
        snprintf(note + used, note_size - used, " (%d interstate dipole%s ignored)",
                 interstate, interstate == 1 ? "" : "s");
    else if (marked > 0 && used < note_size)
        snprintf(note + used, note_size - used, " (%d to assign again)", marked);
    return 1;
}

int predfit_import_load_dir(AppState *s) {
    if (!s) return 0;
    PredFitState *p = &s->predfit;
    char dir[700];
    load_dir(s, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(p->status, sizeof(p->status),
                 "Put Pickett files (name.par/.var/.int/.lin) in %s and press Import again.", dir);
        return 0;
    }
    LoadCandidate found[MAX_LOAD_MODELS];
    int n_found = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || dot == entry->d_name) continue;
        int is_par = strcasecmp(dot, ".par") == 0, is_var = strcasecmp(dot, ".var") == 0;
        int is_int = strcasecmp(dot, ".int") == 0, is_lin = strcasecmp(dot, ".lin") == 0;
        if (!is_par && !is_var && !is_int && !is_lin) continue;
        char stem[64];
        size_t n = (size_t)(dot - entry->d_name);
        if (n == 0 || n >= sizeof(stem)) continue;
        memcpy(stem, entry->d_name, n);
        stem[n] = '\0';
        int slot = load_candidate_slot(found, &n_found, stem);
        if (slot < 0) continue;
        found[slot].has_par |= is_par;
        found[slot].has_var |= is_var;
        found[slot].has_int |= is_int;
        found[slot].has_lin |= is_lin;
    }
    closedir(d);
    qsort(found, (size_t)n_found, sizeof(found[0]), load_candidate_cmp);

    int imported = 0, skipped = 0;
    char report[160] = "", last_error[160] = "";
    for (int i = 0; i < n_found; i++) {
        if (!found[i].has_par && !found[i].has_var) { skipped++; continue; }
        char note[160] = "";
        if (import_one_model(s, dir, &found[i], note, sizeof(note))) {
            imported++;
            if (!report[0]) snprintf(report, sizeof(report), "%s", note);
        } else {
            skipped++;
            snprintf(last_error, sizeof(last_error), "%s", note);
        }
    }
    if (imported > 0) {
        save_assignments(s);
        predfit_save_exclusions(s);
        p->session_dirty = 1;
        p->advanced_nav_state = -1;
        if (imported == 1)
            snprintf(p->status, sizeof(p->status), "Imported %s. Save session to keep it.", report);
        else
            snprintf(p->status, sizeof(p->status),
                     "Imported %d Hamiltonians from %s/ (%s, ...). Save session to keep them.",
                     imported, LOAD_DIR_NAME, report);
    } else if (last_error[0]) {
        snprintf(p->status, sizeof(p->status), "Nothing imported - %s", last_error);
    } else {
        snprintf(p->status, sizeof(p->status),
                 "%s holds no model to import: a .par or a .var is required.", dir);
    }
    (void)skipped;
    return imported;
}

int predfit_restore_latest(AppState *s) {
    PredFitState *p=&s->predfit;
    SessionRestoreInfo restore_info;
    /* data_dir can have changed since init even when no session exists. */
    predfit_refresh_work_dir(s);
    load_session(s, &restore_info);
    char cat_path[600]; work_file(p, "model.cat", cat_path, sizeof(cat_path));
    FILE *cat = fopen(cat_path, "r");
    if (!cat) return 0;
    fclose(cat);
    import_fitted_parameters(p);
    sync_basic_from_parameters(p);
    import_int_settings(p, restore_info.has_int_settings);
    int ignored_exclusions = 0;
    int marked = import_fit_lines(s, &ignored_exclusions);
    app_enqueue_pending_load(s, PENDING_LOAD_CATALOG, cat_path, 1);
    /* Reapply Tcat -> per-species Tred/concentration after main loads CAT. */
    p->generated_catalog_pending=1;
    p->generated_catalog_active=1;
    if (marked > 0)
        snprintf(p->status, sizeof(p->status),
                 "Restored the latest Pred&Fit state from .fit; %d assignments from model.lin have fewer than 3 QN per state: assign them again.",
                 marked);
    else if (ignored_exclusions > 0)
        snprintf(p->status, sizeof(p->status),
                 "Restored the latest Pred&Fit state from .fit; ignored %d malformed fit exclusion%s.",
                 ignored_exclusions, ignored_exclusions == 1 ? "" : "s");
    else
        snprintf(p->status,sizeof(p->status),"Restored the latest Pred&Fit state from .fit.");
    return 1;
}

int predfit_calculate(AppState *s) {
    return predfit_calculate_all_species(s);
}

int predfit_calculate_all_species(AppState *s) {
    PredFitState *p = &s->predfit;
    predfit_publish_shared_state(s);
    if (!write_inputs(s, 0)) return 0;
    if (!have_program(s->settings.spcat_path)) {
        snprintf(p->status, sizeof(p->status), "Set the SPCAT program in Settings > Paths.");
        return 0;
    }

    char model_cat[600];
    work_file(p, "model.cat", model_cat, sizeof(model_cat));
    if (!run_program(s->settings.spcat_path, p->work_dir, p, "SPCAT")) return 0;
    app_enqueue_pending_load(s, PENDING_LOAD_CATALOG, model_cat, 1);
    p->generated_catalog_pending = 1;
    snprintf(p->status, sizeof(p->status), "SPCAT complete: multi-state model written to model.cat.");
    return 1;
}

/* Simulate every checked Hamiltonian and show their catalogues together.
   The project's active Hamiltonian is only borrowed for the runs and is put
   back before returning: simulating must not move the user elsewhere. */
int predfit_simulate(AppState *s) {
    PredFitState *p = &s->predfit;
    if (!have_program(s->settings.spcat_path)) {
        snprintf(p->status, sizeof(p->status), "Set the SPCAT program in Settings > Paths.");
        return 0;
    }
    int wanted = 0;
    for (int h = 0; h < p->n_hamiltonians; h++)
        if (!p->hamiltonian[h].simulate_excluded) wanted++;
    if (!wanted) {
        snprintf(p->status, sizeof(p->status), "Check at least one Hamiltonian in Simulation.");
        return 0;
    }

    const int started = p->active_hamiltonian;
    char failure[200] = "";
    int failed = 0;
    p->n_simulated = 0;
    for (int h = 0; h < p->n_hamiltonians; h++) {
        if (p->hamiltonian[h].simulate_excluded) continue;
        store_active_hamiltonian(p);
        p->active_hamiltonian = h;
        restore_active_model(p, &p->hamiltonian[h].model);
        predfit_refresh_work_dir(s);
        if (!write_inputs(s, 0) ||
            !run_program(s->settings.spcat_path, p->work_dir, p, "SPCAT")) {
            failed++;
            if (!failure[0])
                snprintf(failure, sizeof(failure), "%s: %s", p->hamiltonian[h].name, p->status);
            continue;
        }
        SimulatedCatalog *c = &p->simulated[p->n_simulated++];
        c->hamiltonian_id = p->hamiltonian[h].id;
        c->cat_temp_k = int_temp_at(p, p->temp_k);
        work_file(p, "model.cat", c->cat_path, sizeof(c->cat_path));
    }
    store_active_hamiltonian(p);
    p->active_hamiltonian = started;
    restore_active_model(p, &p->hamiltonian[started].model);
    predfit_refresh_work_dir(s);

    int done = p->n_simulated;
    if (done == 0) {
        snprintf(p->status, sizeof(p->status), "Nothing simulated - %s",
                 failure[0] ? failure : "no Hamiltonian produced a catalogue.");
        return 0;
    }
    app_enqueue_pending_load(s, PENDING_LOAD_SIMULATION, p->simulated[0].cat_path, 1);
    p->generated_catalog_pending = 1;
    if (failed)
        snprintf(p->status, sizeof(p->status),
                 "SPCAT complete for %d Hamiltonian%s; %d failed - %s",
                 done, done == 1 ? "" : "s", failed, failure);
    else
        snprintf(p->status, sizeof(p->status),
                 "SPCAT complete: %d Hamiltonian%s in the plot.", done, done == 1 ? "" : "s");
    return done;
}

/* SPFIT reads the QN layout of this model from its catalogue.  A model just
   imported, or one whose option line was edited, has none yet: make it here
   rather than refusing the fit and asking the user to press another button.
   The plot is deliberately left alone - this only produces the file. */
static int ensure_model_catalog(AppState *s) {
    PredFitState *p = &s->predfit;
    int nqn = 0;
    if (current_model_nqn(s, &nqn)) return 1;
    if (!have_program(s->settings.spcat_path)) {
        snprintf(p->status, sizeof(p->status), "Set the SPCAT program in Settings > Paths.");
        return 0;
    }
    if (!write_inputs(s, 0)) return 0;
    if (!run_program(s->settings.spcat_path, p->work_dir, p, "SPCAT")) return 0;
    if (current_model_nqn(s, &nqn)) return 1;
    snprintf(p->status, sizeof(p->status),
             "SPCAT produced no usable catalogue for %s: check the option line and the parameters.",
             p->hamiltonian[p->active_hamiltonian].name);
    return 0;
}

/* The states whose lines are in the plot right now, with the colour each of
   their broadened traces is drawn in.  The renderer asks for this instead of
   walking the project itself: which models are on screen is Pred&Fit's
   business, drawing them is the view's. */
int predfit_plot_species(const AppState *s, PredfitSpeciesTrace *out, int max) {
    if (!s || !out || max <= 0) return 0;
    const PredFitState *p = &s->predfit;
    if (!p->generated_catalog_active) return 0;
    int n = 0, fallback = 0;
    int n_models = p->n_simulated > 0 ? p->n_simulated : 1;
    for (int i = 0; i < n_models && n < max; i++) {
        int h = p->n_simulated > 0
              ? hamiltonian_index_by_id(p, p->simulated[i].hamiltonian_id)
              : p->active_hamiltonian;
        if (h < 0 || h >= p->n_hamiltonians) continue;
        const PredFitSnapshot *m = h == p->active_hamiltonian ? NULL : &p->hamiltonian[h].model;
        int n_states = m ? m->n_species : p->n_species;
        for (int k = 0; k < n_states && n < max; k++) {
            const PickettSpecies *sp = m ? &m->species[k] : &p->species[k];
            if (!sp->predict_enabled) { fallback++; continue; }
            out[n].hamiltonian_id = p->hamiltonian[h].id;
            out[n].state_index = sp->state_index;
            out[n].color = predfit_species_color(sp, fallback);
            snprintf(out[n].label, sizeof(out[n].label), "%s / %s",
                     p->hamiltonian[h].name, sp->name);
            fallback++;
            n++;
        }
    }
    return n;
}

/* After an action that changed a model, refresh the plot the way it was
   built: the whole simulation when several models share it, the active
   Hamiltonian alone otherwise. */
static int refresh_prediction(AppState *s) {
    return s->predfit.n_simulated > 0 ? predfit_simulate(s) : predfit_calculate_all_species(s);
}

int predfit_fit(AppState *s) {
    PredFitState *p = &s->predfit;
    int owned_assignments = 0;
    for (int i = 0; i < s->n_assignments; i++)
        if (assignment_belongs_to_active_hamiltonian(s, &s->assignments[i])) owned_assignments++;
    if (owned_assignments == 0) { snprintf(p->status, sizeof(p->status), "Assign lines to this Hamiltonian before running SPFIT."); return 0; }
    if (!ensure_model_catalog(s)) return 0;
    predfit_publish_shared_state(s);
    if (!push_fit_snapshot(s)) return 0;
    if (!write_inputs(s, 1)) { p->history_count--; return 0; }
    char cat_path[600];
    if (!have_program(s->settings.spfit_path) || !have_program(s->settings.spcat_path)) {
        snprintf(p->status, sizeof(p->status), "Set the SPFIT and SPCAT programs in Settings > Paths.");
        p->history_count--;
        return 0;
    }
    if (!run_program(s->settings.spfit_path, p->work_dir, p, "SPFIT")) { p->history_count--; return 0; }
    report_invalidate();
    import_fitted_parameters(p);
    if (p->n_simulated > 0) {
        /* The plot holds several models: refresh them all instead of
           replacing the simulation with this one Hamiltonian. */
        if (!predfit_simulate(s)) return 0;
    } else {
        if (!run_program(s->settings.spcat_path, p->work_dir, p, "SPCAT after fit")) return 0;
        work_file(p,"model.cat",cat_path,sizeof(cat_path));
        app_enqueue_pending_load(s, PENDING_LOAD_CATALOG, cat_path, 1);
        p->generated_catalog_pending = 1;
    }
    snprintf(p->status, sizeof(p->status), "SPFIT complete; refreshed SPCAT prediction.");
    fit_summary(p, p->status, sizeof(p->status));
    return 1;
}

int predfit_undo_last_fit(AppState *s) {
    PredFitState *p = &s->predfit;
    if (p->history_count == 0) {
        snprintf(p->status, sizeof(p->status), "No earlier fit state in this session.");
        return 0;
    }
    restore_fit_snapshot(s, &p->history[p->history_count - 1]);
    p->history_count--;
    if (!refresh_prediction(s)) return 0;
    snprintf(p->status, sizeof(p->status), "Restored pre-fit state; SPCAT refreshed.");
    return 1;
}

/* The complete editor is rendered in an independent SDL window.  Its controls
 * deliberately share the same work model as the quick panel. */
void predfit_open_advanced(AppState *s) {
    PredFitState *p = &s->predfit;
    if (p->advanced_window) { SDL_RaiseWindow(p->advanced_window); return; }
    p->advanced_window = SDL_CreateWindow("Pred&Fit Advanced", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          920, 640, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!p->advanced_window) { snprintf(p->status, sizeof(p->status), "Could not open Advanced window."); return; }
    SDL_SetWindowMinimumSize(p->advanced_window, 900, 520);
    p->advanced_renderer = SDL_CreateRenderer(p->advanced_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!p->advanced_renderer) { SDL_DestroyWindow(p->advanced_window); p->advanced_window=NULL; snprintf(p->status, sizeof(p->status), "Could not create Advanced renderer."); return; }
    {
        int ww=0, wh=0, dw=0, dh=0;
        SDL_GetWindowSize(p->advanced_window, &ww, &wh);
        SDL_GetRendererOutputSize(p->advanced_renderer, &dw, &dh);
        if (ww > 0 && dw > 0) {
            float dpi = (float)dw / (float)ww;
            SDL_RenderSetScale(p->advanced_renderer, dpi, dpi);
        }
    }
    p->advanced_window_id = SDL_GetWindowID(p->advanced_window); p->advanced_open = 1; p->advanced_tab = 4; p->advanced_hover_line = -1;
}
void predfit_close_advanced(AppState *s) {
    PredFitState *p=&s->predfit; if (p->advanced_renderer) SDL_DestroyRenderer(p->advanced_renderer);
    if (p->advanced_window) SDL_DestroyWindow(p->advanced_window);
    p->advanced_renderer=NULL; p->advanced_window=NULL; p->advanced_open=0; p->advanced_window_id=0;
    p->advanced_edit_param=-1;
    p->advanced_edit_species=-1;
    p->advanced_edit_hamiltonian=-1;
}

void predfit_dispose(AppState *s) {
    PredFitState *p=&s->predfit;
    predfit_close_advanced(s);
    free(p->history);
    p->history=NULL;
    p->history_count=0;
    p->history_capacity=0;
}

static void advanced_begin_edit(PredFitState *p, int row, int col) {
    if (row < 0 || row >= p->n_param) return;
    PickettParameter *x = &p->param[row];
    p->advanced_edit_param = row;
    p->advanced_edit_col = col;
    p->advanced_edit_replace = 1;
    if (col == 0) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->id);
    else if (col == 1) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.15g", x->value);
    else snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.8g", x->error);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

static void advanced_begin_line_error(PredFitState *p) {
    p->advanced_edit_param = -2;
    p->advanced_edit_col = 0;
    p->advanced_edit_replace = 1;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.8g", p->line_error_mhz);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

static void advanced_begin_hamiltonian_edit(PredFitState *p) {
    p->advanced_edit_param = -4;
    p->advanced_edit_col = 0;
    p->advanced_edit_replace = 1;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%s", p->hamiltonian_line);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

static int hamiltonian_name_is_available(const PredFitState *p, int current, const char *name) {
    PredFitState probe = *p;
    char candidate[128], existing[128];
    HamiltonianModel edited = p->hamiltonian[current];
    snprintf(edited.name, sizeof(edited.name), "%s", name);
    probe.hamiltonian[current] = edited;
    probe.active_hamiltonian = current;
    hamiltonian_file_stem(&probe, candidate, sizeof(candidate));
    for (int i = 0; i < p->n_hamiltonians; i++) {
        if (i == current) continue;
        probe.active_hamiltonian = i;
        hamiltonian_file_stem(&probe, existing, sizeof(existing));
        if (strcmp(candidate, existing) == 0) return 0;
    }
    return 1;
}

static void advanced_begin_hamiltonian_name_edit(PredFitState *p) {
    if (p->active_hamiltonian < 0 || p->active_hamiltonian >= p->n_hamiltonians) return;
    p->advanced_edit_param = -6;
    p->advanced_edit_hamiltonian = p->active_hamiltonian;
    p->advanced_edit_col = 0;
    p->advanced_edit_replace = 1;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%s",
             p->hamiltonian[p->active_hamiltonian].name);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

/* Renaming a state from the sidebar edits the very same name the States tab
   shows; it is a separate edit mode only because it is drawn in place. */
static void advanced_begin_state_name_edit(PredFitState *p, int row) {
    if (row < 0 || row >= p->n_species) return;
    p->advanced_edit_param = -7;
    p->advanced_edit_species = row;
    p->advanced_edit_col = 0;
    p->advanced_edit_replace = 1;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%s", p->species[row].name);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

/* Fields of the .int control card.  QROT has no editor: it is always derived
   from the current A/B/C, temperature and sigma. */
static void advanced_begin_int_edit(PredFitState *p, int field) {
    PickettIntSettings *x = &p->int_settings;
    p->advanced_edit_param = -5;
    p->advanced_edit_col = field;
    p->advanced_edit_replace = 1;
    if (field == 0) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->flags);
    else if (field == 1) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->tag);
    else if (field == 2) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->fbegin);
    else if (field == 3) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->fend);
    else if (field == 4) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", x->intensity_cutoff);
    else if (field == 5) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", x->fqlim_ghz);
    else if (field == 6) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", p->temp_k);
    else if (field == 7) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%d", x->maxv);
    else snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", x->sigma);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

/* State fields use the same regular text editor as parameter cells.  The .int
   belongs to the Hamiltonian; a state owns only its name, dipoles and external
   concentration multiplier. */
static void advanced_begin_species_edit(PredFitState *p, int row, int field) {
    if (row < 0 || row >= p->n_species) return;
    PickettSpecies *sp = &p->species[row];
    p->advanced_edit_param = -3;
    p->advanced_edit_species = row;
    p->advanced_edit_col = field;
    p->advanced_edit_replace = 1;
    if (field == 0) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%s", sp->name);
    else if (field >= 1 && field <= 3) snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", sp->mu[field - 1]);
    else snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.12g", sp->concentration);
    p->advanced_edit_anchor = 0;
    p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf);
    SDL_StartTextInput();
}

static void advanced_edit_clamp(PredFitState *p) {
    int n = (int)strlen(p->advanced_edit_buf);
    if (p->advanced_edit_caret < 0) p->advanced_edit_caret = 0;
    if (p->advanced_edit_caret > n) p->advanced_edit_caret = n;
    if (p->advanced_edit_anchor < 0) p->advanced_edit_anchor = 0;
    if (p->advanced_edit_anchor > n) p->advanced_edit_anchor = n;
}

/* Width of the first `n` characters of `text`: the caret sits between two
   characters, so every caret position is a prefix width. */
static int adv_prefix_w(int font, const char *text, int n) {
    char prefix[sizeof(((PredFitState *)0)->advanced_edit_buf)];
    if (n < 0) n = 0;
    if (n > (int)sizeof(prefix) - 1) n = (int)sizeof(prefix) - 1;
    memcpy(prefix, text, (size_t)n);
    prefix[n] = '\0';
    return ui_text_w(font, prefix);
}

/* A text cursor that cannot be seen is a field the user cannot aim at: the
   edited text is therefore always drawn with its selection and its caret. */
static void adv_edit_text(SDL_Renderer *r, int font, const PredFitState *p,
                          int x, int y, SDL_Color c) {
    const char *text = p->advanced_edit_buf;
    int h = ui_text_h(font);
    int lo = p->advanced_edit_caret < p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    int hi = p->advanced_edit_caret > p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    if (hi > lo) {
        int x0 = x + adv_prefix_w(font, text, lo);
        int x1 = x + adv_prefix_w(font, text, hi);
        ui_fill(r, (SDL_Rect){x0, y - 1, x1 - x0, h + 2}, UI_ACCENT_SOFT);
    }
    ui_text(r, font, text, x, y, c);
    ui_fill(r, (SDL_Rect){x + adv_prefix_w(font, text, p->advanced_edit_caret), y - 1, 2, h + 2},
            UI_ACCENT);
}

/* One cell of a table: the same call site draws the stored value and, while
   that cell is being edited, the live buffer with its caret. */
static void adv_cell_text(SDL_Renderer *r, int font, const PredFitState *p, int editing,
                          const char *text, int x, int y, SDL_Color c) {
    if (editing) adv_edit_text(r, font, p, x, y, c);
    else ui_text(r, font, text, x, y, c);
}

/* Where a click lands inside an edited field: the caret goes between the two
   characters the pointer is closest to. */
static void adv_place_caret(PredFitState *p, int font, int text_x, int click_x) {
    const char *text = p->advanced_edit_buf;
    int n = (int)strlen(text), best = 0;
    double best_d = 1e18;
    for (int i = 0; i <= n; i++) {
        double d = fabs((double)(text_x + adv_prefix_w(font, text, i)) - (double)click_x);
        if (d < best_d) { best_d = d; best = i; }
    }
    p->advanced_edit_caret = p->advanced_edit_anchor = best;
    p->advanced_edit_replace = 0;
}

static int advanced_edit_delete_selection(PredFitState *p) {
    advanced_edit_clamp(p);
    int lo = p->advanced_edit_caret < p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    int hi = p->advanced_edit_caret > p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    if (lo == hi) return 0;
    memmove(p->advanced_edit_buf + lo, p->advanced_edit_buf + hi, strlen(p->advanced_edit_buf + hi) + 1);
    p->advanced_edit_anchor = p->advanced_edit_caret = lo;
    return 1;
}

static void advanced_edit_insert(PredFitState *p, const char *text) {
    advanced_edit_delete_selection(p);
    int n = (int)strlen(p->advanced_edit_buf), add = (int)strlen(text);
    int room = (int)sizeof(p->advanced_edit_buf) - 1 - n;
    if (add > room) add = room;
    if (add <= 0) return;
    memmove(p->advanced_edit_buf + p->advanced_edit_caret + add,
            p->advanced_edit_buf + p->advanced_edit_caret,
            (size_t)(n - p->advanced_edit_caret) + 1);
    memcpy(p->advanced_edit_buf + p->advanced_edit_caret, text, (size_t)add);
    p->advanced_edit_caret += add;
    p->advanced_edit_anchor = p->advanced_edit_caret;
}

static void advanced_edit_copy(PredFitState *p, int cut) {
    advanced_edit_clamp(p);
    int lo = p->advanced_edit_caret < p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    int hi = p->advanced_edit_caret > p->advanced_edit_anchor ? p->advanced_edit_caret : p->advanced_edit_anchor;
    if (lo == hi) return;
    char text[sizeof(p->advanced_edit_buf)];
    int n = hi - lo;
    memcpy(text, p->advanced_edit_buf + lo, (size_t)n);
    text[n] = '\0';
    SDL_SetClipboardText(text);
    if (cut) advanced_edit_delete_selection(p);
}

static void advanced_edit_paste(PredFitState *p) {
    char *clip = SDL_GetClipboardText();
    if (!clip) return;
    char clean[sizeof(p->advanced_edit_buf)];
    int n = 0;
    for (const char *q = clip; *q && n < (int)sizeof(clean) - 1; q++)
        if (*q != '\r' && *q != '\n' && *q != '\t') clean[n++] = *q;
    clean[n] = '\0';
    SDL_free(clip);
    advanced_edit_insert(p, clean);
}

static void advanced_commit_edit(PredFitState *p) {
    if (p->advanced_edit_param != -1) p->session_dirty = 1;
    if (p->advanced_edit_param == -5) {
        PickettIntSettings *x = &p->int_settings;
        char *end = NULL;
        errno = 0;
        double v = strtod(p->advanced_edit_buf, &end);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end != p->advanced_edit_buf && end && *end == '\0' && errno != ERANGE && isfinite(v)) {
            switch (p->advanced_edit_col) {
                case 0: if (v >= 0.0 && v <= INT_MAX) x->flags = (int)lround(v); break;
                case 1: if (v >= 0.0 && v <= INT_MAX) x->tag = (int)lround(v); break;
                case 2: if (v >= 0.0 && v <= INT_MAX) x->fbegin = (int)lround(v); break;
                case 3: if (v >= 0.0 && v <= INT_MAX) x->fend = (int)lround(v); break;
                case 4: x->intensity_cutoff = v; break;
                case 5: if (v >= 0.0) x->fqlim_ghz = v; break;
                case 6:
                    if (v > 0.0) {
                        /* TEMP is Hamiltonian-wide. Keep the legacy cache
                           synchronized for old-session compatibility. */
                        p->temp_k = v;
                        x->temp_k = v;
                    }
                    break;
                case 7: if (v >= -1.0 && v <= INT_MAX) x->maxv = (int)lround(v); break;
                case 8: if (v > 0.0) x->sigma = v; break;
            }
        }
        p->advanced_edit_param = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param == -4) {
        char candidate[sizeof(p->hamiltonian_line)];
        snprintf(candidate, sizeof(candidate), "%s", p->advanced_edit_buf);
        int nvib = 0;
        if (hamiltonian_nvib(candidate, &nvib))
            snprintf(p->hamiltonian_line, sizeof(p->hamiltonian_line), "%s", candidate);
        p->advanced_edit_param = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param == -6) {
        int h = p->advanced_edit_hamiltonian;
        if (h >= 0 && h < p->n_hamiltonians && p->advanced_edit_buf[0] &&
            hamiltonian_name_is_available(p, h, p->advanced_edit_buf)) {
            snprintf(p->hamiltonian[h].name, sizeof(p->hamiltonian[h].name), "%s",
                     p->advanced_edit_buf);
            p->session_dirty = 1;
            snprintf(p->status, sizeof(p->status), "Hamiltonian renamed to %s.", p->hamiltonian[h].name);
        } else {
            snprintf(p->status, sizeof(p->status), "Hamiltonian name is empty or has the same file name as another H.");
        }
        p->advanced_edit_param = -1;
        p->advanced_edit_hamiltonian = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param == -7) {
        int row = p->advanced_edit_species;
        if (row >= 0 && row < p->n_species && p->advanced_edit_buf[0]) {
            snprintf(p->species[row].name, sizeof(p->species[row].name), "%s", p->advanced_edit_buf);
            snprintf(p->status, sizeof(p->status), "State renamed to %s.", p->species[row].name);
        } else if (row >= 0) {
            snprintf(p->status, sizeof(p->status), "A state name cannot be empty.");
        }
        p->advanced_edit_param = -1;
        p->advanced_edit_species = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param == -3) {
        if (p->advanced_edit_species >= 0 && p->advanced_edit_species < p->n_species) {
            PickettSpecies *sp = &p->species[p->advanced_edit_species];
            if (p->advanced_edit_col == 0) {
                if (p->advanced_edit_buf[0]) snprintf(sp->name, sizeof(sp->name), "%s", p->advanced_edit_buf);
            } else {
                char *end = NULL; double v = strtod(p->advanced_edit_buf, &end);
                if (end != p->advanced_edit_buf && isfinite(v) &&
                    (p->advanced_edit_col >= 1 && v >= 0.0)) {
                    if (p->advanced_edit_col <= 3) sp->mu[p->advanced_edit_col - 1] = v;
                    else sp->concentration = v;
                }
            }
            if (p->advanced_edit_species == p->active_species) load_active_species(p);
            p->intensity_dirty = 1;
        }
        p->advanced_edit_param = -1;
        p->advanced_edit_species = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param == -2) {
        char *end = NULL; double v = strtod(p->advanced_edit_buf, &end);
        if (end != p->advanced_edit_buf && isfinite(v) && v > 0.0) p->line_error_mhz = v;
        p->advanced_edit_param = -1;
        p->advanced_edit_replace = 0;
        p->advanced_edit_anchor = p->advanced_edit_caret = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param < 0 || p->advanced_edit_param >= p->n_param) return;
    PickettParameter *x = &p->param[p->advanced_edit_param];
    if (p->advanced_edit_col == 0) {
        char *end = NULL;
        errno = 0;
        long id = strtol(p->advanced_edit_buf, &end, 10);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end != p->advanced_edit_buf && end && *end == '\0' && errno != ERANGE &&
            id > 0 && id <= INT_MAX && ((int)id == x->id || !have_parameter_id(p, (int)id))) x->id = (int)id;
        else snprintf(p->status, sizeof(p->status), "Parameter ID must be positive and unique.");
    } else {
        char *end = NULL; double v = strtod(p->advanced_edit_buf, &end);
        if (end != p->advanced_edit_buf && isfinite(v) &&
            (p->advanced_edit_col == 1 || v >= 0.0)) {
            if (p->advanced_edit_col == 1) x->value = v; else x->error = v;
        }
    }
    parameter_label(x);
    sync_basic_from_parameters(p);
    p->advanced_edit_param = -1;
    p->advanced_edit_replace = 0;
    p->advanced_edit_anchor = p->advanced_edit_caret = 0;
    SDL_StopTextInput();
}

static int advanced_edit_event(AppState *s, const SDL_Event *e) {
    PredFitState *p = &s->predfit;
    if (p->advanced_edit_param == -1) return 0;
    if (e->type == SDL_TEXTINPUT && e->text.windowID == p->advanced_window_id) {
        advanced_edit_insert(p, e->text.text);
        p->advanced_edit_replace = 0;
        return 1;
    }
    if (e->type == SDL_KEYDOWN && e->key.windowID == p->advanced_window_id) {
        SDL_Keycode k = e->key.keysym.sym;
        SDL_Keymod mod = e->key.keysym.mod;
        int cmd = (mod & (KMOD_GUI | KMOD_CTRL)) != 0;
        int shift = (mod & KMOD_SHIFT) != 0;
        if (cmd && k == SDLK_a) { p->advanced_edit_anchor = 0; p->advanced_edit_caret = (int)strlen(p->advanced_edit_buf); return 1; }
        if (cmd && k == SDLK_c) { advanced_edit_copy(p, 0); return 1; }
        if (cmd && k == SDLK_x) { advanced_edit_copy(p, 1); return 1; }
        if (cmd && k == SDLK_v) { advanced_edit_paste(p); return 1; }
        if (k == SDLK_BACKSPACE) {
            if (!advanced_edit_delete_selection(p) && p->advanced_edit_caret > 0) {
                p->advanced_edit_anchor = p->advanced_edit_caret - 1;
                advanced_edit_delete_selection(p);
            }
            return 1;
        }
        if (k == SDLK_DELETE) {
            if (!advanced_edit_delete_selection(p) && p->advanced_edit_caret < (int)strlen(p->advanced_edit_buf)) {
                p->advanced_edit_anchor = p->advanced_edit_caret + 1;
                advanced_edit_delete_selection(p);
            }
            return 1;
        }
        if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_HOME || k == SDLK_END) {
            int caret = p->advanced_edit_caret;
            int length = (int)strlen(p->advanced_edit_buf);
            if (k == SDLK_HOME || (cmd && k == SDLK_LEFT)) caret = 0;
            else if (k == SDLK_END || (cmd && k == SDLK_RIGHT)) caret = length;
            else if (k == SDLK_LEFT) caret--;
            else if (k == SDLK_RIGHT) caret++;
            p->advanced_edit_caret = caret;
            if (!shift) p->advanced_edit_anchor = caret;
            advanced_edit_clamp(p);
            return 1;
        }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { advanced_commit_edit(p); return 1; }
        if (k == SDLK_ESCAPE) { p->advanced_edit_param=-1; p->advanced_edit_hamiltonian=-1; p->advanced_edit_species=-1; p->advanced_edit_replace=0; p->advanced_edit_anchor=p->advanced_edit_caret=0; SDL_StopTextInput(); return 1; }
        return 1;
    }
    return 0;
}

static void add_parameter(PredFitState *p) {
    if (p->n_param >= MAX_PICKETT_PARAMS) return;
    PickettParameter *x = &p->param[p->n_param++];
    *x = (PickettParameter){0, 0.0, 1.0, "Pickett parameter"};
    p->session_dirty = 1;
    advanced_begin_edit(p, p->n_param - 1, 0);
}

/* Removing a row keeps the table contiguous, so the fit inputs written from it
   never carry a stale parameter. */
static void delete_parameter(PredFitState *p, int row) {
    if (row < 0 || row >= p->n_param) return;
    for (int i = row; i < p->n_param - 1; i++) p->param[i] = p->param[i + 1];
    p->n_param--;
    p->session_dirty = 1;
    if (p->advanced_edit_param == row) { p->advanced_edit_param = -1; SDL_StopTextInput(); }
    else if (p->advanced_edit_param > row) p->advanced_edit_param--;
    if (p->advanced_param_scroll > 0 && p->advanced_param_scroll >= p->n_param) p->advanced_param_scroll--;
    sync_basic_from_parameters(p);
}

/* ---------------------------------------------------------------------------
 *  Cached view of the last SPFIT run.
 *  model.fit is read once per change instead of once per drawn row: the fitting
 *  tab shows every parameter and every observation, and re-scanning the file
 *  for each of them made the window redraw in file I/O.
 * ------------------------------------------------------------------------- */
#define MAX_REPORT_PARAM_LINES 64

typedef struct {
    time_t mtime;
    char   path[600];
    char   param_line[MAX_REPORT_PARAM_LINES][100];  /* verbatim SPFIT block   */
    int    n_param_lines;
    FitObservation obs[MAX_ASSIGNMENTS];             /* indexed by line number */
    FitExclusionKey obs_key[MAX_ASSIGNMENTS];         /* identity in model.lin */
    unsigned char obs_key_valid[MAX_ASSIGNMENTS];
    unsigned char bad_line[MAX_ASSIGNMENTS];          /* SPFIT "Bad Line(n)" */
    int    n_obs;
    int    loaded;
} FitReport;

typedef enum {
    FIT_ROW_EXCLUDED,
    FIT_ROW_REJECTED,
    FIT_ROW_USED,
    FIT_ROW_NOT_USED,
    FIT_ROW_STALE,
    FIT_ROW_NOT_READ,
    FIT_ROW_NOT_FITTED,
    FIT_ROW_OTHER_MODEL   /* owned by a Hamiltonian this report is not about */
} FitRowState;

static FitReport g_report;

static void report_reset(FitReport *rep) {
    rep->n_param_lines = 0;
    rep->n_obs = 0;
    rep->loaded = 0;
    memset(rep->obs, 0, sizeof(rep->obs));
    memset(rep->obs_key_valid, 0, sizeof(rep->obs_key_valid));
    memset(rep->bad_line, 0, sizeof(rep->bad_line));
}

static void report_invalidate(void) {
    report_reset(&g_report);
    g_report.mtime = 0;
    g_report.path[0] = '\0';
}

/* Parses one " 12: ..." observation row of model.fit.  SPFIT always emits
   the QNs in a 12I3 (36-character) field.  Its NQN changes how many of those
   slots carry numbers, but never the starting column of EXP.FREQ.  Skipping a
   guessed count of QNs was the reason four-QN/state fits were read as stale
   "reassigned" rows after an otherwise successful run. */
static int parse_observation(const char *line, int *number, FitObservation *out) {
    int n = 0;
    if (sscanf(line, " %d:", &n) != 1 || n <= 0) return 0;
    const char *q = strchr(line, ':');
    if (!q) return 0;
    q++;
    if (strlen(q) < 36) return 0;
    q += 36;
    char *end = NULL;
    double obs = strtod(q, &end); if (end == q) return 0; q = end;
    double calc = strtod(q, &end); if (end == q) return 0; q = end;
    double diff = strtod(q, &end); if (end == q) return 0; q = end;
    double unc = strtod(q, &end); if (end == q || unc <= 0.0) return 0;
    *number = n;
    int used = (fabs(diff) < 999.0 && unc < 999.0);
    *out = (FitObservation){1, used, obs, calc, diff, unc};
    return 1;
}

static void report_refresh(const PredFitState *p) {
    char path[600];
    work_file(p, "model.fit", path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        if (g_report.mtime != 0 || g_report.path[0]) { report_reset(&g_report); g_report.mtime = 0; g_report.path[0] = '\0'; }
        return;
    }
    if (st.st_mtime == g_report.mtime && strcmp(path, g_report.path) == 0) return;

    g_report.mtime = st.st_mtime;
    snprintf(g_report.path, sizeof(g_report.path), "%s", path);
    report_reset(&g_report);
    g_report.loaded = 1;

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        int bad_number = 0;
        if (sscanf(line, "Bad Line( %d):", &bad_number) == 1 &&
            bad_number >= 1 && bad_number <= MAX_ASSIGNMENTS)
            g_report.bad_line[bad_number - 1] = 1;
        /* SPFIT prints the parameter block once per iteration; the last one
           wins, and it is already formatted the way it should be read. */
        if (strstr(line, "NEW PARAMETER (EST. ERROR)")) {
            g_report.n_param_lines = 0;
            while (fgets(line, sizeof(line), fp)) {
                int index = 0, id = 0;
                if (sscanf(line, " %d %d", &index, &id) != 2 || !strchr(line, '/')) break;
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                /* trim the trailing padding SPFIT writes */
                for (int i = (int)strlen(line) - 1; i >= 0 && line[i] == ' '; i--) line[i] = '\0';
                if (g_report.n_param_lines < MAX_REPORT_PARAM_LINES)
                    snprintf(g_report.param_line[g_report.n_param_lines++],
                             sizeof(g_report.param_line[0]), "%s", line);
            }
            continue;
        }
        int number = 0;
        FitObservation o;
        if (parse_observation(line, &number, &o) && number <= MAX_ASSIGNMENTS) {
            g_report.obs[number - 1] = o;              /* the last iteration wins */
            if (number > g_report.n_obs) g_report.n_obs = number;
        }
    }
    fclose(fp);
    /* model.fit numbers only describe the .lin rows present during Fit.
       Bind them to their transition identities so an excluded/deleted/reordered
       assignment cannot make the fitting view show another row's residual. */
    int n_lin = read_lin_rows(p);
    for (int i = 0; i < n_lin; i++) {
        PredLine q;
        lin_row_pred(&g_lin_rows[i], &q);
        exclusion_key_from_pred(&g_report.obs_key[i], &q);
        g_report.obs_key_valid[i] = 1;
    }
}

static FitObservation report_observation(int line_number) {
    if (line_number < 1 || line_number > MAX_ASSIGNMENTS) return (FitObservation){0, 0, 0, 0, 0, 0};
    return g_report.obs[line_number - 1];
}

static int report_line_for_assignment(const Assignment *a) {
    for (int i = 0; i < MAX_ASSIGNMENTS; i++)
        if (g_report.obs_key_valid[i] && exclusion_key_matches(&g_report.obs_key[i], &a->pred)) return i;
    return -1;
}

static int observation_is_current(const Assignment *a, FitObservation o);

static FitRowState fitting_row_state(const Assignment *a, int row) {
    if (!a->fit_enabled) return FIT_ROW_EXCLUDED;
    if (row >= 0 && row < MAX_ASSIGNMENTS && g_report.bad_line[row]) return FIT_ROW_REJECTED;
    FitObservation o = row >= 0 && row < MAX_ASSIGNMENTS ? g_report.obs[row] : (FitObservation){0};
    if (observation_is_current(a, o)) return o.used ? FIT_ROW_USED : FIT_ROW_NOT_USED;
    if (o.found) return FIT_ROW_STALE;
    return g_report.loaded ? FIT_ROW_NOT_READ : FIT_ROW_NOT_FITTED;
}

/* Where one assignment stands in the SPFIT report now in memory, and the
   observation to show beside it.  SPFIT fitted one Hamiltonian, and the
   conformers of one molecule share their quantum numbers, so a line of
   another model is never looked up here: it would be shown somebody else's
   residual. */
static FitRowState fitting_row(const AppState *s, const Assignment *a, FitObservation *out) {
    if (out) *out = (FitObservation){0, 0, 0, 0, 0, 0};
    if (!assignment_belongs_to_active_hamiltonian(s, a)) return FIT_ROW_OTHER_MODEL;
    int row = report_line_for_assignment(a);
    if (out && row >= 0) *out = report_observation(row + 1);
    return fitting_row_state(a, row);
}

/* The assignment editor is the owner of the measured frequency.  A .fit file
   is only the record of a particular SPFIT run, and is therefore stale as soon
   as the same transition is assigned to a different experimental peak. */
static int observation_is_current(const Assignment *a, FitObservation o) {
    return o.found && fabs(o.obs - a->exp_freq) < 1e-5;
}

/* How well a line sits in the fit, read at a glance: red beyond 4 sigma, then
   orange, yellow, and green inside 1.5 sigma. */
static SDL_Color residual_color(double z) {
    double a = fabs(z);
    if (a > 4.0) return (SDL_Color){229,  83,  75, 255};
    if (a > 2.5) return (SDL_Color){232, 138,  58, 255};
    if (a > 1.5) return (SDL_Color){226, 190,  70, 255};
    return (SDL_Color){ 70, 196, 138, 255};
}

/* ---------------------------------------------------------------------------
 *  Layout
 *  The window is resizable, so every rectangle is derived from its current
 *  size, and the renderer and the event handler read them from here.
 * ------------------------------------------------------------------------- */
#define ADV_PAD        18
#define ADV_HEADER_H   42
#define ADV_TAB_Y      48
#define ADV_TAB_H      30
#define ADV_TAB_W     118
#define ADV_TAB_STEP  125
#define ADV_CONTENT_Y  92
#define ADV_FOOTER_H   46

typedef struct {
    int w, h;
    SDL_Rect tab[6];
    SDL_Rect caption;
    SDL_Rect hamiltonian;   /* editable shared third .par/.var line       */
    SDL_Rect int_cell[10];  /* complete .int control record, two rows     */
    SDL_Rect table;         /* frame around the scrolling rows            */
    SDL_Rect rows;          /* the rows themselves, header excluded       */
    int row_h, rows_visible;
    SDL_Rect params;        /* fitting tab: the SPFIT parameter block     */
    int param_rows_visible;
    SDL_Rect footer;
    SDL_Rect btn_a, btn_b, btn_c, btn_d;
    /* This navigator is deliberately present on every page.  A Hamiltonian
       is the unit of a Pickett calculation, while a state is its child: the
       hierarchy must therefore not be hidden behind a separate tab. */
    SDL_Rect project_nav, project_rows, project_up, project_down, project_add,
             project_rename, project_duplicate, project_delete;
    int project_rows_visible;
} AdvUI;

static AdvUI adv_ui(AppState *s, int tab) {
    PredFitState *p = &s->predfit;
    AdvUI u;
    memset(&u, 0, sizeof(u));
    SDL_GetWindowSize(p->advanced_window, &u.w, &u.h);
    if (u.w < 900) u.w = 900;
    if (u.h < 420) u.h = 420;

    for (int i = 0; i < 6; i++) u.tab[i] = (SDL_Rect){16 + i * ADV_TAB_STEP, ADV_TAB_Y, ADV_TAB_W, ADV_TAB_H};

    int footer_y = u.h - ADV_FOOTER_H;
    const int nav_w = 224;
    const int content_x = ADV_PAD + nav_w + 12;
    const int content_w = u.w - content_x - ADV_PAD;
    u.footer  = (SDL_Rect){0, footer_y, u.w, ADV_FOOTER_H};
    u.project_nav = (SDL_Rect){ADV_PAD, ADV_CONTENT_Y, nav_w, footer_y - ADV_CONTENT_Y - 10};
    u.project_rows = (SDL_Rect){u.project_nav.x + 2, u.project_nav.y + 28,
                                u.project_nav.w - 4, u.project_nav.h - 198};
    if (u.project_rows.h < 24) u.project_rows.h = 24;
    u.project_rows_visible = u.project_rows.h / 23;
    if (u.project_rows_visible < 1) u.project_rows_visible = 1;
    u.project_up = (SDL_Rect){u.project_nav.x + 8, u.project_nav.y + u.project_nav.h - 160,
                              (u.project_nav.w - 22) / 2, 26};
    u.project_down = (SDL_Rect){u.project_up.x + u.project_up.w + 6, u.project_up.y,
                                u.project_up.w, 26};
    u.project_add = (SDL_Rect){u.project_nav.x + 8, u.project_nav.y + u.project_nav.h - 126,
                               u.project_nav.w - 16, 26};
    u.project_rename = (SDL_Rect){u.project_nav.x + 8, u.project_nav.y + u.project_nav.h - 92,
                                     (u.project_nav.w - 22) / 2, 26};
    u.project_duplicate = (SDL_Rect){u.project_rename.x + u.project_rename.w + 6,
                                     u.project_rename.y, u.project_rename.w, 26};
    u.project_delete = (SDL_Rect){u.project_nav.x + 8, u.project_nav.y + u.project_nav.h - 58,
                                  u.project_nav.w - 16, 26};
    u.caption = (SDL_Rect){content_x, ADV_CONTENT_Y, content_w, 18};

    int table_top = ADV_CONTENT_Y + 26;
    if (tab == 0) {
        u.hamiltonian = (SDL_Rect){content_x, table_top, content_w, 28};
        table_top += 38;
    } else if (tab == 3) {
        int gap = 6;
        int cw = (content_w - 4 * gap) / 5;
        for (int i = 0; i < 10; i++) {
            int col = i % 5, row = i / 5;
            u.int_cell[i] = (SDL_Rect){content_x + col * (cw + gap), table_top + row * 31, cw, 27};
        }
        table_top += 70;
    }

    if (tab == 2) {
        int lines = g_report.n_param_lines > 0 ? g_report.n_param_lines : 1;
        if (lines > 9) lines = 9;
        u.param_rows_visible = lines;
        u.params = (SDL_Rect){content_x, table_top, content_w, 26 + lines * 18 + 8};
        table_top = u.params.y + u.params.h + 12;
        u.row_h = 22;
    } else {
        u.row_h = tab == 0 ? 25 : tab == 3 ? 27 : 23;
    }

    u.table = (SDL_Rect){content_x, table_top, content_w, footer_y - table_top - 10};
    if (u.table.h < 80) u.table.h = 80;
    u.rows  = (SDL_Rect){u.table.x + 2, u.table.y + 26, u.table.w - 4, u.table.h - 28};
    u.rows_visible = u.rows.h / u.row_h;
    if (u.rows_visible < 1) u.rows_visible = 1;

    u.btn_a = (SDL_Rect){content_x, footer_y + 8, tab == 4 ? 150 : (tab == 0 || tab == 3) ? 142 : 110, 30};
    u.btn_b = (SDL_Rect){u.btn_a.x + u.btn_a.w + 10, footer_y + 8, 132, 30};
    u.btn_c = (SDL_Rect){u.btn_b.x + u.btn_b.w + 10, footer_y + 8, 150, 30};
    u.btn_d = (SDL_Rect){u.btn_c.x + u.btn_c.w + 10, footer_y + 8, 168, 30};
    return u;
}

/* Column x inside a table, as a fraction of its width: the columns follow the
   window when it is resized instead of staying where they were designed. */
static int adv_col(SDL_Rect table, double fraction) {
    return table.x + 10 + (int)((table.w - 20) * fraction);
}

static int adv_scroll_limit(int total, int visible) {
    int limit = total - visible;
    return limit > 0 ? limit : 0;
}

/* A project is read as a visible hierarchy: each Hamiltonian header is
   followed by its states. A state click selects its parent and the state, so
   no hidden next/previous navigation is required. */
static int project_row_count(const PredFitState *p) {
    int count = 0;
    for (int h = 0; h < p->n_hamiltonians; h++) {
        int n_states = h == p->active_hamiltonian ? p->n_species : p->hamiltonian[h].model.n_species;
        count += 1 + n_states;
    }
    return count;
}

static int project_row_info(const PredFitState *p, int row, int *out_h, int *out_state) {
    int at = 0;
    for (int h = 0; h < p->n_hamiltonians; h++) {
        int n_states = h == p->active_hamiltonian ? p->n_species : p->hamiltonian[h].model.n_species;
        if (row == at) { *out_h = h; *out_state = -1; return 1; }
        at++;
        if (row >= at && row < at + n_states) { *out_h = h; *out_state = row - at; return 1; }
        at += n_states;
    }
    return 0;
}

/* A sidebar row is "prefix + name": the prefix says what the row is, the name
   is the part that can be renamed in place.  Both the renderer and the click
   handler measure it here so a caret lands where the glyphs are drawn. */
static int nav_row_prefix(const PredFitState *p, int h_index, int state_index,
                          char *out, size_t size) {
    if (state_index < 0) {
        snprintf(out, size, "H%d  ", p->hamiltonian[h_index].id);
    } else {
        const PickettSpecies *sp = h_index == p->active_hamiltonian
                                 ? &p->species[state_index]
                                 : &p->hamiltonian[h_index].model.species[state_index];
        snprintf(out, size, "  \u2514 State %d  ", sp->state_index);
    }
    return ui_text_w(UI_FONT_SANS_SM, out);
}

/* Which sidebar row a rename is currently editing, or -1 while none is. */
static int nav_editing_row(const PredFitState *p) {
    if (p->advanced_edit_param == -6) {
        int at = 0;
        for (int h = 0; h < p->n_hamiltonians; h++) {
            int n_states = h == p->active_hamiltonian ? p->n_species : p->hamiltonian[h].model.n_species;
            if (h == p->advanced_edit_hamiltonian) return at;
            at += 1 + n_states;
        }
        return -1;
    }
    if (p->advanced_edit_param == -7 && p->advanced_edit_species >= 0) {
        int at = 0;
        for (int h = 0; h < p->n_hamiltonians; h++) {
            int n_states = h == p->active_hamiltonian ? p->n_species : p->hamiltonian[h].model.n_species;
            if (h == p->active_hamiltonian) {
                return p->advanced_edit_species < n_states ? at + 1 + p->advanced_edit_species : -1;
            }
            at += 1 + n_states;
        }
    }
    return -1;
}

/* The state a project row refers to: the live one for the active Hamiltonian,
   the stored snapshot for every other, so a checkbox ticked on a Hamiltonian
   the user is not editing lands in that model and not in this one. */
static PickettSpecies *project_species(PredFitState *p, int h_index, int state_index) {
    if (h_index < 0 || h_index >= p->n_hamiltonians || state_index < 0) return NULL;
    if (h_index == p->active_hamiltonian)
        return state_index < p->n_species ? &p->species[state_index] : NULL;
    PredFitSnapshot *m = &p->hamiltonian[h_index].model;
    return state_index < m->n_species ? &m->species[state_index] : NULL;
}

static void render_project_navigator(SDL_Renderer *r, const PredFitState *p, const AdvUI *u) {
    char b[128], prefix[64];
    ui_fill(r, u->project_nav, UI_INPUT);
    ui_frame(r, u->project_nav, UI_LINE);
    ui_text(r, UI_FONT_MONO_SM, "HAMILTONIANS", u->project_nav.x + 9, u->project_nav.y + 8, UI_DIM);
    ui_hline(r, u->project_nav.x + 2, u->project_nav.x + u->project_nav.w - 2,
             u->project_rows.y - 3, UI_LINE);

    int total = project_row_count(p);
    int editing_row = nav_editing_row(p);
    for (int visible = 0; visible < u->project_rows_visible; visible++) {
        int row_index = p->advanced_project_scroll + visible;
        if (row_index >= total) break;
        int h_index = -1, state_index = -1;
        if (!project_row_info(p, row_index, &h_index, &state_index)) continue;
        const HamiltonianModel *model = &p->hamiltonian[h_index];
        const PredFitSnapshot *stored = &model->model;
        int active_h = h_index == p->active_hamiltonian;
        /* One focus drives Rename and the two Move buttons: the row the user
           last clicked, Hamiltonian header or state. */
        int focused = active_h && (state_index < 0 ? p->advanced_nav_state < 0
                                                   : state_index == p->advanced_nav_state);
        int y = u->project_rows.y + visible * 23;
        SDL_Rect row = {u->project_rows.x, y, u->project_rows.w, 21};
        if (focused)             ui_fill(r, row, UI_ACCENT_SOFT);
        else if (visible % 2)    ui_fill(r, row, UI_PANEL);
        int prefix_w = nav_row_prefix(p, h_index, state_index, prefix, sizeof(prefix));
        int name_x = row.x + 7 + prefix_w;
        SDL_Color name_c = state_index < 0
                         ? (active_h ? UI_ACCENT_TEXT : UI_TEXT)
                         : (active_h && state_index == p->active_species ? UI_OK : UI_DIM);
        ui_text(r, UI_FONT_SANS_SM, prefix, row.x + 7, y + 4, state_index < 0 ? name_c : UI_DIM);
        if (row_index == editing_row) {
            adv_edit_text(r, UI_FONT_SANS_SM, p, name_x, y + 4, UI_ACCENT_TEXT);
        } else {
            const char *name = state_index < 0
                             ? model->name
                             : (active_h ? p->species[state_index].name : stored->species[state_index].name);
            snprintf(b, sizeof(b), "%s", name);
            ui_text(r, UI_FONT_SANS_SM, b, name_x, y + 4, name_c);
        }
    }
    int on_state = p->advanced_nav_state >= 0;
    ui_button(r, u->project_up,   on_state ? "State up"   : "H up",   -1, UI_BTN_QUIET, 0, 0, 0, 0);
    ui_button(r, u->project_down, on_state ? "State down" : "H down", -1, UI_BTN_QUIET, 0, 0, 0, 0);
    ui_button(r, u->project_add, "+ Hamiltonian", -1, UI_BTN_PRIMARY, 0, 0, 0, 0);
    ui_button(r, u->project_rename, on_state ? "Rename state" : "Rename", -1, UI_BTN_QUIET,
              p->advanced_edit_param == -6 || p->advanced_edit_param == -7, 0, 0, 0);
    ui_button(r, u->project_duplicate, "Duplicate", -1, UI_BTN_QUIET, 0, 0, 0, 0);
    int active_id = (p->active_hamiltonian >= 0 && p->active_hamiltonian < p->n_hamiltonians)
                  ? p->hamiltonian[p->active_hamiltonian].id : 0;
    int confirming = p->advanced_delete_hamiltonian_id == active_id;
    int can_delete = p->n_hamiltonians > 1;
    ui_button(r, u->project_delete, can_delete ? (confirming ? "Confirm" : "Delete") : "Protected", -1,
              can_delete ? UI_BTN_DANGER : UI_BTN_QUIET, confirming, 0, 0, 0);
}

int predfit_handle_advanced_event(AppState *s, const SDL_Event *e) {
    PredFitState *p = &s->predfit;
    if (!p->advanced_open) return 0;
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == p->advanced_window_id &&
        e->window.event == SDL_WINDOWEVENT_CLOSE) { predfit_close_advanced(s); return 1; }
    if (advanced_edit_event(s, e)) {
        if (p->intensity_dirty) { predfit_publish_shared_state(s); p->intensity_dirty = 0; }
        return 1;
    }
    if (p->advanced_edit_param != -1 && e->type == SDL_MOUSEBUTTONDOWN &&
        e->button.windowID == p->advanced_window_id) {
        /* Clicking inside the field that is being edited aims the caret.  It
           must not commit and restart the edit, which would select the whole
           text again and leave the click with nothing to show for it. */
        AdvUI hit = adv_ui(s, p->advanced_tab);
        if (p->advanced_edit_param == -4 && p->advanced_tab == 0 &&
            point_in_rect(e->button.x, e->button.y, hit.hamiltonian)) {
            adv_place_caret(p, UI_FONT_MONO_SM, hit.hamiltonian.x + 130, e->button.x);
            return 1;
        }
        if ((p->advanced_edit_param == -6 || p->advanced_edit_param == -7) &&
            point_in_rect(e->button.x, e->button.y, hit.project_rows)) {
            int clicked = p->advanced_project_scroll + (e->button.y - hit.project_rows.y) / 23;
            if (clicked == nav_editing_row(p)) {
                int h = -1, state = -1;
                char prefix[64];
                project_row_info(p, clicked, &h, &state);
                int prefix_w = nav_row_prefix(p, h, state, prefix, sizeof(prefix));
                adv_place_caret(p, UI_FONT_SANS_SM, hit.project_rows.x + 7 + prefix_w, e->button.x);
                return 1;
            }
        }
        advanced_commit_edit(p);
        if (p->intensity_dirty) { predfit_publish_shared_state(s); p->intensity_dirty = 0; }
    }

    report_refresh(p);
    AdvUI u = adv_ui(s, p->advanced_tab);

    if (e->type == SDL_MOUSEWHEEL && e->wheel.windowID == p->advanced_window_id) {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        if (point_in_rect(mx, my, u.project_nav)) {
            p->advanced_project_scroll -= e->wheel.y;
            if (p->advanced_project_scroll < 0) p->advanced_project_scroll = 0;
            int limit = adv_scroll_limit(project_row_count(p), u.project_rows_visible);
            if (p->advanced_project_scroll > limit) p->advanced_project_scroll = limit;
            return 1;
        }
        int *scroll = p->advanced_tab == 0 ? &p->advanced_param_scroll :
                      p->advanced_tab == 3 ? &p->advanced_species_scroll :
                      (p->advanced_tab == 4 || p->advanced_tab == 5) ? &p->advanced_project_scroll
                                                                     : &p->advanced_line_scroll;
        int total   = p->advanced_tab == 0 ? p->n_param :
                      p->advanced_tab == 3 ? p->n_species :
                      (p->advanced_tab == 4 || p->advanced_tab == 5) ? project_row_count(p)
                                                                     : s->n_assignments;
        *scroll -= e->wheel.y;
        if (*scroll < 0) *scroll = 0;
        int limit = adv_scroll_limit(total, u.rows_visible);
        if (*scroll > limit) *scroll = limit;
        return 1;
    }

    if (e->type == SDL_MOUSEMOTION && e->motion.windowID == p->advanced_window_id) {
        p->advanced_hover_line = -1;
        if ((p->advanced_tab == 1 || p->advanced_tab == 2) && point_in_rect(e->motion.x, e->motion.y, u.rows)) {
            int row = p->advanced_line_scroll + (e->motion.y - u.rows.y) / u.row_h;
            if (row >= 0 && row < s->n_assignments) p->advanced_hover_line = row;
        }
        return 1;
    }

    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.windowID != p->advanced_window_id) return 0;
    int x = e->button.x, y = e->button.y;

    for (int i = 0; i < 6; i++)
        if (point_in_rect(x, y, u.tab[i])) { p->advanced_tab = i; return 1; }

    /* The sidebar is global navigation, before the tab-specific editor. */
    if (point_in_rect(x, y, u.project_rows)) {
        int h = -1, state = -1;
        int row = p->advanced_project_scroll + (y - u.project_rows.y) / 23;
        if (project_row_info(p, row, &h, &state)) {
            predfit_select_hamiltonian(s, h);
            if (state >= 0) select_species(s, state);
            p->advanced_nav_state = state;
            p->advanced_delete_hamiltonian_id = 0;
            /* A second click on the focused row renames it, like a file name. */
            if (e->button.clicks >= 2) {
                if (state >= 0) advanced_begin_state_name_edit(p, state);
                else advanced_begin_hamiltonian_name_edit(p);
            }
        }
        return 1;
    }
    if (point_in_rect(x, y, u.project_up) || point_in_rect(x, y, u.project_down)) {
        int delta = point_in_rect(x, y, u.project_up) ? -1 : 1;
        if (p->advanced_nav_state >= 0) {
            int from = p->advanced_nav_state;
            if (predfit_move_species(s, from, delta)) {
                p->advanced_nav_state = from + delta;
                snprintf(p->status, sizeof(p->status), "%s moved %s.",
                         p->species[p->advanced_nav_state].name, delta < 0 ? "up" : "down");
            }
        } else if (predfit_move_hamiltonian(s, p->active_hamiltonian, delta)) {
            snprintf(p->status, sizeof(p->status), "%s moved %s.",
                     p->hamiltonian[p->active_hamiltonian].name, delta < 0 ? "up" : "down");
        }
        return 1;
    }
    if (point_in_rect(x, y, u.project_add) || point_in_rect(x, y, u.project_duplicate)) {
        int made = point_in_rect(x, y, u.project_add)
                   ? predfit_add_hamiltonian(s, NULL)
                   : predfit_duplicate_hamiltonian(s, NULL);
        if (made) p->advanced_project_scroll =
            adv_scroll_limit(project_row_count(p), u.project_rows_visible);
        p->advanced_nav_state = -1;
        p->advanced_delete_hamiltonian_id = 0;
        return 1;
    }
    if (point_in_rect(x, y, u.project_rename)) {
        if (p->advanced_nav_state >= 0) advanced_begin_state_name_edit(p, p->advanced_nav_state);
        else advanced_begin_hamiltonian_name_edit(p);
        return 1;
    }
    if (point_in_rect(x, y, u.project_delete)) {
        int active_id = predfit_active_hamiltonian_id(s);
        if (p->advanced_delete_hamiltonian_id == active_id)
            predfit_delete_hamiltonian(s, p->active_hamiltonian);
        else if (p->n_hamiltonians <= 1)
            snprintf(p->status, sizeof(p->status), "A project must retain at least one Hamiltonian.");
        else {
            p->advanced_delete_hamiltonian_id = active_id;
            snprintf(p->status, sizeof(p->status), "Click Delete again to remove %s and its assignments.",
                     p->hamiltonian[p->active_hamiltonian].name);
        }
        return 1;
    }

    if (p->advanced_tab == 5) {
        if (point_in_rect(x, y, u.rows)) {
            int h = -1, state = -1;
            int row = p->advanced_project_scroll + (y - u.rows.y) / u.row_h;
            if (project_row_info(p, row, &h, &state)) {
                if (x < adv_col(u.table, 0.06)) {
                    /* The only checkbox a Hamiltonian row owns is its own. */
                    if (state < 0) {
                        p->hamiltonian[h].simulate_excluded = !p->hamiltonian[h].simulate_excluded;
                        p->session_dirty = 1;
                    } else {
                        PickettSpecies *sp = project_species(p, h, state);
                        if (sp) { sp->predict_enabled = !sp->predict_enabled; p->session_dirty = 1; }
                    }
                } else if (state >= 0 && x >= adv_col(u.table, 0.43) &&
                           x < adv_col(u.table, 0.50)) {
                    /* The colour of this state's own broadened trace. */
                    PickettSpecies *sp = project_species(p, h, state);
                    if (sp) {
                        SDL_Color chosen = predfit_species_color(sp, state);
                        if (settings_pick_color(&chosen)) {
                            chosen.a = chosen.a ? chosen.a : 235;
                            sp->trace_color = chosen;
                            p->session_dirty = 1;
                        }
                    }
                } else if (state >= 0 && x >= adv_col(u.table, 0.50) &&
                           x < adv_col(u.table, 0.88)) {
                    PickettSpecies *sp = project_species(p, h, state);
                    static const double at[3] = {0.50, 0.64, 0.78};
                    if (sp) for (int k = 2; k >= 0; k--) {
                        if (x < adv_col(u.table, at[k])) continue;
                        sp->mu_excluded[k] = !sp->mu_excluded[k];
                        p->session_dirty = 1;
                        break;
                    }
                } else {
                    predfit_select_hamiltonian(s, h);
                    if (state >= 0) select_species(s, state);
                    p->advanced_nav_state = state;
                }
            }
            return 1;
        }
        if (point_in_rect(x, y, u.btn_a)) { predfit_simulate(s); return 1; }
        if (point_in_rect(x, y, u.btn_d)) {
            /* The same three choices as the Broadening panel, cycled here. */
            p->species_trace_mode = (p->species_trace_mode + 1) % 3;
            p->session_dirty = 1;
            snprintf(p->status, sizeof(p->status),
                     p->species_trace_mode == TRACE_SUM
                         ? "Broadening draws the total trace only."
                         : p->species_trace_mode == TRACE_SUM_AND_STATES
                           ? "Broadening draws the total trace and one per state, in its colour."
                           : "Broadening draws one trace per state only, each in its colour.");
            return 1;
        }
        if (point_in_rect(x, y, u.btn_b) || point_in_rect(x, y, u.btn_c)) {
            int include = point_in_rect(x, y, u.btn_b);
            for (int h = 0; h < p->n_hamiltonians; h++) {
                p->hamiltonian[h].simulate_excluded = !include;
                int n_states = h == p->active_hamiltonian ? p->n_species
                                                          : p->hamiltonian[h].model.n_species;
                for (int i = 0; i < n_states; i++) {
                    PickettSpecies *sp = project_species(p, h, i);
                    if (!sp) continue;
                    sp->predict_enabled = include;
                    for (int k = 0; k < 3; k++) sp->mu_excluded[k] = 0;
                }
            }
            p->session_dirty = 1;
            return 1;
        }
        return 1;
    }

    if (p->advanced_tab == 4) {
        if (point_in_rect(x, y, u.rows)) {
            int h = -1, state = -1;
            int row = p->advanced_project_scroll + (y - u.rows.y) / u.row_h;
            if (project_row_info(p, row, &h, &state)) {
                predfit_select_hamiltonian(s, h);
                if (state >= 0) select_species(s, state);
                p->advanced_nav_state = state;
            }
            return 1;
        }
        if (point_in_rect(x, y, u.btn_a)) {
            int was_dirty = p->session_dirty;
            predfit_save_session(s);
            if (p->session_dirty)
                snprintf(p->status, sizeof(p->status), "Cannot write spectravisual.state in .fit.");
            else
                snprintf(p->status, sizeof(p->status), "Session saved%s.",
                         was_dirty ? "" : " (nothing had changed)");
            return 1;
        }
        if (point_in_rect(x, y, u.btn_b)) {
            char state_path[600];
            snprintf(state_path, sizeof(state_path), "%s/spectravisual.state", p->work_dir);
            app_enqueue_pending_load(s, PENDING_LOAD_SESSION, state_path, 0);
            return 1;
        }
        if (point_in_rect(x, y, u.btn_c)) {
            if (predfit_import_load_dir(s) > 0)
                p->advanced_project_scroll =
                    adv_scroll_limit(project_row_count(p), u.project_rows_visible);
            return 1;
        }
        return 1;
    }

    if (p->advanced_tab == 0) {
        if (point_in_rect(x, y, u.hamiltonian)) {
            /* A long option line is edited in place, not replaced: the first
               click already puts the caret under the pointer. */
            advanced_begin_hamiltonian_edit(p);
            adv_place_caret(p, UI_FONT_MONO_SM, u.hamiltonian.x + 130, x);
            return 1;
        }
        if (point_in_rect(x, y, u.rows)) {
            int row = p->advanced_param_scroll + (y - u.rows.y) / u.row_h;
            if (row >= 0 && row < p->n_param) {
                int del_x = u.table.x + u.table.w - 32;
                if (x >= del_x)                             delete_parameter(p, row);
                else if (x < adv_col(u.table, 0.11))        advanced_begin_edit(p, row, 0);
                else if (x >= adv_col(u.table, 0.56) &&
                         x <  adv_col(u.table, 0.80))       advanced_begin_edit(p, row, 1);
                else if (x >= adv_col(u.table, 0.80))       advanced_begin_edit(p, row, 2);
            }
            return 1;
        }
        if (point_in_rect(x, y, u.btn_a)) { add_parameter(p); return 1; }
        if (point_in_rect(x, y, u.btn_b)) { predfit_simulate(s); return 1; }
        SDL_Rect unc = {u.w - ADV_PAD - 300, u.footer.y + 8, 300, 30};
        if (point_in_rect(x, y, unc)) { advanced_begin_line_error(p); return 1; }
        return 1;
    }

    if (p->advanced_tab == 3) {
        {
            /* QROT (cell 2) is intentionally read-only; it is calculated. */
            static const int int_field[10] = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8};
            for (int i = 0; i < 10; i++) if (point_in_rect(x, y, u.int_cell[i])) {
                if (int_field[i] >= 0) advanced_begin_int_edit(p, int_field[i]);
                return 1;
            }
        }
        if (point_in_rect(x, y, u.rows)) {
            int row = p->advanced_species_scroll + (y - u.rows.y) / u.row_h;
            if (row >= 0 && row < p->n_species) {
                int del_x = u.table.x + u.table.w - 32;
                if (x >= del_x && p->n_species > 1) {
                    for (int i = row; i < p->n_species - 1; i++) p->species[i] = p->species[i + 1];
                    p->n_species--;
                    if (p->active_species >= p->n_species) p->active_species = p->n_species - 1;
                    load_active_species(p); sync_basic_from_parameters(p); predfit_publish_shared_state(s);
                    p->session_dirty = 1;
                } else if (x < adv_col(u.table, 0.10)) {
                    select_species(s, row);
                } else if (x < adv_col(u.table, 0.18)) {
                    p->species[row].predict_enabled = !p->species[row].predict_enabled;
                    p->session_dirty = 1;
                } else if (x < adv_col(u.table, 0.40)) {
                    advanced_begin_species_edit(p, row, 0);
                } else if (x >= adv_col(u.table, 0.47) && x < adv_col(u.table, 0.58)) {
                    advanced_begin_species_edit(p, row, 1);
                } else if (x >= adv_col(u.table, 0.58) && x < adv_col(u.table, 0.68)) {
                    advanced_begin_species_edit(p, row, 2);
                } else if (x >= adv_col(u.table, 0.68) && x < adv_col(u.table, 0.78)) {
                    advanced_begin_species_edit(p, row, 3);
                } else if (x >= adv_col(u.table, 0.78) && x < adv_col(u.table, 0.88)) {
                    advanced_begin_species_edit(p, row, 4);
                }
            }
            return 1;
        }
        if (point_in_rect(x, y, u.btn_a)) { add_species(s); return 1; }
        if (point_in_rect(x, y, u.btn_b)) { predfit_simulate(s); return 1; }
        return 1;
    }

    if (point_in_rect(x, y, u.rows)) {
        int row = p->advanced_line_scroll + (y - u.rows.y) / u.row_h;
        if (row >= 0 && row < s->n_assignments) {
            int del_x = u.table.x + u.table.w - 32;
            if (x >= del_x) {
                /* This is deliberately the same deletion as the main
                   Assignments panel: the assignment disappears everywhere. */
                delete_assignment(s, row);
                int limit = adv_scroll_limit(s->n_assignments, u.rows_visible);
                if (p->advanced_line_scroll > limit) p->advanced_line_scroll = limit;
            } else {
                int previous = s->assignments[row].fit_enabled;
                s->assignments[row].fit_enabled = !previous;
                if (!predfit_save_exclusions(s)) s->assignments[row].fit_enabled = previous;
            }
            p->session_dirty = 1;
        }
        return 1;
    }
    if (p->advanced_tab == 2) {
        if (point_in_rect(x, y, u.btn_a)) { predfit_fit(s); return 1; }
        if (point_in_rect(x, y, u.btn_b)) { predfit_undo_last_fit(s); return 1; }
    }
    return 1;
}

void predfit_render_advanced(AppState *s) {
    PredFitState *p = &s->predfit;
    if (!p->advanced_open || !p->advanced_renderer) return;
    SDL_Renderer *r = p->advanced_renderer;
    char b[256];

    /* follow the display the window currently sits on */
    {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(p->advanced_window, &ww, &wh);
        SDL_GetRendererOutputSize(r, &dw, &dh);
        if (ww > 0 && dw > 0) SDL_RenderSetScale(r, (float)dw / (float)ww, (float)dw / (float)ww);
    }

    report_refresh(p);
    AdvUI u = adv_ui(s, p->advanced_tab);

    SDL_SetRenderDrawColor(r, 19, 20, 22, 255);
    SDL_RenderClear(r);
    ui_fill(r, (SDL_Rect){0, 0, u.w, ADV_HEADER_H}, UI_TITLEBAR);
    ui_text(r, UI_FONT_TITLE, "Pred&Fit Advanced", 18, 12, UI_TEXT);
    ui_hline(r, 0, u.w, u.footer.y, UI_LINE);

    const char *tabs[6] = {"Parameters", "Lines", "Fitting", "States", "Hamiltonians", "Simulation"};
    for (int i = 0; i < 6; i++)
        ui_button(r, u.tab[i], tabs[i], -1, UI_BTN_QUIET, p->advanced_tab == i, 0, 0, 0);

    render_project_navigator(r, p, &u);

    if (p->advanced_tab == 5) {
        ui_text(r, UI_FONT_SANS,
                "Simulation — everything checked is calculated together and plotted together",
                u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);
        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "SIM",                adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "HAMILTONIAN / STATE", adv_col(u.table, 0.06), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "TRACE",              adv_col(u.table, 0.43), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu a",               adv_col(u.table, 0.50), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu b",               adv_col(u.table, 0.64), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu c",               adv_col(u.table, 0.78), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "TROT / CONC.",       adv_col(u.table, 0.90), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);
        int total = project_row_count(p);
        for (int visible = 0; visible < u.rows_visible; visible++) {
            int row_index = p->advanced_project_scroll + visible;
            if (row_index >= total) break;
            int h_index = -1, state_index = -1;
            if (!project_row_info(p, row_index, &h_index, &state_index)) continue;
            const HamiltonianModel *model = &p->hamiltonian[h_index];
            const PredFitSnapshot *stored = &model->model;
            int active_h = h_index == p->active_hamiltonian;
            int in_sim = !model->simulate_excluded;
            int y = u.rows.y + visible * u.row_h;
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (active_h && state_index < 0) ui_fill(r, row, UI_ACCENT_SOFT);
            else if (visible % 2)            ui_fill(r, row, UI_PANEL);
            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO_SM)) / 2;
            if (state_index < 0) {
                snprintf(b, sizeof(b), "[%c]", in_sim ? 'x' : ' ');
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.00), ty, in_sim ? UI_OK : UI_FAINT);
                snprintf(b, sizeof(b), "H%d  %s", model->id, model->name);
                ui_text(r, UI_FONT_SANS_SM, b, adv_col(u.table, 0.06), ty,
                        in_sim ? (active_h ? UI_ACCENT_TEXT : UI_TEXT) : UI_FAINT);
                snprintf(b, sizeof(b), "%.6g K", active_h ? p->temp_k : stored->temp_k);
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.90), ty, in_sim ? UI_DIM : UI_FAINT);
            } else {
                const PickettSpecies *sp = active_h ? &p->species[state_index]
                                                    : &stored->species[state_index];
                int on = in_sim && sp->predict_enabled;
                snprintf(b, sizeof(b), "[%c]", sp->predict_enabled ? 'x' : ' ');
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.00), ty,
                        on ? UI_OK : UI_FAINT);
                snprintf(b, sizeof(b), "   \u2514 State %d  %s", sp->state_index, sp->name);
                ui_text(r, UI_FONT_SANS_SM, b, adv_col(u.table, 0.06), ty, on ? UI_TEXT : UI_FAINT);
                /* The colour this state's broadened trace is drawn in; click
                   the swatch to change it. */
                SDL_Rect swatch = {adv_col(u.table, 0.43), y + 3, 26, u.row_h - 8};
                SDL_Color sc = predfit_species_color(sp, state_index);
                if (p->species_trace_mode == TRACE_SUM) { sc.r /= 3; sc.g /= 3; sc.b /= 3; }
                ui_fill(r, swatch, sc);
                ui_frame(r, swatch, UI_LINE);
                static const double at[3] = {0.50, 0.64, 0.78};
                for (int k = 0; k < 3; k++) {
                    int used = on && !sp->mu_excluded[k] && sp->mu[k] != 0.0;
                    snprintf(b, sizeof(b), "[%c] %.4g", sp->mu_excluded[k] ? ' ' : 'x', sp->mu[k]);
                    ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, at[k]), ty,
                            used ? UI_TEXT : UI_FAINT);
                }
                snprintf(b, sizeof(b), "%.4g", sp->concentration);
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.90), ty, on ? UI_DIM : UI_FAINT);
            }
        }
        int checked = 0;
        for (int h = 0; h < p->n_hamiltonians; h++) if (!p->hamiltonian[h].simulate_excluded) checked++;
        snprintf(b, sizeof(b), checked == 1 ? "Simulate (%d H)" : "Simulate (%d H)", checked);
        ui_button(r, u.btn_a, b, -1, checked ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_b, "Check all", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_c, "Uncheck all", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_d,
                  p->species_trace_mode == TRACE_SUM ? "Traces: sum" :
                  p->species_trace_mode == TRACE_SUM_AND_STATES ? "Traces: sum + states"
                                                                : "Traces: states", -1,
                  UI_BTN_QUIET, p->species_trace_mode != TRACE_SUM, 0, 0, 0);
        ui_text(r, UI_FONT_SANS_SM,
                p->species_trace_mode != TRACE_SUM
                    ? "With broadening on, each state is drawn in its own colour (also in the Broadening panel)."
                    : "One SPCAT run per checked Hamiltonian; the catalogues are shown in one plot.",
                u.btn_d.x + u.btn_d.w + 16, u.footer.y + 17, UI_FAINT);

    } else if (p->advanced_tab == 4) {
        ui_text(r, UI_FONT_SANS, "Project — Hamiltonians and their states",
                u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);
        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "HAMILTONIAN / STATE", adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "TROT / K",             adv_col(u.table, 0.62), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "CONCENTRATION",        adv_col(u.table, 0.78), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);
        int total = project_row_count(p);
        for (int visible = 0; visible < u.rows_visible; visible++) {
            int row_index = p->advanced_project_scroll + visible;
            if (row_index >= total) break;
            int h_index = -1, state_index = -1;
            if (!project_row_info(p, row_index, &h_index, &state_index)) continue;
            const HamiltonianModel *model = &p->hamiltonian[h_index];
            const PredFitSnapshot *stored = &model->model;
            const int is_active_h = h_index == p->active_hamiltonian;
            int y = u.rows.y + visible * u.row_h;
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (is_active_h && state_index < 0) ui_fill(r, row, UI_ACCENT_SOFT);
            else if (visible % 2) ui_fill(r, row, UI_PANEL);
            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO_SM)) / 2;
            if (state_index < 0) {
                snprintf(b, sizeof(b), "H%d  %s", model->id, model->name);
                ui_text(r, UI_FONT_SANS_SM, b, adv_col(u.table, 0.00), ty,
                        is_active_h ? UI_ACCENT_TEXT : UI_TEXT);
                double temp = is_active_h ? p->temp_k : stored->temp_k;
                snprintf(b, sizeof(b), "%.6g", temp);
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.62), ty, UI_DIM);
            } else {
                const PickettSpecies *sp = is_active_h ? &p->species[state_index] : &stored->species[state_index];
                snprintf(b, sizeof(b), "   └─ State %d  %s", sp->state_index, sp->name);
                ui_text(r, UI_FONT_SANS_SM, b, adv_col(u.table, 0.00), ty,
                        is_active_h && state_index == p->active_species ? UI_OK : UI_TEXT);
                snprintf(b, sizeof(b), "%.6g", sp->concentration);
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.78), ty, UI_DIM);
            }
        }
        ui_button(r, u.btn_a, p->session_dirty ? "Save session *" : "Save session", -1,
                  p->session_dirty ? UI_BTN_PRIMARY : UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_b, "Restore session", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_c, "Import .fit/load", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_text(r, UI_FONT_SANS_SM,
                p->session_dirty ? "Unsaved changes: nothing is written to .fit until Save session."
                                 : "Saving and restoring the session are both explicit; states stay grouped under their Hamiltonian.",
                u.btn_c.x + u.btn_c.w + 16, u.footer.y + 17,
                p->session_dirty ? UI_ACCENT_TEXT : UI_FAINT);

    } else if (p->advanced_tab == 0) {
        ui_text(r, UI_FONT_SANS, "Shared Hamiltonian and Pickett parameters",
                u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        ui_fill(r, u.hamiltonian, UI_INPUT);
        ui_frame(r, u.hamiltonian, p->advanced_edit_param == -4 ? UI_ACCENT : UI_LINE);
        ui_text(r, UI_FONT_MONO_SM, "PAR option line", u.hamiltonian.x + 10, u.hamiltonian.y + 8, UI_DIM);
        adv_cell_text(r, UI_FONT_MONO_SM, p, p->advanced_edit_param == -4,
                      p->hamiltonian_line, u.hamiltonian.x + 130, u.hamiltonian.y + 8,
                      p->advanced_edit_param == -4 ? UI_ACCENT_TEXT : UI_TEXT);

        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);

        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "ID",        adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "WATSON-A",  adv_col(u.table, 0.11), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "WATSON-S",  adv_col(u.table, 0.28), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "OTHER",     adv_col(u.table, 0.41), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "VALUE",     adv_col(u.table, 0.56), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "FIT ERROR", adv_col(u.table, 0.80), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);

        for (int i = 0; i < u.rows_visible && p->advanced_param_scroll + i < p->n_param; i++) {
            int actual = p->advanced_param_scroll + i;
            int y = u.rows.y + i * u.row_h;
            PickettParameter *x = &p->param[actual];
            const ParameterName *name = parameter_name(x->id);
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (actual == p->advanced_edit_param) ui_fill(r, row, UI_ACCENT_SOFT);
            else if (i % 2)                       ui_fill(r, row, UI_PANEL);

            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO)) / 2;
            int editing = actual == p->advanced_edit_param;
            SDL_Rect id_cell = {adv_col(u.table, 0.00) - 4, y + 2,
                                adv_col(u.table, 0.11) - adv_col(u.table, 0.00) - 6, u.row_h - 6};
            SDL_Rect value_cell = {adv_col(u.table, 0.56) - 4, y + 2,
                                   adv_col(u.table, 0.80) - adv_col(u.table, 0.56) - 6, u.row_h - 6};
            SDL_Rect error_cell = {adv_col(u.table, 0.80) - 4, y + 2,
                                   u.table.x + u.table.w - 38 - adv_col(u.table, 0.80), u.row_h - 6};
            if (editing && p->advanced_edit_col == 0) { ui_fill(r, id_cell, UI_INPUT); ui_frame(r, id_cell, UI_ACCENT); }
            if (editing && p->advanced_edit_col == 1) { ui_fill(r, value_cell, UI_INPUT); ui_frame(r, value_cell, UI_ACCENT); }
            if (editing && p->advanced_edit_col == 2) { ui_fill(r, error_cell, UI_INPUT); ui_frame(r, error_cell, UI_ACCENT); }

            snprintf(b, sizeof(b), "%d", x->id);
            adv_cell_text(r, UI_FONT_MONO, p, editing && p->advanced_edit_col == 0, b,
                          adv_col(u.table, 0.00), ty, editing && p->advanced_edit_col == 0 ? UI_ACCENT_TEXT : UI_TEXT);
            ui_text(r, UI_FONT_SANS_SM, name && name->watson_a ? name->watson_a : "—", adv_col(u.table, 0.11), ty, UI_DIM);
            ui_text(r, UI_FONT_SANS_SM, name && name->watson_s ? name->watson_s : "—", adv_col(u.table, 0.28), ty, UI_ACCENT_TEXT);
            ui_text(r, UI_FONT_SANS_SM, name && name->other ? name->other : (name ? "—" : x->label), adv_col(u.table, 0.41), ty, UI_DIM);
            snprintf(b, sizeof(b), "%.11E", x->value);
            adv_cell_text(r, UI_FONT_MONO, p, editing && p->advanced_edit_col == 1, b,
                          adv_col(u.table, 0.56), ty, editing && p->advanced_edit_col == 1 ? UI_ACCENT_TEXT : UI_TEXT);
            snprintf(b, sizeof(b), "%.5E", x->error);
            adv_cell_text(r, UI_FONT_MONO, p, editing && p->advanced_edit_col == 2, b,
                          adv_col(u.table, 0.80), ty, editing && p->advanced_edit_col == 2 ? UI_ACCENT_TEXT : UI_TEXT);

            SDL_Rect del = {u.table.x + u.table.w - 32, y + (u.row_h - 2 - 22) / 2, 22, 22};
            ui_draw_icon(r, UI_ICON_CLOSE, (SDL_Rect){del.x + 5, del.y + 5, 12, 12}, UI_DANGER_TEXT);
        }

        ui_button(r, u.btn_a, "+ parameter", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_b, "Simulate", -1, UI_BTN_PRIMARY, 0, 0, 0, 0);
        snprintf(b, sizeof(b), ".lin uncertainty: %.8g MHz", p->line_error_mhz);
        ui_text(r, UI_FONT_SANS_SM, b, u.w - ADV_PAD - 290, u.footer.y + 17,
                p->advanced_edit_param == -2 ? UI_ACCENT_TEXT : UI_DIM);
        ui_text(r, UI_FONT_SANS_SM,
                p->advanced_edit_param >= 0 ? "Enter applies in this field · Esc cancels · × removes a row"
                                             : "QROT is calculated from A/B/C, TEMP and sigma · MAXV=-1 follows states",
                u.btn_b.x + u.btn_b.w + 16, u.footer.y + 17,
                p->advanced_edit_param >= 0 ? UI_ACCENT_TEXT : UI_FAINT);

    } else if (p->advanced_tab == 3) {
        ui_text(r, UI_FONT_SANS, "States — one shared .par/.var/.int; each row owns dipoles and concentration",
                u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        /* The actual .int second record, placed with the species because
           Tcat is the common catalogue reference and the rows below are Tred. */
        static const char *int_label[10] = {
            "FLAGS", "TAG", "QROT (auto)", "FBGN", "FEND",
            "STR0 = STR1", "FQLIM / GHz", "TROT / K", "MAXV", "sigma"
        };
        static const int int_field[10] = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8};
        PickettIntSettings *ix = &p->int_settings;
        for (int i = 0; i < 10; i++) {
            SDL_Rect cell = u.int_cell[i];
            int editing = p->advanced_edit_param == -5 && p->advanced_edit_col == int_field[i];
            ui_fill(r, cell, i == 2 ? UI_PANEL : UI_INPUT);
            ui_frame(r, cell, editing ? UI_ACCENT : UI_LINE);
            ui_text(r, UI_FONT_SANS_SM, int_label[i], cell.x + 6, cell.y + 3, i == 2 ? UI_FAINT : UI_DIM);
            if (i == 0) snprintf(b, sizeof(b), "%d", ix->flags);
            else if (i == 1) snprintf(b, sizeof(b), "%d", ix->tag);
            else if (i == 2) snprintf(b, sizeof(b), "%.9g", qrot_at(p, int_temp_at(p, p->temp_k)));
            else if (i == 3) snprintf(b, sizeof(b), "%d", ix->fbegin);
            else if (i == 4) snprintf(b, sizeof(b), "%d", ix->fend);
            else if (i == 5) snprintf(b, sizeof(b), "%.9g", ix->intensity_cutoff);
            else if (i == 6) snprintf(b, sizeof(b), "%.9g", int_fqlim_at(p));
            else if (i == 7) snprintf(b, sizeof(b), "%.9g", int_temp_at(p, p->temp_k));
            else if (i == 8) snprintf(b, sizeof(b), "%d", int_maxv_at(p));
            else snprintf(b, sizeof(b), "%.9g", ix->sigma);
            adv_cell_text(r, UI_FONT_MONO_SM, p, editing, b,
                          cell.x + 6, cell.y + 14, editing ? UI_ACCENT_TEXT : UI_TEXT);
        }
        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);
        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "USE",           adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "PRED",          adv_col(u.table, 0.10), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "NAME",          adv_col(u.table, 0.18), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "v",             adv_col(u.table, 0.40), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu a",          adv_col(u.table, 0.47), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu b",          adv_col(u.table, 0.58), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "mu c",          adv_col(u.table, 0.68), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "CONC.",         adv_col(u.table, 0.78), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);
        for (int i = 0; i < u.rows_visible && p->advanced_species_scroll + i < p->n_species; i++) {
            int actual = p->advanced_species_scroll + i;
            PickettSpecies *sp = &p->species[actual];
            int y = u.rows.y + i * u.row_h;
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (actual == p->active_species) ui_fill(r, row, UI_ACCENT_SOFT);
            else if (i % 2) ui_fill(r, row, UI_PANEL);
            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO_SM)) / 2;
            int editing = p->advanced_edit_param == -3 && p->advanced_edit_species == actual;
            snprintf(b, sizeof(b), "[%c]", actual == p->active_species ? 'x' : ' ');
            ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.00), ty, actual == p->active_species ? UI_OK : UI_FAINT);
            snprintf(b, sizeof(b), "[%c]", sp->predict_enabled ? 'x' : ' ');
            ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.10), ty, sp->predict_enabled ? UI_OK : UI_FAINT);
            adv_cell_text(r, UI_FONT_SANS_SM, p, editing && p->advanced_edit_col == 0, sp->name,
                          adv_col(u.table, 0.18), ty, editing && p->advanced_edit_col == 0 ? UI_ACCENT_TEXT : sp->predict_enabled ? UI_TEXT : UI_FAINT);
            snprintf(b, sizeof(b), "%d", sp->state_index);
            ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.40), ty, UI_DIM);
            for (int k = 0; k < 3; k++) {
                snprintf(b, sizeof(b), "%.5g", sp->mu[k]);
                double at[] = {0.47, 0.58, 0.68};
                adv_cell_text(r, UI_FONT_MONO_SM, p, editing && p->advanced_edit_col == k + 1, b,
                              adv_col(u.table, at[k]), ty, editing && p->advanced_edit_col == k + 1 ? UI_ACCENT_TEXT : UI_TEXT);
            }
            snprintf(b, sizeof(b), "%.5g", sp->concentration);
            adv_cell_text(r, UI_FONT_MONO_SM, p, editing && p->advanced_edit_col == 4, b,
                          adv_col(u.table, 0.78), ty, editing && p->advanced_edit_col == 4 ? UI_ACCENT_TEXT : UI_TEXT);
            SDL_Rect del = {u.table.x + u.table.w - 28, y + (u.row_h - 2 - 18) / 2, 18, 18};
            if (p->n_species > 1) ui_draw_icon(r, UI_ICON_CLOSE, del, UI_DANGER_TEXT);
        }
        ui_button(r, u.btn_a, "+ species", -1, UI_BTN_QUIET, 0, 0, 0, 0);
        ui_button(r, u.btn_b, "Simulate", -1, UI_BTN_PRIMARY, 0, 0, 0, 0);
        ui_text(r, UI_FONT_SANS_SM, "Trot and cut are Hamiltonian-wide · concentration rescales each state after SPCAT",
                u.btn_b.x + u.btn_b.w + 16, u.footer.y + 17, UI_FAINT);

    } else if (p->advanced_tab == 1) {
        ui_text(r, UI_FONT_SANS, "Assigned transitions — click a row to include/exclude from Fit; × deletes it everywhere",
                u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);

        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "FIT",             adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "OBSERVED / MHz",  adv_col(u.table, 0.26), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "PREDICTED / MHz", adv_col(u.table, 0.50), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "UPPER  —  LOWER", adv_col(u.table, 0.92), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "DEL",             adv_col(u.table, 0.94), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);

        for (int i = 0; i < u.rows_visible && p->advanced_line_scroll + i < s->n_assignments; i++) {
            int actual = p->advanced_line_scroll + i;
            int y = u.rows.y + i * u.row_h;
            Assignment *a = &s->assignments[actual];
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (actual == p->advanced_hover_line) ui_fill(r, row, UI_RAISED);
            else if (i % 2)                       ui_fill(r, row, UI_PANEL);
            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO)) / 2;

            snprintf(b, sizeof(b), "[%c]", a->fit_enabled ? 'x' : ' ');
            ui_text(r, UI_FONT_MONO, b, adv_col(u.table, 0.00), ty, a->fit_enabled ? UI_OK : UI_FAINT);
            snprintf(b, sizeof(b), "%.6f", a->exp_freq);
            ui_text_right(r, UI_FONT_MONO, b, adv_col(u.table, 0.26), ty, a->fit_enabled ? UI_TEXT : UI_FAINT);
            snprintf(b, sizeof(b), "%.6f", a->pred.freq_mhz);
            ui_text_right(r, UI_FONT_MONO, b, adv_col(u.table, 0.50), ty, UI_DIM);
            format_assignment_qn(b, sizeof(b), &a->pred);
            ui_text_right(r, UI_FONT_MONO, b, adv_col(u.table, 0.92), ty, a->fit_enabled ? UI_TEXT : UI_FAINT);
            SDL_Rect del = {u.table.x + u.table.w - 30, y + (u.row_h - 2 - 18) / 2, 18, 18};
            ui_draw_icon(r, UI_ICON_CLOSE, del, UI_DANGER_TEXT);
        }
        ui_text(r, UI_FONT_SANS_SM, "Excluded rows stay in assignments and .fit/exclusions.txt; SPFIT never receives them.",
                u.caption.x, u.footer.y + 17, UI_FAINT);

    } else {
        ui_text(r, UI_FONT_SANS, "SPFIT output", u.caption.x, u.caption.y, UI_ACCENT_TEXT);
        ui_text(r, UI_FONT_MONO_SM,
                p->status[0] ? p->status : "Run SPFIT after selecting the assignments to fit.",
                u.caption.x + 110, u.caption.y + 2, UI_TEXT);

        /* Every fitted parameter, in the form SPFIT reports it:
              1         10000       A  /       1151.36042( 32)   -0.00000     */
        ui_fill(r, u.params, UI_INPUT);
        ui_frame(r, u.params, UI_LINE);
        ui_text(r, UI_FONT_MONO_SM, "FITTED PARAMETERS      N / ID / LABEL / VALUE(EST. ERROR) / CHANGE THIS ITERATION",
                u.params.x + 10, u.params.y + 8, UI_DIM);
        for (int i = 0; i < u.param_rows_visible; i++) {
            int y = u.params.y + 26 + i * 18;
            if (i < g_report.n_param_lines)
                ui_text(r, UI_FONT_MONO_SM, g_report.param_line[i], u.params.x + 10, y, UI_TEXT);
            else if (g_report.n_param_lines == 0 && i == 0)
                ui_text(r, UI_FONT_MONO_SM, "no SPFIT run in .fit yet", u.params.x + 10, y, UI_FAINT);
        }
        if (g_report.n_param_lines > u.param_rows_visible) {
            snprintf(b, sizeof(b), "+%d more — enlarge the window",
                     g_report.n_param_lines - u.param_rows_visible);
            ui_text(r, UI_FONT_MONO_SM, b, u.params.x + u.params.w - 240, u.params.y + 8, UI_FAINT);
        }

        ui_fill(r, u.table, UI_INPUT);
        ui_frame(r, u.table, UI_LINE);
        int hy = u.table.y + 8;
        ui_text(r, UI_FONT_MONO_SM, "FIT",             adv_col(u.table, 0.00), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "QNS",        adv_col(u.table, 0.28), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "OBSERVED",   adv_col(u.table, 0.46), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "CALCULATED", adv_col(u.table, 0.63), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "OBS-CALC",   adv_col(u.table, 0.79), hy, UI_DIM);
        ui_text_right(r, UI_FONT_MONO_SM, "/UNC",       adv_col(u.table, 0.93), hy, UI_DIM);
        ui_text(r, UI_FONT_MONO_SM, "DEL",             adv_col(u.table, 0.94), hy, UI_DIM);
        ui_hline(r, u.table.x + 2, u.table.x + u.table.w - 2, u.rows.y - 3, UI_LINE);

        for (int i = 0; i < u.rows_visible && p->advanced_line_scroll + i < s->n_assignments; i++) {
            int actual = p->advanced_line_scroll + i;
            int y = u.rows.y + i * u.row_h;
            Assignment *a = &s->assignments[actual];
            FitObservation o;
            FitRowState state = fitting_row(s, a, &o);
            SDL_Rect row = {u.rows.x, y, u.rows.w, u.row_h - 2};
            if (actual == p->advanced_hover_line) ui_fill(r, row, UI_RAISED);
            else if (i % 2)                       ui_fill(r, row, UI_PANEL);

            /* The colour is the whole readout: how many sigma this line sits at. */
            SDL_Color c = UI_FAINT;
            double z = 0.0;
            if (state == FIT_ROW_USED) {
                z = o.diff / o.unc;
                c = residual_color(z);
                ui_fill(r, (SDL_Rect){u.rows.x, y, 3, u.row_h - 2}, c);
            } else if (state == FIT_ROW_REJECTED) {
                c = UI_DANGER_TEXT;
            } else if (state == FIT_ROW_NOT_READ || state == FIT_ROW_OTHER_MODEL) {
                c = UI_DIM;
            } else if (a->fit_enabled) {
                c = UI_TEXT;
            }

            int ty = y + (u.row_h - 2 - ui_text_h(UI_FONT_MONO_SM)) / 2;
            char qns[160];
            format_assignment_qn(qns, sizeof(qns), &a->pred);
            snprintf(b, sizeof(b), "[%c]", a->fit_enabled ? 'x' : ' ');
            ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.00) + 6, ty, c);
            ui_text_right(r, UI_FONT_MONO_SM, qns, adv_col(u.table, 0.28), ty, c);
            /* Always take the experimental frequency from the live assignment
               list.  This makes the Lines and Fitting pages two views of the
               exact same data, rather than a live list next to a .fit copy. */
            snprintf(b, sizeof(b), "%.5f", a->exp_freq);
            ui_text_right(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.46), ty, c);
            if (state == FIT_ROW_USED || state == FIT_ROW_NOT_USED) {
                snprintf(b, sizeof(b), "%.5f", o.calc);
                ui_text_right(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.63), ty, c);
                if (state == FIT_ROW_USED) {
                    snprintf(b, sizeof(b), "%+.5f", o.diff);
                    ui_text_right(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.79), ty, c);
                    snprintf(b, sizeof(b), "%+.2f", z);
                    ui_text_right(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.93), ty, c);
                } else {
                    ui_text(r, UI_FONT_MONO_SM, "not used in the fit", adv_col(u.table, 0.64), ty, UI_FAINT);
                }
            } else if (state == FIT_ROW_EXCLUDED) {
                ui_text(r, UI_FONT_MONO_SM, "excluded", adv_col(u.table, 0.47), ty, UI_FAINT);
            } else if (state == FIT_ROW_REJECTED) {
                ui_text(r, UI_FONT_MONO_SM, "rejected by SPFIT", adv_col(u.table, 0.47), ty, UI_DANGER_TEXT);
            } else if (state == FIT_ROW_STALE) {
                ui_text(r, UI_FONT_MONO_SM, "reassigned — run Fit", adv_col(u.table, 0.47), ty, UI_ACCENT_TEXT);
            } else if (state == FIT_ROW_NOT_READ) {
                ui_text(r, UI_FONT_MONO_SM, "not read by SPFIT", adv_col(u.table, 0.47), ty, UI_DIM);
            } else if (state == FIT_ROW_OTHER_MODEL) {
                int owner = hamiltonian_index_by_id(p, a->hamiltonian_id);
                snprintf(b, sizeof(b), "belongs to %s",
                         owner >= 0 ? p->hamiltonian[owner].name : "another Hamiltonian");
                ui_text(r, UI_FONT_MONO_SM, b, adv_col(u.table, 0.47), ty, UI_FAINT);
            } else {
                ui_text(r, UI_FONT_MONO_SM, "not fitted yet", adv_col(u.table, 0.47), ty, UI_FAINT);
            }
            SDL_Rect del = {u.table.x + u.table.w - 30, y + (u.row_h - 2 - 18) / 2, 18, 18};
            ui_draw_icon(r, UI_ICON_CLOSE, del, UI_DANGER_TEXT);
        }

        ui_button(r, u.btn_a, "Fit", -1, UI_BTN_PRIMARY, 0, 0, 0, 0);
        ui_button(r, u.btn_b, "Undo fit", -1, UI_BTN_QUIET, 0, 0, 0, 0);

        /* legend of the residual scale */
        int lx = u.btn_b.x + u.btn_b.w + 24, ly = u.footer.y + 17;
        const char *band[4] = {"0-1.5", "1.5-2.5", "2.5-4", ">4 sigma"};
        double mid[4] = {0.5, 2.0, 3.2, 5.0};
        for (int i = 0; i < 4; i++) {
            ui_fill(r, (SDL_Rect){lx, ly + 3, 8, 8}, residual_color(mid[i]));
            ui_text(r, UI_FONT_SANS_SM, band[i], lx + 13, ly, UI_FAINT);
            lx += 13 + ui_text_w(UI_FONT_SANS_SM, band[i]) + 14;
        }
    }
    SDL_RenderPresent(r);
}
