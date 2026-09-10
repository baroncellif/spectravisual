#ifndef UI_PANELS_H
#define UI_PANELS_H

#include <SDL.h>
#include "ui_theme.h"
#include "ui_chrome.h"
#include "types.h"

/*
 * Geometry of the inspector panels.
 *
 * Every control inside a panel is defined here once, as a function of the
 * panel's rectangle, and both the renderer and the input handler call these.
 * Panels are laid out on a single rhythm: a 24 px control on a 32 px step,
 * 16 px side padding, labels on the left, values right-aligned on the panel
 * edge, so rows line up across every tool.
 */

#define UI_P_PAD    16   /* side padding inside a panel            */
#define UI_P_TOP    10   /* space between the header and the first row */
#define UI_P_ROW    24   /* height of one control                  */
#define UI_P_STEP   32   /* baseline-to-baseline of two rows       */
#define UI_P_BOT    12   /* padding under the last row             */
#define UI_P_FIELD  96   /* default numeric field width            */
#define UI_P_FIELD_S 56  /* min/max pair                           */
#define UI_P_FIELD_XS 44 /* delta fields                           */
#define UI_P_TABLE_ROW 20

static inline int ui_p_body(SDL_Rect w) { return w.y + UI_SECTION_HEAD_H + UI_P_TOP; }

/* Full-width row i of the panel. */
static inline SDL_Rect ui_p_row(SDL_Rect w, int i) {
    return (SDL_Rect){w.x + UI_P_PAD, ui_p_body(w) + i * UI_P_STEP, w.w - 2 * UI_P_PAD, UI_P_ROW};
}
/* Right-aligned control of width fw on row i. */
static inline SDL_Rect ui_p_right(SDL_Rect w, int i, int fw) {
    SDL_Rect r = ui_p_row(w, i);
    return (SDL_Rect){r.x + r.w - fw, r.y, fw, UI_P_ROW};
}
/* Height of a panel whose content ends after `rows` rows. */
static inline int ui_p_height(int rows) {
    return UI_SECTION_HEAD_H + UI_P_TOP + rows * UI_P_STEP + UI_P_BOT - (UI_P_STEP - UI_P_ROW);
}

/* --- Broadening ------------------------------------------------------- */
/* rows: 0 mode, 1..3 widths, then the derived readout, then the switch.   */
static inline SDL_Rect ui_br_mode(SDL_Rect w)  { return ui_p_row(w, 0); }
static inline SDL_Rect ui_br_f1(SDL_Rect w)    { return ui_p_right(w, 1, UI_P_FIELD); }
static inline SDL_Rect ui_br_f2(SDL_Rect w)    { return ui_p_right(w, 2, UI_P_FIELD); }
static inline SDL_Rect ui_br_f3(SDL_Rect w)    { return ui_p_right(w, 3, UI_P_FIELD); }
static inline SDL_Rect ui_br_toggle(SDL_Rect w, int kaiser) { return ui_p_row(w, kaiser ? 6 : 4); }
static inline int      ui_br_rows(int kaiser)  { return kaiser ? 7 : 5; }

/* --- Intensity analysis ----------------------------------------------- */
static inline SDL_Rect ui_int_cat_temp(SDL_Rect w) { return ui_p_right(w, 1, UI_P_FIELD + 24); }
static inline SDL_Rect ui_int_rot_temp(SDL_Rect w) { return ui_p_right(w, 2, UI_P_FIELD + 24); }
static inline SDL_Rect ui_dip_field(SDL_Rect w, int component, int reduced) {
    SDL_Rect r = ui_p_row(w, 5 + component);
    int red_x = r.x + r.w - UI_P_FIELD;
    int cat_x = red_x - 8 - UI_P_FIELD;
    return (SDL_Rect){reduced ? red_x : cat_x, r.y, UI_P_FIELD, UI_P_ROW};
}
static inline SDL_Rect ui_fit_window(SDL_Rect w) { return ui_p_right(w, 10, UI_P_FIELD); }
static inline SDL_Rect ui_fit_temperature(SDL_Rect w) { return ui_p_row(w, 11); }
static inline SDL_Rect ui_fit_dipole(SDL_Rect w, int component) {
    SDL_Rect r = ui_p_row(w, 12);
    int pill_w = 44, gap = 6;
    int group = 3 * pill_w + 2 * gap;
    return (SDL_Rect){r.x + r.w - group + component * (pill_w + gap), r.y, pill_w, UI_P_ROW};
}
static inline SDL_Rect ui_fit_run(SDL_Rect w) {
    SDL_Rect r = ui_p_row(w, 13); return (SDL_Rect){r.x, r.y, 102, UI_P_ROW};
}
static inline SDL_Rect ui_fit_export(SDL_Rect w) {
    SDL_Rect r = ui_fit_run(w); r.x += r.w + 8; r.w = 112; return r;
}
#define UI_DIP_ROWS 20

/* --- Rolling average -------------------------------------------------- */
static inline SDL_Rect ui_avg_field(SDL_Rect w)  { return ui_p_right(w, 0, UI_P_FIELD); }
static inline SDL_Rect ui_avg_toggle(SDL_Rect w) { return ui_p_row(w, 1); }
#define UI_AVG_ROWS 3

/* --- Peak finder ------------------------------------------------------ */
static inline SDL_Rect ui_pf_sig(SDL_Rect w)    { return ui_p_right(w, 0, UI_P_FIELD); }
static inline SDL_Rect ui_pf_noise(SDL_Rect w)  { return ui_p_right(w, 1, UI_P_FIELD); }
static inline SDL_Rect ui_pf_thresh(SDL_Rect w) { return ui_p_right(w, 2, UI_P_FIELD); }
static inline SDL_Rect ui_pf_find(SDL_Rect w)   { SDL_Rect r = ui_p_row(w, 3); r.w = 104; r.h = 26; return r; }
static inline SDL_Rect ui_pf_export(SDL_Rect w) { SDL_Rect r = ui_p_row(w, 3); r.x += 112; r.w = 96; r.h = 26; return r; }
#define UI_PF_ROWS 5

/* --- Intensity range -------------------------------------------------- */
static inline SDL_Rect ui_cut_min(SDL_Rect w) { return ui_p_right(w, 0, UI_P_FIELD); }
static inline SDL_Rect ui_cut_max(SDL_Rect w) { return ui_p_right(w, 1, UI_P_FIELD); }
#define UI_CUT_ROWS 3

/* --- Frequency jump --------------------------------------------------- */
static inline SDL_Rect ui_jump_start(SDL_Rect w) { return ui_p_right(w, 0, UI_P_FIELD + 24); }
static inline SDL_Rect ui_jump_end(SDL_Rect w)   { return ui_p_right(w, 1, UI_P_FIELD + 24); }
#define UI_JUMP_ROWS 2

/* --- Transition filter ------------------------------------------------ */
#define UI_FILT_PILL_W 44
#define UI_FILT_PILL_GAP 6
static inline SDL_Rect ui_filt_master(SDL_Rect w) { return ui_p_row(w, 0); }
static inline SDL_Rect ui_filt_pill(SDL_Rect w, int row, int i) {
    int group = 3 * UI_FILT_PILL_W + 2 * UI_FILT_PILL_GAP;
    SDL_Rect r = ui_p_row(w, row);
    return (SDL_Rect){r.x + r.w - group + i * (UI_FILT_PILL_W + UI_FILT_PILL_GAP), r.y, UI_FILT_PILL_W, UI_P_ROW};
}
static inline SDL_Rect ui_filt_mu(SDL_Rect w, int i)     { return ui_filt_pill(w, 1, i); }
static inline SDL_Rect ui_filt_br(SDL_Rect w, int i)     { return ui_filt_pill(w, 2, i); }
static inline SDL_Rect ui_filt_range(SDL_Rect w)         { return ui_p_row(w, 3); }
/* rows 5,6,7 carry J / Ka / Kc; column 0 = min, 1 = max */
static inline SDL_Rect ui_filt_qn(SDL_Rect w, int i, int col) {
    SDL_Rect r = ui_p_row(w, 5 + i);
    int x_hi = r.x + r.w - UI_P_FIELD_S;
    int x_lo = x_hi - 8 - UI_P_FIELD_S;
    return (SDL_Rect){col ? x_hi : x_lo, r.y, UI_P_FIELD_S, UI_P_ROW};
}
static inline SDL_Rect ui_filt_delta_hdr(SDL_Rect w)     { return ui_p_row(w, 4); }
static inline SDL_Rect ui_filt_jump(SDL_Rect w)          { return ui_p_row(w, 8); }
static inline SDL_Rect ui_filt_delta(SDL_Rect w, int i) {
    SDL_Rect r = ui_p_row(w, 9);
    int step = r.w / 3;
    return (SDL_Rect){r.x + i * step + step - UI_P_FIELD_XS, r.y, UI_P_FIELD_XS, UI_P_ROW};
}
#define UI_FILT_ROWS 10

/* --- Spectra ---------------------------------------------------------- */
#define UI_SPEC_ROW_H 26
#define UI_SPEC_ROW_STEP 28
static inline SDL_Rect ui_spec_layout(SDL_Rect w) { return ui_p_row(w, 0); }
static inline SDL_Rect ui_spec_ynorm(SDL_Rect w)  { return ui_p_row(w, 1); }
static inline SDL_Rect ui_spec_indiv(SDL_Rect w)  { return ui_p_row(w, 2); }
static inline SDL_Rect ui_spec_row(SDL_Rect w, int i) {
    return (SDL_Rect){w.x + UI_P_PAD - 6, ui_p_body(w) + 3 * UI_P_STEP + 6 + i * UI_SPEC_ROW_STEP,
                      w.w - 2 * (UI_P_PAD - 6), UI_SPEC_ROW_H};
}
/* controls inside a trace row, right to left: remove, visibility, +, - */
static inline SDL_Rect ui_spec_del(SDL_Rect w, int i) {
    SDL_Rect r = ui_spec_row(w, i);
    return (SDL_Rect){r.x + r.w - 26, r.y + 2, 22, 22};
}
static inline SDL_Rect ui_spec_vis(SDL_Rect w, int i) {
    SDL_Rect r = ui_spec_del(w, i);
    return (SDL_Rect){r.x - 26, r.y, 22, 22};
}
static inline SDL_Rect ui_spec_plus(SDL_Rect w, int i) {
    SDL_Rect r = ui_spec_vis(w, i);
    return (SDL_Rect){r.x - 28, r.y, 24, 22};
}
static inline SDL_Rect ui_spec_minus(SDL_Rect w, int i) {
    SDL_Rect r = ui_spec_plus(w, i);
    return (SDL_Rect){r.x - 24, r.y, 24, 22};
}
static inline SDL_Rect ui_spec_name(SDL_Rect w, int i) {
    SDL_Rect r = ui_spec_row(w, i), m = ui_spec_minus(w, i);
    return (SDL_Rect){r.x, r.y, m.x - r.x - 6, r.h};
}

/* --- Assignments ------------------------------------------------------ */
#define UI_AS_ROWS_MAX 12
static inline int ui_as_rows(const AppState *s) {
    int n = s->n_assignments;
    if (n < 4) n = 4;
    if (n > UI_AS_ROWS_MAX) n = UI_AS_ROWS_MAX;
    return n;
}
static inline SDL_Rect ui_as_head(SDL_Rect w) {
    return (SDL_Rect){w.x + UI_P_PAD, ui_p_body(w), w.w - 2 * UI_P_PAD, UI_P_TABLE_ROW};
}
static inline SDL_Rect ui_as_row(SDL_Rect w, int i) {
    SDL_Rect h = ui_as_head(w);
    return (SDL_Rect){h.x, h.y + UI_P_TABLE_ROW + i * UI_P_TABLE_ROW, h.w, UI_P_TABLE_ROW};
}
static inline SDL_Rect ui_as_save(const AppState *s, SDL_Rect w) {
    SDL_Rect r = ui_as_row(w, ui_as_rows(s));
    return (SDL_Rect){r.x, r.y + 12, 96, 26};
}
static inline SDL_Rect ui_as_delete(const AppState *s, SDL_Rect w) {
    SDL_Rect r = ui_as_save(s, w);
    return (SDL_Rect){r.x + r.w + 8, r.y, 120, 26};
}

/* Natural height of each panel, so the column packs without dead space. */
static inline int ui_panel_height(int tool, const AppState *s) {
    switch (tool) {
        case UI_TOOL_ASSIGN:
            return UI_SECTION_HEAD_H + UI_P_TOP + (ui_as_rows(s) + 1) * UI_P_TABLE_ROW + 12 + 26 + 10 + 16 + UI_P_BOT;
        case UI_TOOL_PEAKS:   return ui_p_height(UI_PF_ROWS);
        case UI_TOOL_AVG:     return ui_p_height(UI_AVG_ROWS);
        case UI_TOOL_BROAD:   return ui_p_height(ui_br_rows(s->broaden_mode == 1));
        case UI_TOOL_DIP:     return ui_p_height(UI_DIP_ROWS);
        case UI_TOOL_CUT:     return ui_p_height(UI_CUT_ROWS);
        case UI_TOOL_FILTER:  return ui_p_height(UI_FILT_ROWS);
        case UI_TOOL_JUMP:    return ui_p_height(UI_JUMP_ROWS);
        case UI_TOOL_SPECTRA: {
            int n = s->n_spectra > 0 ? s->n_spectra : 1;
            return UI_SECTION_HEAD_H + UI_P_TOP + 3 * UI_P_STEP + 6 + n * UI_SPEC_ROW_STEP + 10 + 32 + UI_P_BOT;
        }
        default: return 160;
    }
}

#endif
