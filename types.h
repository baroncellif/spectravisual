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
#define MAX_SPECTRA 8         // max simultaneously loaded experimental spectra

// --- DATA STRUCTURES ---

typedef struct {
    double x, y;
} Point;

// One loaded experimental spectrum. The "active" one is mirrored into the
// legacy AppState fields (raw_pts/current_pts/n_pts/exp_offset/...) so all
// existing tools keep operating on the selected spectrum unchanged.
typedef struct {
    Point *raw_pts;
    Point *smooth_pts;
    Point *current_pts;      // -> raw_pts or smooth_pts (rolling avg toggle)
    int    n_pts;
    double xmin, xmax, ymin, ymax;
    double exp_offset;        // horizontal alignment offset (MHz)
    double voffset;           // vertical display offset, fraction of plot/band height
    double vscale;            // per-spectrum intensity gain (stack mode), default 1
    int    rolling_avg_active;
    int    visible;
    SDL_Color color;
    char   path[512];
    char   name[64];          // short label for the legend
} Spectrum;

typedef struct {
    double freq_mhz;   
    double lgint;      // log10 integrated intensity at the currently selected T
    double linear_int; // integrated intensity at the currently selected T
    double cat_lgint;  // unmodified LGINT read from the .cat
    double elo_cm;     // lower-state energy ELO, in cm^-1
    double line_strength; // recovered S; valid when Tcat and mu_cat are set
    int    rot_dof;    // rotational degrees of freedom (the .cat DR field)
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

/* One fitted observation, retained so the intensity-fit report can be
 * exported without re-measuring the experimental trace. */
typedef struct {
    int assignment_index;
    int valid;
    int used;
    char component;
    double exp_area;
    double model_int;
    double ratio;
    double residual_log;
} IntFitLine;

typedef struct { 
    double x; 
    double y; 
} Peak;

// --- UI STRUCTURES ---

typedef struct {
    SDL_Rect rect;          // full on-screen geometry (x,y recomputed each frame when docked)
    int visible;            // user intent: panel requested open
    char title[64];
    float anim;             // 0..1 open progress (eased for slide-in / push animation)
    SDL_Rect clip;          // visible (animated) sub-rect used to clip content while sliding
} DraggableWindow;

// Visual style for a toolbar/window button (matches the redesign mockup).
typedef enum {
    BTN_NORMAL = 0,   // transparent idle, dim text, subtle hover
    BTN_DANGER,       // red accent (destructive actions)
    BTN_PRIMARY       // cyan accent (primary actions)
} ButtonStyle;

typedef struct {
    SDL_Rect rect;
    char label[32];
    SDL_Color color;
    int is_toggle;
    int style;         // ButtonStyle; 0 (BTN_NORMAL) by default
    int  icon_id;      // UiIcon drawn left of the label, -1 = none
} Button;

// --- STATE ENUMS ---

typedef enum { 
    INPUT_NONE, 
    INPUT_GAMMA,
    INPUT_GAUSS,
    INPUT_KBETA,
    INPUT_KCEROS,
    INPUT_KINTR,
    INPUT_PF_SIG,
    INPUT_PF_NOISE, 
    INPUT_PF_THRESH,
    INPUT_AVG_PTS,
    INPUT_PRED_MIN,
    INPUT_PRED_MAX,
    INPUT_JUMP_MIN,
    INPUT_JUMP_MAX,
    INPUT_OFFSET,
    INPUT_CAT_TEMP,
    INPUT_ROT_TEMP,
    INPUT_MUCAT_A,
    INPUT_MUCAT_B,
    INPUT_MUCAT_C,
    INPUT_MURED_A,
    INPUT_MURED_B,
    INPUT_MURED_C,
    INPUT_FIT_WINDOW,
    // Quantum-number / branch filter inputs
    INPUT_FILT_JMIN,
    INPUT_FILT_JMAX,
    INPUT_FILT_KAMIN,
    INPUT_FILT_KAMAX,
    INPUT_FILT_KCMIN,
    INPUT_FILT_KCMAX,
    INPUT_FILT_DJ,
    INPUT_FILT_DKA,
    INPUT_FILT_DKC
} InputState;

// --- MASTER APP STATE ---
typedef struct {
    // --- Multi-spectrum store ---
    Spectrum spectra[MAX_SPECTRA];
    int n_spectra;
    int active_spec;          // index into spectra[]; tools operate on this one
    int multi_layout;         // 0 = overlay, 1 = vertical stack (subplots)
    int multi_ynorm;          // 0 = shared Y scale, 1 = normalized per trace
    int multi_indiv_int;      // 0 = intensity controls scale all spectra together,
                              //     1 = only the active spectrum
    char pending_spec_path[512];
    char pending_pred_path[512];
    int pending_select;       // request to switch active spectrum (-1 = none)
    int pending_remove;       // request to remove a spectrum (-1 = none)

    // Data Pointers (mirror of the active spectrum)
    Point *raw_pts;
    Point *smooth_pts;
    Point *current_pts;
    int n_pts;
    int data_loaded;
    int verbose;
    int show_help;
    int pending_load;
    int export_requested;
    char exp_path[512];
    char pred_path[512];
    char error_message[512];
    char status_message[512];
    
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
    double cat_temp_k;       // temperature used to generate LGINT; 0 = unknown
    double rot_temp_k;       // requested rotational temperature; 0 = unset
    double dipole_cat[3];    // a/b/c dipoles used to generate the .cat (Debye)
    double dipole_red[3];    // manually requested a/b/c dipoles (Debye)

    /* Single-species relative-intensity fit.  The scale absorbs arbitrary
       experimental units and sample concentration; dipoles are therefore
       only determined relative to the selected reference component. */
    double intfit_half_window_mhz;
    int    intfit_fit_temperature;
    int    intfit_fit_dipole[3];
    int    intfit_has_result;
    int    intfit_n_candidate;
    int    intfit_n_used;
    int    intfit_n_rejected;
    int    intfit_reference_component;  // 0=a, 1=b, 2=c; -1=no dipole result
    int    intfit_component_n[3];
    double intfit_scale;
    double intfit_log_rms;
    double intfit_component_scale[3];
    char   intfit_message[128];
    IntFitLine intfit_lines[MAX_ASSIGNMENTS];
    
    // Tools State
    int sync_active;
    int bar_active;
    double bar_x;
    double pbar_x;
    
    int broadening_active;
    int broaden_mode;       // 0 = analytic (L/G/V), 1 = Kaiser-FFT lineshape
    double lorentz_gamma;   // Lorentzian HWHM (MHz); 0 disables L component
    double gauss_gamma;     // Gaussian HWHM (MHz); 0 disables G component
                            // both >0 -> Voigt; only one -> pure L or G
    double kaiser_beta;     // Kaiser window parameter (= numpy kaiser alpha)
    int    kaiser_ceros;    // zero-pad factor used in the FFT (multifft 'ceros')
    double kaiser_intrinsic;// molecular/intrinsic line FWHM (MHz), 0 = pure Kaiser

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
    int selected_assignment;
    int assignments_scroll;

    double sidebar_scroll;   // vertical scroll offset of the docked panel stack

    // Windows
    DraggableWindow win_pf;
    DraggableWindow win_br;
    DraggableWindow win_as;
    DraggableWindow win_avg;
    DraggableWindow win_cut;
    DraggableWindow win_jump;
    DraggableWindow win_filt;
    DraggableWindow win_dip;
    DraggableWindow win_spec;

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

    // --- Quantum-number / branch filter ---
    int filter_active;        // master enable for the QN/branch filter
    int filt_mu[3];           // allow dipole type [0]=a [1]=b [2]=c (1 = show)
    int filt_br[3];           // allow branch      [0]=P [1]=Q [2]=R (1 = show)
    int filt_use_range;       // enable J/Ka/Kc min-max range gating (upper state)
    int filt_j_min,  filt_j_max;
    int filt_ka_min, filt_ka_max;
    int filt_kc_min, filt_kc_max;
    int filt_use_delta;       // enable Delta J/Ka/Kc gating (upper - lower)
    int filt_dj, filt_dka, filt_dkc;

    // Text input. The focused field is a real text field: a caret, a selection
    // anchor, and the on-screen rectangle it was last drawn in, which is what
    // lets a click place the caret between two digits.
    int input_caret;        // caret position, 0..strlen(text_input_buf)
    int input_anchor;       // selection anchor; equal to the caret when nothing is selected
    int input_last;         // field that had focus when the current click arrived
    SDL_Rect input_rect;    // where that field was drawn
    char input_unit[12];    // its unit suffix, which shifts the value's right edge

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
    int plot_right;      // right edge available to the plot (shrinks when the inspector is open)
    int inspector_open;  // 1 while at least one tool panel is open
} Layout;

#endif
