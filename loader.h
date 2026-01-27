#ifndef LOADER_H
#define LOADER_H

#include "types.h"

// Reads prediction .cat files (Pickett format)
int read_pred_cat(const char *filename, PredLine *lines, int max_lines, double *min_freq, double *max_freq, double *global_max_int);

// Reads experimental .csv or .txt (Freq, Intensity)
int read_data(const char *filename, Point *pts, int max_pts, double *min_x, double *max_x, double *min_y, double *max_y);

// Loads previously saved assignments
void load_existing_assignments(const char *filename, Assignment *assignments, int *n_assignments);

// Helper to add or update an assignment in the list
void add_or_update_assignment(Assignment *assignments, int *n_assignments, PredLine pred, double exp_freq, double exp_int);

#endif