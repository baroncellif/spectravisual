#ifndef ALGORITHMS_H
#define ALGORITHMS_H

#include "types.h"

// --- SEARCH HELPERS ---
// Fast lookup for Experimental Points
int binary_search_lower(Point *pts, int n, double val);
int binary_search_upper(Point *pts, int n, double val);

// Fast lookup for Prediction Lines
int binary_search_pred_lower(PredLine *pts, int n, double val);
int binary_search_pred_upper(PredLine *pts, int n, double val);

// --- SIGNAL PROCESSING ---

// Apply sliding window average (Smoothing)
void apply_rolling_average(Point *src, Point *dst, int n, int window);

// Find peaks above a noise threshold
// sig_pts: width to check for local max
// threshold: multiplier of RMS noise
void run_peak_finder(Point *pts, int npts, double vxmin, double vxmax, 
                     int sig_pts, int noise_pts, double threshold, 
                     Peak *out_peaks, int *n_peaks);

#endif