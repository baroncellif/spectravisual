#ifndef PLOTGPU_H
#define PLOTGPU_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Line rendering for the spectrum panes, on Dear ImGui's draw list.
 *
 * Nothing of the interface goes through here: the chrome, the axes, the labels
 * and every control are still drawn by view.c exactly as before.  What moved is
 * only the part that was wrong - turning a polyline into anti-aliased triangles
 * with correct joins and a correct width - which is now done by a renderer that
 * has been in production for years instead of by geometry written here.
 *
 * Coordinates and widths are in logical pixels, as everywhere else in the UI;
 * the conversion to the device grid happens inside.
 */
int  plotgpu_init(SDL_Renderer *renderer);
void plotgpu_shutdown(void);

/* Once per frame, before anything is queued. */
void plotgpu_begin_frame(SDL_Renderer *renderer, float device_scale);

/* Queued geometry, drawn when the frame is flushed. */
void plotgpu_polyline(const SDL_FPoint *pts, int n, SDL_Color color, float width);
void plotgpu_segments(const SDL_FPoint *pts, int n, SDL_Color color, float width);

/* The dense envelope: pairs of (x, y_min) (x, y_max), one pair per device
 * column, drawn as pixel-aligned bars at least one pixel tall.  A trace that is
 * denser than the screen has columns thinner than a pixel, and an anti-aliased
 * line there spreads its coverage over two rows and fades out - which is what
 * made the spectrum look like it was losing pieces when zoomed out. */
void plotgpu_columns(const SDL_FPoint *pts, int n, SDL_Color color, float width);

/* Draws everything queued so far, at this point of the frame, so the panes stay
 * under the overlays and the panels. Must be called once per frame. */
void plotgpu_flush(SDL_Renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif
