#include "controller.h"
#include "algorithms.h"
#include "loader.h"
#include "settings.h"
#include "intensity_fit.h"
#include "predfit.h"
#include "layout.h"
#include "ui_chrome.h"
#include "ui_panels.h"
#include "view.h"
#include <SDL.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <errno.h>

#define ABS(x) ((x)<0?-(x):(x))

// --- INTERNAL HELPERS ---
static void handle_keydown(AppState *s, Layout *l, SDL_KeyboardEvent *key);
static void handle_text_input_event(AppState *s, char *text);
static void handle_mouse_down(AppState *s, Layout *l, SDL_MouseButtonEvent *b);
static void handle_mouse_up(AppState *s, Layout *l, SDL_MouseButtonEvent *b);
static void handle_mouse_motion(AppState *s, Layout *l, SDL_MouseMotionEvent *m);
static void handle_mouse_wheel(AppState *s, Layout *l, SDL_MouseWheelEvent *w);
static void run_right_click_peak_find(AppState *s, double x0, double x1);
static void assign_selected_predictions(AppState *s, double exp_freq, double exp_int);
static void clamp_assignment_scroll(AppState *s);
static void commit_text_input(AppState *s);
static int path_looks_like_cat(const char *path);
static const PredLine *current_assignment_prediction(const AppState *s, const Assignment *a);
static int save_assignments(AppState *s);

/* Assignments deliberately keep a copy of the CAT row selected at the time.
   When exporting, however, use the corresponding current CAT row when it is
   still present: temperature/concentration and a post-fit recalculation may
   have changed its calculated frequency or intensity. */
static const PredLine *current_assignment_prediction(const AppState *s, const Assignment *a) {
    if (!s || !a) return NULL;
    const PredLine *q = &a->pred;
    for (int i = 0; i < s->n_pred; i++) {
        const PredLine *p = &s->pred_lines[i];
        if (p->n_qn != q->n_qn) continue;
        if (p->Ju == q->Ju && p->Kau == q->Kau && p->Kcu == q->Kcu &&
            p->M1u == q->M1u && p->M2u == q->M2u && p->M3u == q->M3u &&
            p->Jl == q->Jl && p->Kal == q->Kal && p->Kcl == q->Kcl &&
            p->M1l == q->M1l && p->M2l == q->M2l && p->M3l == q->M3l)
            return p;
    }
    return q;
}

#define SAVE_ASSIGNMENTS_ERROR "Could not save the assignments"
#define SAVE_ASSIGNMENTS_SKIPPED "Assignments saved without"
#define EXPORT_LIST_ERROR      "Could not export the peak list"

/* A failed write is reported in the title bar; the next successful write of
   the same file clears it again. */
static void report_write_error(AppState *s, const char *what, const char *path) {
    snprintf(s->error_message, sizeof(s->error_message), "%s: cannot write %s (%s).",
             what, path, strerror(errno));
}

static void clear_write_error(AppState *s, const char *what) {
    if (strncmp(s->error_message, what, strlen(what)) == 0) s->error_message[0] = '\0';
}

/* Copies src to dst.  A missing src is nothing to keep, not an error. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return errno == ENOENT;
    FILE *out = fopen(dst, "wb");
    if (!out) { int e = errno; fclose(in); errno = e; return 0; }
    char buf[8192];
    size_t n;
    int ok = 1;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}

/* The only writer of assignments.txt: Save all, and every assignment,
   reassignment and deletion.  The list goes to a temporary file which then
   replaces the old one, so a failed write can never leave a truncated list, and
   the previous version is kept as assignments.txt.bak.
   Keep the data part in SPFIT .lin order: upper QNs, lower QNs, observed
   frequency.  The two fields which would be uncertainty and weight in .lin
   are instead the calculated frequency and predicted intensity.  NQN is
   retained at the end solely so this text file can restore the exact CAT QN
   layout. */
static int save_assignments(AppState *s) {
    char path[600], tmp[640], bak[640];
    settings_data_file(s, "assignments.txt", path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) { report_write_error(s, SAVE_ASSIGNMENTS_ERROR, tmp); return 0; }
    /* The header names the layout and its version, so the reader never has
       to guess it from a row (a .lin has the same field count). */
    fprintf(fp, "# SpectraVisual assignments, format %d: upper QNs, lower QNs (SPFIT .lin order), "
                "ObsFreq(MHz) CalcFreq(MHz) CalcIntensity NQN\n", ASSIGNMENT_FORMAT);
    int written = 0, skipped = 0;
    for (int k = 0; k < s->n_assignments; k++) {
        PredLine p2 = *current_assignment_prediction(s, &s->assignments[k]);
        int nq = p2.n_qn;
        /* Without NQN there is no record: inventing one would write numbers
           that are not the transition.  The row stays in the list, marked,
           until it is assigned again from a catalogue. */
        if (nq < 1 || nq > 6) { skipped++; continue; }
        const int upper[6] = {p2.Ju, p2.Kau, p2.Kcu, p2.M1u, p2.M2u, p2.M3u};
        const int lower[6] = {p2.Jl, p2.Kal, p2.Kcl, p2.M1l, p2.M2l, p2.M3l};
        for (int q = 0; q < nq; q++) fprintf(fp, "%3d", upper[q]);
        for (int q = 0; q < nq; q++) fprintf(fp, "%3d", lower[q]);
        /* Match the .lin's fixed 12I3 quantum-number field. */
        for (int q = 2 * nq; q < 12; q++) fputs("   ", fp);
        fprintf(fp, "%15.6f %15.6f %15.6E %d\n",
                s->assignments[k].exp_freq, p2.freq_mhz,
                p2.linear_int, nq);
        written++;
    }
    int failed = ferror(fp);
    if (fclose(fp) != 0 || failed) { report_write_error(s, SAVE_ASSIGNMENTS_ERROR, tmp); remove(tmp); return 0; }
    if (!copy_file(path, bak))     { report_write_error(s, SAVE_ASSIGNMENTS_ERROR, bak); remove(tmp); return 0; }
    if (rename(tmp, path) != 0)    { report_write_error(s, SAVE_ASSIGNMENTS_ERROR, path); remove(tmp); return 0; }
    snprintf(s->status_message, sizeof(s->status_message), "Saved %d assignments to %s.", written, path);
    clear_write_error(s, SAVE_ASSIGNMENTS_ERROR);
    clear_write_error(s, SAVE_ASSIGNMENTS_SKIPPED);
    if (skipped)
        snprintf(s->error_message, sizeof(s->error_message),
                 SAVE_ASSIGNMENTS_SKIPPED " %d row%s whose NQN is unknown: assign %s again from a catalogue.",
                 skipped, skipped == 1 ? "" : "s", skipped == 1 ? "it" : "them");
    return 1;
}

// --- TEXT FIELD EDITING ---------------------------------------------------
// The focused field behaves like any other text field: the value starts
// selected, typing replaces the selection, and the caret can be placed with the
// arrows or with the mouse.

static int input_sel_lo(const AppState *s) { return s->input_caret < s->input_anchor ? s->input_caret : s->input_anchor; }
static int input_sel_hi(const AppState *s) { return s->input_caret > s->input_anchor ? s->input_caret : s->input_anchor; }
static int input_len(const AppState *s)    { return (int)strlen(s->text_input_buf); }

static void input_clamp(AppState *s) {
    int n = input_len(s);
    if (s->input_caret  < 0) s->input_caret  = 0;
    if (s->input_caret  > n) s->input_caret  = n;
    if (s->input_anchor < 0) s->input_anchor = 0;
    if (s->input_anchor > n) s->input_anchor = n;
}

// Removes the selected range. Returns 1 if anything was removed.
static int input_delete_selection(AppState *s) {
    input_clamp(s);
    int lo = input_sel_lo(s), hi = input_sel_hi(s);
    if (lo == hi) return 0;
    memmove(s->text_input_buf + lo, s->text_input_buf + hi, strlen(s->text_input_buf + hi) + 1);
    s->input_caret = s->input_anchor = lo;
    return 1;
}

static void input_insert(AppState *s, const char *text) {
    input_delete_selection(s);
    int n = input_len(s), add = (int)strlen(text);
    int room = (int)sizeof(s->text_input_buf) - 1 - n;
    if (add > room) add = room;
    if (add <= 0) return;
    memmove(s->text_input_buf + s->input_caret + add, s->text_input_buf + s->input_caret,
            (size_t)(n - s->input_caret) + 1);
    memcpy(s->text_input_buf + s->input_caret, text, (size_t)add);
    s->input_caret += add;
    s->input_anchor = s->input_caret;
}

static void input_move(AppState *s, int caret, int keep_selection) {
    s->input_caret = caret;
    if (!keep_selection) s->input_anchor = caret;
    input_clamp(s);
}

// Gives a field focus. Clicking the field that already had it places the caret
// where the pointer is instead of selecting the value again.
static void input_focus(AppState *s, int which, int mx) {
    int again = (s->input_last == which);
    s->input_state = (InputState)which;
    SDL_StartTextInput();
    if (again && s->input_rect.w > 0) {
        s->input_caret = ui_field_caret_at(s->input_rect, s->text_input_buf, s->input_unit, mx);
        s->input_anchor = s->input_caret;
    } else {
        s->input_anchor = 0;
        s->input_caret = input_len(s);      /* the value starts selected */
    }
}

static void input_copy(AppState *s, int cut) {
    input_clamp(s);
    int lo = input_sel_lo(s), hi = input_sel_hi(s);
    if (lo == hi) { lo = 0; hi = input_len(s); }
    char tmp[64];
    int n = hi - lo;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, s->text_input_buf + lo, (size_t)n);
    tmp[n] = '\0';
    SDL_SetClipboardText(tmp);
    if (cut) { s->input_anchor = lo; s->input_caret = hi; input_delete_selection(s); }
}

static void input_paste(AppState *s) {
    char *clip = SDL_GetClipboardText();
    if (!clip) return;
    char clean[64]; int o = 0;
    for (const char *p = clip; *p && o < (int)sizeof(clean) - 1; p++)
        if (*p != '\n' && *p != '\r' && *p != '\t') clean[o++] = *p;
    clean[o] = '\0';
    SDL_free(clip);
    input_insert(s, clean);
}

// Scale the per-spectrum intensity gain. By default all visible spectra are
// scaled together; with "individual intensity" enabled only the active one is.
static void scale_intensity(AppState *s, double factor) {
    if (s->multi_indiv_int) {
        if (s->active_spec >= 0 && s->active_spec < s->n_spectra)
            s->spectra[s->active_spec].vscale *= factor;
    } else {
        for (int i = 0; i < s->n_spectra; i++)
            if (s->spectra[i].visible) s->spectra[i].vscale *= factor;
    }
}

// --- MAIN EVENT LOOP ---
void handle_app_events(AppState *state, Layout *l, int *running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { *running = 0; return; }
        if (predfit_handle_advanced_event(state, &e)) continue;
        if (settings_handle_event(state, &e)) continue;

        Uint32 window_id = 0;
        if (e.type == SDL_TEXTINPUT) window_id = e.text.windowID;
        else if (e.type == SDL_KEYDOWN) window_id = e.key.windowID;
        else if (e.type == SDL_WINDOWEVENT) window_id = e.window.windowID;
        int secondary = window_id != 0 &&
            (window_id == state->predfit.advanced_window_id || window_id == state->settings.window_id);
        if (secondary && e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
            state->input_state != INPUT_NONE) {
            state->input_last = (int)state->input_state;
            commit_text_input(state);
        }
        if (secondary && e.type == SDL_TEXTINPUT) continue;
        if (secondary && e.type == SDL_KEYDOWN) {
            SDL_Keycode key = e.key.keysym.sym;
            SDL_Keymod mod = e.key.keysym.mod;
            int global = (mod & (KMOD_GUI | KMOD_CTRL)) && (key == SDLK_f || key == SDLK_b);
            if (!global) continue;
        }

        if (state->input_state != INPUT_NONE) {
            if (e.type == SDL_TEXTINPUT) {
                handle_text_input_event(state, e.text.text);
                continue;
            } 
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keymod mod = SDL_GetModState();
                int cmd   = (mod & (KMOD_GUI | KMOD_CTRL)) != 0;
                int shift = (mod & KMOD_SHIFT) != 0;
                SDL_Keycode k = e.key.keysym.sym;

                if (cmd && k == SDLK_a) {
                    state->input_anchor = 0;
                    state->input_caret = (int)strlen(state->text_input_buf);
                    continue;
                }
                if (cmd && (k == SDLK_c || k == SDLK_x)) { input_copy(state, k == SDLK_x); continue; }
                if (cmd && k == SDLK_v) { input_paste(state); continue; }

                switch (k) {
                    case SDLK_LEFT:
                        input_move(state, cmd ? 0 : state->input_caret - 1, shift);
                        continue;
                    case SDLK_RIGHT:
                        input_move(state, cmd ? (int)strlen(state->text_input_buf)
                                              : state->input_caret + 1, shift);
                        continue;
                    case SDLK_HOME: input_move(state, 0, shift); continue;
                    case SDLK_END:  input_move(state, (int)strlen(state->text_input_buf), shift); continue;
                    case SDLK_BACKSPACE:
                        if (!input_delete_selection(state) && state->input_caret > 0) {
                            input_move(state, state->input_caret - 1, 1);
                            input_delete_selection(state);
                        }
                        continue;
                    case SDLK_DELETE:
                        if (!input_delete_selection(state) &&
                            state->input_caret < (int)strlen(state->text_input_buf)) {
                            input_move(state, state->input_caret + 1, 1);
                            input_delete_selection(state);
                        }
                        continue;
                    case SDLK_ESCAPE:
                        state->input_state = INPUT_NONE;
                        SDL_StopTextInput();
                        continue;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        commit_text_input(state);
                        continue;
                    default:
                        continue;
                }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) {
                state->input_last = (int)state->input_state;
                commit_text_input(state);
            }
        }

        switch (e.type) {
            case SDL_DROPFILE: {
                char *path = e.drop.file;
                PendingLoadKind kind = path_looks_like_cat(path) ? PENDING_LOAD_CATALOG
                    : predfit_is_session_file(state, path) ? PENDING_LOAD_SESSION
                    : PENDING_LOAD_SPECTRUM;
                app_enqueue_pending_load(state, kind, path, 0);
                SDL_free(path);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                handle_mouse_down(state, l, &e.button);
                state->input_last = INPUT_NONE;
                break;
            case SDL_MOUSEBUTTONUP:   handle_mouse_up(state, l, &e.button); break;
            case SDL_MOUSEMOTION:     handle_mouse_motion(state, l, &e.motion); break;
            case SDL_MOUSEWHEEL:      handle_mouse_wheel(state, l, &e.wheel); break;
            case SDL_KEYDOWN:         handle_keydown(state, l, &e.key); break;
        }
    }
}

// --- MOUSE DOWN (Hit Testing) ---
static void handle_mouse_down(AppState *s, Layout *l, SDL_MouseButtonEvent *b) {
    int mx = b->x; 
    int my = b->y;

    // 1. Inspector panels.
    //    The panels are docked in the right-hand column; every rectangle below
    //    comes from ui_panels.h, the same header the renderer draws from.
    {
        DraggableWindow *panels[UI_TOOL_COUNT] = {
            &s->win_as, &s->win_pf, &s->win_avg, &s->win_br,
            &s->win_dip, &s->win_cut, &s->win_filt, &s->win_jump, &s->win_spec, &s->win_predfit
        };
        for (int t = 0; t < UI_TOOL_COUNT; t++) {
            DraggableWindow *p = panels[t];
            /* p->clip is the part actually on screen.  Scrolling the column
               moves a panel's rectangle up behind the command bar, and testing
               the rectangle alone let it swallow clicks meant for the buttons
               up there - which is why the settings window would not open with
               several panels open. */
            if (!p->visible) continue;
            if (!point_in_rect(mx, my, p->clip)) continue;
            if (!point_in_rect(mx, my, p->rect)) continue;
            SDL_Rect w = p->rect;

            /* header: the only control is close */
            if (my < w.y + UI_SECTION_HEAD_H) {
                if (mx > w.x + w.w - 30) p->visible = 0;
                return;
            }
            if (b->button != SDL_BUTTON_LEFT) return;

            switch (t) {
            case UI_TOOL_ASSIGN: {
                clamp_assignment_scroll(s);
                int rows = ui_as_rows(s);
                int start_idx = s->assignments_scroll;
                if (start_idx > s->n_assignments - rows) start_idx = s->n_assignments - rows;
                if (start_idx < 0) start_idx = 0;
                for (int i = 0; i < rows; i++) {
                    int k = start_idx + i;
                    if (k >= s->n_assignments) break;
                    if (point_in_rect(mx, my, ui_as_row(w, i))) { s->selected_assignment = k; return; }
                }
                if (point_in_rect(mx, my, ui_as_save(s, w))) {
                    /* Persist the same canonical assignment list that SPFIT
                       receives; old sessions may still contain duplicates
                       created before QN-based identity was introduced. */
                    deduplicate_assignments(s->assignments, &s->n_assignments);
                    if (s->selected_assignment >= s->n_assignments)
                        s->selected_assignment = s->n_assignments - 1;
                    save_assignments(s);
                    return;
                }
                if (point_in_rect(mx, my, ui_as_delete(s, w))) { delete_assignment(s, s->selected_assignment); return; }
                return;
            }

            case UI_TOOL_PEAKS:
                if (point_in_rect(mx, my, ui_pf_sig(w))) {
                    snprintf(s->text_input_buf, 32, "%d", s->pf_sig_pts);
                    input_focus(s, INPUT_PF_SIG, mx);
                } else if (point_in_rect(mx, my, ui_pf_noise(w))) {
                    snprintf(s->text_input_buf, 32, "%d", s->pf_noise_pts);
                    input_focus(s, INPUT_PF_NOISE, mx);
                } else if (point_in_rect(mx, my, ui_pf_thresh(w))) {
                    snprintf(s->text_input_buf, 32, "%.1f", s->pf_thresh);
                    input_focus(s, INPUT_PF_THRESH, mx);
                } else if (point_in_rect(mx, my, ui_pf_find(w))) {
                    run_peak_finder(s->current_pts, s->n_pts, s->vxmin, s->vxmax,
                                    s->pf_sig_pts, s->pf_noise_pts, s->pf_thresh, s->peaks, &s->n_peaks);
                } else if (point_in_rect(mx, my, ui_pf_export(w))) {
                    char path[600];
                    settings_data_file(s, "linelist.csv", path, sizeof(path));
                    FILE *fp = fopen(path, "w");
                    if (!fp) { report_write_error(s, EXPORT_LIST_ERROR, path); return; }
                    fprintf(fp, "Freq,Int\n");
                    for (int k = 0; k < s->n_peaks; k++)
                        fprintf(fp, "%.6f,%.6e\n", s->peaks[k].x, s->peaks[k].y);
                    int failed = ferror(fp);
                    if (fclose(fp) != 0 || failed) { report_write_error(s, EXPORT_LIST_ERROR, path); return; }
                    snprintf(s->status_message, sizeof(s->status_message), "Exported %d peaks to %s.", s->n_peaks, path);
                    clear_write_error(s, EXPORT_LIST_ERROR);
                }
                return;

            case UI_TOOL_AVG:
                if (point_in_rect(mx, my, ui_avg_field(w))) {
                    snprintf(s->text_input_buf, 32, "%d", s->rolling_avg_window);
                    input_focus(s, INPUT_AVG_PTS, mx);
                } else if (point_in_rect(mx, my, ui_avg_toggle(w))) {
                    s->rolling_avg_active = !s->rolling_avg_active;
                    if (s->rolling_avg_active) {
                        apply_rolling_average(s->raw_pts, s->smooth_pts, s->n_pts, s->rolling_avg_window);
                        s->current_pts = s->smooth_pts;
                    } else {
                        s->current_pts = s->raw_pts;
                    }
                }
                return;

            case UI_TOOL_BROAD: {
                SDL_Rect mode = ui_br_mode(w);
                if (point_in_rect(mx, my, mode)) {
                    s->broaden_mode = (mx < mode.x + mode.w / 2) ? 0 : 1;
                    s->input_state = INPUT_NONE;
                    return;
                }
                if (point_in_rect(mx, my, ui_br_f1(w))) {
                    if (s->broaden_mode == 0) {
                        snprintf(s->text_input_buf, 32, "%.2f", s->lorentz_gamma);
                        input_focus(s, INPUT_GAMMA, mx);
                    } else {
                        snprintf(s->text_input_buf, 32, "%.2f", s->kaiser_beta);
                        input_focus(s, INPUT_KBETA, mx);
                    }
                } else if (point_in_rect(mx, my, ui_br_f2(w))) {
                    if (s->broaden_mode == 0) {
                        snprintf(s->text_input_buf, 32, "%.2f", s->gauss_gamma);
                        input_focus(s, INPUT_GAUSS, mx);
                    } else {
                        snprintf(s->text_input_buf, 32, "%d", s->kaiser_ceros);
                        input_focus(s, INPUT_KCEROS, mx);
                    }
                } else if (s->broaden_mode == 1 && point_in_rect(mx, my, ui_br_f3(w))) {
                    snprintf(s->text_input_buf, 32, "%.3f", s->kaiser_intrinsic);
                    input_focus(s, INPUT_KINTR, mx);
                } else if (point_in_rect(mx, my, ui_br_toggle(w, s->broaden_mode == 1))) {
                    s->broadening_active = !s->broadening_active;
                }
                return;
            }

            case UI_TOOL_DIP: {
                static const int cat_input[3] = {INPUT_MUCAT_A, INPUT_MUCAT_B, INPUT_MUCAT_C};
                static const int red_input[3] = {INPUT_MURED_A, INPUT_MURED_B, INPUT_MURED_C};
                if (point_in_rect(mx, my, ui_int_cat_temp(w))) {
                    snprintf(s->text_input_buf, 32, "%.2f", s->cat_temp_k);
                    input_focus(s, INPUT_CAT_TEMP, mx);
                    return;
                }
                if (point_in_rect(mx, my, ui_int_rot_temp(w))) {
                    snprintf(s->text_input_buf, 32, "%.2f", s->rot_temp_k);
                    input_focus(s, INPUT_ROT_TEMP, mx);
                    return;
                }
                for (int i = 0; i < 3; i++) {
                    if (point_in_rect(mx, my, ui_dip_field(w, i, 0))) {
                        snprintf(s->text_input_buf, 32, "%.4f", s->dipole_cat[i]);
                        input_focus(s, cat_input[i], mx);
                        return;
                    }
                    if (point_in_rect(mx, my, ui_dip_field(w, i, 1))) {
                        snprintf(s->text_input_buf, 32, "%.4f", s->dipole_red[i]);
                        input_focus(s, red_input[i], mx);
                        return;
                    }
                }
                if (point_in_rect(mx, my, ui_fit_window(w))) {
                    snprintf(s->text_input_buf, 32, "%.4f", s->intfit_half_window_mhz);
                    input_focus(s, INPUT_FIT_WINDOW, mx);
                    return;
                }
                if (point_in_rect(mx, my, ui_fit_temperature(w))) {
                    s->intfit_fit_temperature = !s->intfit_fit_temperature;
                    return;
                }
                for (int i = 0; i < 3; i++) {
                    if (point_in_rect(mx, my, ui_fit_dipole(w, i))) {
                        s->intfit_fit_dipole[i] = !s->intfit_fit_dipole[i];
                        return;
                    }
                }
                if (point_in_rect(mx, my, ui_fit_run(w))) {
                    intensity_fit_run(s);
                    if (s->pred_lines && s->n_pred > 0)
                        rescale_predicted_intensities(s->pred_lines, s->n_pred,
                                                      s->cat_temp_k, s->rot_temp_k,
                                                      s->dipole_cat, s->dipole_red,
                                                      &s->pred_global_max);
                    return;
                }
                if (point_in_rect(mx, my, ui_fit_export(w))) {
                    if (!intensity_fit_export(s, "intensity_fit.ifit"))
                        snprintf(s->intfit_message, sizeof(s->intfit_message), "Run a fit before exporting.");
                    else
                        snprintf(s->intfit_message, sizeof(s->intfit_message), "Saved intensity_fit.ifit");
                    return;
                }
                return;
            }

            case UI_TOOL_CUT:
                if (point_in_rect(mx, my, ui_cut_min(w))) {
                    snprintf(s->text_input_buf, 32, "%.1f", s->pred_min_log_int);
                    input_focus(s, INPUT_PRED_MIN, mx);
                } else if (point_in_rect(mx, my, ui_cut_max(w))) {
                    snprintf(s->text_input_buf, 32, "%.1f", s->pred_max_log_int);
                    input_focus(s, INPUT_PRED_MAX, mx);
                }
                return;

            case UI_TOOL_JUMP:
                if (point_in_rect(mx, my, ui_jump_start(w))) {
                    snprintf(s->text_input_buf, 32, "%.1f", s->vxmin);
                    input_focus(s, INPUT_JUMP_MIN, mx);
                } else if (point_in_rect(mx, my, ui_jump_end(w))) {
                    snprintf(s->text_input_buf, 32, "%.1f", s->vxmax);
                    input_focus(s, INPUT_JUMP_MAX, mx);
                }
                return;

            case UI_TOOL_FILTER: {
                if (point_in_rect(mx, my, ui_filt_master(w))) { s->filter_active = !s->filter_active; return; }
                for (int i = 0; i < 3; i++)
                    if (point_in_rect(mx, my, ui_filt_mu(w, i))) { s->filt_mu[i] = !s->filt_mu[i]; return; }
                for (int i = 0; i < 3; i++)
                    if (point_in_rect(mx, my, ui_filt_br(w, i))) { s->filt_br[i] = !s->filt_br[i]; return; }
                if (point_in_rect(mx, my, ui_filt_range(w))) { s->filt_use_range = !s->filt_use_range; return; }

                int   qn_in[3][2] = {{INPUT_FILT_JMIN,  INPUT_FILT_JMAX},
                                     {INPUT_FILT_KAMIN, INPUT_FILT_KAMAX},
                                     {INPUT_FILT_KCMIN, INPUT_FILT_KCMAX}};
                int  *qn_val[3][2] = {{&s->filt_j_min,  &s->filt_j_max},
                                      {&s->filt_ka_min, &s->filt_ka_max},
                                      {&s->filt_kc_min, &s->filt_kc_max}};
                for (int i = 0; i < 3; i++) {
                    for (int c = 0; c < 2; c++) {
                        if (point_in_rect(mx, my, ui_filt_qn(w, i, c))) {
                            snprintf(s->text_input_buf, 32, "%d", *qn_val[i][c]);
                            input_focus(s, qn_in[i][c], mx);
                            return;
                        }
                    }
                }
                if (point_in_rect(mx, my, ui_filt_jump(w))) { s->filt_use_delta = !s->filt_use_delta; return; }
                int  d_in[3]  = {INPUT_FILT_DJ, INPUT_FILT_DKA, INPUT_FILT_DKC};
                int *d_val[3] = {&s->filt_dj, &s->filt_dka, &s->filt_dkc};
                for (int i = 0; i < 3; i++) {
                    if (point_in_rect(mx, my, ui_filt_delta(w, i))) {
                        snprintf(s->text_input_buf, 32, "%d", *d_val[i]);
                        input_focus(s, d_in[i], mx);
                        return;
                    }
                }
                return;
            }

            case UI_TOOL_SPECTRA: {
                SDL_Rect lay = ui_spec_layout(w), yn = ui_spec_ynorm(w);
                if (point_in_rect(mx, my, lay)) { s->multi_layout = (mx >= lay.x + lay.w / 2); return; }
                if (point_in_rect(mx, my, yn))  { s->multi_ynorm  = (mx >= yn.x + yn.w / 2);  return; }
                if (point_in_rect(mx, my, ui_spec_indiv(w))) { s->multi_indiv_int = !s->multi_indiv_int; return; }
                for (int i = 0; i < s->n_spectra; i++) {
                    if (point_in_rect(mx, my, ui_spec_op_minus(w, i))) {
                        if (s->spectra[i].opacity <= 0) s->spectra[i].opacity = 100;   /* loaded before opacity existed */
                        s->spectra[i].opacity -= 5;
                        if (s->spectra[i].opacity < 10) s->spectra[i].opacity = 10;
                        return;
                    }
                    if (point_in_rect(mx, my, ui_spec_op_plus(w, i))) {
                        if (s->spectra[i].opacity <= 0) s->spectra[i].opacity = 100;
                        s->spectra[i].opacity += 5;
                        if (s->spectra[i].opacity > 100) s->spectra[i].opacity = 100;
                        return;
                    }
                    if (point_in_rect(mx, my, ui_spec_minus(w, i))) { s->spectra[i].voffset -= s->settings.nav_trace_shift; return; }
                    if (point_in_rect(mx, my, ui_spec_plus(w, i)))  { s->spectra[i].voffset += s->settings.nav_trace_shift; return; }
                    if (point_in_rect(mx, my, ui_spec_vis(w, i)))   { s->spectra[i].visible = !s->spectra[i].visible; return; }
                    if (point_in_rect(mx, my, ui_spec_del(w, i)))   { s->pending_remove = i; return; }
                    if (point_in_rect(mx, my, ui_spec_name(w, i)))  { s->pending_select = i; return; }
                }
                return;
            }
            case UI_TOOL_PREDFIT: {
                PredFitState *p=&s->predfit;
                int in[]={INPUT_PF_A,INPUT_PF_B,INPUT_PF_C,INPUT_PF_MUA,INPUT_PF_MUB,INPUT_PF_MUC,INPUT_PF_TEMP,INPUT_PF_FMIN,INPUT_PF_FMAX};
                double *v[]={&p->a,&p->b,&p->c,&p->mu[0],&p->mu[1],&p->mu[2],&p->temp_k,&p->fmin_ghz,&p->fmax_ghz};
                for(int i=0;i<9;i++) if(point_in_rect(mx,my,ui_pf_model(w,i+1))){snprintf(s->text_input_buf,32,"%.8g",*v[i]);input_focus(s,in[i],mx);return;}
                if(point_in_rect(mx,my,ui_pf_calculate(w))){predfit_calculate(s);return;}
                if(point_in_rect(mx,my,ui_pf_fit(w))){predfit_fit(s);return;}
                if(point_in_rect(mx,my,ui_pf_undo(w))){predfit_undo_last_fit(s);return;}
                if(point_in_rect(mx,my,ui_pf_advanced(w))){predfit_open_advanced(s);return;}
                return;
            }

            default:
                return;
            }
        }
    }

    // 2. Rail and command bar.
    //    Both rectangles come from ui_chrome.h, the same source the renderer
    //    draws from, so a visual change cannot move the click targets away.
    if (b->button == SDL_BUTTON_LEFT) {
        DraggableWindow *panels[UI_TOOL_COUNT] = {
            &s->win_as, &s->win_pf, &s->win_avg, &s->win_br,
            &s->win_dip, &s->win_cut, &s->win_filt, &s->win_jump, &s->win_spec, &s->win_predfit
        };
        for (int t = 0; t < UI_TOOL_COUNT; t++) {
            if (point_in_rect(mx, my, ui_rail_rect(t))) {
                panels[t]->visible = !panels[t]->visible;
                return;
            }
        }
        if (ui_top_right_visible(l->win_w) &&
            point_in_rect(mx, my, ui_top_rect(UI_TOP_HELP, l->win_w))) {
            s->show_help = !s->show_help;
            return;
        }
        /* the rail itself swallows clicks so they never reach the plot */
        if (mx < UI_RAIL_W && my >= UI_CONTENT_Y) return;

        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_BAR, l->win_w))) {
            s->bar_active = !s->bar_active;
            if (s->bar_active) {
                s->bar_x  = (s->vxmin + s->vxmax) / 2.0;
                s->pbar_x = (s->pvxmin + s->pvxmax) / 2.0;
            }
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_MEASURE, l->win_w))) {
            s->measure_active = !s->measure_active;
            s->measure_phase = 0;
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_SYNC, l->win_w))) {
            s->sync_active = !s->sync_active;
            if (s->sync_active) { s->pvxmin = s->vxmin; s->pvxmax = s->vxmax; }
            return;
        }
        if (ui_top_right_visible(l->win_w) &&
            point_in_rect(mx, my, ui_top_rect(UI_TOP_SETTINGS, l->win_w))) {
            settings_open(s);
            return;
        }
        if (point_in_rect(mx, my, ui_top_rect(UI_TOP_DELPEAK, l->win_w))) {
            if (s->n_peaks > 0) s->n_peaks--;
            return;
        }
        if (ui_top_right_visible(l->win_w)) {
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_EXPORT, l->win_w))) {
                if (s->data_loaded) s->export_requested = 1;
                return;
            }
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_OFFSET, l->win_w))) {
                snprintf(s->text_input_buf, 32, "%.4f", s->exp_offset);
                input_focus(s, INPUT_OFFSET, mx);
                return;
            }
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_CAT_TEMP, l->win_w))) {
                snprintf(s->text_input_buf, 32, "%.2f", s->cat_temp_k);
                input_focus(s, INPUT_CAT_TEMP, mx);
                return;
            }
            if (point_in_rect(mx, my, ui_top_rect(UI_TOP_ROT_TEMP, l->win_w))) {
                snprintf(s->text_input_buf, 32, "%.2f", s->rot_temp_k);
                input_focus(s, INPUT_ROT_TEMP, mx);
                return;
            }
        }
        /* clicks on the chrome bands never fall through to the plot */
        if (my < UI_CONTENT_Y) return;
    }

    if (!s->data_loaded) return;

    // 3. Canvas Interactions
    SDL_Rect r_exp = {l->exp_x, l->exp_y, l->exp_w, l->exp_h};
    SDL_Rect r_pred = {l->pred_x, l->pred_y, l->pred_w, l->pred_h};

    // --- NEW: Measure Tool Logic ---
    if (s->measure_active && !(SDL_GetModState() & KMOD_ALT) && point_in_rect(mx, my, r_exp) && b->button == SDL_BUTTON_LEFT) {
        // Use the TRUE (un-offset) frequency: the spectrum is only shifted visually.
        double freq = s->vxmin + ((double)(mx - l->exp_x) / l->exp_w) * (s->vxmax - s->vxmin) - s->exp_offset;

        if (s->measure_phase == 0) {
            s->measure_x1 = freq;
            s->measure_phase = 1; // Waiting for second click
        } else {
            double dist = fabs(freq - s->measure_x1);
            printf("Measured Distance: %.4f MHz\n", dist);
            s->measure_phase = 0; // Reset for next measurement
        }
        return; // Consume click so we don't zoom
    }    

    if (point_in_rect(mx, my, r_exp)) {
        if (b->button == SDL_BUTTON_LEFT) {
            if (SDL_GetModState() & KMOD_ALT) {
                s->dragging_offset = 1;
                s->drag_last_x = mx;
                return;
            }
            s->selecting_left = 1;
            s->sel_start = (SDL_Point){mx, my};
            s->sel_cur = s->sel_start;
        } else if (b->button == SDL_BUTTON_RIGHT) {
            s->selecting_right = 1;
            s->sel_start = (SDL_Point){mx, my};
            s->sel_cur = s->sel_start;
        }
    } 
    else if (point_in_rect(mx, my, r_pred)) {
        if (b->button == SDL_BUTTON_LEFT) {
            // Prediction Selection Logic
            int mod = SDL_GetModState();
            int append = (mod & KMOD_GUI) || (mod & KMOD_CTRL);
            if (!append) s->n_selected = 0;

            double freq_per_pixel = (s->pvxmax - s->pvxmin) / l->pred_w;
            double tolerance = 5.0 * freq_per_pixel; 
            double click_freq = s->pvxmin + ((double)(mx - l->pred_x) / l->pred_w) * (s->pvxmax - s->pvxmin);
            double raw_click_freq = click_freq;   // predictions live at their true freq now

            for(int i=0; i < s->n_pred; i++) {
            // Ignore lines hidden by the intensity cut or the active filters
                if (!pred_passes_filter(s, i)) continue;

                if(fabs(s->pred_lines[i].freq_mhz - raw_click_freq) < tolerance) {
                    if(s->n_selected < MAX_SELECTED) {
                        // Avoid duplicates
                        int dup = 0;
                        for(int k=0; k < s->n_selected; k++) if(s->selected_indices[k] == i) dup=1;
                        if(!dup) s->selected_indices[s->n_selected++] = i;
                    }
                }
            }
        }
    }
}

// --- MOUSE MOTION (Dragging) ---
static void handle_mouse_motion(AppState *s, Layout *l, SDL_MouseMotionEvent *m) {
    // Dragging inside the focused field selects text, as in any text field.
    if (s->input_state != INPUT_NONE && (m->state & SDL_BUTTON_LMASK) &&
        s->input_rect.w > 0 && point_in_rect(m->x, m->y, s->input_rect)) {
        s->input_caret = ui_field_caret_at(s->input_rect, s->text_input_buf, s->input_unit, m->x);
        return;
    }
    if (s->drag_target) {
        s->drag_target->rect.x = m->x - s->drag_offset.x;
        s->drag_target->rect.y = m->y - s->drag_offset.y;
    } 
    else if (s->dragging_offset) {
        if (l->exp_w <= 0) return;
        double freq_per_pixel = (s->vxmax - s->vxmin) / (double)l->exp_w;
        s->exp_offset += (m->x - s->drag_last_x) * freq_per_pixel;
        s->drag_last_x = m->x;
    }
    else if (s->selecting_left || s->selecting_right) {
        s->sel_cur.x = m->x;
        s->sel_cur.y = m->y;
    }
}

static void handle_mouse_wheel(AppState *s, Layout *l, SDL_MouseWheelEvent *w) {
    if (w->y == 0) return;

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    // Over the assignments table -> scroll that list.
    if (s->win_as.visible && point_in_rect(mx, my, s->win_as.rect)) {
        s->assignments_scroll += (w->y > 0) ? -3 : 3;
        clamp_assignment_scroll(s);
        return;
    }

    // Anywhere over the docked panel column -> scroll the sidebar stack.
    // (clamping happens in update_sidebars, which knows the content height)
    if (mx > l->plot_right) {
        s->sidebar_scroll += (w->y > 0) ? -s->settings.nav_wheel_scroll : s->settings.nav_wheel_scroll;
    }
}

// --- MOUSE UP (Action Completion) ---
static void handle_mouse_up(AppState *s, Layout *l, SDL_MouseButtonEvent *b) {
    s->drag_target = NULL;
    if (b->button == SDL_BUTTON_LEFT && s->dragging_offset) {
        s->dragging_offset = 0;
        printf("Prediction offset: %.4f MHz\n", s->exp_offset);
        return;
    }

    // A. Left Click Drag (Zoom)
    if (b->button == SDL_BUTTON_LEFT && s->selecting_left) {
        s->selecting_left = 0;
        if (ABS(s->sel_cur.x - s->sel_start.x) > 5) {
            double x0 = s->vxmin + (double)(s->sel_start.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin);
            double x1 = s->vxmin + (double)(s->sel_cur.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin);
            
            if (x1 < x0) { double t=x0; x0=x1; x1=t; }
            s->vxmin = x0; 
            s->vxmax = x1;
            
            // Sync Prediction if needed
            if(s->sync_active) {
                s->pvxmin = x0; 
                s->pvxmax = x1;
            }
        }
    }

    // B. Right Click Drag (Peak Find)
    if (b->button == SDL_BUTTON_RIGHT && s->selecting_right) {
        s->selecting_right = 0;
        // Convert the screen selection to TRUE data coords (subtract the visual
        // offset) so the peak is found and stored at its original frequency.
        double x0 = s->vxmin + (double)(s->sel_start.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin) - s->exp_offset;
        double x1 = s->vxmin + (double)(s->sel_cur.x - l->exp_x)/l->exp_w * (s->vxmax - s->vxmin) - s->exp_offset;
        if (x1 < x0) { double t=x0; x0=x1; x1=t; }

        run_right_click_peak_find(s, x0, x1);
    }
}

// --- HELPER: Right Click Peak Calculation ---
static void run_right_click_peak_find(AppState *s, double raw_x0, double raw_x1) {
    if (!s->data_loaded) return;
    int start = binary_search_lower(s->current_pts, s->n_pts, raw_x0);
    int end   = binary_search_upper(s->current_pts, s->n_pts, raw_x1);
    if(start < 0) start = 0; 
    if(end >= s->n_pts) end = s->n_pts - 1;

    // Find max Y in range
    double best_y = -1e99;
    int bi = -1;
    for(int i=start; i<=end; i++) {
        if(s->current_pts[i].y > best_y) { 
            best_y = s->current_pts[i].y; 
            bi = i; 
        }
    }

    if (bi >= 1 && bi < s->n_pts - 1 && s->n_peaks < MAX_PEAKS) {
        // Parabolic Interpolation
        double y1 = s->current_pts[bi-1].y;
        double y2 = s->current_pts[bi].y;
        double y3 = s->current_pts[bi+1].y;
        double x2 = s->current_pts[bi].x;
        
        double px, py;
        double denom = (y1 - 2*y2 + y3);
        
        if (fabs(denom) < 1e-18) { 
            px = x2; 
            py = y2; 
        } else {
            double dx = 0.5 * ((y1 - y3) / denom);
            // Convert index delta to freq delta
            double freq_step = s->current_pts[bi+1].x - x2; 
            px = x2 + dx * freq_step;
            py = y2 - 0.25 * (y1 - y3) * dx;
        }
        
        s->peaks[s->n_peaks].x = px; 
        s->peaks[s->n_peaks].y = py;
        s->n_peaks++;
        
        // Clipboard
        char clip[64]; snprintf(clip, 64, "%.4f", px);
        SDL_SetClipboardText(clip);
        printf("Peak found: %.4f (Copied to clipboard)\n", px);

        // Auto-assign if prediction is selected
        assign_selected_predictions(s, px, py);
    }
}

static void assign_selected_predictions(AppState *s, double exp_freq, double exp_int) {
    if (s->n_selected <= 0) return;

    int requested = s->n_selected, assigned = 0;
    for(int k = 0; k < requested; k++) {
        int idx = s->selected_indices[k];
        if (idx < 0 || idx >= s->n_pred) continue;
        add_or_update_assignment(s->assignments, &s->n_assignments,
                                 s->pred_lines[idx], exp_freq, exp_int);
        assigned++;
    }

    printf("Assigned %d selected predicted line%s to %.4f MHz\n",
           requested, requested == 1 ? "" : "s", exp_freq);
    s->n_selected = 0;
    s->assignments_scroll = s->n_assignments - 13;
    clamp_assignment_scroll(s);
    if (assigned) save_assignments(s);   /* the file follows every change */
}

void delete_assignment(AppState *s, int idx) {
    if (idx < 0 || idx >= s->n_assignments) return;

    double pred_freq = s->assignments[idx].pred.freq_mhz;
    for (int i = idx; i < s->n_assignments - 1; i++) {
        s->assignments[i] = s->assignments[i + 1];
    }
    s->n_assignments--;

    if (s->n_assignments <= 0) {
        s->selected_assignment = -1;
    } else if (idx >= s->n_assignments) {
        s->selected_assignment = s->n_assignments - 1;
    } else {
        s->selected_assignment = idx;
    }

    clamp_assignment_scroll(s);
    printf("Deleted assignment for %.4f MHz\n", pred_freq);
    if (save_assignments(s)) predfit_save_exclusions(s);
}

static void clamp_assignment_scroll(AppState *s) {
    int max_scroll = s->n_assignments - 13;
    if (max_scroll < 0) max_scroll = 0;
    if (s->assignments_scroll < 0) s->assignments_scroll = 0;
    if (s->assignments_scroll > max_scroll) s->assignments_scroll = max_scroll;
}

static void commit_text_input(AppState *s) {
    if (s->input_state == INPUT_NONE) return;

    if (s->input_state == INPUT_GAMMA) s->lorentz_gamma = atof(s->text_input_buf);
    if (s->input_state == INPUT_GAUSS) s->gauss_gamma = atof(s->text_input_buf);
    if (s->input_state == INPUT_KBETA) s->kaiser_beta = atof(s->text_input_buf);
    if (s->input_state == INPUT_KCEROS) { int c = atoi(s->text_input_buf); s->kaiser_ceros = c > 0 ? c : 1; }
    if (s->input_state == INPUT_KINTR) { double v = atof(s->text_input_buf); s->kaiser_intrinsic = v >= 0 ? v : 0; }
    else if (s->input_state == INPUT_PF_SIG) s->pf_sig_pts = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_PF_NOISE) s->pf_noise_pts = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_PF_THRESH) s->pf_thresh = atof(s->text_input_buf);
    else if (s->input_state == INPUT_AVG_PTS) {
        s->rolling_avg_window = atoi(s->text_input_buf);
        if(s->rolling_avg_active) {
            apply_rolling_average(s->raw_pts, s->smooth_pts, s->n_pts, s->rolling_avg_window);
            s->current_pts = s->smooth_pts;
        }
    }
    else if (s->input_state == INPUT_PRED_MIN) s->pred_min_log_int = atof(s->text_input_buf);
    else if (s->input_state == INPUT_PRED_MAX) s->pred_max_log_int = atof(s->text_input_buf);
    else if (s->input_state == INPUT_JUMP_MIN) {
        s->vxmin = atof(s->text_input_buf);
        if(s->sync_active) s->pvxmin = s->vxmin;
    }
    else if (s->input_state == INPUT_JUMP_MAX) {
        s->vxmax = atof(s->text_input_buf);
        if(s->sync_active) s->pvxmax = s->vxmax;
    }
    else if (s->input_state == INPUT_OFFSET) {
        s->exp_offset = atof(s->text_input_buf);
    }
    else if (s->input_state == INPUT_CAT_TEMP) {
        double temp_k = atof(s->text_input_buf);
        if (isfinite(temp_k) && temp_k > 0.0) s->cat_temp_k = temp_k;
        if (s->pred_lines && s->n_pred > 0)
            rescale_predicted_intensities(s->pred_lines, s->n_pred,
                                          s->cat_temp_k, s->rot_temp_k,
                                          s->dipole_cat, s->dipole_red,
                                          &s->pred_global_max);
    }
    else if (s->input_state == INPUT_ROT_TEMP) {
        double temp_k = atof(s->text_input_buf);
        if (isfinite(temp_k) && temp_k > 0.0) {
            s->rot_temp_k = temp_k;
            predfit_adopt_shared_state(s);
            if (s->pred_lines && s->n_pred > 0)
                rescale_predicted_intensities(s->pred_lines, s->n_pred,
                                              s->cat_temp_k, s->rot_temp_k,
                                              s->dipole_cat, s->dipole_red,
                                              &s->pred_global_max);
        }
    }
    else if (s->input_state >= INPUT_MUCAT_A && s->input_state <= INPUT_MURED_C) {
        double mu = atof(s->text_input_buf);
        int which = s->input_state - INPUT_MUCAT_A;
        int component = which % 3;
        if (isfinite(mu)) {
            if (which < 3) s->dipole_cat[component] = mu;
            else { s->dipole_red[component] = mu; predfit_adopt_shared_state(s); }
        }
        if (s->pred_lines && s->n_pred > 0)
            rescale_predicted_intensities(s->pred_lines, s->n_pred,
                                          s->cat_temp_k, s->rot_temp_k,
                                          s->dipole_cat, s->dipole_red,
                                          &s->pred_global_max);
    }
    else if (s->input_state == INPUT_FIT_WINDOW) {
        double width = atof(s->text_input_buf);
        if (isfinite(width) && width > 0.0) s->intfit_half_window_mhz = width;
    }
    else if (s->input_state >= INPUT_PF_A && s->input_state <= INPUT_PF_FMAX) {
        double x=atof(s->text_input_buf); PredFitState *p=&s->predfit;
        double *v[]={&p->a,&p->b,&p->c,&p->mu[0],&p->mu[1],&p->mu[2],&p->temp_k,&p->fmin_ghz,&p->fmax_ghz};
        int i=s->input_state-INPUT_PF_A;
        if (isfinite(x) && ((i == 7 && x >= 0.0) || (i != 7 && x > 0.0))) {
            *v[i]=x;
            p->session_dirty = 1;
            if (i >= 3 && i <= 6) predfit_publish_shared_state(s);
        }
    }
    else if (s->input_state == INPUT_FILT_JMIN)  s->filt_j_min  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_JMAX)  s->filt_j_max  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KAMIN) s->filt_ka_min = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KAMAX) s->filt_ka_max = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KCMIN) s->filt_kc_min = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_KCMAX) s->filt_kc_max = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DJ)    s->filt_dj  = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DKA)   s->filt_dka = atoi(s->text_input_buf);
    else if (s->input_state == INPUT_FILT_DKC)   s->filt_dkc = atoi(s->text_input_buf);

    s->input_state = INPUT_NONE;
    SDL_StopTextInput();
}

// --- TEXT INPUT BUFFER ---
static void handle_text_input_event(AppState *s, char *text) {
    input_insert(s, text);
}

static int path_looks_like_cat(const char *path) {
    size_t len = strlen(path);
    return len >= 4 && strcmp(path + len - 4, ".cat") == 0;
}

// --- KEYBOARD ---
static void handle_keydown(AppState *s, Layout *l, SDL_KeyboardEvent *key) {
    if (s->input_state != INPUT_NONE) return; // Should be handled by main loop, but safety check

    int sym = key->keysym.sym;
    int mod = key->keysym.mod;
    int is_shift = (mod & KMOD_SHIFT);
    double speed_mult = (mod & KMOD_CAPS) ? s->settings.nav_fast_mult : 1.0;

    /* Command on a Mac, Control elsewhere.  The unmodified keys keep their
       meaning: F is the frequency-jump panel and B the transition filter. */
    if (mod & (KMOD_GUI | KMOD_CTRL)) {
        if (sym == SDLK_f) { predfit_fit(s); return; }            /* run SPFIT      */
        if (sym == SDLK_b) { predfit_undo_last_fit(s); return; }  /* undo that fit  */
    }
    if (sym == SDLK_COMMA) { settings_open(s); return; }
    if (sym == SDLK_h || sym == SDLK_SLASH) {
        s->show_help = !s->show_help;
        return;
    }
    if (sym == SDLK_x) {
        if (s->data_loaded) s->export_requested = 1;
        return;
    }
    if (!s->data_loaded) return;

    // Delete Peak
    if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
        if (is_shift) s->n_peaks = 0; 
        else if (s->n_peaks > 0) s->n_peaks--;
        return;
    }

    if (s->win_as.visible && sym == SDLK_PAGEUP) {
        s->assignments_scroll -= 13;
        clamp_assignment_scroll(s);
        return;
    }
    if (s->win_as.visible && sym == SDLK_PAGEDOWN) {
        s->assignments_scroll += 13;
        clamp_assignment_scroll(s);
        return;
    }

    // Toggle Windows
    if (sym == SDLK_n) s->win_as.visible = !s->win_as.visible;
    if (sym == SDLK_m) s->win_br.visible = !s->win_br.visible;
    if (sym == SDLK_d) s->win_dip.visible = !s->win_dip.visible;
    if (sym == SDLK_p) s->win_pf.visible = !s->win_pf.visible;
    if (sym == SDLK_t) s->win_avg.visible = !s->win_avg.visible;
    if (sym == SDLK_r) { // Reset View
        s->vxmin = s->xmin; s->vxmax = s->xmax;
        // Y range: span all visible spectra (overlay/shared common axis).
        double miny = s->ymin, maxy = s->ymax;
        int seen = 0;
        for (int k = 0; k < s->n_spectra; k++) {
            Spectrum *S = &s->spectra[k];
            if (!S->visible) continue;
            if (!seen) { miny = S->ymin; maxy = S->ymax; seen = 1; }
            else { if (S->ymin < miny) miny = S->ymin; if (S->ymax > maxy) maxy = S->ymax; }
        }
        s->vymin = miny; s->vymax = maxy;
        s->pvxmin = s->xmin; s->pvxmax = s->xmax;
        s->pred_scale = 1.0;
        return;
    }
    if (sym == SDLK_c) s->win_cut.visible = !s->win_cut.visible;
    if (sym == SDLK_f) s->win_jump.visible = !s->win_jump.visible;
    if (sym == SDLK_b) s->win_filt.visible = !s->win_filt.visible;
    
    // Navigation Calc
    double width_px = (double)l->exp_w;
    if(width_px < 1) width_px = 1000; // Safety

    double pan_px = s->settings.nav_pan_px * speed_mult;
    double pan_exp = pan_px * (s->vxmax - s->vxmin) / width_px;
    double pan_pred = pan_px * (s->pvxmax - s->pvxmin) / width_px;
    double pan_y = pan_px * (s->vymax - s->vymin) / (double)l->exp_h;

    double bar_px = s->settings.nav_bar_px * speed_mult;
    double bar_step_exp  = bar_px * (s->vxmax - s->vxmin) / width_px;
    double bar_step_pred = bar_px * (s->pvxmax - s->pvxmin) / width_px;

    switch(sym) {
        // Y-Axis Auto Scale
        case SDLK_TAB: {
            if (is_shift) {
                double max_int = prediction_visible_max(s, l->pred_w);
                if (max_int > 0.0 && s->pred_global_max > 0.0)
                    s->pred_scale = s->pred_global_max / max_int;
                break;
            }
            // Y auto-scale, mode-aware:
            //  - stack OR overlay-normalized: each subplot uses its own per-spectrum
            //    gain (vscale), so fit the GAIN of each visible spectrum to its
            //    windowed max (top of band = windowed max).
            //  - overlay-shared: fit the common vymin/vymax to all visible traces.
            if (s->multi_ynorm) {
                for (int k = 0; k < s->n_spectra; k++) {
                    Spectrum *S = &s->spectra[k];
                    if (!S->visible || S->n_pts < 1) continue;
                    int istart = binary_search_lower(S->current_pts, S->n_pts, s->vxmin - S->exp_offset);
                    int iend   = binary_search_upper(S->current_pts, S->n_pts, s->vxmax - S->exp_offset);
                    if(istart < 0) istart = 0; if(iend >= S->n_pts) iend = S->n_pts - 1;
                    double wmax = -1e99;
                    for(int i=istart; i<=iend; i++)
                        if(S->current_pts[i].y > wmax) wmax = S->current_pts[i].y;
                    double denom = wmax - S->ymin;
                    if (denom > 0) S->vscale = (S->ymax - S->ymin) / denom;
                }
            } else {
                double miny=1e99, maxy=-1e99;
                for (int k = 0; k < s->n_spectra; k++) {
                    Spectrum *S = &s->spectra[k];
                    if (!S->visible || S->n_pts < 1) continue;
                    int istart = binary_search_lower(S->current_pts, S->n_pts, s->vxmin - S->exp_offset);
                    int iend   = binary_search_upper(S->current_pts, S->n_pts, s->vxmax - S->exp_offset);
                    if(istart < 0) istart = 0; if(iend >= S->n_pts) iend = S->n_pts - 1;
                    for(int i=istart; i<=iend; i++) {
                        if(S->current_pts[i].y < miny) miny = S->current_pts[i].y;
                        if(S->current_pts[i].y > maxy) maxy = S->current_pts[i].y;
                    }
                }
                if(miny < maxy) { s->vymin = miny; s->vymax = maxy; }
            }
            break;
        }

        // Panning X
        case SDLK_a:
            if(s->sync_active) { s->vxmin -= pan_exp; s->vxmax -= pan_exp; }
            else { 
                if(is_shift) { s->pvxmin -= pan_pred; s->pvxmax -= pan_pred; } 
                else { s->vxmin -= pan_exp; s->vxmax -= pan_exp; } 
            }
            break;
        case SDLK_s:
            if(s->sync_active) { s->vxmin += pan_exp; s->vxmax += pan_exp; }
            else { 
                if(is_shift) { s->pvxmin += pan_pred; s->pvxmax += pan_pred; } 
                else { s->vxmin += pan_exp; s->vxmax += pan_exp; } 
            }
            break;

        // Zoom In (E) / Out (Q)
        case SDLK_e: { // In
            double f = 1.0 / s->settings.nav_zoom_factor;
            // Logic: shrink range around center (or bar)
            if(s->sync_active) {
                double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                double w = (s->vxmax - s->vxmin) * f;
                s->vxmin = c - w/2; s->vxmax = c + w/2;
            } else {
                if(is_shift) {
                    double c = s->bar_active ? s->pbar_x : (s->pvxmin + s->pvxmax)/2.0;
                    double w = (s->pvxmax - s->pvxmin) * f;
                    s->pvxmin = c - w/2; s->pvxmax = c + w/2;
                } else {
                    double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                    double w = (s->vxmax - s->vxmin) * f;
                    s->vxmin = c - w/2; s->vxmax = c + w/2;
                }
            }
            break;
        }
        case SDLK_q: { // Out
            double f = s->settings.nav_zoom_factor;
            if(s->sync_active) {
                double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                double w = (s->vxmax - s->vxmin) * f;
                s->vxmin = c - w/2; s->vxmax = c + w/2;
            } else {
                if(is_shift) {
                    double c = s->bar_active ? s->pbar_x : (s->pvxmin + s->pvxmax)/2.0;
                    double w = (s->pvxmax - s->pvxmin) * f;
                    s->pvxmin = c - w/2; s->pvxmax = c + w/2;
                } else {
                    double c = s->bar_active ? s->bar_x : (s->vxmin + s->vxmax)/2.0;
                    double w = (s->vxmax - s->vxmin) * f;
                    s->vxmin = c - w/2; s->vxmax = c + w/2;
                }
            }
            break;
        }

        // Vertical Control / Pred Scale.
        // Intensity depends only on shared/norm:
        //  - norm   : change the per-spectrum GAIN (all together, or only the
        //             active one when "individual intensity" is enabled).
        //  - shared : zoom the common vymax (works in overlay and stacked).
        case SDLK_w:
            if(is_shift) s->pred_scale *= s->settings.nav_intensity_factor;
            else if(s->multi_ynorm) scale_intensity(s, s->settings.nav_intensity_factor);
            else s->vymax -= pan_y;
            break;
        case SDLK_z:
            if(is_shift) s->pred_scale /= s->settings.nav_intensity_factor;
            else if(s->multi_ynorm) scale_intensity(s, 1.0 / s->settings.nav_intensity_factor);
            else s->vymax += pan_y;
            break;
        case SDLK_UP:
            if(is_shift) s->pred_scale *= s->settings.nav_intensity_factor; 
            else { s->vymin -= pan_y/10; s->vymax -= pan_y/10; } 
            break;
        case SDLK_DOWN:
            if(is_shift) s->pred_scale /= s->settings.nav_intensity_factor; 
            else { s->vymin += pan_y/10; s->vymax += pan_y/10; } 
            break;

        // Move Bar (K / L)
        case SDLK_k: 
            if(s->bar_active) {
                if(s->sync_active) { s->bar_x -= bar_step_exp; s->pbar_x -= bar_step_exp; }
                else { if(is_shift) s->pbar_x -= bar_step_pred; else s->bar_x -= bar_step_exp; }
            } break;
        case SDLK_l: 
            if(s->bar_active) {
                if(s->sync_active) { s->bar_x += bar_step_exp; s->pbar_x += bar_step_exp; }
                else { if(is_shift) s->pbar_x += bar_step_pred; else s->bar_x += bar_step_exp; }
            } break;
        
        // Future Features (Placeholders for features requested)
 //       case SDLK_c: 
 //           // s->input_state = INPUT_PRED_INT_MIN; ...
 //           break;
        // --- NEW: Toggle Measure Mode ---
        case SDLK_g:
            s->measure_active = !s->measure_active;
            s->measure_phase = 0; // Reset phase
            printf("Measure Mode: %s\n", s->measure_active ? "ON" : "OFF");
            break;
    }
}
