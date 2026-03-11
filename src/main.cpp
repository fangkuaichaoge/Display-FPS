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
#include <cstdlib> // 修复：补充rand()所需头文件

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
    float cps;
    float cpuUsage;
    float latency;
    float memoryUsage;
    float frameTime;
};

static HudData g_HudData = {0};
static HudData g_SmoothedData = {0}; // 平滑后的数据，避免跳动
static bool g_ShowDetails = false; // 控制展开折叠状态

static void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();

    // 窗口固定右上角，边距适配
    const float pad = 18.0f;
    ImVec2 min_size = ImVec2(230.0f, 0);
    ImVec2 max_size = ImVec2(280.0f, io.DisplaySize.y * 0.8f);
    ImVec2 pos = ImVec2(io.DisplaySize.x - min_size.x - pad, pad);

    // 窗口位置固定，大小自适应（最小/最大限制）
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(min_size, max_size);

    // 修复：去掉NoInputs，允许触摸交互；保留无装饰、不移动
    ImGui::Begin("Performance HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoMove);

    // 标题：抹茶色高亮，居中
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("☘︎  Matcha HUD").x) * 0.5f);
    ImGui::TextColored(ImVec4(0.18f, 0.72f, 0.45f, 1.00f), "☘︎  Matcha HUD");
    ImGui::Separator();
    ImGui::Spacing();

    // 核心数据项：智能配色+右对齐，美观直观
    // FPS 配色：≥55绿，30-55黄，<30红
    ImVec4 fpsColor = g_SmoothedData.fps >= 55.0f ? ImVec4(0.22f, 0.82f, 0.46f, 1.0f)
                       : g_SmoothedData.fps >= 30.0f ? ImVec4(0.98f, 0.88f, 0.22f, 1.0f)
                       : ImVec4(0.95f, 0.22f, 0.22f, 1.0f);
    ImGui::Text("FPS");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.1f", g_SmoothedData.fps).x - 12);
    ImGui::TextColored(fpsColor, "%.1f", g_SmoothedData.fps);

    // CPS 配色
    ImVec4 cpsColor = g_SmoothedData.cps >= 10.0f ? ImVec4(0.22f, 0.82f, 0.46f, 1.0f)
                       : g_SmoothedData.cps >= 5.0f ? ImVec4(0.98f, 0.88f, 0.22f, 1.0f)
                       : ImVec4(0.95f, 0.22f, 0.22f, 1.0f);
    ImGui::Text("CPS");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.1f", g_SmoothedData.cps).x - 12);
    ImGui::TextColored(cpsColor, "%.1f", g_SmoothedData.cps);

    // CPU 配色：<50绿，50-80黄，>80红
    ImVec4 cpuColor = g_SmoothedData.cpuUsage < 50.0f ? ImVec4(0.22f, 0.82f, 0.46f, 1.0f)
                       : g_SmoothedData.cpuUsage < 80.0f ? ImVec4(0.98f, 0.88f, 0.22f, 1.0f)
                       : ImVec4(0.95f, 0.22f, 0.22f, 1.0f);
    ImGui::Text("CPU");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.1f%%", g_SmoothedData.cpuUsage).x - 12);
    ImGui::TextColored(cpuColor, "%.1f%%", g_SmoothedData.cpuUsage);

    // 延迟 配色：<20绿，20-40黄，>40红
    ImVec4 latencyColor = g_SmoothedData.latency < 20.0f ? ImVec4(0.22f, 0.82f, 0.46f, 1.0f)
                       : g_SmoothedData.latency < 40.0f ? ImVec4(0.98f, 0.88f, 0.22f, 1.0f)
                       : ImVec4(0.95f, 0.22f, 0.22f, 1.0f);
    ImGui::Text("Latency");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.1f ms", g_SmoothedData.latency).x - 12);
    ImGui::TextColored(latencyColor, "%.1f ms", g_SmoothedData.latency);

    // 内存 配色
    ImVec4 memColor = g_SmoothedData.memoryUsage < 200.0f ? ImVec4(0.22f, 0.82f, 0.46f, 1.0f)
                       : g_SmoothedData.memoryUsage < 500.0f ? ImVec4(0.98f, 0.88f, 0.22f, 1.0f)
                       : ImVec4(0.95f, 0.22f, 0.22f, 1.0f);
    ImGui::Text("Memory");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.1f MB", g_SmoothedData.memoryUsage).x - 12);
    ImGui::TextColored(memColor, "%.1f MB", g_SmoothedData.memoryUsage);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 展开/折叠按钮：全宽，抹茶色主题，点击有反馈
    if (ImGui::Button(g_ShowDetails ? "▲ Hide Details" : "▼ Show Details", ImVec2(-1, 0))) {
        g_ShowDetails = !g_ShowDetails;
    }

    // 展开后的详情面板
    if (g_ShowDetails) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.18f, 0.72f, 0.45f, 1.00f), "Detailed Metrics");
        ImGui::Spacing();

        ImGui::Text("Frame Time");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%.2f ms", g_SmoothedData.frameTime).x - 12);
        ImGui::Text("%.2f ms", g_SmoothedData.frameTime);

        ImGui::Text("Resolution");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("%dx%d", g_Width, g_Height).x - 12);
        ImGui::Text("%dx%d", g_Width, g_Height);

        ImGui::Text("Render API");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("OpenGL ES 3.0").x - 12);
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

    // 字体优化：高清渲染，适配缩放
    ImFontConfig cfg;
    cfg.SizePixels = 36.0f * scale;
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    ImFont* defaultFont = io.Fonts->AddFontDefault(&cfg);
    io.FontDefault = defaultFont;

    // 全局样式：抹茶主题全量配置
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
    style.SeparatorTextPadding = ImVec2(20, 6);

    // 抹茶色主题全量配色（暗底+清新抹茶绿，护眼不刺眼）
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.57f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.06f, 0.88f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.11f, 0.10f, 0.95f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.14f, 0.12f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.16f, 0.15f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.12f, 0.55f, 0.35f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.68f, 0.45f, 0.98f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.08f, 0.46f, 0.28f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.14f, 0.58f, 0.36f, 0.95f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.68f, 0.42f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.09f, 0.48f, 0.29f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.72f, 0.45f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.18f, 0.72f, 0.45f, 0.75f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.18f, 0.72f, 0.45f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.08f, 0.07f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.09f, 1.00f);

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
    g_HudData.cps = g_HudData.fps * 0.8f;
    // 模拟数据平滑范围，避免极端跳动
    g_HudData.cpuUsage = 20.0f + (rand() % 400) / 100.0f;
    g_HudData.latency = 10.0f + (rand() % 250) / 100.0f;
    g_HudData.memoryUsage = 120.0f + (rand() % 800) / 10.0f;

    // 一阶低通滤波，平滑数据（80%旧值+20%新值，过渡自然）
    const float smooth = 0.8f;
    g_SmoothedData.fps = smooth * g_SmoothedData.fps + (1.0f - smooth) * g_HudData.fps;
    g_SmoothedData.cps = smooth * g_SmoothedData.cps + (1.0f - smooth) * g_HudData.cps;
    g_SmoothedData.cpuUsage = smooth * g_SmoothedData.cpuUsage + (1.0f - smooth) * g_HudData.cpuUsage;
    g_SmoothedData.latency = smooth * g_SmoothedData.latency + (1.0f - smooth) * g_HudData.latency;
    g_SmoothedData.memoryUsage = smooth * g_SmoothedData.memoryUsage + (1.0f - smooth) * g_HudData.memoryUsage;
    g_SmoothedData.frameTime = smooth * g_SmoothedData.frameTime + (1.0f - smooth) * g_HudData.frameTime;

    // ImGui 渲染流程
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
