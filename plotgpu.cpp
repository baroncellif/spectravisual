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
static float g_scale = 1.0f;
static SDL_Renderer *g_ren = nullptr;
static std::vector<ImVec2> g_pts;

/* One ImGui context per renderer.  The renderer backend keeps its textures in
   the context, so a second window (the intensity-fit preview draws the very
   same plot through view.c) needs a context of its own rather than borrowing
   the main window's font atlas and draw lists. */
struct Target {
    SDL_Renderer *renderer;
    ImGuiContext *context;
    bool in_frame;
};
static const int MAX_TARGETS = 8;
static Target g_targets[MAX_TARGETS];
static int g_n_targets = 0;

static void configure_context(void) {
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
}

static Target *find_target(SDL_Renderer *renderer) {
    for (int i = 0; i < g_n_targets; i++)
        if (g_targets[i].renderer == renderer) return &g_targets[i];
    return nullptr;
}

static Target *add_target(SDL_Renderer *renderer) {
    if (!renderer || g_n_targets >= MAX_TARGETS) return nullptr;
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    configure_context();
    if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
        return nullptr;
    }
    Target *t = &g_targets[g_n_targets++];
    t->renderer = renderer;
    t->context = context;
    t->in_frame = false;
    return t;
}

/* The target of `renderer`, made current; created on first use once the main
   renderer exists. */
static Target *use_target(SDL_Renderer *renderer) {
    if (!g_ready) return nullptr;
    Target *t = find_target(renderer);
    if (!t) t = add_target(renderer);
    if (t) ImGui::SetCurrentContext(t->context);
    return t;
}

static void destroy_target(int index) {
    Target *t = &g_targets[index];
    ImGui::SetCurrentContext(t->context);
    if (t->in_frame) ImGui::EndFrame();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui::DestroyContext(t->context);
    g_targets[index] = g_targets[--g_n_targets];
    ImGui::SetCurrentContext(g_n_targets > 0 ? g_targets[0].context : nullptr);
}

int plotgpu_init(SDL_Renderer *renderer) {
    if (g_ready) return 1;
    IMGUI_CHECKVERSION();
    g_ready = true;
    if (!add_target(renderer)) { g_ready = false; return 0; }
    ImGui::SetCurrentContext(g_targets[0].context);
    ImPlot::CreateContext();       /* after ImGui: it reads the ImGui style */
    return 1;
}

void plotgpu_release(SDL_Renderer *renderer) {
    if (!g_ready) return;
    for (int i = 1; i < g_n_targets; i++)   /* the main renderer lives until shutdown */
        if (g_targets[i].renderer == renderer) { destroy_target(i); return; }
}

void plotgpu_shutdown(void) {
    if (!g_ready) return;
    if (g_n_targets > 0) ImGui::SetCurrentContext(g_targets[0].context);
    ImPlot::DestroyContext();
    while (g_n_targets > 0) destroy_target(g_n_targets - 1);
    g_ready = false;
}

void plotgpu_begin_frame(SDL_Renderer *renderer, float device_scale) {
    Target *t = use_target(renderer);
    if (!t) return;
    if (t->in_frame) { ImGui::EndFrame(); t->in_frame = false; }

    /* The renderer's own scale is the logical-to-device factor of the window
       it draws, which is right for a second window on another display too. */
    float sx = 0.0f, sy = 0.0f;
    SDL_RenderGetScale(renderer, &sx, &sy);
    g_scale = sx > 0.1f ? sx : (device_scale > 0.1f ? device_scale : 1.0f);
    g_ren = renderer;

    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize(renderer, &dw, &dh);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)dw, (float)dh);      /* device pixels */
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui::NewFrame();
    t->in_frame = true;
}

/* Geometry is queued into the context of the frame currently being built. */
static bool in_frame(void) {
    if (!g_ready || !g_ren) return false;
    Target *t = find_target(g_ren);
    return t && t->in_frame && ImGui::GetCurrentContext() == t->context;
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
    if (!in_frame() || n < 2) return;
    fill_points(pts, n);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    push_current_clip(dl);
    dl->AddPolyline(g_pts.data(), n, to_u32(color), ImDrawFlags_None, width * g_scale);
    dl->PopClipRect();
}

void plotgpu_segments(const SDL_FPoint *pts, int n, SDL_Color color, float width) {
    if (!in_frame() || n < 2) return;
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
    if (!in_frame() || n < 2) return;
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
    Target *t = g_ready ? find_target(renderer) : nullptr;
    if (!t || !t->in_frame) return;
    ImGui::SetCurrentContext(t->context);
    ImGui::Render();
    t->in_frame = false;

    /* Draw at device resolution: the geometry is already in device pixels. */
    float sx = 1.0f, sy = 1.0f;
    SDL_RenderGetScale(renderer, &sx, &sy);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderSetScale(renderer, sx, sy);
}
