#include "intensity_fit.h"
#include "controller.h"
#include "layout.h"
#include "loader.h"
#include "plotgpu.h"
#include "predfit.h"
#include "settings.h"
#include "ui_theme.h"
#include "view.h"

#include <float.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define IF_C2 1.438776877
#define IF_MHZ_PER_CM 29979.2458
#define IF_MAX_PARAMETERS (MAX_INTFIT_SPECIES * 2)

enum { IF_EDIT_NONE, IF_EDIT_FMIN, IF_EDIT_FMAX, IF_EDIT_WINDOW,
       IF_EDIT_TMIN, IF_EDIT_TMAX, IF_EDIT_FRACTION, IF_EDIT_FLOOR };
enum { IF_PARAM_SCALE, IF_PARAM_TEMP };

typedef struct {
    int assignment_index;
    int species_index;
    const PredLine *line;
    double center_mhz;
    double observed;
} IfObservation;

typedef struct { int kind, index; } IfParameter;

static void preview_close(AppState *s);

static int component_index(char mu) {
    if (mu == 'a' || mu == 'A') return 0;
    if (mu == 'b' || mu == 'B') return 1;
    if (mu == 'c' || mu == 'C') return 2;
    return -1;
}

static int branch_index(const PredLine *p) {
    int dj = p->Ju - p->Jl;
    return dj == -1 ? 0 : dj == 0 ? 1 : dj == 1 ? 2 : -1;
}

static const PredFitSnapshot *model_for_hamiltonian(const AppState *s, int h, int *id) {
    const PredFitState *p = &s->predfit;
    if (h < 0 || h >= p->n_hamiltonians) return NULL;
    if (id) *id = p->hamiltonian[h].id;
    return h == p->active_hamiltonian ? (const PredFitSnapshot *)p : &p->hamiltonian[h].model;
}

static double cat_temperature_for(const AppState *s, int hamiltonian_id, const PredFitSnapshot *m) {
    const PredFitState *p = &s->predfit;
    for (int i = 0; i < p->n_simulated; i++)
        if (p->simulated[i].hamiltonian_id == hamiltonian_id && p->simulated[i].cat_temp_k > 0.0)
            return p->simulated[i].cat_temp_k;
    if (p->generated_catalog_active && s->cat_temp_k > 0.0 &&
        hamiltonian_id == predfit_active_hamiltonian_id(s)) return s->cat_temp_k;
    if (m && m->int_settings.temp_k > 0.0) return m->int_settings.temp_k;
    return m && m->temp_k > 0.0 ? m->temp_k : 300.0;
}

static void report_add(IntensityFitWindow *w, const char *fmt, ...) {
    size_t at = strlen(w->report);
    if (at >= sizeof(w->report) - 2) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->report + at, sizeof(w->report) - at, fmt, ap);
    va_end(ap);
}

/* The species Pred&Fit currently offers, with their catalogue values. */
static int collect_predfit_species(const AppState *s, IntensityFitSpecies *out, int cap) {
    const PredFitState *p = &s->predfit;
    int n = 0;
    for (int h = 0; h < p->n_hamiltonians && n < cap; h++) {
        int h_id = 0;
        const PredFitSnapshot *m = model_for_hamiltonian(s, h, &h_id);
        if (!m) continue;
        double tcat = cat_temperature_for(s, h_id, m);
        for (int i = 0; i < m->n_species && n < cap; i++) {
            const PickettSpecies *sp = &m->species[i];
            IntensityFitSpecies *dst = &out[n++];
            memset(dst, 0, sizeof(*dst));
            dst->hamiltonian_id = h_id;
            dst->state_index = sp->state_index;
            snprintf(dst->name, sizeof(dst->name), "%s", sp->name[0] ? sp->name : "Species");
            snprintf(dst->backend_name, sizeof(dst->backend_name), "%s", dst->name);
            /* Python indexes fit data by species name.  Keep the user's
               label exactly when it is unique; a repeated state label from
               another Hamiltonian is the one case that needs disambiguation. */
            int duplicate_name = 0;
            for (int k = 0; k < n - 1; k++) {
                IntensityFitSpecies *prior = &out[k];
                if (strcmp(prior->name, dst->name) != 0) continue;
                duplicate_name = 1;
                snprintf(prior->backend_name, sizeof(prior->backend_name), "%s [H%d]",
                         prior->name, prior->hamiltonian_id);
            }
            if (duplicate_name)
                snprintf(dst->backend_name, sizeof(dst->backend_name), "%s [H%d]",
                         dst->name, dst->hamiltonian_id);
            dst->included = sp->predict_enabled != 0;
            dst->temperature_group = 1;
            dst->concentration = sp->concentration > 0.0 ? sp->concentration : 1.0;
            dst->temperature_k = m->temp_k > 0.0 ? m->temp_k : tcat;
            dst->cat_temperature_k = tcat;
            dst->fitted_concentration = dst->concentration;
            dst->fitted_temperature_k = dst->temperature_k;
            for (int c = 0; c < 3; c++) {
                dst->mu_cat[c] = sp->mu[c];
                dst->fitted_mu[c] = sp->mu[c];
            }
        }
    }
    return n;
}

/* Default choices.  The windows the analysis already has stay open. */
static void initialise_from_predfit(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    SDL_Window *window = w->window;
    SDL_Renderer *renderer = w->renderer;
    Uint32 window_id = w->window_id;
    int open = w->open;
    memset(w, 0, sizeof(*w));
    w->window = window;
    w->renderer = renderer;
    w->window_id = window_id;
    w->open = open;
    w->fit_concentration = 1;
    w->fit_temperature = 1;
    w->fit_dipoles = 0;
    w->fit_mode = 0;
    w->extraction_mode = 1;
    w->common_temperature = 1;
    w->branch_enabled[0] = w->branch_enabled[1] = w->branch_enabled[2] = 1;
    w->mu_enabled[0] = w->mu_enabled[1] = w->mu_enabled[2] = 1;
    w->residual_weighting = 0;
    w->temp_min_k = 0.1;
    w->temp_max_k = 100.0;
    w->extraction_window_mhz = 0.05;
    w->intensity_uncertainty_fraction = 0.20;
    w->intensity_uncertainty_floor = 0.02;
    w->fmin_mhz = s->n_pts ? s->current_pts[0].x : 0.0;
    w->fmax_mhz = s->n_pts ? s->current_pts[s->n_pts - 1].x : 0.0;
    w->n_species = collect_predfit_species(s, w->species, MAX_INTFIT_SPECIES);
    w->initialized = 1;
}

static int lower_point(const Point *p, int n, double x) {
    int lo = 0, hi = n;
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (p[m].x < x) lo = m + 1; else hi = m; }
    return lo;
}

static int extract_observation(const AppState *s, double center, int mode, double half_window, double *out) {
    if (!s->current_pts || s->n_pts < 2 || !(half_window > 0.0)) return 0;
    const Point *p = s->current_pts;
    double left = center - half_window, right = center + half_window;
    if (left < p[0].x || right > p[s->n_pts - 1].x) return 0;
    int i = lower_point(p, s->n_pts, left);
    if (i <= 0 || i >= s->n_pts) return 0;
    if (mode == 0) {
        int best = i;
        for (int k = i - 1; k <= i; k++) if (k >= 0 && k < s->n_pts && fabs(p[k].x - center) < fabs(p[best].x - center)) best = k;
        *out = p[best].y;
        return isfinite(*out);
    }
    if (mode == 1) {
        double best = -DBL_MAX;
        for (int k = i - 1; k < s->n_pts && p[k].x <= right; k++) if (p[k].x >= left && p[k].y > best) best = p[k].y;
        *out = best;
        return isfinite(*out);
    }
    /* Same trapezoidal integral used by the legacy public routine, including
       linearly interpolated edges. */
    double x0=p[i-1].x, x1=p[i].x;
    double prev=p[i-1].y + (left-x0)*(p[i].y-p[i-1].y)/(x1-x0), px=left, sum=0.0;
    for (; i < s->n_pts && p[i].x < right; i++) { sum += .5*(prev+p[i].y)*(p[i].x-px); px=p[i].x; prev=p[i].y; }
    if (i >= s->n_pts) return 0;
    x0=p[i-1].x; x1=p[i].x;
    double yr=p[i-1].y + (right-x0)*(p[i].y-p[i-1].y)/(x1-x0);
    *out=sum+.5*(prev+yr)*(right-px);
    return isfinite(*out);
}

static int find_species(const IntensityFitWindow *w, const Assignment *a) {
    int state = a->pred.n_qn >= 4 ? a->pred.M1l : 0;
    for (int i=0;i<w->n_species;i++)
        if (w->species[i].hamiltonian_id == a->hamiltonian_id && w->species[i].state_index == state) return i;
    return -1;
}

static int collect_observations(const AppState *s, IfObservation **out) {
    const IntensityFitWindow *w = &s->intensity_window;
    IfObservation *items = calloc((size_t)s->n_assignments, sizeof(*items));
    if (!items) return -1;
    int n = 0;
    for (int i=0;i<s->n_assignments;i++) {
        const Assignment *a=&s->assignments[i];
        int sp=find_species(w,a), bi=branch_index(&a->pred), mu=component_index(a->pred.mu);
        if (sp < 0 || !w->species[sp].included || bi < 0 || mu < 0 || !w->branch_enabled[bi] || !w->mu_enabled[mu]) continue;
        if (a->exp_freq < w->fmin_mhz || a->exp_freq > w->fmax_mhz) continue;
        double observed=0.0;
        if (!extract_observation(s,a->exp_freq,w->extraction_mode,w->extraction_window_mhz,&observed)) continue;
        items[n++] = (IfObservation){i,sp,&a->pred,a->exp_freq,observed};
    }
    *out=items;
    return n;
}

static double temperature_factor(const PredLine *p, double tcat, double tret) {
    if (!(tcat>0.0) || !(tret>0.0)) return 1.0;
    double nu=p->freq_mhz/IF_MHZ_PER_CM;
    double stim_r=-expm1(-IF_C2*nu/tret), stim_c=-expm1(-IF_C2*nu/tcat);
    double pop_r=exp(-IF_C2*p->elo_cm/tret)*stim_r/pow(tret,.5*p->rot_dof);
    double pop_c=exp(-IF_C2*p->elo_cm/tcat)*stim_c/pow(tcat,.5*p->rot_dof);
    return pop_c>0.0 && isfinite(pop_r) ? pop_r/pop_c : 0.0;
}

static void decode_parameters(const IntensityFitWindow *w, const IfParameter *params, int np,
                              const double *x, double *scale, double *temp) {
    for (int i=0;i<w->n_species;i++) { scale[i]=w->species[i].concentration; temp[i]=w->species[i].temperature_k; }
    for (int k=0;k<np;k++) {
        if (params[k].kind==IF_PARAM_SCALE) scale[params[k].index]=exp(x[k]);
        else { int group=params[k].index; double t=exp(x[k]); for(int i=0;i<w->n_species;i++) if(w->species[i].temperature_group==group) temp[i]=t; }
    }
}

static void evaluate_lines(const IntensityFitWindow *w, const IfObservation *obs, int n,
                           const double *scale, const double *temp, const double (*mu_scale)[3], double *model) {
    for (int i=0;i<n;i++) {
        const IntensityFitSpecies *sp=&w->species[obs[i].species_index];
        int c=component_index(obs[i].line->mu);
        double base=pow(10.0,obs[i].line->cat_lgint);
        double factor=temperature_factor(obs[i].line,sp->cat_temperature_k,temp[obs[i].species_index]);
        model[i]=scale[obs[i].species_index]*base*factor*mu_scale[obs[i].species_index][c]*mu_scale[obs[i].species_index][c];
    }
}

static double residual_vector(const IntensityFitWindow *w, const IfObservation *obs, int n,
                              const IfParameter *params, int np, const double *x,
                              const double (*mu_scale)[3], double *residual, double *model_out) {
    double scale[MAX_INTFIT_SPECIES], temp[MAX_INTFIT_SPECIES];
    double *model=model_out ? model_out : calloc((size_t)n,sizeof(*model));
    if (!model) return DBL_MAX;
    decode_parameters(w,params,np,x,scale,temp);
    evaluate_lines(w,obs,n,scale,temp,mu_scale,model);
    double cost=0.0;
    for(int i=0;i<n;i++) {
        double r;
        if(w->fit_log_space) { double sig=fmax(log10(1.0+w->intensity_uncertainty_fraction),1e-12); r=(log10(fmax(obs[i].observed,1e-30))-log10(fmax(model[i],1e-30)))/sig; }
        else {
            double sigma=1.0;
            if(w->residual_weighting==1) sigma=fmax(fabs(obs[i].observed)*w->intensity_uncertainty_fraction,DBL_MIN);
            else if(w->residual_weighting==2) sigma=fmax(hypot(fabs(obs[i].observed)*w->intensity_uncertainty_fraction,w->intensity_uncertainty_floor),DBL_MIN);
            r=(obs[i].observed-model[i])/sigma;
        }
        if(residual) residual[i]=r;
        cost+=r*r;
    }
    if(!model_out) free(model);
    return cost;
}

static int solve_linear(double *a, double *b, int n) {
    for(int k=0;k<n;k++) {
        int pivot=k; for(int i=k+1;i<n;i++) if(fabs(a[i*n+k])>fabs(a[pivot*n+k])) pivot=i;
        if(fabs(a[pivot*n+k])<1e-20) return 0;
        if(pivot!=k) for(int j=k;j<n;j++){double z=a[k*n+j];a[k*n+j]=a[pivot*n+j];a[pivot*n+j]=z;} 
        if(pivot!=k){double z=b[k];b[k]=b[pivot];b[pivot]=z;}
        double d=a[k*n+k]; for(int j=k;j<n;j++)a[k*n+j]/=d; b[k]/=d;
        for(int i=0;i<n;i++) if(i!=k){double q=a[i*n+k]; if(q!=0.0){for(int j=k;j<n;j++)a[i*n+j]-=q*a[k*n+j];b[i]-=q*b[k];}}
    }
    return 1;
}

static int fit_first_step(IntensityFitWindow *w, const IfObservation *obs, int n,
                          double *scale_out, double *temp_out, int *npar_out) {
    IfParameter params[IF_MAX_PARAMETERS]; double x[IF_MAX_PARAMETERS]; int np=0;
    if(w->fit_concentration) for(int i=0;i<w->n_species;i++) if(w->species[i].included) { params[np]=(IfParameter){IF_PARAM_SCALE,i}; x[np++]=log(fmax(w->species[i].concentration,1e-30)); }
    if(w->fit_temperature) {
        int seen[MAX_INTFIT_GROUPS+1]={0};
        for(int i=0;i<w->n_species;i++) if(w->species[i].included) {int g=w->common_temperature?1:w->species[i].temperature_group; if(g<1)g=1; if(!seen[g]){seen[g]=1;params[np]=(IfParameter){IF_PARAM_TEMP,g};x[np++]=log(fmax(w->species[i].temperature_k,w->temp_min_k));}}
    }
    double mu[MAX_INTFIT_SPECIES][3]; for(int i=0;i<w->n_species;i++)for(int c=0;c<3;c++)mu[i][c]=1.0;
    if(np==0){decode_parameters(w,params,0,x,scale_out,temp_out);*npar_out=0;return 1;}
    double *r=malloc((size_t)n*sizeof(*r)), *rp=malloc((size_t)n*sizeof(*rp));
    double *jac=malloc((size_t)n*np*sizeof(*jac)), *model=malloc((size_t)n*sizeof(*model));
    if(!r||!rp||!jac||!model){free(r);free(rp);free(jac);free(model);return 0;}
    double lambda=1e-2, cost=residual_vector(w,obs,n,params,np,x,mu,r,model);
    for(int iter=0;iter<80;iter++) {
        for(int k=0;k<np;k++) { double old=x[k], h=1e-5; x[k]=old+h; residual_vector(w,obs,n,params,np,x,mu,rp,NULL); x[k]=old; for(int i=0;i<n;i++)jac[i*np+k]=(rp[i]-r[i])/h; }
        double *normal=calloc((size_t)np*np,sizeof(*normal)), *rhs=calloc((size_t)np,sizeof(*rhs)), *trial=malloc((size_t)np*sizeof(*trial));
        if(!normal||!rhs||!trial){free(normal);free(rhs);free(trial);break;}
        for(int i=0;i<n;i++)for(int k=0;k<np;k++){rhs[k]-=jac[i*np+k]*r[i];for(int l=0;l<np;l++)normal[k*np+l]+=jac[i*np+k]*jac[i*np+l];}
        for(int k=0;k<np;k++)normal[k*np+k]+=lambda*(normal[k*np+k]+1.0);
        if(!solve_linear(normal,rhs,np)){free(normal);free(rhs);free(trial);break;}
        for(int k=0;k<np;k++){trial[k]=x[k]+rhs[k]; if(params[k].kind==IF_PARAM_SCALE){if(trial[k]<-50)trial[k]=-50;if(trial[k]>50)trial[k]=50;}else{double lo=log(w->temp_min_k),hi=log(w->temp_max_k);if(trial[k]<lo)trial[k]=lo;if(trial[k]>hi)trial[k]=hi;}}
        double next=residual_vector(w,obs,n,params,np,trial,mu,rp,NULL);
        if(next<cost){memcpy(x,trial,(size_t)np*sizeof(*x));cost=next;residual_vector(w,obs,n,params,np,x,mu,r,model);lambda=fmax(lambda*.35,1e-12);double step=0;for(int k=0;k<np;k++)step+=rhs[k]*rhs[k];if(step<1e-14){free(normal);free(rhs);free(trial);break;}}
        else lambda=fmin(lambda*8.0,1e12);
        free(normal);free(rhs);free(trial);
    }
    decode_parameters(w,params,np,x,scale_out,temp_out); *npar_out=np;
    free(r);free(rp);free(jac);free(model);return 1;
}

/* The numerical source of truth is the Python package selected by the user.
 * The GUI deliberately writes a minimal, temporary Pickett/config bundle and
 * invokes that package instead of maintaining a second optimiser whose
 * tolerances or robust-loss implementation could quietly diverge from SciPy. */
static int if_transition_selected(const AppState *s, const Assignment *a, int *species_out) {
    const IntensityFitWindow *w = &s->intensity_window;
    int sp = find_species(w, a), br = branch_index(&a->pred), mu = component_index(a->pred.mu);
    if (sp < 0 || !w->species[sp].included || br < 0 || mu < 0 ||
        !w->branch_enabled[br] || !w->mu_enabled[mu] ||
        a->exp_freq < w->fmin_mhz || a->exp_freq > w->fmax_mhz) return 0;
    if (species_out) *species_out = sp;
    return 1;
}

static void if_json_string(FILE *f, const char *text) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p >= 32) fputc(*p, f);
    }
    fputc('"', f);
}

static void if_qn_slot(char out[3], int value) {
    if (value >= 0 && value <= 99) snprintf(out, 3, "%02d", value);
    else if (value >= 100 && value <= 359) { out[0] = (char)('A' + value / 10 - 10); out[1] = (char)('0' + value % 10); out[2] = '\0'; }
    else if (value <= -10 && value >= -269) { int a = -value; out[0] = (char)('a' + a / 10 - 1); out[1] = (char)('0' + a % 10); out[2] = '\0'; }
    else snprintf(out, 3, "%2d", value);
}

static void if_qn_values(const PredLine *p, int qn[12]) {
    const int u[6] = {p->Ju,p->Kau,p->Kcu,p->M1u,p->M2u,p->M3u};
    const int l[6] = {p->Jl,p->Kal,p->Kcl,p->M1l,p->M2l,p->M3l};
    for (int i=0;i<6;i++) { qn[i]=u[i]; qn[6+i]=l[i]; }
}

/* The Python reader uses the QNs as the identity of an observation.  In a
 * normal multi-state SPCAT catalogue NQN is already >= 4 and M1 is the
 * state.  Older / external 3-QN catalogues have no such field, though, so
 * give the temporary, per-species input a fourth QN containing the known
 * Pred&Fit state.  LIN and CAT go through exactly the same conversion. */
static int if_temp_qn(const Assignment *a, const IntensityFitSpecies *species,
                      int qn[12]) {
    int nq = a->pred.n_qn;
    if (nq < 1 || nq > 6) nq = 3;
    if_qn_values(&a->pred, qn);
    if (nq < 4) {
        nq = 4;
        qn[3] = species->state_index;
        qn[9] = species->state_index;
    }
    return nq;
}

static int if_same_temp_transition(const Assignment *a, const IntensityFitSpecies *a_species,
                                   const Assignment *b, const IntensityFitSpecies *b_species) {
    int aqn[12], bqn[12];
    int an = if_temp_qn(a, a_species, aqn);
    int bn = if_temp_qn(b, b_species, bqn);
    if (an != bn) return 0;
    for (int i = 0; i < an; i++)
        if (aqn[i] != bqn[i] || aqn[6 + i] != bqn[6 + i]) return 0;
    return 1;
}

/* lgint_at_tcat is SPCAT's intensity at the catalogue TEMP, written as it is:
   the Python model scales each species from the TEMP of its .int, which
   states species->cat_temperature_k.  For a blend it is the sum of the
   components, written on the strongest one's QNs. */
static int if_write_lin_cat(FILE *lin, FILE *cat, const Assignment *a,
                            const IntensityFitSpecies *species, double lgint_at_tcat) {
    int qn[12];
    int nq = if_temp_qn(a, species, qn);
    /* PredLine is six upper slots followed by six lower slots.  A Pickett
       record instead stores NQN upper values immediately followed by NQN
       lower values.  Do not slice the fixed PredLine representation as a
       flat 2*NQN array: that was the source of the fake duplicate QNs. */
    int packed[12];
    for (int i = 0; i < nq; i++) {
        packed[i] = qn[i];
        packed[nq + i] = qn[6 + i];
    }
    for (int i=0;i<12;i++) {
        if (i < 2*nq) fprintf(lin, "%3d", packed[i]); else fputs("   ", lin);
    }
    fprintf(lin, " %14.7f %10.4f %10.4f\n", a->exp_freq, 0.01, 1.0);
    fprintf(cat, "%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d",
            a->pred.freq_mhz, 0.0, lgint_at_tcat, a->pred.rot_dof,
            a->pred.elo_cm, 1, 0, 300 + nq);
    for (int i=0;i<2*nq;i++) { char slot[3]; if_qn_slot(slot, packed[i]); fputs(slot, cat); }
    fputc('\n', cat);
    return 1;
}

static int if_copy_file_to_buffer(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, out_size - 1, f);
    int err = ferror(f);
    fclose(f);
    out[n] = '\0';
    return !err;
}

/* Pandas writes proper CSV (including quoted names).  Do not use strtok here:
 * a perfectly valid species label may contain a comma, and the first field is
 * the link between Python's result and the state shown in this window. */
static int if_csv_field(const char *line, int wanted, char *out, size_t out_size) {
    int field = 0, quoted = 0;
    size_t used = 0;
    if (!line || !out || out_size == 0) return 0;
    out[0] = '\0';
    for (const char *p = line; ; p++) {
        char c = *p;
        if (c == '"') {
            if (quoted && p[1] == '"') {
                if (field == wanted && used + 1 < out_size) out[used++] = '"';
                p++;
                continue;
            }
            quoted = !quoted;
            continue;
        }
        if ((c == ',' && !quoted) || c == '\0' || c == '\n' || c == '\r') {
            if (field == wanted) { out[used] = '\0'; return 1; }
            if (c == ',' && !quoted) { field++; used = 0; continue; }
            return 0;
        }
        if (field == wanted && used + 1 < out_size) out[used++] = c;
    }
}

static int if_summary_species_index(const IntensityFitWindow *w, const char *name) {
    for (int i = 0; i < w->n_species; i++)
        if (strcmp(w->species[i].backend_name, name) == 0) return i;
    return -1;
}

static void finish_python_reference_fit(AppState *s, int status) {
    IntensityFitWindow *w = &s->intensity_window;
    w->fit_running = 0;
    w->fit_pid = 0;
    w->console_log[0] = '\0';
    if (w->fit_console_path[0])
        if_copy_file_to_buffer(w->fit_console_path, w->console_log, sizeof(w->console_log));
    w->report_scroll = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(w->message, sizeof(w->message), "Python reference fit failed; see the fit log below.");
        return;
    }
    char report_path[700];
    snprintf(report_path, sizeof(report_path), "%s/liveplot_intensity.ifit", w->fit_output_dir);
    if (!if_copy_file_to_buffer(report_path, w->report, sizeof(w->report))) {
        snprintf(w->message, sizeof(w->message), "Python fit completed but its report was not found.");
        return;
    }
    char summary_path[700], line[1024];
    snprintf(summary_path, sizeof(summary_path), "%s/species_summary.csv", w->fit_output_dir);
    FILE *sum = fopen(summary_path, "r");
    if (sum) {
        fgets(line, sizeof(line), sum);
        while (fgets(line, sizeof(line), sum)) {
            char species_name[sizeof(w->species[0].backend_name)], scale[64], temperature[64];
            if (!if_csv_field(line, 0, species_name, sizeof(species_name)) ||
                !if_csv_field(line, 11, scale, sizeof(scale)) ||
                !if_csv_field(line, 15, temperature, sizeof(temperature))) continue;
            int i = if_summary_species_index(w, species_name);
            if (i >= 0) {
                w->species[i].fitted_concentration = strtod(scale, NULL);
                w->species[i].fitted_temperature_k = strtod(temperature, NULL);
            }
        }
        fclose(sum);
    }
    for (int i = 0; i < w->n_species; i++) for (int c = 0; c < 3; c++) {
        char key[192]; snprintf(key, sizeof(key), "%s:mu_%c", w->species[i].backend_name, 'a' + c);
        char *mu_line = strstr(w->report, key);
        if (!mu_line) continue;
        char *scale_word = strstr(mu_line, "scale=");
        if (!scale_word) continue;
        double factor = strtod(scale_word + 6, NULL);
        if (isfinite(factor) && factor > 0.0)
            w->species[i].fitted_mu[c] = w->species[i].mu_cat[c] * factor;
    }
    w->has_result = 1; w->result_generation++; w->n_candidates = 0; w->n_parameters = 0;
    s->predfit.session_dirty = 1;   /* the fit is part of the session */
    char *hit = strstr(w->report, "LINES USED=");
    if (hit) sscanf(hit, "LINES USED=%d  NUMBER OF PARAMETERS=%d", &w->n_candidates, &w->n_parameters);
    hit = strstr(w->report, "RMS ERROR =");
    if (hit) sscanf(hit, "RMS ERROR = %lf", &w->rmse);
    snprintf(w->message, sizeof(w->message),
             "Python fit complete: %d lines; %d blend component%s summed, %d duplicate%s omitted. Preview is non-destructive.",
             w->fit_line_count, w->fit_blend_count, w->fit_blend_count == 1 ? "" : "s",
             w->fit_duplicate_count, w->fit_duplicate_count == 1 ? "" : "s");
}

static void poll_python_reference_fit(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (!w->fit_running || w->fit_pid <= 0) return;
    int status = 0;
    pid_t result = waitpid((pid_t)w->fit_pid, &status, WNOHANG);
    if (result == 0) {
        if (w->fit_console_path[0])
            if_copy_file_to_buffer(w->fit_console_path, w->console_log, sizeof(w->console_log));
        return;
    }
    if (result < 0) {
        w->fit_running = 0; w->fit_pid = 0;
        snprintf(w->message, sizeof(w->message), "Could not collect the Python fit process.");
        return;
    }
    finish_python_reference_fit(s, status);
}

void intensity_analysis_poll(AppState *s) {
    poll_python_reference_fit(s);
    if (s->intensity_window.species_sync_pending && s->intensity_window.open)
        intensity_analysis_sync_species(s);
}

/* Assignments of one species at the same experimental frequency are the
   components of one blend: they share the measured area. */
#define IF_BLEND_TOL_MHZ 5e-5

/* Temperature of the first species that contributes lines to a group
   (group 0: every species). */
static double if_group_temperature(const IntensityFitWindow *w, const int *line_count, int group) {
    for (int i = 0; i < w->n_species; i++) {
        if (!line_count[i]) continue;
        int g = w->common_temperature ? 1 : w->species[i].temperature_group;
        if (group == 0 || g == group) return w->species[i].temperature_k;
    }
    return 0.0;
}

int intensity_analysis_write_python_inputs(AppState *s, const char *dir,
                                           char *config, size_t config_size,
                                           char *output, size_t output_size) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->n_species <= 0) { snprintf(w->message,sizeof(w->message),"Pred&Fit contains no species to analyse."); return 0; }
    if (!s->current_pts || s->n_pts < 2) { snprintf(w->message,sizeof(w->message),"Load an experimental spectrum first."); return 0; }
    /* The Python fit takes a fixed scale as an absolute factor on the
       experimental intensity, whose units are arbitrary; Pred&Fit's relative
       concentrations would pin every line to a meaningless height. */
    if (!w->fit_concentration) {
        snprintf(w->message, sizeof(w->message),
                 "Concentrations must be fitted: the experimental intensity scale is arbitrary.");
        return 0;
    }
    /* Tests and old in-memory sessions can predate backend_name.  Give those
       rows a readable label too; never silently fall back to S0/S1. */
    for (int i = 0; i < w->n_species; i++) {
        if (w->species[i].backend_name[0]) continue;
        if (w->species[i].name[0])
            snprintf(w->species[i].backend_name, sizeof(w->species[i].backend_name), "%s", w->species[i].name);
        else
            snprintf(w->species[i].backend_name, sizeof(w->species[i].backend_name), "Species %d", i + 1);
    }

    char spectrum[700];
    snprintf(spectrum,sizeof(spectrum),"%s/spectrum.txt",dir);
    snprintf(config,config_size,"%s/run.json",dir);
    snprintf(output,output_size,"%s/output",dir);
    FILE *sf=fopen(spectrum,"w");
    if (!sf) { snprintf(w->message,sizeof(w->message),"Cannot write temporary spectrum."); return 0; }
    for(int i=0;i<s->n_pts;i++) fprintf(sf,"%.12g %.12g\n",s->current_pts[i].x,s->current_pts[i].y);
    fclose(sf);

    int line_count[MAX_INTFIT_SPECIES]={0};
    FILE *lin[MAX_INTFIT_SPECIES]={0}, *cat[MAX_INTFIT_SPECIES]={0};
    char lin_path[MAX_INTFIT_SPECIES][700], cat_path[MAX_INTFIT_SPECIES][700], int_path[MAX_INTFIT_SPECIES][700];
    int *chosen = s->n_assignments > 0 ? malloc((size_t)s->n_assignments * sizeof(*chosen)) : NULL;
    if (s->n_assignments > 0 && !chosen) { snprintf(w->message,sizeof(w->message),"Not enough memory for the intensity fit."); return 0; }
    for(int i=0;i<w->n_species;i++) if(w->species[i].included) {
        snprintf(lin_path[i],sizeof(lin_path[i]),"%s/species_%03d.lin",dir,i);
        snprintf(cat_path[i],sizeof(cat_path[i]),"%s/species_%03d.cat",dir,i);
        snprintf(int_path[i],sizeof(int_path[i]),"%s/species_%03d.int",dir,i);
        lin[i]=fopen(lin_path[i],"w"); cat[i]=fopen(cat_path[i],"w");
        if(!lin[i]||!cat[i]) { snprintf(w->message,sizeof(w->message),"Cannot write temporary Pickett inputs."); goto write_fail; }
    }
    /* A LIN file permits one observed position per transition.  Keep the
       assignment closest to its calculated frequency when an old session or
       a repeated manual assignment contains the same transition twice.  The
       Python reference rightly refuses duplicates; resolving them here gives
       it a valid representation without changing the user's assignment list. */
    int duplicate_count = 0;
    for (int a = 0; a < s->n_assignments; a++) {
        int sp = -1;
        chosen[a] = -1;
        if (!if_transition_selected(s, &s->assignments[a], &sp)) continue;
        int keep = 1;
        double this_error = fabs(s->assignments[a].exp_freq - s->assignments[a].pred.freq_mhz);
        for (int b = 0; b < s->n_assignments; b++) {
            int other_sp = -1;
            if (b == a || !if_transition_selected(s, &s->assignments[b], &other_sp) || other_sp != sp)
                continue;
            if (!if_same_temp_transition(&s->assignments[a], &w->species[sp],
                                         &s->assignments[b], &w->species[other_sp]))
                continue;
            double other_error = fabs(s->assignments[b].exp_freq - s->assignments[b].pred.freq_mhz);
            if (other_error < this_error || (other_error == this_error && b < a)) {
                keep = 0;
                break;
            }
        }
        if (!keep) { duplicate_count++; continue; }
        chosen[a] = sp;
    }
    /* Written one by one, every component of a blend was compared with the
       area of the whole feature.  The blend is one observation instead: its
       intensity at the catalogue TEMP is the sum of the components, written
       on the strongest one's QNs (and so with its ELO and dipole component). */
    int blend_count = 0;
    for (int a = 0; a < s->n_assignments; a++) {
        int sp = chosen[a];
        if (sp < 0) continue;
        const Assignment *rep = &s->assignments[a];
        double ref = rep->pred.cat_lgint, sum = 1.0;
        for (int b = a + 1; b < s->n_assignments; b++) {
            if (chosen[b] != sp || fabs(s->assignments[b].exp_freq - rep->exp_freq) > IF_BLEND_TOL_MHZ)
                continue;
            sum += pow(10.0, s->assignments[b].pred.cat_lgint - ref);
            if (s->assignments[b].pred.cat_lgint > rep->pred.cat_lgint) rep = &s->assignments[b];
            chosen[b] = -1;
            blend_count++;
        }
        if_write_lin_cat(lin[sp], cat[sp], rep, &w->species[sp], ref + log10(sum));
        line_count[sp]++;
    }
    free(chosen);
    chosen = NULL;
    for(int i=0;i<w->n_species;i++) { if(lin[i])fclose(lin[i]);if(cat[i])fclose(cat[i]);lin[i]=cat[i]=NULL; }

    int total_lines = 0;
    for (int i = 0; i < w->n_species; i++) total_lines += line_count[i];
    if (total_lines < 2) {
        snprintf(w->message, sizeof(w->message), "Need at least two selected assigned transitions for an intensity fit.");
        return 0;
    }

    FILE *cf=fopen(config,"w");
    if(!cf) { snprintf(w->message,sizeof(w->message),"Cannot write temporary intensity configuration."); return 0; }
    fputs("{\n  \"run\": {\"name\": \"liveplot_intensity\", \"spectrum\": ",cf);if_json_string(cf,spectrum);fputs(", \"output_dir\": ",cf);if_json_string(cf,output);fputs("},\n  \"fit\": {\n",cf);
    fprintf(cf,"    \"fit_mode\": \"%s\",\n",w->fit_mode?"spectrum":"line_intensity");
    fprintf(cf,"    \"common_temperature\": %s,\n",w->common_temperature?"true":"false");
    fputs("    \"species_temperature_groups\": {",cf); int first=1;for(int i=0;i<w->n_species;i++)if(line_count[i]){if(!first)fputc(',',cf);if_json_string(cf,w->species[i].backend_name);fputc(':',cf);char group[32];snprintf(group,sizeof(group),"G%d",w->common_temperature?1:w->species[i].temperature_group);if_json_string(cf,group);first=0;}fputs("},\n",cf);
    /* "Fit Trot" off holds each group at its Pred&Fit Trot; without these
       keys the Python fit frees the temperature regardless. */
    if (!w->fit_temperature) {
        if (w->common_temperature) {
            fprintf(cf,"    \"fixed_temperature_K\": %.12g,\n",if_group_temperature(w,line_count,0));
        } else {
            int seen[MAX_INTFIT_GROUPS+1]={0};
            fputs("    \"fixed_group_temperatures\": {",cf); first=1;
            for(int i=0;i<w->n_species;i++){int g=w->species[i].temperature_group;if(!line_count[i]||g<1||g>MAX_INTFIT_GROUPS||seen[g])continue;seen[g]=1;if(!first)fputc(',',cf);fprintf(cf,"\"G%d\": %.12g",g,if_group_temperature(w,line_count,g));first=0;}
            fputs("},\n",cf);
        }
    }
    double temp_init = fmin(fmax(if_group_temperature(w,line_count,0),w->temp_min_k),w->temp_max_k);
    fprintf(cf,"    \"extraction_mode\": \"%s\",\n",w->extraction_mode==0?"sample":w->extraction_mode==1?"local_max":"area");
    fprintf(cf,"    \"extraction_window_MHz\": %.12g,\n",w->extraction_window_mhz);
    fprintf(cf,"    \"fit_in_log_space\": %s,\n",w->fit_log_space?"true":"false");
    fprintf(cf,"    \"use_exact_temperature_scaling\": %s,\n",w->exact_temperature_scaling?"true":"false");
    fprintf(cf,"    \"temp_init_K\": %.12g, \"temp_min_K\": %.12g, \"temp_max_K\": %.12g,\n",temp_init,w->temp_min_k,w->temp_max_k);
    fprintf(cf,"    \"intensity_uncertainty_fraction\": %.12g, \"intensity_uncertainty_floor\": %.12g,\n",w->intensity_uncertainty_fraction,w->intensity_uncertainty_floor);
    fprintf(cf,"    \"residual_weighting\": \"%s\", \"loss\": \"linear\", \"loss_f_scale\": 1.0,\n",w->residual_weighting==1?"fractional":w->residual_weighting==2?"hybrid":"none");
    int has_mu=0;for(int i=0;i<w->n_species;i++)for(int c=0;c<3;c++)if(w->species[i].fit_dipole[c])has_mu=1;
    fprintf(cf,"    \"dipole_model\": \"%s\", \"dipole_fit_second_step\": true, \"dipole_fit_reject_outliers\": true, \"dipole_fit_reject_sigma\": 5.0, \"dipole_fit_min_lines\": 3,\n",w->fit_dipoles&&has_mu?"component_scaled":"from_cat");
    double fwhm = s->gauss_gamma>0 ? 2*s->gauss_gamma : s->lorentz_gamma>0 ? 2*s->lorentz_gamma : 0.10;
    fprintf(cf,"    \"lineshape\": {\"profile\": \"%s\", \"fwhm_MHz\": %.12g, \"fit_fwhm\": false, \"window_MHz\": %.12g, \"normalize\": \"area\"},\n",s->lorentz_gamma>0&&s->gauss_gamma<=0?"lorentzian":"gaussian",fwhm,fmax(5*fwhm,w->extraction_window_mhz));
    fputs("    \"plot\": {\"enabled\": false}\n  },\n  \"species\": [\n",cf);
    /* The .int TEMP is the temperature the CAT rows were generated at: the
       Python model scales every intensity of the species from it to Trot. */
    first=1;for(int i=0;i<w->n_species;i++)if(line_count[i]){FILE*inf=fopen(int_path[i],"w");if(!inf){fclose(cf);snprintf(w->message,sizeof(w->message),"Cannot write temporary dipoles.");return 0;}fprintf(inf,"SpectraVisual intensity preview\n0 0 1 0 0 0 0 0 %.12g 0\n1 %.12g /a dipole/\n2 %.12g /b dipole/\n3 %.12g /c dipole/\n",w->species[i].cat_temperature_k,w->species[i].mu_cat[0],w->species[i].mu_cat[1],w->species[i].mu_cat[2]);fclose(inf);if(!first)fputs(",\n",cf);fputs("    {\"name\": ",cf);if_json_string(cf,w->species[i].backend_name);fputs(", \"lin\": ",cf);if_json_string(cf,lin_path[i]);fputs(", \"cat\": ",cf);if_json_string(cf,cat_path[i]);fputs(", \"int\": ",cf);if_json_string(cf,int_path[i]);if(w->fit_dipoles&&has_mu){fputs(", \"fit_dipole_components\": [",cf);int comma=0;for(int c=0;c<3;c++)if(w->species[i].fit_dipole[c]){if(comma++)fputc(',',cf);fprintf(cf,"\"%c\"",'a'+c);}fputc(']',cf);}fputc('}',cf);first=0;}
    fputs("\n  ]\n}\n",cf);fclose(cf);
    w->fit_duplicate_count = duplicate_count;
    w->fit_blend_count = blend_count;
    w->fit_line_count = total_lines;
    return 1;
write_fail:
    free(chosen);
    for(int i=0;i<w->n_species;i++){if(lin[i])fclose(lin[i]);if(cat[i])fclose(cat[i]);}
    return 0;
}

static int run_python_reference_fit(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->fit_running) {
        snprintf(w->message, sizeof(w->message), "Python fit is already running; the interface remains usable.");
        return 0;
    }
    const char *root = getenv("SPECTRAVISUAL_INTENSITY_FIT_ROOT");
    if (!root || !root[0]) root = "/Users/filippobaroncelli/Desktop/coding/Python/relative_intensity";
    char probe[700]; snprintf(probe, sizeof(probe), "%s/intensity_fit/workflow.py", root);
    FILE *check = fopen(probe, "r");
    if (!check) { snprintf(w->message,sizeof(w->message),"Python reference not found. Set SPECTRAVISUAL_INTENSITY_FIT_ROOT."); return 0; }
    fclose(check);

    char template_path[] = "/private/tmp/spectravisual-intfit-XXXXXX";
    char *dir = mkdtemp(template_path);
    if (!dir) { snprintf(w->message,sizeof(w->message),"Cannot create temporary fit directory."); return 0; }
    char config[700], output[700];
    if (!intensity_analysis_write_python_inputs(s, dir, config, sizeof(config), output, sizeof(output)))
        return 0;
    snprintf(w->fit_console_path, sizeof(w->fit_console_path), "%s/intensity_fit_console.log", dir);
    w->console_log[0] = '\0';
    w->report_scroll = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int log_fd = open(w->fit_console_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        if (chdir(root) != 0) _exit(126);
        execlp("python3", "python3", "-m", "intensity_fit", "run", config, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        snprintf(w->message, sizeof(w->message), "Could not start Python reference fit.");
        return 0;
    }
    w->fit_running = 1;
    w->fit_pid = (int)pid;
    snprintf(w->fit_output_dir, sizeof(w->fit_output_dir), "%s", output);
    w->has_result = 0;
    snprintf(w->message, sizeof(w->message), "Python line-intensity fit is running in the background (%d observed lines)…", w->fit_line_count);
    return 1;
}

static int run_fit(AppState *s) {
    return run_python_reference_fit(s);
    IntensityFitWindow *w=&s->intensity_window; IfObservation *obs=NULL; int n=collect_observations(s,&obs);
    w->report[0]='\0'; w->has_result=0;
    if(n<2){snprintf(w->message,sizeof(w->message),"Need at least two assigned, selected observations in the chosen range.");free(obs);return 0;}
    double scale[MAX_INTFIT_SPECIES],temp[MAX_INTFIT_SPECIES],mu[MAX_INTFIT_SPECIES][3];
    for(int i=0;i<w->n_species;i++)for(int c=0;c<3;c++)mu[i][c]=1.0;
    int npar=0; if(!fit_first_step(w,obs,n,scale,temp,&npar)){snprintf(w->message,sizeof(w->message),"Not enough memory for the intensity fit.");free(obs);return 0;}
    /* Python's component_scaled default is a second step: abundance and T are
       frozen, then each selected species/component gets its own log fit. */
    if(w->fit_dipoles) for(int sp=0;sp<w->n_species;sp++)for(int c=0;c<3;c++) if(w->species[sp].included&&w->species[sp].fit_dipole[c]) {
        double sum=0.0;int used=0;
        for(int i=0;i<n;i++)if(obs[i].species_index==sp&&component_index(obs[i].line->mu)==c){double base=pow(10.0,obs[i].line->cat_lgint)*temperature_factor(obs[i].line,w->species[sp].cat_temperature_k,temp[sp])*scale[sp];if(base>0&&obs[i].observed>0){sum+=log(obs[i].observed/base);used++;}}
        if(used>=3){double log_scale=sum/(double)used;mu[sp][c]=exp(.5*fmax(2*log(.05),fmin(2*log(20.),log_scale)));npar++;}
    }
    double *model=malloc((size_t)n*sizeof(*model)); if(!model){free(obs);return 0;}
    evaluate_lines(w,obs,n,scale,temp,mu,model);
    double rss=0,raw=0;for(int i=0;i<n;i++){double rr=obs[i].observed-model[i];raw+=rr*rr;double sig=1;if(w->residual_weighting==1)sig=fmax(fabs(obs[i].observed)*w->intensity_uncertainty_fraction,DBL_MIN);else if(w->residual_weighting==2)sig=fmax(hypot(fabs(obs[i].observed)*w->intensity_uncertainty_fraction,w->intensity_uncertainty_floor),DBL_MIN);if(w->fit_log_space)rr=(log10(fmax(obs[i].observed,1e-30))-log10(fmax(model[i],1e-30)))/fmax(log10(1+w->intensity_uncertainty_fraction),1e-12);else rr/=sig;rss+=rr*rr;}
    for(int i=0;i<w->n_species;i++){w->species[i].fitted_concentration=scale[i];w->species[i].fitted_temperature_k=temp[i];for(int c=0;c<3;c++)w->species[i].fitted_mu[c]=w->species[i].mu_cat[c]*mu[i][c];}
    w->has_result=1;w->result_generation++;w->n_candidates=n;w->n_used=n;w->n_rejected=0;w->n_parameters=npar;w->rss=rss;w->rmse=sqrt(rss/(double)fmax(1,n-npar));w->raw_rmse=sqrt(raw/(double)n);
    report_add(w,"INTFIT: RELATIVE INTENSITY FIT\n\nFIT QUALITY\n LINES USED=%5d  NUMBER OF PARAMETERS=%4d  DOF=%5d\n RMS ERROR = %12.6f\n RAW RMS = %13.6e\n\nOPTIONS\n extraction_mode        %s\n fit_mode               %s\n fmin/fmax MHz          %.6f / %.6f\n extraction_window MHz  %.6f\n T bounds K             %.6f / %.6f\n broadening             Pred&Fit active profile (fixed)\n\nSTEP 1: ABUNDANCE AND TEMPERATURE FIT\n",n,npar,fmax(1,n-npar),w->rmse,w->raw_rmse,w->extraction_mode==0?"sample":w->extraction_mode==1?"local_max":"area",w->fit_mode?"spectrum":"line_intensity",w->fmin_mhz,w->fmax_mhz,w->extraction_window_mhz,w->temp_min_k,w->temp_max_k);
    for(int i=0;i<w->n_species;i++)if(w->species[i].included)report_add(w," %-30s scale=%12.5e  Trot=%9.4f K  group=%d\n",w->species[i].name,scale[i],temp[i],w->common_temperature?1:w->species[i].temperature_group);
    if(w->fit_dipoles){report_add(w,"\nSTEP 2: DIPOLE-ONLY FIT\n");for(int i=0;i<w->n_species;i++)if(w->species[i].included)report_add(w," %-30s mu_a=%9.5f  mu_b=%9.5f  mu_c=%9.5f\n",w->species[i].name,w->species[i].fitted_mu[0],w->species[i].fitted_mu[1],w->species[i].fitted_mu[2]);}
    snprintf(w->message,sizeof(w->message),"Fit complete: %d lines, %d parameters, RMS %.5g. Preview is non-destructive.",n,npar,w->rmse);
    free(model);free(obs);return 1;
}

static SDL_Rect if_button(int x,int y,int w){return (SDL_Rect){x,y,w,25};}
static SDL_Rect if_field(int x,int y){return (SDL_Rect){x,y,112,25};}
static SDL_Rect if_species_row(int y,int row){return (SDL_Rect){18,y+row*28,964,26};}

/* Reserve a real terminal panel in every usable window size.  The species
 * list remains scrollable, so the log never disappears merely because a
 * project has many states. */
static int if_visible_species_rows(int height) {
    int rows = (height - 150 - 12 - 25 - 12 - 170 - 18) / 28;
    if (rows < 3) rows = 3;
    if (rows > 14) rows = 14;
    return rows;
}

static int if_actions_y(int height) {
    return 150 + if_visible_species_rows(height) * 28 + 10;
}

static SDL_Rect if_log_rect(int width, int height) {
    int y = if_actions_y(height) + 35;
    return (SDL_Rect){18, y, width - 36, height - y - 18};
}

static int if_log_line_count(const char *text) {
    if (!text || !*text) return 1;
    int lines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n' && p[1]) lines++;
    return lines;
}

static const char *if_log_line_at(const char *text, int line) {
    if (!text) return "";
    while (line-- > 0) {
        text = strchr(text, '\n');
        if (!text) return "";
        text++;
    }
    return text;
}

static void render_fit_log(SDL_Renderer *r, IntensityFitWindow *w, int width, int height) {
    SDL_Rect box = if_log_rect(width, height);
    if (box.h < 40) return;
    ui_fill(r, box, UI_INPUT);
    ui_frame(r, box, UI_LINE);
    ui_text(r, UI_FONT_MONO_SM, "PYTHON FIT LOG", box.x + 8, box.y + 5, UI_ACCENT_TEXT);

    SDL_Rect clip = {box.x + 7, box.y + 22, box.w - 14, box.h - 28};
    const char *text = w->console_log[0] ? w->console_log :
                       w->fit_running ? "[ intensity_fit is running… ]" :
                       "[ Run fit to produce the Python intensity_fit log ]";
    int line_h = ui_text_h(UI_FONT_MONO_SM) + 2;
    int visible = clip.h / line_h;
    int line_count = if_log_line_count(text);
    int max_scroll = line_count > visible ? line_count - visible : 0;
    if (w->report_scroll < 0) w->report_scroll = 0;
    if (w->report_scroll > max_scroll) w->report_scroll = max_scroll;
    int max_chars = (clip.w - 4) / fmax(1, ui_text_w(UI_FONT_MONO_SM, "0"));

    SDL_RenderSetClipRect(r, &clip);
    for (int row = 0; row < visible; row++) {
        const char *start = if_log_line_at(text, w->report_scroll + row);
        if (!*start) break;
        char line[1024];
        size_t n = strcspn(start, "\r\n");
        if ((int)n > max_chars && max_chars > 3) {
            n = (size_t)(max_chars - 3);
            if (n >= sizeof(line) - 4) n = sizeof(line) - 4;
            memcpy(line, start, n);
            memcpy(line + n, "...", 4);
        } else {
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, start, n);
            line[n] = '\0';
        }
        ui_text(r, UI_FONT_MONO_SM, line, clip.x + 2, clip.y + row * line_h, UI_TEXT);
    }
    SDL_RenderSetClipRect(r, NULL);
    if (max_scroll) {
        char position[64];
        int last_visible = w->report_scroll + visible;
        if (last_visible > line_count) last_visible = line_count;
        snprintf(position, sizeof(position), "%d-%d / %d", w->report_scroll + 1,
                 last_visible, line_count);
        ui_text_right(r, UI_FONT_MONO_SM, position, box.x + box.w - 7, box.y + 5, UI_DIM);
    }
}

static void begin_edit(IntensityFitWindow *w,int field,double value){w->edit_field=field;snprintf(w->edit_text,sizeof(w->edit_text),"%.10g",value);SDL_StartTextInput();}
static void commit_edit(IntensityFitWindow *w){double v=strtod(w->edit_text,NULL);if(isfinite(v)){if(w->edit_field==IF_EDIT_FMIN)w->fmin_mhz=v;else if(w->edit_field==IF_EDIT_FMAX)w->fmax_mhz=v;else if(w->edit_field==IF_EDIT_WINDOW&&v>0)w->extraction_window_mhz=v;else if(w->edit_field==IF_EDIT_TMIN&&v>0)w->temp_min_k=v;else if(w->edit_field==IF_EDIT_TMAX&&v>w->temp_min_k)w->temp_max_k=v;else if(w->edit_field==IF_EDIT_FRACTION&&v>=0)w->intensity_uncertainty_fraction=v;else if(w->edit_field==IF_EDIT_FLOOR&&v>=0)w->intensity_uncertainty_floor=v;}w->edit_field=IF_EDIT_NONE;SDL_StopTextInput();}

static void export_report(AppState *s){IntensityFitWindow*w=&s->intensity_window;char path[600];settings_data_file(s,"intensity_fit.txt",path,sizeof(path));FILE*f=fopen(path,"w");if(!f){snprintf(w->message,sizeof(w->message),"Cannot write %s",path);return;}fputs(w->report,f);fclose(f);snprintf(w->message,sizeof(w->message),"Saved %s",path);}

/* The window keeps its choices and its result - also those restored from a
 * session - for as long as Pred&Fit offers the same species.  Their catalogue
 * values are read again every time: a different species list starts over,
 * and a catalogue generated at another TEMP or with other dipoles no longer
 * matches the fit, whose result is then dropped rather than misdrawn. */
void intensity_analysis_sync_species(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->fit_running) return;
    w->species_sync_pending = 0;
    static IntensityFitSpecies fresh[MAX_INTFIT_SPECIES];
    int n = collect_predfit_species(s, fresh, MAX_INTFIT_SPECIES);
    int same = w->initialized && n == w->n_species;
    for (int i = 0; same && i < n; i++)
        same = fresh[i].hamiltonian_id == w->species[i].hamiltonian_id &&
               fresh[i].state_index == w->species[i].state_index;
    if (!same) {
        preview_close(s);
        initialise_from_predfit(s);
        return;
    }
    int catalogue_changed = 0;
    for (int i = 0; i < n; i++) {
        IntensityFitSpecies *sp = &w->species[i];
        const IntensityFitSpecies *f = &fresh[i];
        if (sp->cat_temperature_k != f->cat_temperature_k ||
            memcmp(sp->mu_cat, f->mu_cat, sizeof(sp->mu_cat)) != 0) catalogue_changed = 1;
        memcpy(sp->name, f->name, sizeof(sp->name));
        memcpy(sp->backend_name, f->backend_name, sizeof(sp->backend_name));
        sp->concentration = f->concentration;
        sp->temperature_k = f->temperature_k;
        sp->cat_temperature_k = f->cat_temperature_k;
        memcpy(sp->mu_cat, f->mu_cat, sizeof(sp->mu_cat));
    }
    if (catalogue_changed && w->has_result) {
        preview_close(s);
        w->has_result = 0;
        snprintf(w->message, sizeof(w->message),
                 "The Pred&Fit catalogue changed since the fit: run it again.");
    }
}

/* ---------------------------------------------------------------- session
 * One intfit_* record per line of spectravisual.state.  Report and fit log are
 * stored line by line; a long line continues in "+" records, so no record
 * outgrows the session reader's line buffer. */
enum { IF_SESSION_CHUNK = 500 };
static int g_if_session_skip;

static void if_session_write_text(FILE *fp, const char *key, const char *text) {
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p), off = 0;
        do {
            size_t chunk = len - off > IF_SESSION_CHUNK ? IF_SESSION_CHUNK : len - off;
            fprintf(fp, "%s%s ", key, off ? "+" : "");
            for (size_t i = 0; i < chunk; i++) if (p[off + i] != '\r') fputc(p[off + i], fp);
            fputc('\n', fp);
            off += chunk;
        } while (off < len);
        p += len;
        if (*p == '\n') p++;
    }
}

static void if_session_append(char *text, size_t size, const char *chunk, int continuation) {
    size_t at = strlen(text);
    if (continuation && at > 0 && text[at - 1] == '\n') text[--at] = '\0';
    snprintf(text + at, size - at, "%s\n", chunk);
}

void intensity_analysis_write_session(const AppState *s, FILE *fp) {
    const IntensityFitWindow *w = &s->intensity_window;
    if (!w->initialized || !fp) return;
    fprintf(fp, "intfit_options %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
                "%.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
            w->fit_concentration, w->fit_temperature, w->fit_dipoles, w->fit_mode,
            w->extraction_mode, w->branch_enabled[0], w->branch_enabled[1], w->branch_enabled[2],
            w->mu_enabled[0], w->mu_enabled[1], w->mu_enabled[2], w->common_temperature,
            w->residual_weighting, w->fit_log_space, w->exact_temperature_scaling, w->loss,
            w->fmin_mhz, w->fmax_mhz, w->extraction_window_mhz, w->temp_min_k, w->temp_max_k,
            w->intensity_uncertainty_fraction, w->intensity_uncertainty_floor);
    for (int i = 0; i < w->n_species; i++) {
        const IntensityFitSpecies *sp = &w->species[i];
        fprintf(fp, "intfit_species %d %d %d %d %d %d %d "
                    "%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %s\n",
                sp->hamiltonian_id, sp->state_index, sp->included != 0, sp->temperature_group,
                sp->fit_dipole[0] != 0, sp->fit_dipole[1] != 0, sp->fit_dipole[2] != 0,
                sp->concentration, sp->temperature_k, sp->cat_temperature_k,
                sp->mu_cat[0], sp->mu_cat[1], sp->mu_cat[2],
                sp->fitted_concentration, sp->fitted_temperature_k,
                sp->fitted_mu[0], sp->fitted_mu[1], sp->fitted_mu[2], sp->name);
    }
    if (!w->has_result) return;
    fprintf(fp, "intfit_result %d %d %d %d %.17g %.17g %.17g %d %d %d\n",
            w->n_candidates, w->n_used, w->n_rejected, w->n_parameters,
            w->rss, w->rmse, w->raw_rmse, w->fit_line_count, w->fit_blend_count,
            w->fit_duplicate_count);
    if_session_write_text(fp, "intfit_message", w->message);
    if_session_write_text(fp, "intfit_report", w->report);
    if_session_write_text(fp, "intfit_console", w->console_log);
}

void intensity_analysis_session_begin(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    /* A fit still running belongs to the species it was started on. */
    g_if_session_skip = w->fit_running;
    if (g_if_session_skip) return;
    preview_close(s);
    initialise_from_predfit(s);
    w->n_species = 0;
    w->initialized = 0;
    /* The species list is read again once Pred&Fit has finished loading. */
    w->species_sync_pending = 1;
}

int intensity_analysis_read_session_line(AppState *s, const char *line) {
    if (strncmp(line, "intfit_", 7) != 0) return 0;
    IntensityFitWindow *w = &s->intensity_window;
    if (g_if_session_skip) return 1;
    if (strncmp(line, "intfit_options ", 15) == 0) {
        IntensityFitWindow o = {0};
        if (sscanf(line + 15, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
                              "%lf %lf %lf %lf %lf %lf %lf",
                   &o.fit_concentration, &o.fit_temperature, &o.fit_dipoles, &o.fit_mode,
                   &o.extraction_mode, &o.branch_enabled[0], &o.branch_enabled[1], &o.branch_enabled[2],
                   &o.mu_enabled[0], &o.mu_enabled[1], &o.mu_enabled[2], &o.common_temperature,
                   &o.residual_weighting, &o.fit_log_space, &o.exact_temperature_scaling, &o.loss,
                   &o.fmin_mhz, &o.fmax_mhz, &o.extraction_window_mhz, &o.temp_min_k, &o.temp_max_k,
                   &o.intensity_uncertainty_fraction, &o.intensity_uncertainty_floor) != 23) return 1;
        w->fit_concentration = o.fit_concentration != 0;
        w->fit_temperature = o.fit_temperature != 0;
        w->fit_dipoles = o.fit_dipoles != 0;
        w->fit_mode = o.fit_mode != 0;
        w->extraction_mode = o.extraction_mode >= 0 && o.extraction_mode <= 2 ? o.extraction_mode : 1;
        for (int c = 0; c < 3; c++) {
            w->branch_enabled[c] = o.branch_enabled[c] != 0;
            w->mu_enabled[c] = o.mu_enabled[c] != 0;
        }
        w->common_temperature = o.common_temperature != 0;
        w->residual_weighting = o.residual_weighting >= 0 && o.residual_weighting <= 2 ? o.residual_weighting : 0;
        w->fit_log_space = o.fit_log_space != 0;
        w->exact_temperature_scaling = o.exact_temperature_scaling != 0;
        w->loss = o.loss >= 0 && o.loss <= 4 ? o.loss : 0;
        w->fmin_mhz = o.fmin_mhz;
        w->fmax_mhz = o.fmax_mhz;
        if (o.extraction_window_mhz > 0.0) w->extraction_window_mhz = o.extraction_window_mhz;
        if (o.temp_min_k > 0.0 && o.temp_max_k > o.temp_min_k) {
            w->temp_min_k = o.temp_min_k;
            w->temp_max_k = o.temp_max_k;
        }
        if (o.intensity_uncertainty_fraction >= 0.0) w->intensity_uncertainty_fraction = o.intensity_uncertainty_fraction;
        if (o.intensity_uncertainty_floor >= 0.0) w->intensity_uncertainty_floor = o.intensity_uncertainty_floor;
        w->n_species = 0;
        w->initialized = 1;
        return 1;
    }
    if (strncmp(line, "intfit_species ", 15) == 0) {
        IntensityFitSpecies sp = {0};
        int consumed = 0;
        if (!w->initialized || w->n_species >= MAX_INTFIT_SPECIES) return 1;
        if (sscanf(line + 15, "%d %d %d %d %d %d %d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %n",
                   &sp.hamiltonian_id, &sp.state_index, &sp.included, &sp.temperature_group,
                   &sp.fit_dipole[0], &sp.fit_dipole[1], &sp.fit_dipole[2],
                   &sp.concentration, &sp.temperature_k, &sp.cat_temperature_k,
                   &sp.mu_cat[0], &sp.mu_cat[1], &sp.mu_cat[2],
                   &sp.fitted_concentration, &sp.fitted_temperature_k,
                   &sp.fitted_mu[0], &sp.fitted_mu[1], &sp.fitted_mu[2], &consumed) != 18) return 1;
        const char *name = line + 15 + consumed;
        snprintf(sp.name, sizeof(sp.name), "%s", *name ? name : "Species");
        snprintf(sp.backend_name, sizeof(sp.backend_name), "%s", sp.name);
        sp.included = sp.included != 0;
        if (sp.temperature_group < 1 || sp.temperature_group > MAX_INTFIT_GROUPS) sp.temperature_group = 1;
        for (int c = 0; c < 3; c++) sp.fit_dipole[c] = sp.fit_dipole[c] != 0;
        w->species[w->n_species++] = sp;
        return 1;
    }
    if (strncmp(line, "intfit_result ", 14) == 0) {
        if (!w->initialized) return 1;
        if (sscanf(line + 14, "%d %d %d %d %lf %lf %lf %d %d %d",
                   &w->n_candidates, &w->n_used, &w->n_rejected, &w->n_parameters,
                   &w->rss, &w->rmse, &w->raw_rmse, &w->fit_line_count, &w->fit_blend_count,
                   &w->fit_duplicate_count) == 10) {
            w->has_result = 1;
            w->result_generation++;
        }
        return 1;
    }
    static const struct { const char *key; size_t offset, size; } texts[] = {
        {"intfit_message", offsetof(IntensityFitWindow, message), sizeof(((IntensityFitWindow *)0)->message)},
        {"intfit_report", offsetof(IntensityFitWindow, report), sizeof(((IntensityFitWindow *)0)->report)},
        {"intfit_console", offsetof(IntensityFitWindow, console_log), sizeof(((IntensityFitWindow *)0)->console_log)},
    };
    for (size_t k = 0; k < sizeof(texts) / sizeof(texts[0]); k++) {
        size_t n = strlen(texts[k].key);
        if (strncmp(line, texts[k].key, n) != 0) continue;
        int continuation = line[n] == '+';
        const char *chunk = line + n + continuation;
        if (*chunk != ' ') continue;
        char *text = (char *)w + texts[k].offset;
        if (texts[k].offset == offsetof(IntensityFitWindow, message)) {
            /* The status line is a single line: no newline of its own. */
            size_t at = continuation ? strlen(text) : 0;
            snprintf(text + at, texts[k].size - at, "%s", chunk + 1);
        } else {
            if_session_append(text, texts[k].size, chunk + 1, continuation);
        }
        return 1;
    }
    return 1;
}

void intensity_analysis_open(AppState *s) {
    IntensityFitWindow *w=&s->intensity_window;
    if(w->window){SDL_RaiseWindow(w->window);return;}
    if (!w->fit_running) intensity_analysis_sync_species(s);
    w->window=SDL_CreateWindow("Intensity analysis",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1000,900,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!w->window){snprintf(s->status_message,sizeof(s->status_message),"Could not open Intensity analysis.");return;}
    SDL_SetWindowMinimumSize(w->window, 1000, 760);
    w->renderer=SDL_CreateRenderer(w->window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!w->renderer){SDL_DestroyWindow(w->window);w->window=NULL;snprintf(s->status_message,sizeof(s->status_message),"Could not create Intensity renderer.");return;}
    w->window_id=SDL_GetWindowID(w->window);w->open=1;
}

/* ===========================================================================
 *  Fit preview
 *
 *  The preview is not a plot of its own.  It is the main viewer - the same
 *  app_compute_layout(), render_app() and controller handlers - drawn in a
 *  second window from a private copy of the application state.  The copy
 *  differs from the working state in one respect only: its prediction rows
 *  carry the intensities recalculated with the fitted concentrations,
 *  temperatures and dipoles.  Every broadening, peak-preserving column
 *  reduction, tick and colour therefore comes from view.c itself, and a
 *  narrow line cannot vanish from the preview while it is visible in the
 *  main plot.
 *
 *  The copy shares the experimental point buffers read-only; it never frees
 *  them, and it is rebuilt whenever the working state replaces one.
 * ======================================================================== */

static Layout g_preview_layout;

static int preview_species_for_line(const IntensityFitWindow *w, const PredLine *p) {
    int state = p->n_qn >= 4 ? p->M1l : 0;
    for (int i = 0; i < w->n_species; i++)
        if (w->species[i].hamiltonian_id == p->hamiltonian_id && w->species[i].state_index == state)
            return i;
    return -1;
}

/* One catalogue row as the fit sees it, through Pred&Fit's own per-state
   rescaling.  A state outside the analysis keeps its working intensity: its
   fitted values are the Pred&Fit ones, and they are not re-derived here. */
static void preview_fitted_line(const IntensityFitWindow *w, PredLine *p) {
    int sp = preview_species_for_line(w, p);
    if (sp < 0) return;
    const IntensityFitSpecies *f = &w->species[sp];
    double conc = isfinite(f->fitted_concentration) && f->fitted_concentration >= 0.0
                ? f->fitted_concentration : f->concentration;
    double trot = isfinite(f->fitted_temperature_k) && f->fitted_temperature_k > 0.0
                ? f->fitted_temperature_k : f->temperature_k;
    PickettSpecies one;
    memset(&one, 0, sizeof(one));
    one.state_index = f->state_index;
    one.concentration = conc;
    rescale_predicted_intensities_by_species(p, 1, f->cat_temperature_k, trot, &one, 1, NULL);
    int c = component_index(p->mu);
    if (c >= 0 && f->mu_cat[c] != 0.0 && isfinite(f->fitted_mu[c])) {
        double r = f->fitted_mu[c] / f->mu_cat[c];
        p->linear_int *= r * r;
        p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
    }
}

/* The fitted concentrations carry the experimental units, so the fitted rows
   are not on the intensity scale the working plot's cut and filters were set
   for.  One global constant restores it: the strongest fitted row of the
   analysed states takes the intensity of their strongest working row.  The
   ratios the fit determined are untouched. */
static double preview_fit_anchor(const IntensityFitWindow *w, const PredLine *lines, int n) {
    double fit_max = 0.0, work_max = 0.0;
    for (int i = 0; i < n; i++) {
        if (preview_species_for_line(w, &lines[i]) < 0) continue;
        PredLine fitted = lines[i];
        preview_fitted_line(w, &fitted);
        if (fitted.linear_int > fit_max) fit_max = fitted.linear_int;
        if (lines[i].linear_int > work_max) work_max = lines[i].linear_int;
    }
    double g = fit_max > 0.0 && work_max > 0.0 ? fit_max / work_max : 1.0;
    return isfinite(g) && g > 0.0 ? g : 1.0;
}

static void preview_anchored_line(const IntensityFitWindow *w, PredLine *p, double anchor) {
    if (preview_species_for_line(w, p) < 0) return;
    preview_fitted_line(w, p);
    p->linear_int /= anchor;
    p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
}

static int preview_cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Factor that takes the fitted prediction to the experimental intensity
   scale, for display only: the median of observed/predicted over the very
   transitions the fit selected.  The panes show heights, so an area
   extraction is measured here as the local maximum instead. */
static double preview_experimental_factor(const AppState *s, const AppState *v, int *n_used) {
    const IntensityFitWindow *w = &s->intensity_window;
    *n_used = 0;
    double *ratio = s->n_assignments > 0 ? malloc((size_t)s->n_assignments * sizeof(*ratio)) : NULL;
    int n = 0;
    int mode = w->extraction_mode == 2 ? 1 : w->extraction_mode;
    for (int i = 0; ratio && i < s->n_assignments; i++) {
        const Assignment *a = &s->assignments[i];
        if (!if_transition_selected(s, a, NULL)) continue;
        double observed = 0.0;
        if (!extract_observation(s, a->exp_freq, mode, w->extraction_window_mhz, &observed)) continue;
        PredLine model = a->pred;
        preview_anchored_line(w, &model, w->preview_fit_anchor);
        if (observed > 0.0 && model.linear_int > 0.0 && isfinite(observed / model.linear_int))
            ratio[n++] = observed / model.linear_int;
    }
    double k = 0.0;
    if (n > 0) {
        qsort(ratio, (size_t)n, sizeof(*ratio), preview_cmp_double);
        k = n % 2 ? ratio[n / 2] : 0.5 * (ratio[n / 2 - 1] + ratio[n / 2]);
        *n_used = n;
    }
    free(ratio);
    if (k > 0.0 && isfinite(k)) return k;

    /* No usable observation: match the tallest line to the tallest sample of
       the view the preview opens on. */
    double exp_max = 0.0, pred_max = 0.0;
    if (v->current_pts && v->n_pts > 0) {
        int a = lower_point(v->current_pts, v->n_pts, v->vxmin - v->exp_offset);
        for (int i = a; i < v->n_pts && v->current_pts[i].x <= v->vxmax - v->exp_offset; i++)
            if (v->current_pts[i].y > exp_max) exp_max = v->current_pts[i].y;
    }
    for (int i = 0; i < v->n_pred; i++) {
        double f = v->pred_lines[i].freq_mhz;
        if (f >= v->pvxmin && f <= v->pvxmax && v->pred_lines[i].linear_int > pred_max)
            pred_max = v->pred_lines[i].linear_int;
    }
    k = exp_max > 0.0 && pred_max > 0.0 ? exp_max / pred_max : 1.0;
    return isfinite(k) && k > 0.0 ? k : 1.0;
}

/* Experimental intensity shown as 1 on the preview's axis: the maximum of the
   active trace, over its whole range, so the axis does not change with the
   region the preview happens to open on. */
static double preview_axis_norm(const AppState *v) {
    double norm = 0.0;
    if (v->active_spec >= 0 && v->active_spec < v->n_spectra)
        norm = v->spectra[v->active_spec].ymax;
    if (!(norm > 0.0)) norm = v->ymax;
    return norm > 0.0 && isfinite(norm) ? norm : 1.0;
}

/* Both panes on one relative axis.  A drawn line is linear_int /
   pred_global_max * pred_scale of the prediction pane, and k * linear_int is
   the fitted line in experimental units, so pred_global_max = norm / k makes
   a line of relative intensity r as tall as a sample of r * norm above it. */
static void preview_set_vertical_scale(AppState *v, double k, double top) {
    v->intensity_preview = 1;
    v->intensity_axis_norm = preview_axis_norm(v);
    v->intensity_axis_top = top;
    v->pred_global_max = k > 0.0 ? v->intensity_axis_norm / k : v->intensity_axis_norm;
    viewer_apply_intensity_axis(v);
}

static void preview_record_sources(IntensityFitWindow *w, const AppState *s) {
    w->preview_generation = w->result_generation;
    w->preview_src_pred = s->pred_lines;
    w->preview_src_n_pred = s->n_pred;
    w->preview_src_pred_max = s->pred_global_max;
    w->preview_src_lin = s->lin_data;
    w->preview_src_n_lin = s->n_lin_data;
    w->preview_src_n_assignments = s->n_assignments;
    w->preview_src_n_spectra = s->n_spectra;
    for (int i = 0; i < MAX_SPECTRA; i++) {
        int live = i < s->n_spectra;
        w->preview_src_raw[i] = live ? s->spectra[i].raw_pts : NULL;
        w->preview_src_smooth[i] = live ? s->spectra[i].smooth_pts : NULL;
        w->preview_src_n_pts[i] = live ? s->spectra[i].n_pts : 0;
    }
}

static int preview_sources_changed(const IntensityFitWindow *w, const AppState *s) {
    if (w->preview_generation != w->result_generation) return 1;
    if (w->preview_src_pred != s->pred_lines || w->preview_src_n_pred != s->n_pred) return 1;
    if (w->preview_src_pred_max != s->pred_global_max) return 1;
    if (w->preview_src_lin != s->lin_data || w->preview_src_n_lin != s->n_lin_data) return 1;
    if (w->preview_src_n_assignments != s->n_assignments) return 1;
    if (w->preview_src_n_spectra != s->n_spectra) return 1;
    for (int i = 0; i < s->n_spectra; i++)
        if (w->preview_src_raw[i] != s->spectra[i].raw_pts ||
            w->preview_src_smooth[i] != s->spectra[i].smooth_pts ||
            w->preview_src_n_pts[i] != s->spectra[i].n_pts) return 1;
    return 0;
}

/* Navigation belongs to the preview window once it is open. */
typedef struct {
    double vxmin, vxmax, vymin, vymax, pvxmin, pvxmax, pred_scale;
    double bar_x, pbar_x, measure_x1, exp_offset, intensity_axis_top;
    int sync_active, bar_active, measure_active, measure_phase, show_help;
    double vscale[MAX_SPECTRA], voffset[MAX_SPECTRA], spec_offset[MAX_SPECTRA];
} PreviewView;

static void preview_save_view(const AppState *v, PreviewView *out) {
    out->vxmin = v->vxmin; out->vxmax = v->vxmax; out->vymin = v->vymin; out->vymax = v->vymax;
    out->pvxmin = v->pvxmin; out->pvxmax = v->pvxmax; out->pred_scale = v->pred_scale;
    out->bar_x = v->bar_x; out->pbar_x = v->pbar_x; out->measure_x1 = v->measure_x1;
    out->exp_offset = v->exp_offset;
    out->intensity_axis_top = v->intensity_axis_top;
    out->sync_active = v->sync_active; out->bar_active = v->bar_active;
    out->measure_active = v->measure_active; out->measure_phase = v->measure_phase;
    out->show_help = v->show_help;
    for (int i = 0; i < MAX_SPECTRA; i++) {
        out->vscale[i] = v->spectra[i].vscale;
        out->voffset[i] = v->spectra[i].voffset;
        out->spec_offset[i] = v->spectra[i].exp_offset;
    }
}

static void preview_restore_view(AppState *v, const PreviewView *in) {
    v->vxmin = in->vxmin; v->vxmax = in->vxmax; v->vymin = in->vymin; v->vymax = in->vymax;
    v->pvxmin = in->pvxmin; v->pvxmax = in->pvxmax; v->pred_scale = in->pred_scale;
    v->bar_x = in->bar_x; v->pbar_x = in->pbar_x; v->measure_x1 = in->measure_x1;
    v->exp_offset = in->exp_offset;
    v->sync_active = in->sync_active; v->bar_active = in->bar_active;
    v->measure_active = in->measure_active; v->measure_phase = in->measure_phase;
    v->show_help = in->show_help;
    for (int i = 0; i < MAX_SPECTRA; i++) {
        v->spectra[i].vscale = in->vscale[i];
        v->spectra[i].voffset = in->voffset[i];
        v->spectra[i].exp_offset = in->spec_offset[i];
    }
}

/* Take a fresh copy of the working state.  With keep_view the navigation of
   the preview survives the rebuild; otherwise it opens on the main view. */
static int preview_build(AppState *s, int keep_view) {
    IntensityFitWindow *w = &s->intensity_window;
    PreviewView view;
    if (keep_view && w->preview_state) preview_save_view(w->preview_state, &view);
    else keep_view = 0;

    AppState *v = w->preview_state ? w->preview_state : malloc(sizeof(AppState));
    PredLine *lines = NULL;
    if (!v) return 0;
    if (s->n_pred > 0 && s->pred_lines) {
        lines = malloc((size_t)s->n_pred * sizeof(*lines));
        if (!lines) { if (!w->preview_state) free(v); return 0; }
        memcpy(lines, s->pred_lines, (size_t)s->n_pred * sizeof(*lines));
    }
    free(w->preview_pred_lines);
    w->preview_pred_lines = lines;
    w->preview_state = v;

    memcpy(v, s, sizeof(*v));
    /* Nothing the copy points at may be owned, closed or run through it. */
    memset(&v->intensity_window, 0, sizeof(v->intensity_window));
    v->predfit.advanced_open = 0;
    v->predfit.advanced_window = NULL;
    v->predfit.advanced_renderer = NULL;
    v->predfit.advanced_window_id = 0;
    v->predfit.history = NULL;
    v->predfit.history_count = v->predfit.history_capacity = 0;
    v->settings.open = 0;
    v->settings.window = NULL;
    v->settings.renderer = NULL;
    v->settings.window_id = 0;
    v->pending_load_count = 0;
    v->pending_select = v->pending_remove = -1;
    v->export_requested = 0;
    v->session_has_view = 0;
    v->input_state = INPUT_NONE;
    v->drag_target = NULL;
    v->selecting_left = v->selecting_right = 0;
    v->dragging_offset = 0;
    v->n_selected = 0;
    v->error_message[0] = '\0';
    DraggableWindow *panels[] = {&v->win_as, &v->win_pf, &v->win_avg, &v->win_br, &v->win_dip,
                                 &v->win_cut, &v->win_filt, &v->win_jump, &v->win_spec, &v->win_predfit};
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) { panels[i]->visible = 0; panels[i]->anim = 0.0f; }
    v->viewer_readonly = 1;

    v->pred_lines = lines;
    v->n_pred = lines ? s->n_pred : 0;
    w->preview_fit_anchor = preview_fit_anchor(w, lines, v->n_pred);
    double max_int = 0.0;
    for (int i = 0; i < v->n_pred; i++) {
        preview_anchored_line(w, &lines[i], w->preview_fit_anchor);
        if (lines[i].linear_int > max_int) max_int = lines[i].linear_int;
    }
    v->pred_global_max = max_int > 0.0 ? max_int : 1.0;

    if (keep_view) preview_restore_view(v, &view);
    w->preview_intensity_scale = preview_experimental_factor(s, v, &w->preview_scale_lines);
    /* One shared axis needs one overlaid, commonly scaled experimental pane. */
    v->multi_layout = 0;
    v->multi_ynorm = 0;
    preview_set_vertical_scale(v, w->preview_intensity_scale, keep_view ? view.intensity_axis_top : 1.0);
    snprintf(v->view_caption, sizeof(v->view_caption),
             "fit preview, not applied \xC2\xB7 intensity 1 = experimental maximum");
    preview_record_sources(w, s);
    return 1;
}

/* Presentation choices keep following the working state while the preview is
   open: colours, widths, broadening, the intensity cut and the filters. */
static void preview_follow_presentation(const AppState *s, AppState *v) {
    AppSettings settings = s->settings;
    settings.open = 0;
    settings.window = NULL;
    settings.renderer = NULL;
    settings.window_id = 0;
    v->settings = settings;
    v->broadening_active = s->broadening_active;
    v->broaden_mode = s->broaden_mode;
    v->lorentz_gamma = s->lorentz_gamma;
    v->gauss_gamma = s->gauss_gamma;
    v->kaiser_beta = s->kaiser_beta;
    v->kaiser_ceros = s->kaiser_ceros;
    v->kaiser_intrinsic = s->kaiser_intrinsic;
    v->pred_min_log_int = s->pred_min_log_int;
    v->pred_max_log_int = s->pred_max_log_int;
    v->filter_active = s->filter_active;
    memcpy(v->filt_mu, s->filt_mu, sizeof(v->filt_mu));
    memcpy(v->filt_br, s->filt_br, sizeof(v->filt_br));
    v->filt_use_range = s->filt_use_range;
    v->filt_j_min = s->filt_j_min;   v->filt_j_max = s->filt_j_max;
    v->filt_ka_min = s->filt_ka_min; v->filt_ka_max = s->filt_ka_max;
    v->filt_kc_min = s->filt_kc_min; v->filt_kc_max = s->filt_kc_max;
    v->filt_use_delta = s->filt_use_delta;
    v->filt_dj = s->filt_dj; v->filt_dka = s->filt_dka; v->filt_dkc = s->filt_dkc;
    v->predfit.species_trace_mode = s->predfit.species_trace_mode;
    v->intensity_window.open = s->intensity_window.open;   /* the rail marks the open tool */
    /* Stacked or per-trace scaled panes cannot share the preview's axis. */
    v->multi_layout = 0;
    v->multi_ynorm = 0;
    v->multi_indiv_int = s->multi_indiv_int;
    v->rolling_avg_window = s->rolling_avg_window;
    for (int i = 0; i < s->n_spectra && i < v->n_spectra; i++) {
        v->spectra[i].color = s->spectra[i].color;
        v->spectra[i].opacity = s->spectra[i].opacity;
        v->spectra[i].visible = s->spectra[i].visible;
        v->spectra[i].rolling_avg_active = s->spectra[i].rolling_avg_active;
        v->spectra[i].current_pts = s->spectra[i].current_pts;
    }
    if (v->active_spec >= 0 && v->active_spec < v->n_spectra) {
        const Spectrum *S = &v->spectra[v->active_spec];
        v->current_pts = S->current_pts;
        v->rolling_avg_active = S->rolling_avg_active;
    }
}

AppState *intensity_analysis_preview_state(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (!w->has_result && !w->preview_state) return NULL;
    /* While another fit runs, the species still hold the result on show, so a
       rebuild for replaced buffers keeps presenting that result. */
    if (!w->preview_state) {
        if (!preview_build(s, 0)) return NULL;
    } else if (preview_sources_changed(w, s)) {
        if (!preview_build(s, 1)) return NULL;
    }
    preview_follow_presentation(s, w->preview_state);
    return w->preview_state;
}

static void preview_close(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->preview_renderer) {
        plotgpu_release(w->preview_renderer);
        SDL_DestroyRenderer(w->preview_renderer);
    }
    if (w->preview_window) SDL_DestroyWindow(w->preview_window);
    w->preview_renderer = NULL;
    w->preview_window = NULL;
    w->preview_window_id = 0;
    w->preview_open = 0;
    free(w->preview_state);
    free(w->preview_pred_lines);
    w->preview_state = NULL;
    w->preview_pred_lines = NULL;
    memset(&g_preview_layout, 0, sizeof(g_preview_layout));
}

static void preview_window_title(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (!w->preview_window) return;
    char title[256];
    if (w->preview_scale_lines > 0)
        snprintf(title, sizeof(title),
                 "Intensity fit preview \xE2\x80\x94 not applied to Pred&Fit \xC2\xB7 prediction \xC3\x97%.4g "
                 "to experimental intensity (median of %d fitted lines, display only)",
                 w->preview_intensity_scale, w->preview_scale_lines);
    else
        snprintf(title, sizeof(title),
                 "Intensity fit preview \xE2\x80\x94 not applied to Pred&Fit \xC2\xB7 prediction \xC3\x97%.4g "
                 "to experimental intensity (peak ratio in view, display only)",
                 w->preview_intensity_scale);
    SDL_SetWindowTitle(w->preview_window, title);
}

static void open_preview(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (!w->has_result) {
        snprintf(w->message, sizeof(w->message), "Run a fit first: the preview shows its result.");
        return;
    }
    if (w->preview_window) { SDL_RaiseWindow(w->preview_window); return; }
    if (!intensity_analysis_preview_state(s)) {
        snprintf(w->message, sizeof(w->message), "Not enough memory for the preview.");
        return;
    }
    /* The main window's size and constraints, so the preview lays out like it. */
    int ww = 1400, wh = 884;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        if (ww > usable.w - 40) ww = usable.w - 40;
        if (wh > usable.h - 40) wh = usable.h - 40;
    }
    w->preview_window = SDL_CreateWindow("Intensity fit preview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w->preview_window) { preview_close(s); return; }
    SDL_SetWindowMinimumSize(w->preview_window, 900, 560);
    w->preview_renderer = SDL_CreateRenderer(w->preview_window, -1,
                                             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!w->preview_renderer) { preview_close(s); return; }
    SDL_SetRenderDrawBlendMode(w->preview_renderer, SDL_BLENDMODE_BLEND);
    w->preview_window_id = SDL_GetWindowID(w->preview_window);
    w->preview_open = 1;
    preview_window_title(s);
}

static Uint32 preview_event_window(const SDL_Event *e) {
    switch (e->type) {
        case SDL_WINDOWEVENT:     return e->window.windowID;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:   return e->button.windowID;
        case SDL_MOUSEMOTION:     return e->motion.windowID;
        case SDL_MOUSEWHEEL:      return e->wheel.windowID;
        case SDL_KEYDOWN:
        case SDL_KEYUP:           return e->key.windowID;
        case SDL_TEXTINPUT:       return e->text.windowID;
        case SDL_DROPFILE:
        case SDL_DROPTEXT:        return e->drop.windowID;
        default:                  return 0;
    }
}

static int preview_handle_event(AppState *s, const SDL_Event *e) {
    IntensityFitWindow *w = &s->intensity_window;
    if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_CLOSE) {
        preview_close(s);
        return 1;
    }
    if (e->type == SDL_DROPFILE || e->type == SDL_DROPTEXT) {
        SDL_free(e->drop.file);          /* nothing is ever loaded into a preview */
        return 1;
    }
    AppState *v = w->preview_state;
    if (!v) return 1;
    if (g_preview_layout.win_w <= 0 && w->preview_window) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w->preview_window, &ww, &wh);
        app_compute_layout(v, &g_preview_layout, ww, wh);
    }
    handle_viewer_event(v, &g_preview_layout, e);
    return 1;
}

static void render_preview(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (!w->preview_open || !w->preview_renderer) return;
    double shown_scale = w->preview_intensity_scale;
    AppState *v = intensity_analysis_preview_state(s);
    if (!v) { preview_close(s); return; }
    if (shown_scale != w->preview_intensity_scale) preview_window_title(s);

    SDL_Renderer *r = w->preview_renderer;
    int ww = 0, wh = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(w->preview_window, &ww, &wh);
    SDL_GetRendererOutputSize(r, &dw, &dh);
    float density = ww > 0 && dw > 0 ? (float)dw / (float)ww : 1.0f;
    SDL_RenderSetScale(r, density, density);

    /* The same frame sequence as the main loop: layout, end-of-input state,
       render.  Events arrived earlier in the frame against the layout drawn
       last time, which is what the main window does too. */
    app_compute_layout(v, &g_preview_layout, ww, wh);
    app_sync_view_state(v);
    render_app(r, ui_font(UI_FONT_SANS), v, &g_preview_layout);
}

void intensity_analysis_close(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window) SDL_DestroyWindow(w->window);
    w->renderer = NULL;
    w->window = NULL;
    w->window_id = 0;
    w->open = 0;
}

void intensity_analysis_dispose(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->fit_running && w->fit_pid > 0) kill((pid_t)w->fit_pid, SIGTERM);
    preview_close(s);
    intensity_analysis_close(s);
}

int intensity_analysis_handle_event(AppState *s, const SDL_Event *e) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->preview_open && w->preview_window_id && preview_event_window(e) == w->preview_window_id)
        return preview_handle_event(s, e);
    Uint32 id = e->type == SDL_WINDOWEVENT ? e->window.windowID :
                e->type == SDL_MOUSEBUTTONDOWN ? e->button.windowID :
                e->type == SDL_MOUSEBUTTONUP ? e->button.windowID :
                e->type == SDL_MOUSEMOTION ? e->motion.windowID :
                e->type == SDL_MOUSEWHEEL ? e->wheel.windowID :
                e->type == SDL_KEYDOWN ? e->key.windowID :
                e->type == SDL_TEXTINPUT ? e->text.windowID : 0;
    if (!w->open || !w->window_id || id != w->window_id) return 0;
    if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_CLOSE) {
        intensity_analysis_close(s); return 1;
    }
    if (e->type == SDL_TEXTINPUT && w->edit_field) {
        size_t n = strlen(w->edit_text), a = strlen(e->text.text);
        if (n + a < sizeof(w->edit_text)) memcpy(w->edit_text + n, e->text.text, a + 1);
        return 1;
    }
    if (e->type == SDL_KEYDOWN && w->edit_field) {
        if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) commit_edit(w);
        else if (e->key.keysym.sym == SDLK_ESCAPE) { w->edit_field = 0; SDL_StopTextInput(); }
        else if (e->key.keysym.sym == SDLK_BACKSPACE) { size_t n = strlen(w->edit_text); if (n) w->edit_text[n - 1] = '\0'; }
        return 1;
    }
    int ww, hh; SDL_GetWindowSize(w->window, &ww, &hh);
    if (e->type == SDL_MOUSEWHEEL) {
        int mx, my; SDL_GetMouseState(&mx, &my);
        if (point_in_rect(mx, my, if_log_rect(ww, hh))) w->report_scroll -= e->wheel.y;
        else w->species_scroll -= e->wheel.y;
        if (w->report_scroll < 0) w->report_scroll = 0;
        if (w->species_scroll < 0) w->species_scroll = 0;
        int max_species = w->n_species - if_visible_species_rows(hh);
        if (max_species < 0) max_species = 0;
        if (w->species_scroll > max_species) w->species_scroll = max_species;
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.button != SDL_BUTTON_LEFT) return 1;
    int x = e->button.x, y = e->button.y;
    if (w->edit_field) commit_edit(w);
    if (point_in_rect(x,y,if_button(18,52,104))) { w->fit_concentration = !w->fit_concentration; return 1; }
    if (point_in_rect(x,y,if_button(128,52,104))) { w->fit_temperature = !w->fit_temperature; return 1; }
    if (point_in_rect(x,y,if_button(238,52,104))) { w->fit_dipoles = !w->fit_dipoles; return 1; }
    if (point_in_rect(x,y,if_button(348,52,104))) { w->common_temperature = !w->common_temperature; return 1; }
    for (int i=0;i<3;i++) {
        if (point_in_rect(x,y,if_button(18+i*54,86,48))) { w->branch_enabled[i] = !w->branch_enabled[i]; return 1; }
        if (point_in_rect(x,y,if_button(192+i*54,86,48))) { w->mu_enabled[i] = !w->mu_enabled[i]; return 1; }
    }
    if (point_in_rect(x,y,if_button(365,86,95))) { w->extraction_mode = (w->extraction_mode + 1) % 3; return 1; }
    if (point_in_rect(x,y,if_button(467,86,95))) { w->fit_mode = !w->fit_mode; return 1; }
    if (point_in_rect(x,y,if_field(690,52))) { begin_edit(w,IF_EDIT_FMIN,w->fmin_mhz); return 1; }
    if (point_in_rect(x,y,if_field(850,52))) { begin_edit(w,IF_EDIT_FMAX,w->fmax_mhz); return 1; }
    if (point_in_rect(x,y,if_field(690,86))) { begin_edit(w,IF_EDIT_WINDOW,w->extraction_window_mhz); return 1; }
    if (point_in_rect(x,y,if_field(850,86))) { begin_edit(w,IF_EDIT_FRACTION,w->intensity_uncertainty_fraction); return 1; }
    int actions_y = if_actions_y(hh);
    if (point_in_rect(x,y,if_button(18,actions_y,90))) { run_fit(s); return 1; }
    if (point_in_rect(x,y,if_button(116,actions_y,90))) { export_report(s); return 1; }
    if (point_in_rect(x,y,if_button(214,actions_y,110))) { open_preview(s); return 1; }
    for (int row=0; row<if_visible_species_rows(hh); row++) {
        int i = w->species_scroll + row;
        if (i >= w->n_species) break;
        SDL_Rect row_rect = if_species_row(150,row);
        if (!point_in_rect(x,y,row_rect)) continue;
        IntensityFitSpecies *sp = &w->species[i];
        if (x < 48) { sp->included = !sp->included; return 1; }
        if (x >= 510 && x < 580) { sp->temperature_group = sp->temperature_group % MAX_INTFIT_GROUPS + 1; return 1; }
        if (x >= 760 && x < 910) { int c=(x-760)/50; if(c>=0&&c<3) { sp->fit_dipole[c]=!sp->fit_dipole[c]; return 1; } }
    }
    return 1;
}

void intensity_analysis_render(AppState *s) {
    IntensityFitWindow *w = &s->intensity_window;
    if (w->open && w->renderer) {
        SDL_Renderer *r = w->renderer; int ww, hh;
        SDL_GetWindowSize(w->window, &ww, &hh);
        SDL_SetRenderDrawColor(r,19,20,22,255); SDL_RenderClear(r);
        ui_text(r,UI_FONT_TITLE,"Intensity analysis",18,14,UI_TEXT);
        ui_text(r,UI_FONT_SANS_SM,"Python intensity_fit workflow — preview-only results",18,35,UI_ACCENT_TEXT);
        int mx,my; int down=(SDL_GetMouseState(&mx,&my)&SDL_BUTTON(SDL_BUTTON_LEFT))!=0;
        ui_button(r,if_button(18,52,104),"Fit conc.",-1,UI_BTN_QUIET,w->fit_concentration,mx,my,down);
        ui_button(r,if_button(128,52,104),"Fit Trot",-1,UI_BTN_QUIET,w->fit_temperature,mx,my,down);
        ui_button(r,if_button(238,52,104),"Fit dipoles",-1,UI_BTN_QUIET,w->fit_dipoles,mx,my,down);
        ui_button(r,if_button(348,52,104),w->common_temperature?"Common T":"Groups",-1,UI_BTN_QUIET,w->common_temperature,mx,my,down);
        const char *br[]={"P","Q","R"}, *mu[]={"mu a","mu b","mu c"};
        for(int i=0;i<3;i++) { ui_button(r,if_button(18+i*54,86,48),br[i],-1,UI_BTN_QUIET,w->branch_enabled[i],mx,my,down); ui_button(r,if_button(192+i*54,86,48),mu[i],-1,UI_BTN_QUIET,w->mu_enabled[i],mx,my,down); }
        ui_button(r,if_button(365,86,95),w->extraction_mode==0?"sample":w->extraction_mode==1?"local max":"area",-1,UI_BTN_QUIET,0,mx,my,down);
        ui_button(r,if_button(467,86,95),w->fit_mode?"spectrum":"line fit",-1,UI_BTN_QUIET,w->fit_mode,mx,my,down);
        char b[96]; snprintf(b,sizeof(b),"%.6g",w->fmin_mhz); ui_field(r,if_field(690,52),"f min",w->edit_field==IF_EDIT_FMIN?w->edit_text:b,w->edit_field==IF_EDIT_FMIN);
        snprintf(b,sizeof(b),"%.6g",w->fmax_mhz); ui_field(r,if_field(850,52),"f max",w->edit_field==IF_EDIT_FMAX?w->edit_text:b,w->edit_field==IF_EDIT_FMAX);
        snprintf(b,sizeof(b),"%.5g",w->extraction_window_mhz); ui_field(r,if_field(690,86),"window",w->edit_field==IF_EDIT_WINDOW?w->edit_text:b,w->edit_field==IF_EDIT_WINDOW);
        snprintf(b,sizeof(b),"%.3g",w->intensity_uncertainty_fraction); ui_field(r,if_field(850,86),"unc. frac",w->edit_field==IF_EDIT_FRACTION?w->edit_text:b,w->edit_field==IF_EDIT_FRACTION);
        SDL_Rect head={18,124,ww-36,22}; ui_fill(r,head,UI_INPUT);
        ui_text(r,UI_FONT_MONO_SM,"USE  SPECIES / mu cat(a,b,c)          Tcat    Trot    concentration   group     mu fit (a b c)",head.x+7,head.y+4,UI_DIM);
        for(int row=0;row<if_visible_species_rows(hh);row++) {
            int i=w->species_scroll+row; if(i>=w->n_species) break;
            IntensityFitSpecies *sp=&w->species[i]; SDL_Rect rr=if_species_row(150,row); rr.w=ww-36;
            ui_fill(r,rr,sp->included?UI_PANEL:UI_INPUT); ui_frame(r,rr,UI_LINE_SOFT);
            ui_text(r,UI_FONT_MONO_SM,sp->included?"[x]":"[ ]",rr.x+7,rr.y+5,sp->included?UI_OK:UI_FAINT);
            ui_text(r,UI_FONT_SANS_SM,sp->name,rr.x+48,rr.y+5,sp->included?UI_TEXT:UI_DIM);
            snprintf(b,sizeof(b),"%.3g/%.3g/%.3g",sp->mu_cat[0],sp->mu_cat[1],sp->mu_cat[2]); ui_text(r,UI_FONT_MONO_SM,b,rr.x+215,rr.y+5,UI_DIM);
            snprintf(b,sizeof(b),"%6.2f",sp->cat_temperature_k); ui_text(r,UI_FONT_MONO_SM,b,rr.x+420,rr.y+5,UI_DIM);
            snprintf(b,sizeof(b),"%6.2f",sp->temperature_k); ui_text(r,UI_FONT_MONO_SM,b,rr.x+500,rr.y+5,UI_DIM);
            snprintf(b,sizeof(b),"%10.4g",sp->concentration); ui_text(r,UI_FONT_MONO_SM,b,rr.x+580,rr.y+5,UI_DIM);
            snprintf(b,sizeof(b),"G%d",w->common_temperature?1:sp->temperature_group); ui_button(r,(SDL_Rect){rr.x+690,rr.y+2,52,22},b,-1,UI_BTN_QUIET,0,mx,my,down);
            for(int c=0;c<3;c++) ui_button(r,(SDL_Rect){rr.x+742+c*50,rr.y+2,46,22},mu[c]+3,-1,UI_BTN_QUIET,sp->fit_dipole[c],mx,my,down);
        }
        int actions_y=if_actions_y(hh); ui_hline(r,18,ww-18,actions_y-10,UI_LINE);
        ui_button(r,if_button(18,actions_y,90),"Run fit",-1,UI_BTN_PRIMARY,0,mx,my,down);
        ui_button(r,if_button(116,actions_y,90),"Export txt",-1,UI_BTN_QUIET,0,mx,my,down);
        ui_button(r,if_button(214,actions_y,110),"Open preview",-1,UI_BTN_QUIET,0,mx,my,down);
        ui_text(r,UI_FONT_SANS_SM,w->message[0]?w->message:"Select species, filters and free parameters; source values stay untouched.",342,actions_y+6,w->has_result?UI_OK:UI_DIM);
        render_fit_log(r,w,ww,hh); SDL_RenderPresent(r);
    }
    render_preview(s);
}
