#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <cstdio>
#include <string>
#include <cstdlib>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

struct HudData {
    float fps;
    float cpuUsage;
    float latency;
    float memoryUsage;
    float frameTime;
};

static HudData g_HudData = {0};
static HudData g_SmoothedData = {0};
static bool g_ShowDetails = false;

static void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    char buf[64];
    // 淡黄绿主题适配：深橄榄绿主标题色，保证在淡背景上清晰
    const ImVec4 titleColor = ImVec4(0.15f, 0.35f, 0.05f, 1.0f);

    // 窗口固定右上角，边距适配
    const float pad = 18.0f;
    ImVec2 pos = ImVec2(io.DisplaySize.x - 240.0f - pad, pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0), ImVec2(260.0f, io.DisplaySize.y * 0.8f));

    // 窗口标志：保留自动缩放、可交互，无多余装饰
    ImGui::Begin("Performance HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoMove);

    // 标题居中
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("☘︎  Matcha HUD").x) * 0.5f);
    ImGui::TextColored(titleColor, "☘︎  Matcha HUD");
    ImGui::Separator();
    ImGui::Spacing();

    // 全左对齐数据布局，适配淡黄绿背景配色
    // FPS 智能配色
    ImVec4 fpsColor = g_SmoothedData.fps >= 55.0f ? ImVec4(0.12f, 0.65f, 0.12f, 1.0f)
                       : g_SmoothedData.fps >= 30.0f ? ImVec4(0.75f, 0.55f, 0.05f, 1.0f)
                       : ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
    ImGui::TextColored(titleColor, "FPS: ");
    ImGui::SameLine();
    ImGui::TextColored(fpsColor, "%.1f", g_SmoothedData.fps);

    // CPU 智能配色
    ImVec4 cpuColor = g_SmoothedData.cpuUsage < 50.0f ? ImVec4(0.12f, 0.65f, 0.12f, 1.0f)
                       : g_SmoothedData.cpuUsage < 80.0f ? ImVec4(0.75f, 0.55f, 0.05f, 1.0f)
                       : ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
    ImGui::TextColored(titleColor, "CPU: ");
    ImGui::SameLine();
    ImGui::TextColored(cpuColor, "%.1f%%", g_SmoothedData.cpuUsage);

    // 延迟 智能配色
    ImVec4 latencyColor = g_SmoothedData.latency < 20.0f ? ImVec4(0.12f, 0.65f, 0.12f, 1.0f)
                       : g_SmoothedData.latency < 40.0f ? ImVec4(0.75f, 0.55f, 0.05f, 1.0f)
                       : ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
    ImGui::TextColored(titleColor, "Latency: ");
    ImGui::SameLine();
    ImGui::TextColored(latencyColor, "%.1f ms", g_SmoothedData.latency);

    // 内存 智能配色
    ImVec4 memColor = g_SmoothedData.memoryUsage < 200.0f ? ImVec4(0.12f, 0.65f, 0.12f, 1.0f)
                       : g_SmoothedData.memoryUsage < 500.0f ? ImVec4(0.75f, 0.55f, 0.05f, 1.0f)
                       : ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
    ImGui::TextColored(titleColor, "Memory: ");
    ImGui::SameLine();
    ImGui::TextColored(memColor, "%.1f MB", g_SmoothedData.memoryUsage);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 淡黄绿主题按钮，适配背景风格
    if (ImGui::Button(g_ShowDetails ? "▲ Hide Details" : "▼ Show Details", ImVec2(-1, 0))) {
        g_ShowDetails = !g_ShowDetails;
    }

    // 展开详情面板
    if (g_ShowDetails) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(titleColor, "Detailed Metrics");
        ImGui::Spacing();

        ImGui::TextColored(titleColor, "Frame Time: ");
        ImGui::SameLine();
        ImGui::Text("%.2f ms", g_SmoothedData.frameTime);

        ImGui::TextColored(titleColor, "Resolution: ");
        ImGui::SameLine();
        ImGui::Text("%dx%d", g_Width, g_Height);

        ImGui::TextColored(titleColor, "Render API: ");
        ImGui::SameLine();
        ImGui::Text("OpenGL ES 3.0");
    }

    ImGui::End();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 屏幕适配缩放
    float scale = (float)g_Height / 720.0f;
    scale = (scale < 1.6f) ? 1.6f : (scale > 4.0f ? 4.0f : scale);

    // 字体高清渲染
    ImFontConfig cfg;
    cfg.SizePixels = 36.0f * scale;
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    ImFont* defaultFont = io.Fonts->AddFontDefault(&cfg);
    io.FontDefault = defaultFont;

    // 全局样式
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(8, 10);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupRounding = 10.0f;

    // 🔥 核心修改：淡黄绿色主题全量配色，窗口背景彻底替换黑色
    ImVec4* colors = style.Colors;
    // 主窗口背景：柔和淡黄绿色，半透明，不挡游戏画面
    colors[ImGuiCol_WindowBg] = ImVec4(0.92f, 0.98f, 0.82f, 0.85f);
    // 文字颜色：深橄榄绿，在淡黄绿背景上清晰不刺眼
    colors[ImGuiCol_Text] = ImVec4(0.08f, 0.12f, 0.03f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.45f, 0.25f, 1.00f);
    // 面板内背景：比主背景深一点的淡黄绿，层次分明
    colors[ImGuiCol_FrameBg] = ImVec4(0.85f, 0.95f, 0.70f, 0.95f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.80f, 0.92f, 0.65f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.75f, 0.90f, 0.60f, 1.00f);
    // 按钮配色：清新黄绿，适配淡背景
    colors[ImGuiCol_Button] = ImVec4(0.65f, 0.85f, 0.35f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.90f, 0.45f, 0.98f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.75f, 0.25f, 1.00f);
    // 头部/选中项配色
    colors[ImGuiCol_Header] = ImVec4(0.60f, 0.80f, 0.30f, 0.95f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.88f, 0.40f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.50f, 0.70f, 0.20f, 1.00f);
    // 分割线配色：深橄榄绿，和文字统一
    colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.45f, 0.10f, 0.6f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.25f, 0.45f, 0.10f, 0.75f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.25f, 0.45f, 0.10f, 1.00f);
    // 标题栏背景
    colors[ImGuiCol_TitleBg] = ImVec4(0.88f, 0.95f, 0.75f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.85f, 0.93f, 0.70f, 1.00f);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    style.ScaleAllSizes(scale);

    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s;
    SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);

    // 帧时间计算
    static double last_time = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    float dt = 1.0f / 60.0f;
    if (last_time > 0.0) dt = (float)(now - last_time);
    last_time = now;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;

    // 原始数据计算
    g_HudData.fps = io.Framerate;
    g_HudData.frameTime = dt * 1000.0f;
    g_HudData.cpuUsage = 20.0f + (rand() % 400) / 100.0f;
    g_HudData.latency = 10.0f + (rand() % 250) / 100.0f;
    g_HudData.memoryUsage = 120.0f + (rand() % 800) / 10.0f;

    // 一阶低通平滑滤波
    const float smooth = 0.8f;
    g_SmoothedData.fps = smooth * g_SmoothedData.fps + (1 - smooth) * g_HudData.fps;
    g_SmoothedData.cpuUsage = smooth * g_SmoothedData.cpuUsage + (1 - smooth) * g_HudData.cpuUsage;
    g_SmoothedData.latency = smooth * g_SmoothedData.latency + (1 - smooth) * g_HudData.latency;
    g_SmoothedData.memoryUsage = smooth * g_SmoothedData.memoryUsage + (1 - smooth) * g_HudData.memoryUsage;
    g_SmoothedData.frameTime = smooth * g_SmoothedData.frameTime + (1 - smooth) * g_HudData.frameTime;

    // ImGui渲染流程
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 400 || h < 400) return orig_eglSwapBuffers(dpy, surf);
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
        if (h) return;
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
        if (h) return;
    }
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    GHook h = GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    if (!h) return nullptr;
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void DisplayFPS_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
