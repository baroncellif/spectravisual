#ifndef TYPES_H
#define TYPES_H

#include <SDL.h>
#include <SDL_ttf.h>

// --- CONSTANTS ---
#define MAXPTS 5000000
#define MAX_PEAKS 10000
#define MAX_ASSIGNMENTS 5000
#define MAX_SELECTED 100
#define MAX_LIN_POINTS 50000 // New constant for LIN file

// --- DATA STRUCTURES ---

typedef struct { 
    double x, y; 
} Point;

typedef struct {
    double freq_mhz;   
    double lgint;      
    double linear_int; 
    // Quantum Numbers
    int Ju, Kau, Kcu, M1u, M2u, M3u;
    int Jl, Kal, Kcl, M1l, M2l, M3l;
    char branch;       
    char mu;           
} PredLine;

typedef struct {
    PredLine pred;
    double exp_freq;
    double exp_int;
} Assignment;

typedef struct { 
    double x; 
    double y; 
} Peak;

// --- UI STRUCTURES ---

typedef struct {
    SDL_Rect rect;
    int visible;
    char title[64];
} DraggableWindow;

typedef struct { 
    SDL_Rect rect;     
    char label[32];    
    SDL_Color color;   
    int is_toggle;     
} Button;

// --- STATE ENUMS ---

typedef enum { 
    INPUT_NONE, 
    INPUT_GAMMA, 
    INPUT_PF_SIG, 
    INPUT_PF_NOISE, 
    INPUT_PF_THRESH,
    INPUT_AVG_PTS,
    INPUT_PRED_MIN,
    INPUT_PRED_MAX,
    INPUT_JUMP_MIN,
    INPUT_JUMP_MAX,
    INPUT_OFFSET
} InputState;

// --- MASTER APP STATE ---
typedef struct {
    // Data Pointers
    Point *raw_pts;
    Point *smooth_pts;
    Point *current_pts; 
    int n_pts;
    
    PredLine *pred_lines;
    int n_pred;
    double pred_global_max;

    // NEW: Assigned LIN Data
    double *lin_data;
    int n_lin_data;

    // Ranges
    double xmin, xmax, ymin, ymax; 
    double pxmin, pxmax;           

    // View State
    double vxmin, vxmax, vymin, vymax;   
    double pvxmin, pvxmax;               
    double pred_scale;
    
    // Tools State
    int sync_active;
    int bar_active;
    double bar_x;
    double pbar_x;
    
    int broadening_active;
    double lorentz_gamma;

    int rolling_avg_active;
    int rolling_avg_window;

    int pf_sig_pts;
    int pf_noise_pts;
    double pf_thresh;
    Peak peaks[MAX_PEAKS];
    int n_peaks;

    int selected_indices[MAX_SELECTED];
    int n_selected;
    Assignment assignments[MAX_ASSIGNMENTS];
    int n_assignments;

    // Windows
    DraggableWindow win_pf;
    DraggableWindow win_br;
    DraggableWindow win_as;
    DraggableWindow win_avg;
    DraggableWindow win_cut;  
    DraggableWindow win_jump; 

    DraggableWindow *drag_target;
    SDL_Point drag_offset;

    InputState input_state;
    char text_input_buf[64];

    int selecting_left;
    int selecting_right;
    SDL_Point sel_start;
    SDL_Point sel_cur;

    // Offset State
    double exp_offset;
    int dragging_offset; 
    int drag_last_x;     
    
    // Intensity Cut Variables
    double pred_min_log_int;    
    double pred_max_log_int;    

    // Measure Tool State
    int measure_active;
    int measure_phase; 
    double measure_x1;

} AppState;

typedef struct {
    int win_w, win_h;
    int exp_x, exp_y, exp_w, exp_h;
    int pred_x, pred_y, pred_w, pred_h;
    int plot_x, gap;
} Layout;

#endif