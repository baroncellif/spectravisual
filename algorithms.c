#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// --- BINARY SEARCH IMPLEMENTATIONS ---

int binary_search_lower(Point *pts, int n, double val) {
    int l = 0, r = n - 1, idx = n;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        if (pts[mid].x >= val) { idx = mid; r = mid - 1; } 
        else { l = mid + 1; }
    }
    return idx; 
}

int binary_search_upper(Point *pts, int n, double val) {
    int l = 0, r = n - 1, idx = n;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        if (pts[mid].x > val) { idx = mid; r = mid - 1; } 
        else { l = mid + 1; }
    }
    return idx; 
}

int binary_search_pred_lower(PredLine *pts, int n, double val) {
    int l = 0, r = n - 1, idx = n;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        if (pts[mid].freq_mhz >= val) { idx = mid; r = mid - 1; } 
        else { l = mid + 1; }
    }
    return idx; 
}

int binary_search_pred_upper(PredLine *pts, int n, double val) {
    int l = 0, r = n - 1, idx = n;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        if (pts[mid].freq_mhz > val) { idx = mid; r = mid - 1; } 
        else { l = mid + 1; }
    }
    return idx; 
}

// --- SIGNAL PROCESSING ---

void apply_rolling_average(Point *src, Point *dst, int n, int window) {
    if(window < 1) window = 1;
    for(int i = 0; i < n; i++) {
        dst[i].x = src[i].x; // Preserve frequency
        
        int start = i - window / 2;
        int end = i + window / 2;
        if(start < 0) start = 0;
        if(end >= n) end = n - 1;
        
        double sum = 0;
        int count = 0;
        
        // Simple Average loop
        for(int k = start; k <= end; k++) {
            sum += src[k].y;
            count++;
        }
        dst[i].y = (count > 0) ? (sum / count) : src[i].y;
    }
    printf("Rolling average applied (Window: %d)\n", window);
}

void run_peak_finder(Point *pts, int npts, double vxmin, double vxmax, 
                     int sig_pts, int noise_pts, double threshold, 
                     Peak *out_peaks, int *n_peaks) 
{
    *n_peaks = 0;
    
    // Only search in current view
    int start_idx = binary_search_lower(pts, npts, vxmin);
    int end_idx   = binary_search_upper(pts, npts, vxmax);
    if(start_idx < 0) start_idx = 0;
    if(end_idx >= npts) end_idx = npts - 1;

    // 1. Calculate RMS Noise of the visible region
    double sum_sq = 0;
    int count = 0;
    for(int i = start_idx; i <= end_idx; i++) {
        sum_sq += pts[i].y * pts[i].y;
        count++;
    }
    double rms = (count > 0) ? sqrt(sum_sq / count) : 1.0;
    double cut_level = rms * threshold;

    printf("RMS: %.2e | Threshold: %.2e | SigPts: %d\n", rms, cut_level, sig_pts);

    // 2. Scan for peaks
    for (int i = start_idx + sig_pts; i <= end_idx - sig_pts; i++) {
        
        // Threshold check
        if (pts[i].y > cut_level) {
            
            // Local Maximum Check (in +/- sig_pts window)
            int is_max = 1;
            for(int k=1; k<=sig_pts; k++) {
                if(pts[i-k].y >= pts[i].y || pts[i+k].y > pts[i].y) {
                    is_max = 0; 
                    break;
                }
            }

            if(is_max && *n_peaks < MAX_PEAKS) {
                // Parabolic Interpolation for freq accuracy
                double y1 = pts[i-1].y;
                double y2 = pts[i].y;
                double y3 = pts[i+1].y;
                double x2 = pts[i].x;
                
                double px, py;
                double denom = (y1 - 2*y2 + y3);
                if(fabs(denom) < 1e-15) {
                    px = x2; py = y2;
                } else {
                    double dx = 0.5 * ((y1 - y3) / denom);
                    // dx is in units of index steps, convert to freq using neighbor
                    px = x2 + dx * (pts[i+1].x - x2); 
                    py = y2 - 0.25 * (y1 - y3) * dx;
                }

                out_peaks[*n_peaks].x = px;
                out_peaks[*n_peaks].y = py;
                (*n_peaks)++;
                
                // Skip forward
                i += sig_pts / 2;
            }
        }
    }
    printf("Found %d peaks.\n", *n_peaks);
}