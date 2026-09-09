#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- PRIVATE HELPER FUNCTIONS ---

typedef enum { SEP_COMMA, SEP_TAB, SEP_SPACE } Separator;

static int parse_qn2(const char *s) {
    if (s[0] == ' ' && s[1] == ' ') return 0;
    char buf[3] = {s[0], s[1], '\0'};
    return atoi(buf);
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

// The first fixed-width fields of a Pickett .cat line are
// FREQ(13), ERR(8), LGINT(8), DR(2), ELO(10), ... .
static void parse_cat_intensity_fields(const char *line, PredLine *pl) {
    int dr = 0;
    double elo = 0.0;
    if (sscanf(line + 29, "%2d%10lf", &dr, &elo) != 2) {
        dr = 0;
        elo = 0.0;
    }
    pl->cat_lgint = pl->lgint;
    pl->elo_cm = elo;
    pl->rot_dof = dr;
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
        if ((int)strlen(line) < 80) continue;
        double freq = 0.0, err = 0.0, lgint = 0.0;
        if (sscanf(line, "%lf %lf %lf", &freq, &err, &lgint) < 3) continue;

        PredLine pl;
        memset(&pl, 0, sizeof(pl));
        pl.freq_mhz = freq;
        pl.lgint    = lgint;
        pl.linear_int = pow(10.0, lgint); 
        parse_cat_intensity_fields(line, &pl);

        const int qn0 = 55;
        pl.Ju  = parse_qn2(line + qn0 +  0); pl.Kau = parse_qn2(line + qn0 +  2); pl.Kcu = parse_qn2(line + qn0 +  4);
        pl.M1u = parse_qn2(line + qn0 +  6); pl.M2u = parse_qn2(line + qn0 +  8); pl.M3u = parse_qn2(line + qn0 + 10);
        pl.Jl  = parse_qn2(line + qn0 + 12); pl.Kal = parse_qn2(line + qn0 + 14); pl.Kcl = parse_qn2(line + qn0 + 16);
        pl.M1l = parse_qn2(line + qn0 + 18); pl.M2l = parse_qn2(line + qn0 + 20); pl.M3l = parse_qn2(line + qn0 + 22);

        pl.branch = branch_from_qn(pl.Ju, pl.Jl);
        pl.mu     = mu_from_qn(pl.Kau, pl.Kal, pl.Kcu, pl.Kcl);

        out[n++] = pl;
        if (pl.freq_mhz < *xmin) *xmin = pl.freq_mhz;
        if (pl.freq_mhz > *xmax) *xmax = pl.freq_mhz;
        if (pl.linear_int > *global_max_int) *global_max_int = pl.linear_int;
    }
    fclose(f);
    return n;
}

int read_pred_cat_alloc(const char *fname, PredLine **out,
                        double *xmin, double *xmax,
                        double *global_max_int)
{
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
        if ((int)strlen(line) < 80) continue;
        double freq = 0.0, err = 0.0, lgint = 0.0;
        if (sscanf(line, "%lf %lf %lf", &freq, &err, &lgint) < 3) continue;

        if (n >= cap) {
            int new_cap = cap * 2;
            PredLine *tmp = realloc(arr, sizeof(PredLine) * new_cap);
            if (!tmp) break;
            arr = tmp;
            cap = new_cap;
        }

        PredLine pl;
        memset(&pl, 0, sizeof(pl));
        pl.freq_mhz = freq;
        pl.lgint    = lgint;
        pl.linear_int = pow(10.0, lgint); 
        parse_cat_intensity_fields(line, &pl);

        const int qn0 = 55;
        pl.Ju  = parse_qn2(line + qn0 +  0); pl.Kau = parse_qn2(line + qn0 +  2); pl.Kcu = parse_qn2(line + qn0 +  4);
        pl.M1u = parse_qn2(line + qn0 +  6); pl.M2u = parse_qn2(line + qn0 +  8); pl.M3u = parse_qn2(line + qn0 + 10);
        pl.Jl  = parse_qn2(line + qn0 + 12); pl.Kal = parse_qn2(line + qn0 + 14); pl.Kcl = parse_qn2(line + qn0 + 16);
        pl.M1l = parse_qn2(line + qn0 + 18); pl.M2l = parse_qn2(line + qn0 + 20); pl.M3l = parse_qn2(line + qn0 + 22);

        pl.branch = branch_from_qn(pl.Ju, pl.Jl);
        pl.mu     = mu_from_qn(pl.Kau, pl.Kal, pl.Kcu, pl.Kcl);

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

    PredLine *shrunk = realloc(arr, sizeof(PredLine) * n);
    *out = shrunk ? shrunk : arr;
    return n;
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
                    double *xmin, double *xmax, double *ymin, double *ymax)
{
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

    Point *shrunk = realloc(arr, sizeof(Point) * n);
    *pts = shrunk ? shrunk : arr;
    return n;
}

void add_or_update_assignment(Assignment *list, int *n, PredLine p, double exp_f, double exp_i) {
    for(int i=0; i<*n; i++) {
        // Check if freq matches (using small epsilon)
        if(fabs(list[i].pred.freq_mhz - p.freq_mhz) < 1e-6) {
            list[i].exp_freq = exp_f;
            list[i].exp_int  = exp_i;
            list[i].pred = p; 
            printf("Updated assignment for %.4f MHz\n", p.freq_mhz);
            return;
        }
    }
    if(*n < MAX_ASSIGNMENTS) {
        list[*n].pred = p;
        list[*n].exp_freq = exp_f;
        list[*n].exp_int = exp_i;
        (*n)++;
        printf("Added assignment for %.4f MHz\n", p.freq_mhz);
    }
}

void load_existing_assignments(const char *filename, Assignment *list, int *n) {
    FILE *fp = fopen(filename, "r");
    if(!fp) return;

    char line[512];
    int loaded = 0;
    while(fgets(line, sizeof(line), fp)) {
        if(line[0] == '#' || strlen(line) < 10) continue;
        for(int i=0; line[i]; i++) if(line[i]=='|') line[i]=' ';

        PredLine p; memset(&p, 0, sizeof(p));
        double ef, ei;

        int res = sscanf(line, "%lf %d %d %d %d %d %d %d %d %d %d %d %d %lf %lf",
               &p.freq_mhz,
               &p.Ju, &p.Kau, &p.Kcu, &p.M1u, &p.M2u, &p.M3u,
               &p.Jl, &p.Kal, &p.Kcl, &p.M1l, &p.M2l, &p.M3l,
               &ef, &ei);

        if(res < 15) {
            // Legacy 0.9 format: 12 quantum numbers followed by ExpFreq ExpInt.
            // There was no predicted frequency field, so keep the quantum-number
            // order and use ExpFreq as a stable key for old assignments.
            double old_ef = 0.0, old_ei = 0.0;
            PredLine oldp; memset(&oldp, 0, sizeof(oldp));
            int old_res = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d %lf %lf",
                   &oldp.Ju, &oldp.Kau, &oldp.Kcu, &oldp.M1u, &oldp.M2u, &oldp.M3u,
                   &oldp.Jl, &oldp.Kal, &oldp.Kcl, &oldp.M1l, &oldp.M2l, &oldp.M3l,
                   &old_ef, &old_ei);
            if (old_res >= 14) {
                p = oldp;
                p.freq_mhz = old_ef;
                ef = old_ef;
                ei = old_ei;
                res = 15;
            }
        }

        if(res >= 15) {
            p.branch = branch_from_qn(p.Ju, p.Jl);
            p.mu = mu_from_qn(p.Kau, p.Kal, p.Kcu, p.Kcl);
            p.lgint = 0; 
            add_or_update_assignment(list, n, p, ef, ei);
            loaded++;
        }
    }
    fclose(fp);
    printf("Loaded %d assignments from %s\n", loaded, filename);
}
