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

/* One fixed-width .cat record (calpgm/calcat.c:700-709).  Returns 1 and fills
   *pl - and *err_mhz, which PredLine does not keep, when not NULL - for a
   record; 0 for a line that is not one (shorter than the QNFMT column, or a
   non-numeric FREQ/ERR/LGINT/QNFMT); -1 for a record whose NQN (QNFMT % 10)
   is 0 or above 6, which the app does not support. */
int parse_cat_record(const char *line, PredLine *pl, double *err_mhz);

/* read_pred_cat_alloc, also counting in *n_unsupported (may be NULL) the
   records skipped because parse_cat_record returned -1. */
int read_pred_cat_alloc_counted(const char *fname, PredLine **out,
                                double *xmin, double *xmax,
                                double *global_max_int, int *n_unsupported);

// Rescale Pickett catalog intensities from the temperature used to create the
// .cat to a requested LTE rotational temperature. The partition function uses
// the rigid-rotor approximation Qrot(T) / Qrot(Tcat) = (T / Tcat)^(DR/2).
// Invalid temperatures leave the catalog intensities unchanged. For a/b/c
// components with both dipoles set, recover S from mu_cat and apply mu_red.
void rescale_predicted_intensities(PredLine *lines, int n, double cat_temp_k,
                                   double rot_temp_k,
                                   const double dipole_cat[3],
                                   const double dipole_red[3],
                                   double *global_max_int);

/* For a common multi-state SPCAT catalogue, rescale every diagonal-state
   transition from its shared Tcat to that state's Tred, then apply the
   species concentration.  State identity comes from the state QN printed by
   SPCAT (the fourth QN in its standard multistate record). */
void rescale_predicted_intensities_by_species(PredLine *lines, int n,
                                              double cat_temp_k,
                                              const PickettSpecies *species, int n_species,
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
/* An assignment is identified by its complete upper/lower quantum-number
   tuple, never by a calculated frequency (which changes after SPFIT). */
void deduplicate_assignments(Assignment *list, int *n);
void load_existing_assignments(const char *filename, Assignment *list, int *n);

#endif
