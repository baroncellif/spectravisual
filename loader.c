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

static int parse_line(const char *line, double *a, double *b, Separator sep) {
    if (sep == SEP_COMMA) return (sscanf(line, "%lf , %lf", a, b) == 2);
    if (sep == SEP_TAB)   return (sscanf(line, "%lf\t%lf", a, b) == 2);
    return (sscanf(line, "%lf %lf", a, b) == 2);
}

// --- PUBLIC FUNCTIONS ---

// NEW: Read LIN file
int read_lin_file(const char *fname, double *out, int maxn) {
    FILE *f = fopen(fname, "r");
    if (!f) return 0;
    
    int n = 0;
    double val;
    
    // Read every whitespace-separated token
    while (n < maxn) {
        if (fscanf(f, "%lf", &val) == 1) {
            out[n++] = val;
        } else {
            // If it's not a number, consume the string token and continue
            char temp[256];
            if (fscanf(f, "%s", temp) != 1) break; // End of file
        }
    }
    
    fclose(f);
    printf("Loaded %d numeric values from %s\n", n, fname);
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

        const int qn0 = 55;
        pl.Ju  = parse_qn2(line + qn0 +  0); pl.Kau = parse_qn2(line + qn0 +  2); pl.Kcu = parse_qn2(line + qn0 +  4);
        pl.M1u = parse_qn2(line + qn0 +  6); pl.M2u = parse_qn2(line + qn0 +  8); pl.M3u = parse_qn2(line + qn0 + 10);
        pl.Jl  = parse_qn2(line + qn0 + 12); pl.Kal = parse_qn2(line + qn0 + 14); pl.Kcl = parse_qn2(line + qn0 + 16);
        pl.M1l = parse_qn2(line + qn0 + 19); pl.M2l = parse_qn2(line + qn0 + 20); pl.M3l = parse_qn2(line + qn0 + 22);

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

        int res = sscanf(line, "%lf %d %d %d %d %d %d %lf %lf",
               &p.freq_mhz, &p.Ju, &p.Kau, &p.Kcu, &p.Jl, &p.Kal, &p.Kcl, &ef, &ei);

        if(res >= 8) {
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