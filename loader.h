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
   transition from its shared Tcat to the Hamiltonian's Trot, then apply the
   state concentration. State identity comes from the state QN printed by
   SPCAT (the fourth QN in its standard multistate record). */
void rescale_predicted_intensities_by_species(PredLine *lines, int n,
                                              double cat_temp_k,
                                              double rot_temp_k,
                                              const PickettSpecies *species, int n_species,
                                              double *global_max_int);

/* One Hamiltonian of a simulation, as the intensity code needs to see it. */
typedef struct {
    int hamiltonian_id;
    double cat_temp_k;                 /* TEMP the catalogue was produced at */
    double rot_temp_k;                 /* temperature it is displayed at now */
    const PickettSpecies *species;
    int n_species;
} PredIntensityModel;

/* Same rescaling for a plot holding the catalogues of several Hamiltonians at
   once: every row is rescaled with the model that produced it, found through
   the Hamiltonian id the row carries.  Rows of an unknown Hamiltonian keep the
   intensity their catalogue stated. */
void rescale_predicted_intensities_multi(PredLine *lines, int n,
                                         const PredIntensityModel *models, int n_models,
                                         double *global_max_int);

// Reads a standard X Y data file
int read_data(const char *fname, Point *pts, int maxpts,
              double *xmin, double *xmax, double *ymin, double *ymax);
int read_data_alloc(const char *fname, Point **pts,
                    double *xmin, double *xmax, double *ymin, double *ymax,
                    int *was_descending);

// Assignment helpers
void add_or_update_assignment(Assignment *list, int *n, PredLine p, double exp_f, double exp_i,
                              int hamiltonian_id);
/* An assignment is identified by its complete upper/lower quantum-number
   tuple, never by a calculated frequency (which changes after SPFIT). */
void deduplicate_assignments(Assignment *list, int *n);
/* Call before (re)assigning a batch of predictions to the same observed peak:
   drops whichever OLD assignments under the touched Hamiltonian(s) sit at
   that same exp_f but are not part of the new batch, so reassigning a peak
   to a different prediction replaces the old assignment instead of leaving
   it behind as a duplicate that still feeds the fit. */
void clear_superseded_assignments(Assignment *list, int *n, double exp_f,
                                   const int *hamiltonian_ids, const PredLine *keep, int n_keep);
void load_existing_assignments(const char *filename, Assignment *list, int *n);

/* Version written in the header line of assignments.txt. */
#define ASSIGNMENT_FORMAT 5

/* What reading assignments.txt had to decide, for the message shown to the user. */
typedef struct {
    int format;       /* header version; 0 = no header (layouts written before 1f4df65) */
    int loaded;       /* rows read as assignments                                        */
    int ignored;      /* rows in no known layout, a .lin for instance                    */
    int duplicates;   /* rows of a transition already read: the last occurrence is kept  */
    int to_reassign;  /* assignments marked for reassignment: NQN unknown or truncated   */
} AssignmentFileReport;

/* load_existing_assignments, also filling *report (may be NULL). */
int  load_assignments_file(const char *filename, Assignment *list, int *n, AssignmentFileReport *report);
/* One line for the title bar about *report; empty when there is nothing to say. */
void assignment_file_message(const AssignmentFileReport *report, const char *path, char *out, size_t size);

#endif
