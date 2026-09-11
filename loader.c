#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- PRIVATE HELPER FUNCTIONS ---

typedef enum { SEP_COMMA, SEP_TAB, SEP_SPACE } Separator;

static int compare_point_frequency(const void *a, const void *b) {
    const Point *x = a, *y = b;
    return (x->x > y->x) - (x->x < y->x);
}

/* One two-character QN field of a .cat record, decoded like Pickett's readqn
   (calpgm/catutil.c:10-60): a blank field is 0; the first character is a
   blank, a tens digit, '-' (-1..-9), an upper-case letter for the hundreds
   (A0 = 100) or a lower-case one for -10 and below (a0 = -10, a1 = -11). */
static int parse_qn2(const char *s) {
    char tens = s[0], units = s[1];
    if (units < '0' || units > '9') return 0;
    int v = units - '0';
    if (tens == ' ')                return v;
    if (tens == '-')                return -v;
    if (tens >= '0' && tens <= '9') return v + 10 * (tens - '0');
    if (tens >= 'A' && tens <= 'Z') return v + 10 * (tens - 'A' + 10);
    if (tens >= 'a' && tens <= 'z') return -v - 10 * (tens - 'a' + 1);
    return 0;
}

static char branch_from_qn(int Ju,int Jl) {
    int dJ = Ju - Jl;
    if (dJ ==  1) return 'R';
    if (dJ == -1) return 'P';
    if (dJ ==  0) return 'Q';
    return '?';
}

static char mu_from_qn(int Kau,int Kal,int Kcu,int Kcl) {
    int dKa = abs(Kau - Kal);
    int dKc = abs(Kcu - Kcl);
    int eKa = (dKa % 2 == 0);
    int eKc = (dKc % 2 == 0);
    if ( eKa && !eKc) return 'a';
    if (!eKa && !eKc) return 'b';
    if (!eKa &&  eKc) return 'c';
    return '?';
}

/* A .cat record is fixed width (calpgm/calcat.c:700-709): FREQ 13, ERR 8,
   LGINT 8, DR 2, ELO 10, GUP 3, TAG 7, QNFMT 4, then twelve two-character QN
   fields (six upper, six lower).  Neighbouring fields can touch - an ERR of
   158.2229 follows the frequency without a blank, a three-digit GUP follows
   ELO - so every field is cut at its own columns before it is converted. */
enum { CAT_FREQ = 0, CAT_ERR = 13, CAT_LGINT = 21, CAT_DR = 29, CAT_ELO = 31,
       CAT_QNFMT = 51, CAT_QN = 55 };

/* Field [start, start + width) as a number.  Columns past the end of the line
   count as blank; a blank or non-numeric field is rejected. */
static int cat_number(const char *line, int len, int start, int width, double *value) {
    char field[16];
    int n = 0;
    for (int i = start; i < start + width && i < len; i++) field[n++] = line[i];
    field[n] = '\0';
    char *end = NULL;
    *value = strtod(field, &end);
    if (end == field) return 0;
    while (*end == ' ') end++;
    return *end == '\0';
}

int parse_cat_record(const char *line, PredLine *pl, double *err_mhz) {
    int len = (int)strcspn(line, "\r\n");
    double freq, err, lgint, dr, elo, qnfmt;
    if (len < CAT_QN) return 0;
    if (!cat_number(line, len, CAT_FREQ, 13, &freq) || !cat_number(line, len, CAT_ERR, 8, &err) ||
        !cat_number(line, len, CAT_LGINT, 8, &lgint) || !cat_number(line, len, CAT_QNFMT, 4, &qnfmt))
        return 0;
    /* QNFMT's final digit is NQN, the number of quantum numbers printed for
       each state; Pickett reads 0 as 10 (calpgm/ulib.c:778).  Preserve it with
       the transition rather than deriving a format from zeros, spin, or the
       UI.  At most six QN per state are kept, so NQN 0 and NQN above 6 are
       not supported (D6 of docs/audit/PIANO-FIX.md). */
    int nqn = (int)qnfmt % 10;
    if (nqn < 1 || nqn > 6) return -1;
    if (!cat_number(line, len, CAT_DR, 2, &dr)) dr = 0.0;
    if (!cat_number(line, len, CAT_ELO, 10, &elo)) elo = 0.0;

    memset(pl, 0, sizeof(*pl));
    pl->freq_mhz = freq;
    pl->lgint = lgint;
    pl->cat_lgint = lgint;
    pl->linear_int = pow(10.0, lgint);
    pl->rot_dof = (int)dr;
    pl->elo_cm = elo;
    pl->n_qn = nqn;

    /* A record saved without its trailing blanks simply has blank QN fields. */
    char qn[24];
    for (int i = 0; i < 24; i++) qn[i] = CAT_QN + i < len ? line[CAT_QN + i] : ' ';
    int *field[12] = {&pl->Ju, &pl->Kau, &pl->Kcu, &pl->M1u, &pl->M2u, &pl->M3u,
                      &pl->Jl, &pl->Kal, &pl->Kcl, &pl->M1l, &pl->M2l, &pl->M3l};
    for (int k = 0; k < 12; k++) *field[k] = parse_qn2(qn + 2 * k);
    pl->branch = branch_from_qn(pl->Ju, pl->Jl);
    pl->mu     = mu_from_qn(pl->Kau, pl->Kal, pl->Kcu, pl->Kcl);
    if (err_mhz) *err_mhz = err;
    return 1;
}

static int dipole_index(char mu) {
    if (mu == 'a') return 0;
    if (mu == 'b') return 1;
    if (mu == 'c') return 2;
    return -1;
}

static int parse_line(const char *line, double *a, double *b, Separator sep) {
    if (sep == SEP_COMMA) return (sscanf(line, "%lf , %lf", a, b) == 2);
    if (sep == SEP_TAB)   return (sscanf(line, "%lf\t%lf", a, b) == 2);
    return (sscanf(line, "%lf %lf", a, b) == 2);
}

// --- PUBLIC FUNCTIONS ---

static void trim_whitespace(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
    if (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static int parse_config_line(char *line, char *out_path, int out_size) {
    trim_whitespace(line);
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') return 0;

    char *eq = strchr(line, '=');
    if (eq) {
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);
        if (strcmp(key, "assigned_file") != 0 &&
            strcmp(key, "lin_file") != 0 &&
            strcmp(key, "assigned_lines") != 0) {
            return 0;
        }
        snprintf(out_path, out_size, "%s", value);
        return out_path[0] != '\0';
    }

    snprintf(out_path, out_size, "%s", line);
    return out_path[0] != '\0';
}

int find_assigned_frequency_file(char *out_path, int out_size) {
    const char *configs[] = {
        "spectravisual.ini",
        "liveplot.ini",
        "assignments.ini",
        "config.ini"
    };

    for (int i = 0; i < (int)(sizeof(configs) / sizeof(configs[0])); i++) {
        FILE *f = fopen(configs[i], "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (parse_config_line(line, out_path, out_size)) {
                fclose(f);
                printf("Using assigned frequency file from %s: %s\n", configs[i], out_path);
                return 1;
            }
        }
        fclose(f);
    }

    FILE *fallback = fopen("assigned.lin", "r");
    if (fallback) {
        fclose(fallback);
        snprintf(out_path, out_size, "%s", "assigned.lin");
        printf("Using fallback assigned frequency file: %s\n", out_path);
        return 1;
    }
    return 0;
}

static int parse_assigned_frequency_line(const char *line, double *freq) {
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') return 0;

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", line);

    char *comment = strchr(buf, '#');
    if (comment) *comment = '\0';
    comment = strchr(buf, ';');
    if (comment) *comment = '\0';

    char *tok = strtok(buf, " \t,|");
    double values[32];
    int n_values = 0;

    while (tok && n_values < (int)(sizeof(values) / sizeof(values[0]))) {
        char *end = NULL;
        double v = strtod(tok, &end);
        if (end != tok) {
            values[n_values++] = v;
        }
        tok = strtok(NULL, " \t,|");
    }

    if (n_values == 1) {
        *freq = values[0];
        return 1;
    }

    for (int qn_count = 6; qn_count <= 12; qn_count += 2) {
        if (n_values == qn_count + 1 || n_values == qn_count + 3) {
            if (fabs(values[qn_count]) > 1000.0) {
                *freq = values[qn_count];
                return 1;
            }
        }
    }

    for (int i = n_values - 1; i >= 0; i--) {
        if (fabs(values[i]) > 1000.0) {
            *freq = values[i];
            return 1;
        }
    }
    return 0;
}

int read_assigned_frequencies(const char *fname, double *out, int maxn) {
    FILE *f = fopen(fname, "r");
    if (!f) return 0;
    
    int n = 0;
    char line[512];

    while (n < maxn && fgets(line, sizeof(line), f)) {
        double freq = 0.0;
        if (parse_assigned_frequency_line(line, &freq)) {
            out[n++] = freq;
        }
    }
    
    fclose(f);
    printf("Loaded %d assigned frequencies from %s\n", n, fname);
    return n;
}

static int compare_pred_frequency(const void *a, const void *b) {
    const PredLine *pa = a, *pb = b;
    return (pa->freq_mhz > pb->freq_mhz) - (pa->freq_mhz < pb->freq_mhz);
}

int read_pred_cat(const char *fname, PredLine *out, int maxn,
                  double *xmin, double *xmax,
                  double *global_max_int)
{
    FILE *f = fopen(fname, "r");
    if (!f) return 0;

    char line[512];
    int n = 0;

    *xmin =  1e99; *xmax = -1e99;
    *global_max_int = -1.0;

    while (fgets(line, sizeof(line), f) && n < maxn) {
        PredLine pl;
        if (parse_cat_record(line, &pl, NULL) != 1) continue;
        out[n++] = pl;
        if (pl.freq_mhz < *xmin) *xmin = pl.freq_mhz;
        if (pl.freq_mhz > *xmax) *xmax = pl.freq_mhz;
        if (pl.linear_int > *global_max_int) *global_max_int = pl.linear_int;
    }
    fclose(f);
    /* The drawing, profile and picking paths use binary searches in frequency.
       SPCAT files are normally ordered, but a combined multi-species catalogue
       is assembled in species blocks and is not.  Keep that implementation
       detail out of every caller by restoring the required invariant here. */
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(*out), compare_pred_frequency);
    }
    return n;
}

int read_pred_cat_alloc_counted(const char *fname, PredLine **out,
                                double *xmin, double *xmax,
                                double *global_max_int, int *n_unsupported)
{
    if (n_unsupported) *n_unsupported = 0;
    FILE *f = fopen(fname, "r");
    if (!f) return 0;

    int cap = 16384;
    int n = 0;
    PredLine *arr = malloc(sizeof(PredLine) * cap);
    if (!arr) {
        fclose(f);
        return 0;
    }

    char line[512];
    *xmin =  1e99; *xmax = -1e99;
    *global_max_int = -1.0;

    while (fgets(line, sizeof(line), f)) {
        PredLine pl;
        int parsed = parse_cat_record(line, &pl, NULL);
        if (parsed < 0 && n_unsupported) (*n_unsupported)++;
        if (parsed != 1) continue;

        if (n >= cap) {
            int new_cap = cap * 2;
            PredLine *tmp = realloc(arr, sizeof(PredLine) * new_cap);
            if (!tmp) break;
            arr = tmp;
            cap = new_cap;
        }

        arr[n++] = pl;
        if (pl.freq_mhz < *xmin) *xmin = pl.freq_mhz;
        if (pl.freq_mhz > *xmax) *xmax = pl.freq_mhz;
        if (pl.linear_int > *global_max_int) *global_max_int = pl.linear_int;
    }
    fclose(f);

    if (n == 0) {
        free(arr);
        *out = NULL;
        return 0;
    }

    if (n > 1) {
        qsort(arr, (size_t)n, sizeof(*arr), compare_pred_frequency);
    }
    PredLine *shrunk = realloc(arr, sizeof(PredLine) * n);
    *out = shrunk ? shrunk : arr;
    return n;
}

int read_pred_cat_alloc(const char *fname, PredLine **out,
                        double *xmin, double *xmax,
                        double *global_max_int)
{
    return read_pred_cat_alloc_counted(fname, out, xmin, xmax, global_max_int, NULL);
}

void rescale_predicted_intensities(PredLine *lines, int n, double cat_temp_k,
                                   double rot_temp_k,
                                   const double dipole_cat[3],
                                   const double dipole_red[3],
                                   double *global_max_int)
{
    // A .cat does not store the temperature used to calculate LGINT.  When
    // both temperatures are supplied, rescale its integrated LTE intensity:
    // I(T)/I(Tcat) = Q(Tcat)/Q(T) exp[-c2 E_l (1/T - 1/Tcat)]
    //                 * (1-exp(-c2 nu/T)) / (1-exp(-c2 nu/Tcat)).
    const double c2 = 1.438776877;       // hc/k_B, K cm
    const double mhz_per_cm = 29979.2458;
    int valid_temps = isfinite(cat_temp_k) && cat_temp_k > 0.0
                   && isfinite(rot_temp_k) && rot_temp_k > 0.0;

    double max_int = -1.0;
    for (int i = 0; i < n; i++) {
        PredLine *p = &lines[i];
        if (!valid_temps) {
            p->lgint = p->cat_lgint;
            p->linear_int = pow(10.0, p->cat_lgint);
            p->line_strength = 0.0;
            if (p->linear_int > max_int) max_int = p->linear_int;
            continue;
        }
        double nu_cm = p->freq_mhz / mhz_per_cm;
        double stim_t = -expm1(-c2 * nu_cm / rot_temp_k);
        double stim_cat = -expm1(-c2 * nu_cm / cat_temp_k);
        double pop_t = exp(-c2 * p->elo_cm / rot_temp_k) * stim_t
                     / pow(rot_temp_k, 0.5 * p->rot_dof);
        double pop_cat = exp(-c2 * p->elo_cm / cat_temp_k) * stim_cat
                       / pow(cat_temp_k, 0.5 * p->rot_dof);
        int component = dipole_index(p->mu);
        double mu_cat = component >= 0 && dipole_cat ? dipole_cat[component] : 0.0;
        double mu_red = component >= 0 && dipole_red ? dipole_red[component] : 0.0;

        // With both dipoles specified, recover the catalog-independent line
        // strength S = Icat / (population(Tcat) * mu_cat^2), then predict
        // I(Trot) = S * population(Trot) * mu_red^2.  If a component was not
        // supplied, retain the catalog dipole for that component.
        if (pop_cat > 0.0 && mu_cat != 0.0 && mu_red != 0.0) {
            p->line_strength = pow(10.0, p->cat_lgint) / (pop_cat * mu_cat * mu_cat);
            p->linear_int = p->line_strength * pop_t * mu_red * mu_red;
        } else {
            p->line_strength = 0.0;
            p->linear_int = pop_cat > 0.0 ? pow(10.0, p->cat_lgint) * pop_t / pop_cat
                                          : pow(10.0, p->cat_lgint);
        }
        p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
        if (p->linear_int > max_int) max_int = p->linear_int;
    }
    if (global_max_int) *global_max_int = max_int;
}

void rescale_predicted_intensities_by_species(PredLine *lines, int n,
                                              double cat_temp_k,
                                              const PickettSpecies *species, int n_species,
                                              double *global_max_int)
{
    const double c2 = 1.438776877;
    const double mhz_per_cm = 29979.2458;
    double max_int = -1.0;
    for (int i = 0; i < n; i++) {
        PredLine *p = &lines[i];
        /* In SPCAT's multistate rotational record the first three QNs are
           rotor QNs and the next one is the state.  We use the number that
           SPCAT printed, without altering the QN representation. */
        int state = p->n_qn >= 4 ? p->M1u : 0;
        const PickettSpecies *sp = NULL;
        for (int k = 0; k < n_species; k++)
            if (species[k].state_index == state) { sp = &species[k]; break; }
        if (!sp || !(sp->temp_k > 0.0) || !(cat_temp_k > 0.0)) {
            p->line_strength = 0.0;
            p->linear_int = pow(10.0, p->cat_lgint);
        } else {
            double nu_cm = p->freq_mhz / mhz_per_cm;
            double stim_red = -expm1(-c2 * nu_cm / sp->temp_k);
            double stim_cat = -expm1(-c2 * nu_cm / cat_temp_k);
            double pop_red = exp(-c2 * p->elo_cm / sp->temp_k) * stim_red
                           / pow(sp->temp_k, 0.5 * p->rot_dof);
            double pop_cat = exp(-c2 * p->elo_cm / cat_temp_k) * stim_cat
                           / pow(cat_temp_k, 0.5 * p->rot_dof);
            double base = pow(10.0, p->cat_lgint);
            p->line_strength = pop_cat > 0.0 ? base / pop_cat : 0.0;
            p->linear_int = pop_cat > 0.0 ? base * pop_red / pop_cat : base;
            p->linear_int *= sp->concentration;
        }
        p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
        if (p->linear_int > max_int) max_int = p->linear_int;
    }
    if (global_max_int) *global_max_int = max_int;
}

int read_data(const char *fname, Point *pts, int maxpts,
              double *xmin, double *xmax, double *ymin, double *ymax)
{
    FILE *f = fopen(fname, "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    Separator sep = SEP_SPACE;
    int detected = 0;

    while (fgets(line, sizeof(line), f)) {
        double a,b;
        if (sscanf(line, "%lf", &a) != 1) continue;
        if (parse_line(line, &a, &b, SEP_COMMA)) {sep = SEP_COMMA; detected = 1; break;}
        if (parse_line(line, &a, &b, SEP_TAB))   {sep = SEP_TAB;   detected = 1; break;}
        if (parse_line(line, &a, &b, SEP_SPACE)) {sep = SEP_SPACE; detected = 1; break;}
    }
    if (!detected) {fclose(f); return 0;}
    rewind(f);

    *xmin=1e99; *xmax=-1e99; *ymin=1e99; *ymax=-1e99;
    while (fgets(line, sizeof(line), f) && n < maxpts) {
        double x,y;
        if (parse_line(line, &x, &y, sep)) {
            pts[n++] = (Point){x,y};
            if (x < *xmin) *xmin=x; if (x > *xmax) *xmax=x;
            if (y < *ymin) *ymin=y; if (y > *ymax) *ymax=y;
        }
    }
    fclose(f);
    return n;
}

int read_data_alloc(const char *fname, Point **pts,
                    double *xmin, double *xmax, double *ymin, double *ymax,
                    int *was_descending)
{
    if (was_descending) *was_descending = 0;
    FILE *f = fopen(fname, "r");
    if (!f) return 0;
    char line[512];
    Separator sep = SEP_SPACE;
    int detected = 0;

    while (fgets(line, sizeof(line), f)) {
        double a,b;
        if (sscanf(line, "%lf", &a) != 1) continue;
        if (parse_line(line, &a, &b, SEP_COMMA)) {sep = SEP_COMMA; detected = 1; break;}
        if (parse_line(line, &a, &b, SEP_TAB))   {sep = SEP_TAB;   detected = 1; break;}
        if (parse_line(line, &a, &b, SEP_SPACE)) {sep = SEP_SPACE; detected = 1; break;}
    }
    if (!detected) {
        fclose(f);
        *pts = NULL;
        return 0;
    }
    rewind(f);

    int cap = 65536;
    int n = 0;
    Point *arr = malloc(sizeof(Point) * cap);
    if (!arr) {
        fclose(f);
        *pts = NULL;
        return 0;
    }

    *xmin=1e99; *xmax=-1e99; *ymin=1e99; *ymax=-1e99;
    while (fgets(line, sizeof(line), f)) {
        double x,y;
        if (!parse_line(line, &x, &y, sep)) continue;
        if (n >= cap) {
            int new_cap = cap * 2;
            Point *tmp = realloc(arr, sizeof(Point) * new_cap);
            if (!tmp) break;
            arr = tmp;
            cap = new_cap;
        }
        arr[n++] = (Point){x,y};
        if (x < *xmin) *xmin=x; if (x > *xmax) *xmax=x;
        if (y < *ymin) *ymin=y; if (y > *ymax) *ymax=y;
    }
    fclose(f);

    if (n == 0) {
        free(arr);
        *pts = NULL;
        return 0;
    }

    /* Point consumers use binary search, so accept only a strict monotonic
       trace.  Descending files are common exports and can safely be restored
       to the one invariant those consumers need; mixed/duplicate x values
       are ambiguous and are rejected rather than silently mismeasured. */
    int direction = 0;
    for (int i = 1; i < n; i++) {
        double delta = arr[i].x - arr[i - 1].x;
        if (delta == 0.0 || (direction && (delta > 0.0) != (direction > 0))) {
            free(arr);
            *pts = NULL;
            return -2;
        }
        if (!direction) direction = delta > 0.0 ? 1 : -1;
    }
    if (direction < 0) {
        qsort(arr, (size_t)n, sizeof(*arr), compare_point_frequency);
        if (was_descending) *was_descending = 1;
    }

    Point *shrunk = realloc(arr, sizeof(Point) * n);
    *pts = shrunk ? shrunk : arr;
    return n;
}

static int same_assignment_transition(const PredLine *a, const PredLine *b) {
    return a->n_qn == b->n_qn &&
           a->Ju  == b->Ju  && a->Kau == b->Kau && a->Kcu == b->Kcu &&
           a->M1u == b->M1u && a->M2u == b->M2u && a->M3u == b->M3u &&
           a->Jl  == b->Jl  && a->Kal == b->Kal && a->Kcl == b->Kcl &&
           a->M1l == b->M1l && a->M2l == b->M2l && a->M3l == b->M3l;
}

void deduplicate_assignments(Assignment *list, int *n) {
    if (!list || !n || *n < 2) return;
    int keep = 0;
    for (int i = 0; i < *n; i++) {
        int seen = -1;
        for (int j = 0; j < keep; j++)
            if (same_assignment_transition(&list[j].pred, &list[i].pred)) { seen = j; break; }
        if (seen >= 0) {
            /* Keep the latest assignment: when an old session is read, or a
               line is reassigned, the final occurrence is the user's choice. */
            list[seen] = list[i];
        } else {
            if (keep != i) list[keep] = list[i];
            keep++;
        }
    }
    *n = keep;
}

void add_or_update_assignment(Assignment *list, int *n, PredLine p, double exp_f, double exp_i) {
    /* Frequencies from SPCAT are model-dependent.  QNs are the identity of a
       transition, so a post-fit prediction must update the old row rather
       than append a visually identical assignment. */
    deduplicate_assignments(list, n);
    for(int i=0; i<*n; i++) {
        if (same_assignment_transition(&list[i].pred, &p)) {
            int fit_enabled = list[i].fit_enabled;
            list[i].exp_freq = exp_f;
            list[i].exp_int  = exp_i;
            list[i].pred = p;
            /* Reassignment refreshes the observation and CAT values, but it
               must not silently undo the user's temporary Fit exclusion. */
            list[i].fit_enabled = fit_enabled;
            list[i].needs_reassign = 0;
            printf("Updated assignment for transition at %.4f MHz\n", p.freq_mhz);
            return;
        }
    }
    if(*n < MAX_ASSIGNMENTS) {
        list[*n].pred = p;
        list[*n].exp_freq = exp_f;
        list[*n].exp_int = exp_i;
        list[*n].fit_enabled = 1;
        list[*n].needs_reassign = 0;
        (*n)++;
        printf("Added assignment for %.4f MHz\n", p.freq_mhz);
    }
}

/* assignments.txt is deliberately a readable .lin-like record:
   upper QNs, lower QNs, observed frequency, calculated frequency, calculated
   intensity, NQN.  The trailing NQN is not an SPFIT field; it is the small
   piece of CAT/QNFMT information needed to reconstruct whether the QN record
   used 3, 4, 5 or 6 fields per state on the next launch.

   The layout is announced by the header the writer puts on the first line,
   "... (SPFIT .lin order) ...", which carries a format version from format 1
   on.  A row is never recognised from its field count alone: a .lin with an
   NQN column has the same count, with the uncertainty and the weight where
   CalcFreq and CalcIntensity are.  A file without that header is read only in
   the two layouts written before the header existed. */

typedef struct { double v[32]; int integer[32]; int n; } NumberRow;

static int number_separator(char c) {
    return c == ' ' || c == '\t' || c == '|' || c == ',' || c == '\r' || c == '\n';
}

/* The numbers of a line.  Returns 1, 0 for a blank line, -1 if a field is not
   a number. */
static int split_numbers(const char *line, NumberRow *row) {
    row->n = 0;
    const char *q = line;
    for (;;) {
        while (*q && number_separator(*q)) q++;
        if (!*q) break;
        if (row->n >= 32) return -1;
        char *end = NULL;
        double x = strtod(q, &end);
        if (end == q || (*end && !number_separator(*end))) return -1;
        int integer = 1;
        for (const char *c = q; c < end; c++) if (*c == '.' || *c == 'e' || *c == 'E') integer = 0;
        row->v[row->n] = x;
        row->integer[row->n] = integer;
        row->n++;
        q = end;
    }
    return row->n > 0;
}

static int all_integers(const NumberRow *r, int from, int to) {
    for (int i = from; i < to; i++) if (!r->integer[i]) return 0;
    return 1;
}

static void set_row_qn(PredLine *p, const double *upper, const double *lower, int nq) {
    int *u[6] = {&p->Ju, &p->Kau, &p->Kcu, &p->M1u, &p->M2u, &p->M3u};
    int *l[6] = {&p->Jl, &p->Kal, &p->Kcl, &p->M1l, &p->M2l, &p->M3l};
    for (int i = 0; i < nq; i++) { *u[i] = (int)lround(upper[i]); *l[i] = (int)lround(lower[i]); }
}

/* A row of the current layout: 2 NQN QN, ObsFreq, CalcFreq, CalcIntensity, NQN. */
static int parse_assignment_row(const NumberRow *r, PredLine *p, double *exp_freq) {
    if (r->n < 6 || !r->integer[r->n - 1]) return 0;
    int nq = (int)lround(r->v[r->n - 1]);
    if (nq < 1 || nq > 6 || r->n != 2 * nq + 4 || !all_integers(r, 0, 2 * nq)) return 0;
    set_row_qn(p, r->v, r->v + nq, nq);
    *exp_freq = r->v[2 * nq];
    p->freq_mhz = r->v[2 * nq + 1];
    p->linear_int = r->v[2 * nq + 2];
    p->lgint = p->linear_int > 0.0 ? log10(p->linear_int) : -INFINITY;
    p->n_qn = nq;
    return 1;
}

/* The layouts written before the header existed: 12 QN, ExpFreq, ExpInt (14
   fields; ExpFreq also stands for the calculated frequency), or PredFreq, 12
   QN, ExpFreq, ExpInt and an optional NQN (15 or 16 fields).  Unless the 16th
   field gives it, their NQN is unknown. */
static int parse_legacy_row(const NumberRow *r, PredLine *p, double *exp_freq, double *exp_int) {
    if (r->n == 14 && all_integers(r, 0, 12)) {
        set_row_qn(p, r->v, r->v + 6, 6);
        *exp_freq = p->freq_mhz = r->v[12];
        *exp_int = r->v[13];
        p->n_qn = 0;
        return 1;
    }
    if ((r->n == 15 || r->n == 16) && !r->integer[0] && all_integers(r, 1, 13)) {
        p->freq_mhz = r->v[0];
        set_row_qn(p, r->v + 1, r->v + 7, 6);
        *exp_freq = r->v[13];
        *exp_int = r->v[14];
        int nq = (r->n == 16 && r->integer[15]) ? (int)lround(r->v[15]) : 0;
        p->n_qn = (nq >= 1 && nq <= 6) ? nq : 0;
        return 1;
    }
    return 0;
}

/* 0 for a line that is not the header; otherwise its format version (1 for the
   header written before versions existed). */
static int header_format(const char *line) {
    if (line[0] != '#' || !strstr(line, "(SPFIT .lin order)")) return 0;
    const char *v = strstr(line, "format ");
    int version = 1;
    if (v && sscanf(v + 7, "%d", &version) != 1) version = 1;
    return version;
}

/* For a three-digit QNFMT, B-01 wrote the tens digit of J as NQN: 1 for J in
   10..19, 2 for J in 20..29.  Such a row cannot be told from a genuine one for
   certain, so it is kept and marked for reassignment. */
static int looks_truncated(const PredLine *p) {
    return (p->n_qn == 1 && p->Ju >= 10 && p->Ju <= 19) || (p->n_qn == 2 && p->Ju >= 20 && p->Ju <= 29);
}

int load_assignments_file(const char *filename, Assignment *list, int *n, AssignmentFileReport *report) {
    AssignmentFileReport local;
    AssignmentFileReport *rep = report ? report : &local;
    memset(rep, 0, sizeof(*rep));
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') {
            int version = header_format(line);
            if (version) rep->format = version;
            continue;
        }
        NumberRow r;
        int numbers = split_numbers(line, &r);
        if (numbers == 0) continue;                       /* blank line */
        PredLine p;
        memset(&p, 0, sizeof(p));
        double ef = 0.0, ei = 0.0;                       /* the file has no observed intensity */
        int ok = numbers > 0 && rep->format <= ASSIGNMENT_FORMAT &&
                 (rep->format ? parse_assignment_row(&r, &p, &ef) : parse_legacy_row(&r, &p, &ef, &ei));
        if (!ok) { rep->ignored++; continue; }
        p.branch = branch_from_qn(p.Ju, p.Jl);
        p.mu = mu_from_qn(p.Kau, p.Kal, p.Kcu, p.Kcl);
        /* Loading 5000 lines used to call add_or_update_assignment for every
           record; that deduplicates the whole accumulated list each time.
           Append raw rows, then perform the same last-row-wins consolidation
           once below. */
        if (*n >= MAX_ASSIGNMENTS) { rep->ignored++; continue; }
        Assignment *a = &list[(*n)++];
        memset(a, 0, sizeof(*a));
        a->pred = p;
        a->exp_freq = ef;
        a->exp_int = ei;
        a->fit_enabled = 1;
        a->needs_reassign = p.n_qn == 0 || looks_truncated(&p);
        rep->loaded++;
    }
    fclose(fp);
    int before_dedup = *n;
    deduplicate_assignments(list, n);
    rep->duplicates = before_dedup - *n;
    for (int i = 0; i < *n; i++) if (list[i].needs_reassign) rep->to_reassign++;
    printf("Loaded %d assignments from %s\n", rep->loaded, filename);
    return rep->loaded;
}

void load_existing_assignments(const char *filename, Assignment *list, int *n) {
    load_assignments_file(filename, list, n, NULL);
}

void assignment_file_message(const AssignmentFileReport *r, const char *path, char *out, size_t size) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    out[0] = '\0';
    if (r->format > ASSIGNMENT_FORMAT) {
        snprintf(out, size, "%s was written by a newer SpectraVisual (format %d) and was not read.", name, r->format);
        return;
    }
    if (!r->ignored && !r->duplicates && !r->to_reassign) return;
    size_t used = (size_t)snprintf(out, size, "%s:", name);
    if (r->ignored && used < size)
        used += (size_t)snprintf(out + used, size - used, " %d lines ignored (%s);", r->ignored,
                                 r->format ? "not an assignment row" : "no header: not a known layout, a .lin?");
    if (r->duplicates && used < size)
        used += (size_t)snprintf(out + used, size - used, " %d duplicate row%s (the last occurrence of each transition was kept);",
                                 r->duplicates, r->duplicates == 1 ? "" : "s");
    if (r->to_reassign && used < size)
        snprintf(out + used, size - used, " %d assignment%s to assign again (NQN unknown or truncated by an older version).",
                 r->to_reassign, r->to_reassign == 1 ? "" : "s");
}
