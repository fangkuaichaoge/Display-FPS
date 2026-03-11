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
#include <filesystem>
#include <algorithm>

// ===================== Third-party Header Files =====================
#include <zip.h>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

// ===================== Config =====================
#define BACKUP_DIR          "/storage/emulated/0/MCBackup"
#define TEMP_DIR            "/storage/emulated/0/Android/data/com.stardust.mclauncher/cache"
#define MAX_LOGS            30

namespace fs = std::filesystem;

// ===================== Backup State =====================
struct BackupState {
    bool is_backing_up = false;
    std::string status = "Ready";
    std::vector<std::string> logs;

    int mode = 0; // 0: Auto Internal Data, 1: Stardust External, 2: Slauncher

    std::vector<std::string> slauncher_versions;
    int selected_version = 0;

    enum { NONE, SUCCESS, FAILED } result = NONE;
};

static BackupState g_bs;
static std::mutex g_mutex;

// ===================== Globals =====================
static bool g_imGuiReady = false;
static int g_w = 0, g_h = 0;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static ImFont* g_font = nullptr;

// ===================== Hooks =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== Util =====================
static bool ensureDir(const std::string& path) {
    try { return fs::create_directories(path) || fs::is_directory(path); } catch (...) { return false; }
}

static std::string tempZip() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string(TEMP_DIR) + "/w_" + std::to_string(ms) + ".zip";
}

static void addLog(const std::string& msg) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_bs.logs.size() >= MAX_LOGS) g_bs.logs.erase(g_bs.logs.begin());
    g_bs.logs.push_back(msg);
}

// ===================== CORE: Auto get own data directory =====================
static std::string getSelfDataDir() {
    char path[256];
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return "";
    char pkg[128] = {0};
    fread(pkg, 1, 127, f);
    fclose(f);

    snprintf(path, sizeof(path), "/data/data/%s/", pkg);
    return path;
}

// ===================== Auto find minecraftWorlds =====================
static std::string findWorldDir() {
    std::vector<std::string> bases;

    std::lock_guard<std::mutex> l(g_mutex);
    switch (g_bs.mode) {
        case 0: {
            // AUTO: own data dir
            bases.push_back(getSelfDataDir());
            break;
        }
        case 1: {
            // Stardust external
            bases.push_back("/storage/emulated/0/Android/data/com.stardust.mclauncher/files/");
            break;
        }
        case 2: {
            if (g_bs.slauncher_versions.empty() || g_bs.selected_version < 0 ||
                g_bs.selected_version >= (int)g_bs.slauncher_versions.size())
                return "";
            bases.push_back("/storage/emulated/0/slauncher/version/" +
                            g_bs.slauncher_versions[g_bs.selected_version] + "/");
            break;
        }
    }

    std::vector<std::string> suffixes = {
        "games/com.mojang/minecraftWorlds",
        "com.mojang/minecraftWorlds",
        "minecraftWorlds",
        "games/minecraftWorlds"
    };

    for (auto &base : bases) {
        if (base.empty()) continue;
        for (auto &suf : suffixes) {
            std::string full = base + suf;
            try {
                if (fs::is_directory(full)) {
                    addLog("Found world dir: " + full);
                    return full;
                }
            } catch (...) {}
        }
    }
    return "";
}

// ===================== Slauncher =====================
static void refreshSlauncherVersions() {
    std::lock_guard<std::mutex> l(g_mutex);
    g_bs.slauncher_versions.clear();
    g_bs.selected_version = 0;
    std::string root = "/storage/emulated/0/slauncher/version/";
    try {
        for (auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory())
                g_bs.slauncher_versions.push_back(entry.path().filename().string());
        }
    } catch (...) {}
}

// ===================== Zip =====================
static bool zipFolder(const std::string& src, const std::string& dst) {
    std::string tmp = tempZip();
    remove(dst.c_str());
    remove(tmp.c_str());

    int ec = 0;
    zip_t* z = zip_open(tmp.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &ec);
    if (!z) { addLog("zip open failed"); return false; }

    try {
        for (auto& entry : fs::recursive_directory_iterator(src)) {
            if (entry.is_directory()) {
                std::string rel = entry.path().string().substr(src.size() + 1);
                if (!rel.empty()) zip_dir_add(z, rel.c_str(), ZIP_FL_ENC_UTF_8);
                continue;
            }
            if (!entry.is_regular_file()) continue;
            std::string fp = entry.path().string();
            std::string rel = fp.substr(src.size() + 1);
            if (rel.empty() || rel[0] == '.') continue;

            zip_source_t* s = zip_source_file(z, fp.c_str(), 0, -1);
            if (s) zip_file_add(z, rel.c_str(), s, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        }
    } catch (...) {
        zip_discard(z); remove(tmp.c_str());
        addLog("scan failed");
        return false;
    }

    if (zip_close(z) != 0) { remove(tmp.c_str()); addLog("zip close failed"); return false; }
    try { fs::copy_file(tmp, dst, fs::copy_options::overwrite_existing); }
    catch (...) { remove(tmp.c_str()); addLog("copy failed"); return false; }
    remove(tmp.c_str());
    return true;
}

// ===================== Backup Task =====================
static void doBackup() {
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_bs.is_backing_up = true;
        g_bs.status = "Starting...";
        g_bs.result = BackupState::NONE;
        g_bs.logs.clear();
    }

    ensureDir(BACKUP_DIR);
    ensureDir(TEMP_DIR);

    std::string worldRoot = findWorldDir();
    if (worldRoot.empty()) {
        addLog("minecraftWorlds not found");
        std::lock_guard<std::mutex> l(g_mutex);
        g_bs.is_backing_up = false;
        g_bs.result = BackupState::FAILED;
        g_bs.status = "Failed";
        return;
    }

    int total = 0, success = 0;
    try {
        for (auto& entry : fs::directory_iterator(worldRoot)) {
            if (!entry.is_directory()) continue;
            total++;
            std::string name = entry.path().filename().string();
            std::string out = BACKUP_DIR + std::string("/") + name + ".mcworld";

            addLog("Backing up: " + name);
            {
                std::lock_guard<std::mutex> l(g_mutex);
                g_bs.status = "Backup: " + name;
            }

            if (zipFolder(entry.path().string(), out)) {
                success++;
                addLog("OK: " + name);
            } else {
                addLog("FAIL: " + name);
            }
        }
    } catch (...) {
        addLog("Error during backup");
    }

    addLog("Done: " + std::to_string(success) + "/" + std::to_string(total));
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_bs.is_backing_up = false;
        g_bs.result = success > 0 ? BackupState::SUCCESS : BackupState::FAILED;
        g_bs.status = "Completed";
    }
}

// ===================== UI =====================
static void drawUI() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::Begin("MC World Backup", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(ImVec4(0.2f, 0.5f, 0.1f, 1), "MC World Backup Tool");
    ImGui::Separator();

    BackupState st;
    { std::lock_guard<std::mutex> _(g_mutex); st = g_bs; }

    ImGui::Text("Source:");
    ImGui::RadioButton("Auto Internal Data", &g_bs.mode, 0);
    ImGui::RadioButton("Stardust External", &g_bs.mode, 1);
    ImGui::RadioButton("Slauncher Versions", &g_bs.mode, 2);

    if (st.mode == 2) {
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Refresh Versions")) refreshSlauncherVersions();

        if (st.slauncher_versions.empty()) {
            ImGui::Text("No versions");
        } else {
            ImGui::Text("Select Version:");
            for (int i = 0; i < (int)st.slauncher_versions.size(); i++) {
                if (ImGui::Selectable(st.slauncher_versions[i].c_str(), i == st.selected_version))
                    g_bs.selected_version = i;
            }
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (st.is_backing_up) {
        ImGui::BeginDisabled();
        ImGui::Button("Backing up...", ImVec2(-1, 50));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Start Backup", ImVec2(-1, 50)))
            std::thread(doBackup).detach();
    }

    ImGui::Text("Status: ");
    ImGui::SameLine();
    if (st.is_backing_up)
        ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.0f, 1), "%s", st.status.c_str());
    else if (st.result == BackupState::SUCCESS)
        ImGui::TextColored(ImVec4(0.2f, 0.7f, 0.2f, 1), "Completed");
    else if (st.result == BackupState::FAILED)
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1), "Failed");
    else
        ImGui::Text("Ready");

    ImGui::Separator();
    ImGui::Text("Log:");
    ImGui::BeginChild("log", ImVec2(-1, 220), true);
    for (auto& s : st.logs) ImGui::TextWrapped("%s", s.c_str());
    ImGui::EndChild();

    ImGui::End();
}

// ===================== GL & Render =====================
struct GLState {
    GLint prog, tex, aBuf, eBuf, vao, fbo, vp[4];
    GLboolean blend, depth, cull;
};

static void saveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    s.blend = glIsEnabled(GL_BLEND);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE);
}

static void restoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
}

static void setupImGui() {
    if (g_imGuiReady) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    float scale = std::clamp((float)g_h / 720.0f, 1.5f, 3.0f);
    ImFontConfig cfg;
    cfg.SizePixels = 32 * scale;
    g_font = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_imGuiReady = true;
}

static void render() {
    if (!g_imGuiReady) return;
    GLState s; saveGL(s);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_w, g_h);
    ImGui::NewFrame();
    drawUI();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    restoreGL(s);
}

// ===================== EGL Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, surf);
    EGLContext ctx = eglGetCurrentContext();

    EGLint w=0,h=0;
    eglQuerySurface(d, surf, EGL_WIDTH, &w);
    eglQuerySurface(d, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, surf);

    if (g_eglCtx == EGL_NO_CONTEXT) {
        g_eglCtx = ctx;
        g_eglSurf = surf;
    }
    if (ctx != g_eglCtx || surf != g_eglSurf)
        return orig_eglSwapBuffers(d, surf);

    g_w = w; g_h = h;
    setupImGui();
    render();
    return orig_eglSwapBuffers(d, surf);
}

// ===================== Input Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (g_imGuiReady && thiz)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_imGuiReady)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void hookInput() {
    void* h1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (h1) GlossHook(h1, (void*)hook_Input1, (void**)&orig_Input1);

    void* h2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (h2) GlossHook(h2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    sleep(2);
    GlossInit(true);
    refreshSlauncherVersions();

    void* egl = GlossOpen("libEGL.so");
    if (egl) {
        void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    }
    hookInput();
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
