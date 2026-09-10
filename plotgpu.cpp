#include "plotgpu.h"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_sdlrenderer2.h"
#include "third_party/implot/implot.h"

#include <vector>
#include <cmath>

/*
 * Only the renderer backend is used, not the SDL2 platform backend: the
 * application keeps its own event loop and its own input handling, and ImGui
 * must not see the mouse or the keyboard.  It is here as a drawing engine, not
 * as a user interface.
 */
static bool  g_ready = false;
static bool  g_in_frame = false;
static float g_scale = 1.0f;
static SDL_Renderer *g_ren = nullptr;
static std::vector<ImVec2> g_pts;

int plotgpu_init(SDL_Renderer *renderer) {
    if (g_ready) return 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;      /* no imgui.ini next to the working directory */
    io.LogFilename = nullptr;
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;

    /* The draw list is built in device pixels, so the anti-aliased edge is one
       real pixel wide rather than one logical pixel stretched over two. */
    ImGui::GetStyle().AntiAliasedLines = true;
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;   /* geometry, not a texture ramp */
    ImGui::GetStyle().AntiAliasedFill = true;

    if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        return 0;
    }
    g_ready = true;
    return 1;
}

void plotgpu_shutdown(void) {
    if (!g_ready) return;
    if (g_in_frame) { ImGui::EndFrame(); g_in_frame = false; }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    g_ready = false;
}

void plotgpu_begin_frame(SDL_Renderer *renderer, float device_scale) {
    if (!g_ready) return;
    if (g_in_frame) { ImGui::EndFrame(); g_in_frame = false; }

    g_scale = device_scale > 0.1f ? device_scale : 1.0f;
    g_ren = renderer;

    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize(renderer, &dw, &dh);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)dw, (float)dh);      /* device pixels */
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui::NewFrame();
    g_in_frame = true;
}

/* The pane clip set on the SDL renderer has to be repeated for the draw list:
   the two do not share clipping state, and in stack mode a trace must stay
   inside its own band. */
static void push_current_clip(ImDrawList *dl) {
    ImGuiIO &io = ImGui::GetIO();
    if (!g_ren || !SDL_RenderIsClipEnabled(g_ren)) {
        dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(io.DisplaySize.x, io.DisplaySize.y), false);
        return;
    }
    SDL_Rect r;
    SDL_RenderGetClipRect(g_ren, &r);
    dl->PushClipRect(ImVec2(r.x * g_scale, r.y * g_scale),
                     ImVec2((r.x + r.w) * g_scale, (r.y + r.h) * g_scale), true);
}

static ImU32 to_u32(SDL_Color c) { return IM_COL32(c.r, c.g, c.b, c.a); }

static void fill_points(const SDL_FPoint *pts, int n) {
    g_pts.clear();
    g_pts.reserve((size_t)n);
    for (int i = 0; i < n; i++) g_pts.push_back(ImVec2(pts[i].x * g_scale, pts[i].y * g_scale));
}

void plotgpu_polyline(const SDL_FPoint *pts, int n, SDL_Color color, float width) {
    if (!g_ready || !g_in_frame || n < 2) return;
    fill_points(pts, n);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    push_current_clip(dl);
    dl->AddPolyline(g_pts.data(), n, to_u32(color), ImDrawFlags_None, width * g_scale);
    dl->PopClipRect();
}

void plotgpu_segments(const SDL_FPoint *pts, int n, SDL_Color color, float width) {
    if (!g_ready || !g_in_frame || n < 2) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    push_current_clip(dl);
    ImU32 col = to_u32(color);
    float w = width * g_scale;
    for (int i = 0; i + 1 < n; i += 2)
        dl->AddLine(ImVec2(pts[i].x * g_scale, pts[i].y * g_scale),
                    ImVec2(pts[i+1].x * g_scale, pts[i+1].y * g_scale), col, w);
    dl->PopClipRect();
}

void plotgpu_columns(const SDL_FPoint *pts, int n, SDL_Color color, float width) {
    if (!g_ready || !g_in_frame || n < 2) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    push_current_clip(dl);
    ImU32 col = to_u32(color);
    float w = width * g_scale;
    if (w < 1.0f) w = 1.0f;
    for (int i = 0; i + 1 < n; i += 2) {
        float x  = pts[i].x * g_scale;
        float y0 = pts[i].y * g_scale;
        float y1 = pts[i+1].y * g_scale;
        if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
        /* snapped to the pixel grid, and never thinner than one pixel */
        float x0 = std::floor(x - w * 0.5f);
        float x1 = x0 + std::floor(w + 0.5f);
        if (x1 <= x0) x1 = x0 + 1.0f;
        float top = std::floor(y0);
        float bot = std::floor(y1) + 1.0f;
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), col);
    }
    dl->PopClipRect();
}

void plotgpu_flush(SDL_Renderer *renderer) {
    if (!g_ready) return;
    if (!g_in_frame) return;
    ImGui::Render();
    g_in_frame = false;

    /* Draw at device resolution: the geometry is already in device pixels. */
    float sx = 1.0f, sy = 1.0f;
    SDL_RenderGetScale(renderer, &sx, &sy);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderSetScale(renderer, sx, sy);
}
