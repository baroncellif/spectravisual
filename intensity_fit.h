#ifndef INTENSITY_FIT_H
#define INTENSITY_FIT_H

#include "types.h"

/* Fit the active experimental trace against the assigned transitions. */
int intensity_fit_run(AppState *state);

/* Write the last fit, including each accepted/rejected observation. */
int intensity_fit_export(const AppState *state, const char *filename);

#endif
