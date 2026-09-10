#include "ui_chrome.h"
#include "layout.h"
#include <math.h>

/*
 * Line-art icons drawn as polylines on a 16x16 grid.
 *
 * They replace the Unicode glyphs the toolbar used to borrow from Arial
 * Unicode: those depended on a font that happens to sit in /System on this
 * machine, had inconsistent weights, and could not be aligned optically. These
 * are drawn by the renderer, so they stay crisp at any backing scale.
 */

#define END  (-100)   /* end of stroke  */
#define STOP (-101)   /* end of icon    */

/* Each icon is a flat list of x,y pairs; END breaks the polyline. */
static const signed char ICON_DATA[UI_ICON_COUNT][64] = {
    /* BAR: crosshair */
    { 8,1, 8,15, END, 1,8, 15,8, STOP },
    /* MEASURE: ruler */
    { 1,10, 10,1, 14,5, 5,14, 1,10, END, 4,7, 6,9, END, 7,4, 9,6, END, 10,2, 12,4, STOP },
    /* SYNC: two arcs with heads */
    { 2,9, 3,5, 6,3, 10,3, 13,5, END, 14,7, 13,11, 10,13, 6,13, 3,11, END,
      11,1, 13,5, 9,6, END, 5,15, 3,11, 7,10, STOP },
    /* TRASH */
    { 3,4, 13,4, END, 6,4, 6,2, 10,2, 10,4, END, 4,4, 5,14, 11,14, 12,4, STOP },
    /* LIST */
    { 2,4, 14,4, END, 2,8, 14,8, END, 2,12, 9,12, STOP },
    /* PEAK */
    { 1,13, 4,13, 6,3, 8,11, 10,6, 11,13, 15,13, STOP },
    /* WAVE */
    { 1,9, 3,4, 5,9, 7,14, 9,9, 11,4, 13,9, 15,9, STOP },
    /* BELL: broadened line profile */
    { 1,13, 4,12, 6,8, 8,3, 10,8, 12,12, 15,13, END, 8,3, 8,13, STOP },
    /* DIPOLE: positive and negative ends */
    { 8,2, 8,14, END, 5,4, 7,4, END, 6,3, 6,5, END, 10,12, 14,12, STOP },
    /* RANGE: bounded interval */
    { 3,3, 3,13, END, 13,3, 13,13, END, 5,8, 11,8, END, 7,6, 5,8, 7,10, END, 9,6, 11,8, 9,10, STOP },
    /* JUMP: go to frequency */
    { 2,8, 11,8, END, 8,5, 11,8, 8,11, END, 14,3, 14,13, STOP },
    /* FILTER: funnel */
    { 2,3, 14,3, 9,9, 9,13, 7,12, 7,9, 2,3, STOP },
    /* LAYERS */
    { 8,2, 14,5, 8,8, 2,5, 8,2, END, 3,8, 8,10, 13,8, END, 3,11, 8,13, 13,11, STOP },
    /* EXPORT: save to disk */
    { 8,2, 8,10, END, 5,7, 8,10, 11,7, END, 2,11, 2,14, 14,14, 14,11, STOP },
    /* HELP: filled by code (circle + hook) */
    { STOP },
    /* CLOSE */
    { 4,4, 12,12, END, 12,4, 4,12, STOP },
    /* EYE: filled by code */
    { STOP }
};

static void stroke_icon(SDL_Renderer *ren, const signed char *d, int ox, int oy) {
    SDL_Point pts[32];
    int n = 0;
    for (int i = 0; ; i += 2) {
        if (d[i] == STOP) { if (n > 1) SDL_RenderDrawLines(ren, pts, n); break; }
        if (d[i] == END)  { if (n > 1) SDL_RenderDrawLines(ren, pts, n); n = 0; i -= 1; continue; }
        if (n < 32) { pts[n].x = ox + ui_dev(d[i]); pts[n].y = oy + ui_dev(d[i + 1]); n++; }
    }
}

static void arc(SDL_Renderer *ren, double cx, double cy, double r,
                double a0, double a1, int steps) {
    SDL_Point p[40];
    if (steps > 39) steps = 39;
    for (int i = 0; i <= steps; i++) {
        double a = a0 + (a1 - a0) * i / (double)steps;
        p[i].x = (int)lround(cx + r * cos(a));
        p[i].y = (int)lround(cy + r * sin(a));
    }
    SDL_RenderDrawLines(ren, p, steps + 1);
}

void ui_draw_icon(SDL_Renderer *ren, int icon, SDL_Rect box, SDL_Color c) {
    if (icon < 0 || icon >= UI_ICON_COUNT) return;

    // Drawn on the device grid: at 2x the 16-unit grid becomes 32 real pixels
    // with one-pixel strokes, instead of a doubled 16-pixel drawing.
    ui_dev_begin(ren);
    int ox = ui_dev(box.x + (box.w - 16) / 2);
    int oy = ui_dev(box.y + (box.h - 16) / 2);

    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);

    double u = ui_dev(1000) / 1000.0;   /* device pixels per icon unit */
    if (icon == UI_ICON_HELP) {
        arc(ren, ox + 8 * u, oy + 8 * u, 6.3 * u, 0, 2 * M_PI, (int)(28 * u));
        arc(ren, ox + 8 * u, oy + 6.3 * u, 2.0 * u, M_PI, 2.15 * M_PI, (int)(12 * u));
        SDL_RenderDrawLine(ren, (int)(ox + 8 * u), (int)(oy + 8 * u),
                                (int)(ox + 8 * u), (int)(oy + 10 * u));
        for (int k = 0; k < (int)u + 1; k++)
            SDL_RenderDrawPoint(ren, (int)(ox + 8 * u), (int)(oy + 12 * u) + k);
        ui_dev_end(ren);
        return;
    }
    if (icon == UI_ICON_EYE) {
        arc(ren, ox + 8 * u, oy + 12 * u, 7.0 * u, 1.15 * M_PI, 1.85 * M_PI, (int)(16 * u));
        arc(ren, ox + 8 * u, oy + 4 * u,  7.0 * u, 0.15 * M_PI, 0.85 * M_PI, (int)(16 * u));
        arc(ren, ox + 8 * u, oy + 8 * u,  2.1 * u, 0, 2 * M_PI, (int)(16 * u));
        ui_dev_end(ren);
        return;
    }
    stroke_icon(ren, ICON_DATA[icon], ox, oy);
    ui_dev_end(ren);
}

const char *ui_tool_title(int tool) {
    switch (tool) {
        case UI_TOOL_ASSIGN:  return "Assignments";
        case UI_TOOL_PEAKS:   return "Peak finder";
        case UI_TOOL_AVG:     return "Rolling average";
        case UI_TOOL_BROAD:   return "Broadening";
        case UI_TOOL_DIP:     return "Intensity analysis";
        case UI_TOOL_CUT:     return "Intensity range";
        case UI_TOOL_FILTER:  return "Transition filter";
        case UI_TOOL_JUMP:    return "Frequency jump";
        case UI_TOOL_SPECTRA: return "Spectra";
        default:              return "";
    }
}

const char *ui_tool_key(int tool) {
    switch (tool) {
        case UI_TOOL_ASSIGN: return "N";
        case UI_TOOL_PEAKS:  return "P";
        case UI_TOOL_AVG:    return "T";
        case UI_TOOL_BROAD:  return "M";
        case UI_TOOL_DIP:    return "D";
        case UI_TOOL_CUT:    return "C";
        case UI_TOOL_FILTER: return "B";
        case UI_TOOL_JUMP:   return "F";
        default:             return "";     /* Spectra has no key binding */
    }
}

int ui_tool_icon(int tool) {
    switch (tool) {
        case UI_TOOL_ASSIGN:  return UI_ICON_LIST;
        case UI_TOOL_PEAKS:   return UI_ICON_PEAK;
        case UI_TOOL_AVG:     return UI_ICON_WAVE;
        case UI_TOOL_BROAD:   return UI_ICON_BELL;
        case UI_TOOL_DIP:     return UI_ICON_DIPOLE;
        case UI_TOOL_CUT:     return UI_ICON_RANGE;
        case UI_TOOL_FILTER:  return UI_ICON_FILTER;
        case UI_TOOL_JUMP:    return UI_ICON_JUMP;
        case UI_TOOL_SPECTRA: return UI_ICON_LAYERS;
        default:              return UI_ICON_LIST;
    }
}
