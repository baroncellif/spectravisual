#ifndef LOADER_H
#define LOADER_H

#include "types.h"

// Reads a Pickett .cat file
int read_pred_cat(const char *fname, PredLine *out, int maxn,
                  double *xmin, double *xmax,
                  double *global_max_int);
int read_pred_cat_alloc(const char *fname, PredLine **out,
                        double *xmin, double *xmax,
                        double *global_max_int);

// Rescale Pickett catalog intensities from the temperature used to create the
// .cat to a requested LTE rotational temperature. The partition function uses
// the rigid-rotor approximation Qrot(T) / Qrot(Tcat) = (T / Tcat)^(DR/2).
// Invalid temperatures leave the catalog intensities unchanged.
void rescale_predicted_intensities(PredLine *lines, int n, double cat_temp_k,
                                   double rot_temp_k,
                                   double *global_max_int);

// Reads a standard X Y data file
int read_data(const char *fname, Point *pts, int maxpts,
              double *xmin, double *xmax, double *ymin, double *ymax);
int read_data_alloc(const char *fname, Point **pts,
                    double *xmin, double *xmax, double *ymin, double *ymax);

// Reads assigned line frequencies. The config file can contain a line such as
// assigned_file=assigned.lin, or simply the filename on a non-comment line.
int find_assigned_frequency_file(char *out_path, int out_size);
int read_assigned_frequencies(const char *fname, double *out, int maxn);

// Assignment helpers
void add_or_update_assignment(Assignment *list, int *n, PredLine p, double exp_f, double exp_i);
void load_existing_assignments(const char *filename, Assignment *list, int *n);

#endif
