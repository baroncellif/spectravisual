#include "predfit.h"
#include "layout.h"
#include "loader.h"
#include "ui_theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define FIT_ROOT ".fit"
#define SPCAT_BIN "/Users/filippobaroncelli/Desktop/Programmi_SP/calpgm/spcat"
#define SPFIT_BIN "/Users/filippobaroncelli/Desktop/Programmi_SP/calpgm/spfit"

static double qrot(const PredFitState *p) {
    if (!(p->a > 0 && p->b > 0 && p->c > 0 && p->temp_k > 0)) return 0.0;
    return 5.3311e6 * sqrt((p->temp_k * p->temp_k * p->temp_k) / (p->a * p->b * p->c));
}

static int prepare_fit_dir(PredFitState *p) {
    if (mkdir(FIT_ROOT, 0700) != 0 && errno != EEXIST) {
        snprintf(p->status, sizeof(p->status), "Cannot create %s.", FIT_ROOT);
        return 0;
    }
    snprintf(p->work_dir,sizeof(p->work_dir),"%s",FIT_ROOT);
    return 1;
}

static void work_file(const PredFitState *p, const char *name, char *out, size_t size) {
    snprintf(out,size,"%s/%s",p->work_dir,name);
}

/* SPFIT overwrites the parameter uncertainties in .var/.par with estimated
   errors.  Our table's uncertainty is instead the user's fit-control value:
   1.0 by default, or 0/fixed/a custom prior set in Advanced. */
static void save_manual_parameter_errors(const PredFitState *p) {
    char path[600]; work_file(p,"spectravisual.state",path,sizeof(path));
    FILE *fp=fopen(path,"w"); if (!fp) return;
    fputs("# Pickett parameter ID and user-selected fit uncertainty\n",fp);
    for (int i=0;i<p->n_param;i++) fprintf(fp,"%d %.17g\n",p->param[i].id,p->param[i].error);
    fclose(fp);
}

static void load_manual_parameter_errors(PredFitState *p) {
    char path[600]; work_file(p,"spectravisual.state",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return;
    int id=0; double error=0; char line[160];
    while (fgets(line,sizeof(line),fp)) {
        if (line[0]=='#' || sscanf(line,"%d %lf",&id,&error)!=2) continue;
        for (int i=0;i<p->n_param;i++) if (p->param[i].id==id) { p->param[i].error=error; break; }
    }
    fclose(fp);
}

/* Trot and the requested (red) dipoles describe the active molecular
   prediction, so they are deliberately shared with Intensity analysis.  CAT
   provenance (Tcat/mu_cat) is separate and only filled automatically for a
   catalogue which SPCAT has just generated for us. */
void predfit_publish_shared_state(AppState *s) {
    PredFitState *p=&s->predfit;
    s->rot_temp_k=p->temp_k;
    memcpy(s->dipole_red,p->mu,sizeof(p->mu));
    if (s->pred_lines && s->n_pred > 0)
        rescale_predicted_intensities(s->pred_lines,s->n_pred,s->cat_temp_k,s->rot_temp_k,
                                      s->dipole_cat,s->dipole_red,&s->pred_global_max);
}

void predfit_adopt_shared_state(AppState *s) {
    PredFitState *p=&s->predfit;
    if (s->rot_temp_k > 0.0) p->temp_k=s->rot_temp_k;
    for (int c=0;c<3;c++) if (s->dipole_red[c] != 0.0) p->mu[c]=s->dipole_red[c];
}

void predfit_adopt_generated_catalog(AppState *s) {
    PredFitState *p=&s->predfit;
    if (!p->generated_catalog_pending) return;
    p->generated_catalog_pending=0;
    s->cat_temp_k=p->temp_k;
    memcpy(s->dipole_cat,p->mu,sizeof(p->mu));
    predfit_publish_shared_state(s);
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
    snap->a=p->a; snap->b=p->b; snap->c=p->c;
    memcpy(snap->mu, p->mu, sizeof(snap->mu));
    snap->temp_k=p->temp_k; snap->fmin_ghz=p->fmin_ghz; snap->fmax_ghz=p->fmax_ghz;
    snap->line_error_mhz=p->line_error_mhz;
    snap->n_param=p->n_param;
    memcpy(snap->param, p->param, sizeof(snap->param));
    snap->n_assignments=s->n_assignments;
    for (int i=0; i<s->n_assignments; i++)
        snap->assignment_fit_enabled[i]=(unsigned char)(s->assignments[i].fit_enabled != 0);
    return 1;
}

static void restore_fit_snapshot(AppState *s, const PredFitSnapshot *snap) {
    PredFitState *p = &s->predfit;
    p->a=snap->a; p->b=snap->b; p->c=snap->c;
    memcpy(p->mu, snap->mu, sizeof(p->mu));
    p->temp_k=snap->temp_k; p->fmin_ghz=snap->fmin_ghz; p->fmax_ghz=snap->fmax_ghz;
    p->line_error_mhz=snap->line_error_mhz;
    p->n_param=snap->n_param;
    memcpy(p->param, snap->param, sizeof(p->param));
    int n=snap->n_assignments < s->n_assignments ? snap->n_assignments : s->n_assignments;
    for (int i=0; i<n; i++) s->assignments[i].fit_enabled=snap->assignment_fit_enabled[i];
}

void predfit_init(AppState *s) {
    PredFitState *p = &s->predfit;
    p->a = 10000.0; p->b = 1000.0; p->c = 900.0;
    p->mu[0] = p->mu[1] = p->mu[2] = 1.0;
    p->temp_k = 5.0; p->fmin_ghz = 0.0; p->fmax_ghz = 20.0;
    p->line_error_mhz = 0.01;
    p->n_param = 3;
    /* A non-zero a-priori error makes the three supplied constants float in
       SPFIT.  The Advanced table can set an error to zero to keep one fixed. */
    p->param[0] = (PickettParameter){10000, p->a, 1.0, "A"};
    p->param[1] = (PickettParameter){20000, p->b, 1.0, "B"};
    p->param[2] = (PickettParameter){30000, p->c, 1.0, "C"};
    p->advanced_edit_param = -1;
    p->advanced_hover_line = -1;
}

static void sync_basic_parameters(PredFitState *p) {
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id == 10000) p->param[i].value = p->a;
        if (p->param[i].id == 20000) p->param[i].value = p->b;
        if (p->param[i].id == 30000) p->param[i].value = p->c;
    }
    save_manual_parameter_errors(p);
}

static void sync_basic_from_parameters(PredFitState *p) {
    for (int i = 0; i < p->n_param; i++) {
        if (p->param[i].id == 10000) p->a = p->param[i].value;
        if (p->param[i].id == 20000) p->b = p->param[i].value;
        if (p->param[i].id == 30000) p->c = p->param[i].value;
    }
}

typedef struct { int id; const char *watson_a, *watson_s, *other; } ParameterName;

/* Pickett's identifiers are the actual interface; a typed ID therefore gets
   a physical label immediately, without relying on a separate lookup file. */
static const ParameterName PARAMETER_NAMES[] = {
    {10000,"A","A",NULL},{20000,"B","B",NULL},{30000,"C","C",NULL},
    {200,"DeltaJ","DJ",NULL},{1100,"DeltaJK","DJK",NULL},{2000,"DeltaK","DK",NULL},
    {40100,"deltaJ","d1",NULL},{41000,"deltaK","d2",NULL},
    {300,"PhiJ","HJ",NULL},{1200,"PhiJK","HJK",NULL},{2100,"PhiKJ","HKJ",NULL},{3000,"PhiK","HK",NULL},
    {40200,"phiJ","h1",NULL},{41100,"phiJK","h2",NULL},{42000,"phiK","h3",NULL},
    {400,"LJ","LJ",NULL},{1300,"LJJK","LJJK",NULL},{2200,"LJK","LJK",NULL},{3100,"LKKJ","LKKJ",NULL},{4000,"LK","LK",NULL},
    {40300,"lJ","l1",NULL},{41200,"lJK","l2",NULL},{42100,"lKJ","l3",NULL},{43000,"lK","l4",NULL},
    {500,"PJ","PJ",NULL},{1400,"PJJK","PJJK",NULL},{2300,"PJK","PJK",NULL},{3200,"PKJ","PKJ",NULL},{4100,"PKKJ","PKKJ",NULL},{5000,"PK","PK",NULL},
    {40400,"pJ","p1",NULL},{41300,"pJJK","p2",NULL},{42200,"pJK","p3",NULL},{43100,"pKKJ","p4",NULL},{44000,"pK","p5",NULL},
    {110010000,NULL,NULL,"chi.aa*3/2"},{110020000,NULL,NULL,"chi.bb*3/2"},{110030000,NULL,NULL,"chi.cc*3/2"},{110040000,NULL,NULL,"chi(b-c)/4"},
    {110610000,NULL,NULL,"chi.ab"},{110210000,NULL,NULL,"chi.bc"},{110410000,NULL,NULL,"chi.ac"},{110011000,NULL,NULL,"chi.k*3/2"},{110010100,NULL,NULL,"chi.J*3/2"},
    {120010000,NULL,NULL,"D.aa*3/2"},{120040000,NULL,NULL,"D(b-c)/4"},{120610000,NULL,NULL,"Dab"},
    {10010000,NULL,NULL,"M.aa"},{10020000,NULL,NULL,"M.bb"},{10030000,NULL,NULL,"M.cc"},
    {11,NULL,NULL,"E1"},{200001,NULL,NULL,"Ga"},{210001,NULL,NULL,"Fbc"},{400001,NULL,NULL,"Gb"},{410001,NULL,NULL,"Fca"},{600001,NULL,NULL,"Gc"},{610001,NULL,NULL,"Fab"},
    {1,NULL,NULL,"Fermi F"},{200000,NULL,NULL,"D_a"},{200100,NULL,NULL,"D_aJ"},{201000,NULL,NULL,"D_aK"},{400000,NULL,NULL,"D_b"},{600000,NULL,NULL,"D_c"}
};

static const ParameterName *parameter_name(int id) {
    for (size_t i=0;i<sizeof(PARAMETER_NAMES)/sizeof(PARAMETER_NAMES[0]);i++)
        if (PARAMETER_NAMES[i].id==id) return &PARAMETER_NAMES[i];
    return NULL;
}

static void parameter_label(PickettParameter *x) {
    const ParameterName *name=parameter_name(x->id);
    const char *label=name ? (name->watson_s ? name->watson_s : name->other) : NULL;
    if (label) snprintf(x->label,sizeof(x->label),"%s",label);
    else if (!x->label[0]) snprintf(x->label,sizeof(x->label),"Pickett parameter");
}

static int write_inputs(AppState *s, int for_fit) {
    PredFitState *p = &s->predfit;
    if (!prepare_fit_dir(p)) return 0;
    sync_basic_parameters(p);
    char var_path[600], int_path[600], par_path[600], lin_path[600];
    work_file(p,"model.var",var_path,sizeof(var_path)); work_file(p,"model.int",int_path,sizeof(int_path));
    work_file(p,"model.par",par_path,sizeof(par_path)); work_file(p,"model.lin",lin_path,sizeof(lin_path));
    FILE *var = fopen(var_path, "w");
    FILE *in = fopen(int_path, "w");
    FILE *par = for_fit ? fopen(par_path, "w") : NULL;
    FILE *lin = for_fit ? fopen(lin_path, "w") : NULL;
    if (!var || !in || (for_fit && (!par || !lin))) {
        if (var) fclose(var); if (in) fclose(in); if (par) fclose(par); if (lin) fclose(lin);
        snprintf(p->status, sizeof(p->status), "Cannot write Pickett working files.");
        return 0;
    }
    /* NLINE is the number of physical rows in the .lin.  The 90000+ sentinel
       excludes individual observations, but they remain rows in that file. */
    int nline = for_fit ? s->n_assignments : 0;
    fprintf(var, "SpectraVisual Pred&Fit quick model\n%4d%5d%5d%5d %15.4E %15.4E %15.4E %.10f\ns 1 1\n",
            p->n_param, nline, 0, 0, 0.0, 1e6, 1.0, 1.0);
    if (par) fprintf(par, "SpectraVisual Pred&Fit quick model\n%4d%5d%5d%5d %15.4E %15.4E %15.4E %.10f\ns 1 1\n",
                     p->n_param, nline, 50, 0, 0.0, 1e6, 1.0, 1.0);
    for (int i = 0; i < p->n_param; i++) {
        PickettParameter *x = &p->param[i];
        fprintf(var, "%12d % .15E % .8E /%s/\n", x->id, x->value, x->error, x->label);
        if (par) fprintf(par, "%12d % .15E % .8E /%s/\n", x->id, x->value, x->error, x->label);
    }
    fprintf(in, "SpectraVisual Pred&Fit quick model\n0 1 %.12g %.8g %.8g -8 -8 %.8g %.8g\n",
            qrot(p), p->fmin_ghz, p->fmax_ghz, p->fmax_ghz, p->temp_k);
    fprintf(in, "001 %.10g /a dipole moment\n002 %.10g /b dipole moment\n003 %.10g /c dipole moment\n", p->mu[0], p->mu[1], p->mu[2]);
    if (lin) for (int i = 0; i < s->n_assignments; i++) {
        Assignment *a = &s->assignments[i]; PredLine *q = &a->pred;
        double freq = a->fit_enabled ? a->exp_freq : 90000.0 + fabs(a->exp_freq);
        /* SPFIT's basic, spin-free record is three upper followed immediately
           by three lower QNs.  Do not emit the zero-valued M placeholders:
           they turn a no-spin record into a malformed 12-QN one.  If an
           assigned catalogue genuinely carries extra QNs, retain as many
           paired fields as it uses. */
        int u[6] = {q->Ju,q->Kau,q->Kcu,q->M1u,q->M2u,q->M3u};
        int l[6] = {q->Jl,q->Kal,q->Kcl,q->M1l,q->M2l,q->M3l};
        int nq = 3;
        for (int k = 5; k >= 3; k--) if (u[k] != 0 || l[k] != 0) { nq = k + 1; break; }
        for (int k = 0; k < nq; k++) fprintf(lin, "%3d", u[k]);
        for (int k = 0; k < nq; k++) fprintf(lin, "%3d", l[k]);
        /* The first 36 characters are the complete 12-I3 QN field, even
           when the molecule only uses six of those slots. */
        for (int k = 2 * nq; k < 12; k++) fputs("   ", lin);
        fprintf(lin, "%15.6f %10.6f 1.0\n", freq, p->line_error_mhz);
    }
    fclose(var); fclose(in); if (par) fclose(par); if (lin) fclose(lin);
    return 1;
}

static int run(const char *cmd, PredFitState *p, const char *what) {
    int rc = system(cmd);
    if (rc != 0) { snprintf(p->status, sizeof(p->status), "%s failed (exit %d).", what, rc); return 0; }
    return 1;
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
    }
    fclose(fp);
    if (last_rms[0])
        snprintf(out, outsz, "SPFIT stopped after %d/50 iterations; %s", p->last_fit_iterations, last_rms);
}

typedef struct { int found; double obs, calc, diff, unc; } FitObservation;

static FitObservation fit_observation(const PredFitState *p, int line_number) {
    FitObservation result={0};
    char path[600]; work_file(p,"model.fit",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return result;
    char line[512];
    while (fgets(line,sizeof(line),fp)) {
        int n=0; if (sscanf(line," %d:",&n)!=1 || n!=line_number) continue;
        char *q=strchr(line,':'); if (!q) continue; q++;
        for (int k=0;k<6;k++) { char *end=NULL; strtol(q,&end,10); if (end==q) break; q=end; }
        char *end=NULL;
        double obs=strtod(q,&end); if (end==q) continue; q=end;
        double calc=strtod(q,&end); if (end==q) continue; q=end;
        double diff=strtod(q,&end); if (end==q) continue; q=end;
        double unc=strtod(q,&end); if (end==q || unc<=0.0) continue;
        result=(FitObservation){1,obs,calc,diff,unc}; /* retain final iteration */
    }
    fclose(fp);
    return result;
}

static int fitted_parameter(const PredFitState *p, int id, double *value, double *error) {
    char path[600]; work_file(p,"model.var",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return 0;
    char line[256]; int found=0;
    while (fgets(line,sizeof(line),fp)) {
        int got_id=0; double got_value=0, got_error=0;
        if (!strchr(line,'/') || sscanf(line,"%d %lf %lf",&got_id,&got_value,&got_error)!=3 || got_id!=id) continue;
        *value=got_value; *error=got_error; found=1;
    }
    fclose(fp);
    return found;
}

static void import_int_settings(PredFitState *p) {
    char path[600]; work_file(p,"model.int",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return;
    char line[256];
    if (!fgets(line,sizeof(line),fp) || !fgets(line,sizeof(line),fp)) { fclose(fp); return; }
    int flags=0, tag=0; double q=0, fmin=0, fmax=0, s0=0, s1=0, limit=0, temp=0;
    if (sscanf(line,"%d %d %lf %lf %lf %lf %lf %lf %lf",&flags,&tag,&q,&fmin,&fmax,&s0,&s1,&limit,&temp)==9) {
        if (temp > 0.0) p->temp_k=temp;
        p->fmin_ghz=fmin; p->fmax_ghz=fmax;
    }
    while (fgets(line,sizeof(line),fp)) {
        int id=0; double mu=0;
        if (sscanf(line,"%d %lf",&id,&mu)!=2) continue;
        if (id>=1 && id<=3) p->mu[id-1]=mu;
    }
    fclose(fp);
}

static void import_fit_lines(AppState *s) {
    PredFitState *p=&s->predfit;
    char path[600]; work_file(p,"model.lin",path,sizeof(path));
    FILE *fp=fopen(path,"r"); if (!fp) return;
    s->n_assignments=0;
    char line[256];
    while (s->n_assignments < MAX_ASSIGNMENTS && fgets(line,sizeof(line),fp)) {
        int q[6]={0};
        if (sscanf(line,"%d %d %d %d %d %d",&q[0],&q[1],&q[2],&q[3],&q[4],&q[5]) != 6) continue;
        double freq=atof(line+36);
        if (!(freq > 0.0)) continue;
        Assignment *a=&s->assignments[s->n_assignments++];
        memset(a,0,sizeof(*a));
        a->pred.Ju=q[0]; a->pred.Kau=q[1]; a->pred.Kcu=q[2];
        a->pred.Jl=q[3]; a->pred.Kal=q[4]; a->pred.Kcl=q[5];
        a->exp_freq=freq >= 90000.0 ? freq-90000.0 : freq;
        a->fit_enabled=freq < 90000.0;
    }
    fclose(fp);
}

int predfit_restore_latest(AppState *s) {
    PredFitState *p=&s->predfit;
    char flat_cat[600]; snprintf(flat_cat,sizeof(flat_cat),FIT_ROOT "/model.cat");
    FILE *flat=fopen(flat_cat,"r");
    if (!flat) return 0;
    fclose(flat);
    snprintf(p->work_dir,sizeof(p->work_dir),"%s",FIT_ROOT);
    import_fitted_parameters(p);
    load_manual_parameter_errors(p);
    import_int_settings(p);
    import_fit_lines(s);
    char cat_path[600]; work_file(p,"model.cat",cat_path,sizeof(cat_path));
    snprintf(s->pending_pred_path,sizeof(s->pending_pred_path),"%s",cat_path);
    s->pending_load=1;
    p->generated_catalog_pending=1;
    snprintf(p->status,sizeof(p->status),"Restored the latest Pred&Fit state from .fit.");
    return 1;
}

int predfit_calculate(AppState *s) {
    PredFitState *p = &s->predfit;
    predfit_publish_shared_state(s);
    if (!write_inputs(s, 0)) return 0;
    char cmd[700], cat_path[600];
    snprintf(cmd,sizeof(cmd),"cd %s && %s model",p->work_dir,SPCAT_BIN);
    if (!run(cmd, p, "SPCAT")) return 0;
    work_file(p,"model.cat",cat_path,sizeof(cat_path));
    snprintf(s->pending_pred_path, sizeof(s->pending_pred_path), "%s",cat_path);
    s->pending_load = 1;
    p->generated_catalog_pending=1;
    snprintf(p->status, sizeof(p->status), "SPCAT complete; Qrot = %.6g.", qrot(p));
    return 1;
}

int predfit_fit(AppState *s) {
    PredFitState *p = &s->predfit;
    if (s->n_assignments == 0) { snprintf(p->status, sizeof(p->status), "Assign lines before running SPFIT."); return 0; }
    predfit_publish_shared_state(s);
    if (!push_fit_snapshot(s)) return 0;
    if (!write_inputs(s, 1)) { p->history_count--; return 0; }
    char cmd[700], cat_path[600];
    snprintf(cmd,sizeof(cmd),"cd %s && %s model",p->work_dir,SPFIT_BIN);
    if (!run(cmd, p, "SPFIT")) { p->history_count--; return 0; }
    import_fitted_parameters(p);
    snprintf(cmd,sizeof(cmd),"cd %s && %s model",p->work_dir,SPCAT_BIN);
    if (!run(cmd, p, "SPCAT after fit")) return 0;
    work_file(p,"model.cat",cat_path,sizeof(cat_path));
    snprintf(s->pending_pred_path, sizeof(s->pending_pred_path), "%s",cat_path);
    s->pending_load = 1;
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
    if (!predfit_calculate(s)) return 0;
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
    p->advanced_window_id = SDL_GetWindowID(p->advanced_window); p->advanced_open = 1; p->advanced_tab = 0; p->advanced_hover_line = -1;
}
void predfit_close_advanced(AppState *s) {
    PredFitState *p=&s->predfit; if (p->advanced_renderer) SDL_DestroyRenderer(p->advanced_renderer);
    if (p->advanced_window) SDL_DestroyWindow(p->advanced_window);
    p->advanced_renderer=NULL; p->advanced_window=NULL; p->advanced_open=0; p->advanced_window_id=0;
    p->advanced_edit_param=-1;
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
    SDL_StartTextInput();
}

static void advanced_begin_line_error(PredFitState *p) {
    p->advanced_edit_param = -2;
    p->advanced_edit_col = 0;
    p->advanced_edit_replace = 1;
    snprintf(p->advanced_edit_buf, sizeof(p->advanced_edit_buf), "%.8g", p->line_error_mhz);
    SDL_StartTextInput();
}

static void advanced_commit_edit(PredFitState *p) {
    if (p->advanced_edit_param == -2) {
        char *end = NULL; double v = strtod(p->advanced_edit_buf, &end);
        if (end != p->advanced_edit_buf && isfinite(v) && v > 0.0) p->line_error_mhz = v;
        p->advanced_edit_param = -1;
        p->advanced_edit_replace = 0;
        SDL_StopTextInput();
        return;
    }
    if (p->advanced_edit_param < 0 || p->advanced_edit_param >= p->n_param) return;
    PickettParameter *x = &p->param[p->advanced_edit_param];
    if (p->advanced_edit_col == 0) {
        char *end = NULL; long id = strtol(p->advanced_edit_buf, &end, 10);
        if (end != p->advanced_edit_buf && id > 0 && id < 1000000) x->id = (int)id;
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
    SDL_StopTextInput();
}

static int advanced_edit_event(AppState *s, const SDL_Event *e) {
    PredFitState *p = &s->predfit;
    if (p->advanced_edit_param < 0) return 0;
    if (e->type == SDL_TEXTINPUT && e->text.windowID == p->advanced_window_id) {
        if (p->advanced_edit_replace) { p->advanced_edit_buf[0] = '\0'; p->advanced_edit_replace = 0; }
        size_t n = strlen(p->advanced_edit_buf), add = strlen(e->text.text);
        if (n + add < sizeof(p->advanced_edit_buf)) strcat(p->advanced_edit_buf, e->text.text);
        return 1;
    }
    if (e->type == SDL_KEYDOWN && e->key.windowID == p->advanced_window_id) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_BACKSPACE) { if (p->advanced_edit_replace) { p->advanced_edit_buf[0]='\0'; p->advanced_edit_replace=0; } else { size_t n = strlen(p->advanced_edit_buf); if (n) p->advanced_edit_buf[n - 1] = '\0'; } return 1; }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { advanced_commit_edit(p); return 1; }
        if (k == SDLK_ESCAPE) { p->advanced_edit_param=-1; p->advanced_edit_replace=0; SDL_StopTextInput(); return 1; }
        return 1;
    }
    return 0;
}

static void add_parameter(PredFitState *p) {
    if (p->n_param >= MAX_PICKETT_PARAMS) return;
    PickettParameter *x = &p->param[p->n_param++];
    *x = (PickettParameter){0, 0.0, 1.0, "Pickett parameter"};
    advanced_begin_edit(p, p->n_param - 1, 0);
}

int predfit_handle_advanced_event(AppState *s, const SDL_Event *e) {
    PredFitState *p=&s->predfit;
    if (!p->advanced_open) return 0;
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == p->advanced_window_id && e->window.event == SDL_WINDOWEVENT_CLOSE) { predfit_close_advanced(s); return 1; }
    if (advanced_edit_event(s, e)) return 1;
    if (p->advanced_edit_param != -1 && e->type == SDL_MOUSEBUTTONDOWN && e->button.windowID == p->advanced_window_id)
        advanced_commit_edit(p);
    if (e->type == SDL_MOUSEWHEEL && e->wheel.windowID == p->advanced_window_id) {
        int *scroll = p->advanced_tab == 0 ? &p->advanced_param_scroll : &p->advanced_line_scroll;
        int limit = p->advanced_tab == 0 ? p->n_param - 14 : s->n_assignments - (p->advanced_tab == 1 ? 16 : 11);
        *scroll -= e->wheel.y;
        if (*scroll < 0) *scroll = 0;
        if (*scroll > limit) *scroll = limit > 0 ? limit : 0;
        return 1;
    }
    if (e->type == SDL_MOUSEMOTION && e->motion.windowID == p->advanced_window_id) {
        if (p->advanced_tab == 2 && e->motion.y >= 285 && e->motion.y < 525) {
            int row=p->advanced_line_scroll+(e->motion.y-285)/24;
            p->advanced_hover_line=row < s->n_assignments ? row : -1;
        } else p->advanced_hover_line=-1;
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.windowID != p->advanced_window_id) return 0;
    int x=e->button.x, y=e->button.y;
    if (y >= 48 && y < 82) { p->advanced_tab = x < 180 ? 0 : x < 320 ? 1 : 2; return 1; }
    if (p->advanced_tab == 0) {
        if (y >= 146 && y < 538) { int row=p->advanced_param_scroll+(y-146)/28; if (row < p->n_param) { if (x < 110) advanced_begin_edit(p,row,0); else if (x >= 500 && x < 700) advanced_begin_edit(p,row,1); else if (x >= 700) advanced_begin_edit(p,row,2); return 1; } }
        if (y >= 540 && y < 574 && x >= 300 && x < 500) { advanced_begin_line_error(p); return 1; }
        if (y >= 550 && x < 160) { add_parameter(p); return 1; }
    }
    if (p->advanced_tab == 1 && y >= 154 && y < 546) { int row=p->advanced_line_scroll+(y-154)/24; if (row >= 0 && row < s->n_assignments) s->assignments[row].fit_enabled=!s->assignments[row].fit_enabled; return 1; }
    if (p->advanced_tab == 2) {
        if (y >= 285 && y < 525) { int row=p->advanced_line_scroll+(y-285)/24; if (row >= 0 && row < s->n_assignments) s->assignments[row].fit_enabled=!s->assignments[row].fit_enabled; return 1; }
        if (y > 550 && x < 140) { predfit_fit(s); return 1; }
        if (y > 550 && x >= 140 && x < 280) { predfit_undo_last_fit(s); return 1; }
    }
    return 1;
}
void predfit_render_advanced(AppState *s) {
    PredFitState *p=&s->predfit; if (!p->advanced_open || !p->advanced_renderer) return;
    SDL_Renderer *r=p->advanced_renderer; char b[256];
    SDL_SetRenderDrawColor(r,19,20,22,255); SDL_RenderClear(r);
    ui_fill(r,(SDL_Rect){0,0,920,42},UI_TITLEBAR);
    ui_text(r,UI_FONT_TITLE,"Pred&Fit Advanced",18,12,UI_TEXT);
    const char *tabs[]={"Parameters","Lines","Fitting"};
    for(int i=0;i<3;i++) ui_button(r,(SDL_Rect){16+i*145,48,132,30},tabs[i],-1,UI_BTN_QUIET,p->advanced_tab==i,0,0,0);

    if (p->advanced_tab==0) {
        ui_text(r,UI_FONT_SANS,"Pickett parameter table — edit ID, value, or fit uncertainty",18,100,UI_ACCENT_TEXT);
        SDL_Rect table={18,130,884,392}; ui_fill(r,table,UI_INPUT); ui_frame(r,table,UI_LINE);
        ui_text(r,UI_FONT_MONO_SM,"ID",28,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"WATSON-A",118,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"WATSON-S",260,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"OTHER",380,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"VALUE",510,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"FIT ERROR",710,140,UI_DIM);
        for(int i=0;i<14 && p->advanced_param_scroll+i<p->n_param;i++) {
            int actual=p->advanced_param_scroll+i, y=164+i*25; PickettParameter*x=&p->param[actual]; const ParameterName *name=parameter_name(x->id);
            SDL_Rect row={20,y-3,880,23}; if(actual==p->advanced_edit_param) ui_fill(r,row,UI_ACCENT_SOFT); else if(i%2) ui_fill(r,row,UI_PANEL);
            snprintf(b,sizeof(b),"%d",x->id); ui_text(r,UI_FONT_MONO,b,28,y,actual==p->advanced_edit_param&&p->advanced_edit_col==0?UI_ACCENT_TEXT:UI_TEXT);
            ui_text(r,UI_FONT_SANS_SM,name&&name->watson_a?name->watson_a:"—",118,y,UI_DIM);
            ui_text(r,UI_FONT_SANS_SM,name&&name->watson_s?name->watson_s:"—",260,y,UI_ACCENT_TEXT);
            ui_text(r,UI_FONT_SANS_SM,name&&name->other?name->other:(name?"—":x->label),380,y,UI_DIM);
            snprintf(b,sizeof(b),"%.11E",x->value); ui_text(r,UI_FONT_MONO,b,510,y,actual==p->advanced_edit_param&&p->advanced_edit_col==1?UI_ACCENT_TEXT:UI_TEXT);
            snprintf(b,sizeof(b),"%.5E",x->error); ui_text(r,UI_FONT_MONO,b,710,y,actual==p->advanced_edit_param&&p->advanced_edit_col==2?UI_ACCENT_TEXT:UI_TEXT);
        }
        if (p->advanced_edit_param>=0) { snprintf(b,sizeof(b),"Editing: %s",p->advanced_edit_buf); ui_text(r,UI_FONT_MONO,b,18,540,UI_ACCENT_TEXT); }
        snprintf(b,sizeof(b),".lin uncertainty: %.8g MHz",p->line_error_mhz); ui_text(r,UI_FONT_SANS_SM,b,330,570,p->advanced_edit_param==-2?UI_ACCENT_TEXT:UI_DIM);
        ui_text(r,UI_FONT_SANS_SM,"Scroll table • 0 fit error = fixed parameter",18,570,UI_FAINT);
        ui_button(r,(SDL_Rect){18,582,142,30},"+ parameter",-1,UI_BTN_QUIET,0,0,0,0);
    } else if (p->advanced_tab==1) {
        ui_text(r,UI_FONT_SANS,"Assigned transitions — click a row to exclude it from SPFIT only",18,100,UI_ACCENT_TEXT);
        SDL_Rect table={18,130,884,402}; ui_fill(r,table,UI_INPUT); ui_frame(r,table,UI_LINE);
        ui_text(r,UI_FONT_MONO_SM,"FIT",28,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"OBSERVED / MHz",90,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"PREDICTED / MHz",245,140,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"UPPER  —  LOWER",440,140,UI_DIM);
        for(int i=0;i<16 && p->advanced_line_scroll+i<s->n_assignments;i++) { int actual=p->advanced_line_scroll+i,y=164+i*23; Assignment*a=&s->assignments[actual]; if(i%2) ui_fill(r,(SDL_Rect){20,y-3,880,21},UI_PANEL); snprintf(b,sizeof(b),"[%c]",a->fit_enabled?'x':' ');ui_text(r,UI_FONT_MONO,b,28,y,a->fit_enabled?UI_OK:UI_FAINT);snprintf(b,sizeof(b),"%.6f",a->exp_freq);ui_text(r,UI_FONT_MONO,b,90,y,UI_TEXT);snprintf(b,sizeof(b),"%.6f",a->pred.freq_mhz);ui_text(r,UI_FONT_MONO,b,245,y,UI_DIM);snprintf(b,sizeof(b),"%d %d %d  —  %d %d %d",a->pred.Ju,a->pred.Kau,a->pred.Kcu,a->pred.Jl,a->pred.Kal,a->pred.Kcl);ui_text(r,UI_FONT_MONO,b,440,y,UI_TEXT); }
        ui_text(r,UI_FONT_SANS_SM,"Excluded rows become 90000 + frequency only in .fit/model.lin.",18,570,UI_FAINT);
    } else {
        ui_text(r,UI_FONT_SANS,"SPFIT output",18,100,UI_ACCENT_TEXT); ui_text(r,UI_FONT_MONO,p->status[0]?p->status:"Run SPFIT after selecting assignments.",18,124,UI_TEXT);
        SDL_Rect pt={18,154,615,102}; ui_fill(r,pt,UI_INPUT); ui_frame(r,pt,UI_LINE); ui_text(r,UI_FONT_MONO_SM,"FINAL PARAMETERS (.fit / model.var)",28,164,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"ID / LABEL",28,184,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"VALUE",250,184,UI_DIM); ui_text(r,UI_FONT_MONO_SM,"SPFIT SIGMA",455,184,UI_DIM);
        for(int i=0;i<3 && i<p->n_param;i++) { PickettParameter*x=&p->param[i]; double value=x->value,sigma=0; int got=fitted_parameter(p,x->id,&value,&sigma); const ParameterName*name=parameter_name(x->id); int y=204+i*16; snprintf(b,sizeof(b),"%d  %s",x->id,name?(name->watson_s?name->watson_s:name->other):x->label);ui_text(r,UI_FONT_MONO_SM,b,28,y,UI_TEXT);snprintf(b,sizeof(b),"%.10E",value);ui_text(r,UI_FONT_MONO_SM,b,250,y,UI_TEXT);snprintf(b,sizeof(b),got?"%.4E":"—",sigma);ui_text(r,UI_FONT_MONO_SM,b,455,y,got?UI_ACCENT_TEXT:UI_FAINT); }
        SDL_Rect lt={18,270,615,258};ui_fill(r,lt,UI_INPUT);ui_frame(r,lt,UI_LINE);ui_text(r,UI_FONT_MONO_SM,"FIT  QNS                         OBSERVED       CALCULATED       DIFF       /UNC",28,280,UI_DIM);
        for(int i=0;i<10 && p->advanced_line_scroll+i<s->n_assignments;i++) { int actual=p->advanced_line_scroll+i,y=304+i*22; Assignment*a=&s->assignments[actual]; FitObservation o=fit_observation(p,actual+1); SDL_Color c=actual==p->advanced_hover_line?UI_ACCENT_TEXT:(a->fit_enabled?UI_TEXT:UI_FAINT); if(actual==p->advanced_hover_line)ui_fill(r,(SDL_Rect){20,y-3,611,20},UI_ACCENT_SOFT); snprintf(b,sizeof(b),"[%c] %2d %2d %2d - %2d %2d %2d",a->fit_enabled?'x':' ',a->pred.Ju,a->pred.Kau,a->pred.Kcu,a->pred.Jl,a->pred.Kal,a->pred.Kcl);ui_text(r,UI_FONT_MONO_SM,b,28,y,c); if(o.found){snprintf(b,sizeof(b),"%11.5f %11.5f %+.5f %+.2f",o.obs,o.calc,o.diff,o.diff/o.unc);ui_text(r,UI_FONT_MONO_SM,b,230,y,c);} }
        SDL_Rect dash={650,154,252,374};ui_fill(r,dash,UI_PANEL);ui_frame(r,dash,UI_LINE);ui_text(r,UI_FONT_SANS,"Residual dashboard",662,166,UI_ACCENT_TEXT);int h=p->advanced_hover_line; if(h>=0&&h<s->n_assignments){FitObservation o=fit_observation(p,h+1);if(o.found){double z=o.diff/o.unc;snprintf(b,sizeof(b),"line %d   (obs-calc)/unc",h+1);ui_text(r,UI_FONT_MONO_SM,b,662,202,UI_DIM);snprintf(b,sizeof(b),"%+.4f",z);ui_text(r,UI_FONT_TITLE,b,662,228,fabs(z)>3.0?UI_DANGER:UI_OK);snprintf(b,sizeof(b),"obs   %.7f MHz",o.obs);ui_text(r,UI_FONT_MONO_SM,b,662,270,UI_TEXT);snprintf(b,sizeof(b),"calc  %.7f MHz",o.calc);ui_text(r,UI_FONT_MONO_SM,b,662,290,UI_TEXT);snprintf(b,sizeof(b),"diff  %+.7f MHz",o.diff);ui_text(r,UI_FONT_MONO_SM,b,662,310,UI_TEXT);snprintf(b,sizeof(b),"unc   %.7f MHz",o.unc);ui_text(r,UI_FONT_MONO_SM,b,662,330,UI_TEXT);SDL_Rect axis={670,382,210,8};ui_fill(r,axis,UI_LINE);SDL_Rect zero={774,374,2,24};ui_fill(r,zero,UI_TEXT);double clipped=fmax(-5.0,fmin(5.0,z));int w=(int)(fabs(clipped)*20.0);SDL_Rect bar={clipped<0?774-w:776,382,w,8};ui_fill(r,bar,fabs(z)>3.0?UI_DANGER:UI_OK);ui_text(r,UI_FONT_MONO_SM,"-5σ          0          +5σ",670,402,UI_DIM);}else ui_text(r,UI_FONT_SANS_SM,"No calculated row yet.",662,202,UI_FAINT);}else ui_text(r,UI_FONT_SANS_SM,"Hover a fit row to inspect\nits normalized residual.",662,202,UI_FAINT);
        ui_button(r,(SDL_Rect){18,582,110,30},"Fit",-1,UI_BTN_PRIMARY,0,0,0,0);ui_button(r,(SDL_Rect){138,582,132,30},"Undo fit",-1,UI_BTN_QUIET,0,0,0,0);ui_text(r,UI_FONT_SANS_SM,"Latest generated files are kept in .fit/ and updated by Undo.",300,592,UI_FAINT);
    }
    SDL_RenderPresent(r);
}
