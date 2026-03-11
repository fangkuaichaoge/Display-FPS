// ===================== System Header Files =====================
#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <fstream>
#include <cstring>
#include <sys/mman.h>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"
// ===================== Zoom Camera =====================
static bool g_ZoomInitialized = false;
static bool g_ZoomEnable = false;
static bool g_ZoomAnimated = true;
static uint64_t g_ZoomLevel = 5345000000ULL;
static uint64_t g_LastClientFov = 5360000000ULL;
static Transition g_ZoomTransition;

static constexpr uint64_t NORMAL_FOV = 5360000000ULL;
static constexpr uint64_t MIN_FOV    = 5310000000ULL;

static uint64_t (*orig_CameraAPI_tryGetFOV)(void*) = nullptr;

static uint64_t unsignedDiff(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static int clamp(int minVal, int v, int maxVal) {
    if (v < minVal) return minVal;
    if (v > maxVal) return maxVal;
    return v;
}

static uint64_t CameraAPI_tryGetFOV_hook(void* thisPtr) {
    if (!orig_CameraAPI_tryGetFOV)
        return NORMAL_FOV;

    uint64_t original = orig_CameraAPI_tryGetFOV(thisPtr);
    g_LastClientFov = original;

    if (!g_ZoomAnimated) {
        return g_ZoomEnable ? g_ZoomLevel : original;
    }

    if (g_ZoomTransition.inProgress() || g_ZoomEnable) {
        g_ZoomTransition.tick();
        uint64_t curr = g_ZoomTransition.getCurrent();
        return (curr == 0) ? original : curr;
    }

    return original;
}

static bool findAndHookCameraAPI() {
    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib)
        mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    if (!mcLib) return false;

    uintptr_t libBase = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (libBase == 0)
                    libBase = start;
            }
        }
    }
    if (!libBase) return false;

    const char* typeinfoName = "9CameraAPI";
    size_t nameLen = strlen(typeinfoName);
    uintptr_t typeinfoNameAddr = 0;

    std::ifstream maps2("/proc/self/maps");
    while (std::getline(maps2, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos && line.find("r-xp") == std::string::npos) continue;

        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        for (uintptr_t addr = start; addr < end - nameLen; addr++) {
            if (memcmp((void*)addr, typeinfoName, nameLen) == 0) {
                typeinfoNameAddr = addr;
                break;
            }
        }
        if (typeinfoNameAddr) break;
    }
    if (!typeinfoNameAddr) return false;

    uintptr_t typeinfoAddr = 0;
    std::ifstream maps3("/proc/self/maps");
    while (std::getline(maps3, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos || line.find("r--p") == std::string::npos) continue;
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoNameAddr) {
                typeinfoAddr = addr - sizeof(void*);
                break;
            }
        }
        if (typeinfoAddr) break;
    }
    if (!typeinfoAddr) return false;

    uintptr_t vtableAddr = 0;
    std::ifstream maps4("/proc/self/maps");
    while (std::getline(maps4, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos || line.find("r--p") == std::string::npos) continue;
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoAddr) {
                vtableAddr = addr + sizeof(void*);
                break;
            }
        }
        if (vtableAddr) break;
    }
    if (!vtableAddr) return false;

    uint64_t* tryGetFOVSlot = (uint64_t*)(vtableAddr + 7 * sizeof(void*));
    orig_CameraAPI_tryGetFOV = (uint64_t(*)(void*))(*tryGetFOVSlot);

    uintptr_t pageStart = (uintptr_t)tryGetFOVSlot & ~(4095UL);
    mprotect((void*)pageStart, 4096, PROT_READ | PROT_WRITE);
    *tryGetFOVSlot = (uint64_t)CameraAPI_tryGetFOV_hook;
    mprotect((void*)pageStart, 4096, PROT_READ);

    return true;
}

static void InitZoom() {
    if (g_ZoomInitialized) return;
    findAndHookCameraAPI();
    g_ZoomInitialized = true;
}

static void ToggleZoom() {
    if (!g_ZoomInitialized)
        InitZoom();

    g_ZoomEnable = !g_ZoomEnable;

    if (g_ZoomAnimated) {
        if (g_ZoomEnable) {
            uint64_t diff = unsignedDiff(g_LastClientFov, g_ZoomLevel);
            g_ZoomTransition.startTransition(g_LastClientFov, g_ZoomLevel,
                clamp(100, (int)(diff / 150000), 250));
        } else {
            uint64_t diff = unsignedDiff(g_ZoomLevel, g_LastClientFov);
            g_ZoomTransition.startTransition(g_ZoomLevel, g_LastClientFov,
                clamp(100, (int)(diff / 150000), 250));
        }
    }
}

// ===================== ImGui Globals =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;
static ImFont* g_UIFont = nullptr;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== Style =====================
static void ForceStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    c[ImGuiCol_WindowBg] = ImVec4(0.93f, 0.98f, 0.83f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.70f, 0.90f, 0.30f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 1.00f, 0.40f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.80f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.86f, 0.96f, 0.76f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.86f, 0.96f, 0.76f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.93f, 0.98f, 0.83f, 1.0f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.86f, 0.96f, 0.76f, 1.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.70f, 0.90f, 0.30f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.80f, 1.00f, 0.40f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.60f, 0.80f, 0.20f, 1.0f);

    c[ImGuiCol_Header] = ImVec4(0.70f, 0.90f, 0.30f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 1.00f, 0.40f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.80f, 0.20f, 0.8f);

    c[ImGuiCol_Separator] = ImVec4(0.3f,0.5f,0.1f,0.5f);
    c[ImGuiCol_Border] = ImVec4(0.3f,0.5f,0.1f,0.3f);

    s.WindowRounding = 16;
    s.FrameRounding = 10;
    s.GrabRounding = 10;
    s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(16,16);
    s.FramePadding = ImVec2(12,10);
    s.TouchExtraPadding = ImVec2(8,8);
}

// ===================== Zoom UI =====================
static void DrawZoomUI() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(380, 140), ImVec2(500, 180));

    ImGui::Begin("Camera Zoom", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::TextColored(ImVec4(0.2f, 0.5f, 0.1f, 1), "Camera Zoom Control");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    if (ImGui::Button(g_ZoomEnable ? "Reset FOV" : "Zoom In", ImVec2(-1, 55))) {
        ToggleZoom();
    }

    ImGui::Checkbox("Smooth Animation", &g_ZoomAnimated);
    ImGui::End();
}

// ===================== GL State =====================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst, bSrcA, bDstA;
    GLboolean blend, cull, depth, scissor, stencil, dither;
    GLint frontFace, activeTexture;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.activeTexture);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFuncSeparate(s.bSrc, s.bDst, s.bSrcA, s.bDstA);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glFrontFace(s.frontFace);
}

// ===================== ImGui Init =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    float scale = (float)g_Height / 720.0f;
    scale = std::clamp(scale, 1.6f, 4.0f);

    ImFontConfig cfg;
    cfg.SizePixels = 36 * scale;
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    g_UIFont = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ForceStyle();

    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;

    GLState s;
    SaveGL(s);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();

    DrawZoomUI();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ===================== EGL Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers)
        return orig_eglSwapBuffers(d, s);

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT)
        return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500)
        return orig_eglSwapBuffers(d, s);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
        }
    }

    if (ctx != g_TargetContext || s != g_TargetSurface)
        return orig_eglSwapBuffers(d, s);

    g_Width = w;
    g_Height = h;
    Setup();
    Render();

    return orig_eglSwapBuffers(d, s);
}

// ===================== Input Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1)
        orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void HookInput() {
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1)
        GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2)
        GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);

    GHandle egl = GlossOpen("libEGL.so");
    if (egl) {
        void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
        if (swap)
            GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    }

    HookInput();
    InitZoom();
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
