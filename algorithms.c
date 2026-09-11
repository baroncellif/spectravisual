#include "algorithms.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

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
    if (!pts || npts < 3 || !out_peaks) return;
    
    // Only search in current view
    int start_idx = binary_search_lower(pts, npts, vxmin);
    int end_idx   = binary_search_upper(pts, npts, vxmax) - 1;
    if(start_idx < 0) start_idx = 0;
    if(end_idx >= npts) end_idx = npts - 1;
    if (end_idx < start_idx) return;

    int visible = end_idx - start_idx + 1;
    if (sig_pts < 1) sig_pts = 1;
    if (sig_pts * 2 >= visible) sig_pts = (visible - 1) / 2;
    if (sig_pts < 1) return;
    if (noise_pts < 1) noise_pts = 1;
    if (noise_pts <= sig_pts) noise_pts = sig_pts + 1;
    if (noise_pts > visible) noise_pts = visible;
    if (!isfinite(threshold) || threshold < 0.0) threshold = 0.0;

    /* A peak is judged above its nearby baseline.  The two noise windows sit
       outside the local-maximum guard area, so a constant (or slowly varying)
       offset does not become part of either signal or noise. */
    double *prefix = calloc((size_t)visible + 1, sizeof(*prefix));
    double *residual = malloc((size_t)visible * sizeof(*residual));
    double *scratch = malloc((size_t)visible * sizeof(*scratch));
    if (!prefix || !residual || !scratch) {
        free(prefix); free(residual); free(scratch);
        return;
    }
    for (int k = 0; k < visible; k++) prefix[k + 1] = prefix[k] + pts[start_idx + k].y;
    for (int k = 0; k < visible; k++) {
        int i = start_idx + k;
        int left0 = i - noise_pts, left1 = i - sig_pts - 1;
        int right0 = i + sig_pts + 1, right1 = i + noise_pts;
        if (left0 < start_idx) left0 = start_idx;
        if (left1 > end_idx) left1 = end_idx;
        if (right0 < start_idx) right0 = start_idx;
        if (right1 > end_idx) right1 = end_idx;
        double sum = 0.0;
        int count = 0;
        if (left0 <= left1) {
            sum += prefix[left1 - start_idx + 1] - prefix[left0 - start_idx];
            count += left1 - left0 + 1;
        }
        if (right0 <= right1) {
            sum += prefix[right1 - start_idx + 1] - prefix[right0 - start_idx];
            count += right1 - right0 + 1;
        }
        residual[k] = count ? pts[i].y - sum / count : 0.0;
        scratch[k] = residual[k];
    }

    qsort(scratch, (size_t)visible, sizeof(*scratch), compare_double);
    double median = (visible & 1) ? scratch[visible / 2]
                                  : 0.5 * (scratch[visible / 2 - 1] + scratch[visible / 2]);
    for (int k = 0; k < visible; k++) scratch[k] = fabs(residual[k] - median);
    qsort(scratch, (size_t)visible, sizeof(*scratch), compare_double);
    double mad = (visible & 1) ? scratch[visible / 2]
                               : 0.5 * (scratch[visible / 2 - 1] + scratch[visible / 2]);
    double noise = 1.4826 * mad;
    double cut_level = noise * threshold;

    printf("Noise: %.2e | Threshold: %.2e | SigPts: %d\n", noise, cut_level, sig_pts);

    // 2. Scan for peaks
    for (int i = start_idx + sig_pts; i <= end_idx - sig_pts; i++) {
        
        // Threshold check
        if (residual[i - start_idx] > cut_level) {
            
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
    free(prefix); free(residual); free(scratch);
    printf("Found %d peaks.\n", *n_peaks);
}
