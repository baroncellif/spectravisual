#ifndef INTENSITY_FIT_H
#define INTENSITY_FIT_H

#include "types.h"

/* Fit the active experimental trace against the assigned transitions. */
int intensity_fit_run(AppState *state);

/* Area used by the fit for one measured transition.  Kept public so every
   caller that needs the measurement uses the same sorted-point invariant. */
int intensity_fit_integrate_area(const AppState *state, double center,
                                 double half_width, double *area);

/* Write the last fit, including each accepted/rejected observation. */
int intensity_fit_export(const AppState *state, const char *filename);

#endif
