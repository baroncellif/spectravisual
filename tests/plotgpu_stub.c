/* Test-suite stub (copy of docs/audit/repro/plotgpu_stub.c): the tests never
   render, so the ImGui line renderer is replaced by no-ops.  Only the linker
   needs these symbols (view.c). */
#include <SDL.h>
int  plotgpu_init(SDL_Renderer *r) { (void)r; return 1; }
void plotgpu_shutdown(void) {}
void plotgpu_begin_frame(SDL_Renderer *r, float s) { (void)r; (void)s; }
void plotgpu_polyline(const SDL_FPoint *p, int n, SDL_Color c, float w) { (void)p; (void)n; (void)c; (void)w; }
void plotgpu_segments(const SDL_FPoint *p, int n, SDL_Color c, float w) { (void)p; (void)n; (void)c; (void)w; }
void plotgpu_columns(const SDL_FPoint *p, int n, SDL_Color c, float w) { (void)p; (void)n; (void)c; (void)w; }
void plotgpu_flush(SDL_Renderer *r) { (void)r; }
