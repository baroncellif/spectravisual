#ifndef LOADER_H
#define LOADER_H

#include "types.h"

// Reads a Pickett .cat file
int read_pred_cat(const char *fname, PredLine *out, int maxn,
                  double *xmin, double *xmax,
                  double *global_max_int);

// Reads a standard X Y data file
int read_data(const char *fname, Point *pts, int maxpts,
              double *xmin, double *xmax, double *ymin, double *ymax);

// NEW: Reads an assigned.lin file (reading all numbers as doubles)
int read_lin_file(const char *fname, double *out, int maxn);

// Assignment helpers
void add_or_update_assignment(Assignment *list, int *n, PredLine p, double exp_f, double exp_i);
void load_existing_assignments(const char *filename, Assignment *list, int *n);

#endif