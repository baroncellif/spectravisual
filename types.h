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
#define MAX_PENDING_LOADS 32  // files/actions waiting to be opened, in FIFO order
#define MAX_PICKETT_PARAMS 128
#define MAX_PICKETT_LABEL 128
#define MAX_PICKETT_SPECIES 16
#define MAX_PICKETT_HAMILTONIANS 16

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
    int    opacity;        /* percent; set from the settings, per trace */
    char   path[512];
    char   name[64];          // short label for the legend
} Spectrum;

/* On-disk description of one experimental trace.  The point data stay in the
   original file; this records the non-destructive state needed to reopen it
   exactly as it was shown. */
typedef struct {
    char   path[512];
    double exp_offset;
    double voffset;
    double vscale;
    int    rolling_avg_active;
    int    visible;
    int    opacity;
} SessionSpectrum;

typedef enum {
    PENDING_LOAD_SPECTRUM,
    PENDING_LOAD_CATALOG,
    PENDING_LOAD_SESSION,
    PENDING_LOAD_SIMULATION   /* merge the catalogues listed in predfit.simulated */
} PendingLoadKind;

typedef struct {
    PendingLoadKind kind;
    int generated_catalog;    /* produced by SPCAT, not a manually opened .cat */
    char path[512];
} PendingLoadRequest;

typedef struct {
    double freq_mhz;   
    double lgint;      // log10 integrated intensity at the currently selected T
    double linear_int; // integrated intensity at the currently selected T
    double cat_lgint;  // unmodified LGINT read from the .cat
    double elo_cm;     // lower-state energy ELO, in cm^-1
    double line_strength; // recovered S; valid when Tcat and mu_cat are set
    int    rot_dof;    // rotational degrees of freedom (the .cat DR field)
    // Quantum Numbers
    int    n_qn;       // QNFMT % 10: quantum numbers per state in this CAT
    int Ju, Kau, Kcu, M1u, M2u, M3u;
    int Jl, Kal, Kcl, M1l, M2l, M3l;
    char branch;       
    char mu;           
    int hamiltonian_id; /* 0 = external/legacy; >0 = generated model owner */
} PredLine;

typedef struct {
    PredLine pred;
    double exp_freq;
    double exp_int;
    int fit_enabled;          // assignment remains visible when this is 0
    int needs_reassign;       // restored from a .lin row with fewer than 3 QN per state
    int hamiltonian_id;       // stable owner; 0 denotes a pre-v2 legacy assignment
} Assignment;

/* A fit exclusion is deliberately keyed by the transition, not its place in
   the assignment list.  The list can be reordered, deduplicated or edited
   while a fit-history snapshot still exists. */
typedef struct {
    int n_qn;
    int qn[12];               /* upper then lower, including used spin QNs */
} FitExclusionKey;

typedef struct {
    int id;
    double value;
    double error;
    char label[MAX_PICKETT_LABEL];
} PickettParameter;

/* Which broadened traces reach the plot.  The total is what is compared with
   the experiment; the per-state traces say who contributes what. */
typedef enum {
    TRACE_SUM = 0,          /* the total only - the default */
    TRACE_SUM_AND_STATES,   /* the total and one trace per state */
    TRACE_STATES            /* the states only */
} SpeciesTraceMode;

/* A state belongs to one Hamiltonian.  Its dipoles are separate cards in that
   Hamiltonian's single .int; temperature, cutoffs and range stay on the
   Hamiltonian because SPCAT accepts them once per .int file.  Concentration
   is a SpectraVisual-only multiplier applied after SPCAT. */
typedef struct {
    char name[64];
    int state_index;          /* v=0,1,... in .lin; parameter suffix 00,11... */
    int predict_enabled;      /* include this species when Simulate is pressed */
    double mu[3];
    double concentration;     /* external relative-intensity multiplier, default 1 */
    /* Which dipole components take part in the simulation.  Stored inverted so
       that a zeroed structure - a new state, an old session - simulates with
       every component, and only an explicit exclusion removes one. */
    int mu_excluded[3];
    /* Colour of this state's own broadened trace.  A zero alpha means "never
       chosen", and the default palette answers for it. */
    SDL_Color trace_color;
} PickettSpecies;

/* The manually controllable .int fields.  QROT is deliberately absent: it is
   always calculated from A, B, C, T and sigma.  A zero FQLIM or TEMP and
   MAXV=-1 mean "derive it from the current prediction". */
typedef struct {
    int flags, tag;
    int fbegin, fend;
    double intensity_cutoff;
    double fqlim_ghz, temp_k;
    int maxv;
    double sigma;
} PickettIntSettings;

/* A reversible point saved immediately before an SPFIT run.  It remains in
   RAM only, so closing the app intentionally starts a fresh fit history. */
typedef struct {
    double a, b, c;
    double mu[3];
    double temp_k;
    double fmin_ghz, fmax_ghz;
    double line_error_mhz;
    PickettIntSettings int_settings;
    int n_param;
    PickettParameter param[MAX_PICKETT_PARAMS];
    char hamiltonian_line[256]; /* shared third line of .par/.var */
    PickettSpecies species[MAX_PICKETT_SPECIES];
    int n_species;
    int active_species;
    int n_exclusions;
    FitExclusionKey exclusions[MAX_ASSIGNMENTS];
} PredFitSnapshot;

/* A project-level model.  PredFitState keeps one model projected into its
   legacy editing fields while this list retains the independent models that
   are not selected.  Keeping the serializable model separate from SDL/UI
   state lets the multi-Hamiltonian migration happen without duplicating UI
   resources or sharing Pickett control-card values by accident. */
typedef struct {
    int id;                         /* stable project identity, never an index */
    char name[64];
    /* Inverted like mu_excluded above: every Hamiltonian takes part in a
       simulation unless the user unchecks it. */
    int simulate_excluded;
    PredFitSnapshot model;
} HamiltonianModel;

/* One catalogue of a simulation: which Hamiltonian produced it and at which
   temperature, so its rows can be rescaled with that model's states while the
   rows of another Hamiltonian in the same plot follow theirs. */
typedef struct {
    int hamiltonian_id;
    double cat_temp_k;              /* TEMP of the .int that produced the rows */
    char cat_path[512];
} SimulatedCatalog;

typedef struct {
    double a, b, c;
    double mu[3];
    double temp_k;
    double fmin_ghz, fmax_ghz;
    double line_error_mhz;
    PickettIntSettings int_settings;
    int n_param;
    PickettParameter param[MAX_PICKETT_PARAMS];
    char hamiltonian_line[256]; /* shared third line of .par/.var */
    PickettSpecies species[MAX_PICKETT_SPECIES];
    int n_species;
    int active_species;
    HamiltonianModel hamiltonian[MAX_PICKETT_HAMILTONIANS];
    int n_hamiltonians;
    int active_hamiltonian;
    int next_hamiltonian_id;
    int advanced_open;
    int advanced_tab;
    SDL_Window *advanced_window;
    SDL_Renderer *advanced_renderer;
    Uint32 advanced_window_id;
    int advanced_edit_param;  // -1 while no cell in the Parameters table is edited
    int advanced_edit_col;    // 0=id, 1=value, 2=parameter uncertainty
    int advanced_edit_species;
    int advanced_edit_hamiltonian;
    int advanced_nav_state;   /* sidebar focus: -1 the Hamiltonian, else its state */
    int advanced_edit_replace;
    int advanced_edit_caret;
    int advanced_edit_anchor;
    int advanced_param_scroll;
    int advanced_line_scroll;
    int advanced_species_scroll;
    int advanced_project_scroll;
    int advanced_delete_hamiltonian_id; /* second click confirms this H only */
    int advanced_hover_line;
    char advanced_edit_buf[256];
    PredFitSnapshot *history;
    int history_count;
    int history_capacity;
    int generated_catalog_pending;
    int generated_catalog_active; /* current prediction is a generated catalogue */
    /* The catalogues merged into the current prediction, one per simulated
       Hamiltonian.  Empty while the prediction is a single external .cat. */
    SimulatedCatalog simulated[MAX_PICKETT_HAMILTONIANS];
    int n_simulated;
    /* Which broadened traces are drawn: see SpeciesTraceMode.  The total
       alone until the user asks for more - with many states the plot becomes
       a thicket. */
    int species_trace_mode;
    /* Rotates through the default palette so that two states of the project -
       in the same Hamiltonian or in two of them - never start the same
       colour. */
    int species_color_next;
    int intensity_dirty;
    int session_dirty;             /* save on exit only after an actual edit */
    char work_dir[512];       // persistent .fit working state (latest run)
    int last_fit_iterations;
    char status[160];
} PredFitState;

/* How the spectra are drawn.  Saved next to the executable, so the choices
 * follow the program instead of the directory it happens to be launched from.
 * Everything here is presentation only: no setting changes a measurement. */
typedef struct {
    int  trace_width;                    /* experimental trace, 1..5 px      */
    SDL_Color trace_color[MAX_SPECTRA];  /* colour assigned to each trace    */
    int  pred_width;                     /* predicted stick, 1..5 px         */
    int  profile_width;                  /* simulated profile, 1..5 px       */
    SDL_Color profile_color;
    int  profile_opacity;                /* percent: the sticks must stay readable */
    int  trace_opacity;                  /* percent, the default for a new trace */
    int  stick_opacity;                  /* percent, predicted sticks            */
    SDL_Color peak_color;                /* found peaks                      */
    SDL_Color assigned_color;            /* already-assigned frequencies     */
    SDL_Color bar_color;                 /* the movable bar                  */
    SDL_Color cursor_color;              /* pointer crosshair and readout    */
    int  show_grid;
    int  show_legend;
    int  show_peak_labels;
    int  show_blend_marks;               /* tick over blended predicted lines */
    int  plot_bg;                        /* 0 graphite, 1 black, 2 deep navy */

    /* Its own window, like Pred&Fit Advanced. */
    int  open;
    SDL_Window   *window;
    SDL_Renderer *renderer;
    Uint32 window_id;
    int  scroll;                         /* content offset, for a short window */
    char status[160];
    /* --- navigation: how far one keystroke moves the view --------------- */
    double nav_pan_px;            /* A / S, in pixels of the pane           */
    double nav_zoom_factor;       /* Q / E, > 1 (E uses its reciprocal)     */
    double nav_intensity_factor;  /* W / Z and shift-arrows                 */
    double nav_bar_px;            /* K / L, in pixels                       */
    double nav_fast_mult;         /* Caps Lock multiplier                   */
    double nav_trace_shift;       /* - / + on a trace, fraction of the pane */
    double nav_wheel_scroll;      /* inspector wheel, pixels per notch      */

    /* --- values a tool starts from ------------------------------------- */
    int    def_pf_sig, def_pf_noise;
    double def_pf_thresh;
    int    def_avg_window;
    double def_lorentz, def_gauss;
    double def_kaiser_beta;
    int    def_kaiser_ceros;
    double def_kaiser_intrinsic;
    double def_line_error;        /* .lin uncertainty written for SPFIT     */
    double def_int_min, def_int_max;

    /* --- where things are --------------------------------------------- */
    char spcat_path[512];         /* Pickett SPCAT executable               */
    char spfit_path[512];         /* Pickett SPFIT executable               */
    char data_dir[512];           /* working files; empty = launch directory */

    int  page;                    /* which settings page is shown           */

    /* A value being typed. Steppers are fine for a nudge and useless for a
       tenfold change, so every numeric field can also be edited directly. */
    int  edit_id;                 /* -1 when nothing is being edited        */
    char edit_buf[64];
    int  edit_caret, edit_anchor;
} AppSettings;

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
    INPUT_PF_A,
    INPUT_PF_B,
    INPUT_PF_C,
    INPUT_PF_MUA,
    INPUT_PF_MUB,
    INPUT_PF_MUC,
    INPUT_PF_TEMP,
    INPUT_PF_FMIN,
    INPUT_PF_FMAX,
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
    PendingLoadRequest pending_loads[MAX_PENDING_LOADS];
    int pending_load_head;
    int pending_load_count;
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
    DraggableWindow win_predfit;

    PredFitState predfit;

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

    AppSettings settings;

    /* Experimental spectra remembered from the previous session, read back from
       .fit/spectravisual.state so reopening the app restores the whole working
       set and not only the prediction. */
    SessionSpectrum session_spectrum[MAX_SPECTRA];
    int  n_session_spec;
    int  session_active_spec;
    int  session_has_view;
    int  session_sync_active;
    int  session_rolling_avg_window;
    double session_vxmin, session_vxmax, session_vymin, session_vymax;
    double session_pvxmin, session_pvxmax;

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

/* Producers include the event controller and Pred&Fit; consumption happens
   in main after events have been collected for the frame. */
int app_enqueue_pending_load(AppState *state, PendingLoadKind kind,
                             const char *path, int generated_catalog);

typedef struct {
    int win_w, win_h;
    int exp_x, exp_y, exp_w, exp_h;
    int pred_x, pred_y, pred_w, pred_h;
    int plot_x, gap;
    int plot_right;      // right edge available to the plot (shrinks when the inspector is open)
    int inspector_open;  // 1 while at least one tool panel is open
} Layout;

#endif
