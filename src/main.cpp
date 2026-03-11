// ===================== 系统标准库头文件 =====================
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

// ===================== 第三方库头文件 =====================
#include <zip.h>

// ===================== 项目依赖头文件 =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

// ===================== 备份路径常量配置 =====================
#define BASE_DIR        "/storage/emulated/0/Android/data/com.stardust.mclauncher/files/games/com.mojang/"
#define ROOT_DIR        "/storage/emulated/0/MC外部备份"
#define DIRS_BEHAVIOR   "behavior_packs"
#define DIRS_RESOURCE   "resource_packs"
#define DIRS_SKIN       "skin_packs"
#define DIRS_WORLD      "minecraftWorlds"
#define TMP_ZIP         "/data/local/tmp/backup_tmp.zip"
#define MAX_LOG_COUNT   30
#define READ_CHUNK_SIZE 4096

// ===================== 线程安全的备份状态管理 =====================
struct BackupState {
    bool is_backing_up = false;
    std::string current_status = "等待执行备份";
    std::vector<std::string> recent_logs;
    enum Result { NONE, SUCCESS, FAILED } last_result = NONE;
};
static BackupState g_backup_state;
static std::mutex g_state_mutex;

// ===================== ImGui渲染核心全局状态 =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;
static ImFont* g_ChineseFont = nullptr;

// ===================== Hook函数指针声明 =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== 日志功能 =====================
static std::string get_log_path() {
    return std::string(ROOT_DIR) + "/backup_log.txt";
}

static void log(const std::string& msg) {
    std::string formatted_msg;
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char time_buf[32] = {0};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now_time));
        formatted_msg = "[" + std::string(time_buf) + "] " + msg;
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.recent_logs.push_back(formatted_msg);
        if (g_backup_state.recent_logs.size() > MAX_LOG_COUNT)
            g_backup_state.recent_logs.erase(g_backup_state.recent_logs.begin());
    }

    try {
        std::filesystem::create_directories(ROOT_DIR);
        FILE* f = fopen(get_log_path().c_str(), "a+");
        if (f) {
            fprintf(f, "%s\n", formatted_msg.c_str());
            fclose(f);
        }
    } catch (...) {}
}

// ===================== 压缩功能 =====================
static bool compress_folder(const std::string& src_path, const std::string& dest_path) {
    remove(TMP_ZIP);
    int error = 0;
    zip_t* zip = zip_open(TMP_ZIP, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip) return false;

    bool all_ok = true;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            std::string full_path = entry.path().string();
            std::string relative_path = full_path.substr(src_path.length() + 1);
            if (relative_path.empty() || relative_path[0] == '.') continue;

            if (entry.is_directory()) {
                zip_dir_add(zip, relative_path.c_str(), 0);
            } else if (entry.is_regular_file()) {
                FILE* f = fopen(full_path.c_str(), "rb");
                if (!f) continue;

                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);

                zip_source_t* source = zip_source_filep(zip, f, 0, file_size);
                if (!source) { fclose(f); continue; }
                if (zip_file_add(zip, relative_path.c_str(), source, ZIP_FL_OVERWRITE) < 0) {
                    zip_source_free(source); fclose(f); continue;
                }
            }
        }
    } catch (...) { all_ok = false; }

    if (zip_close(zip) < 0) { zip_discard(zip); all_ok = false; }
    if (!all_ok) { remove(TMP_ZIP); return false; }

    FILE* tmp_f = fopen(TMP_ZIP, "rb");
    FILE* dest_f = fopen(dest_path.c_str(), "wb");
    if (!tmp_f || !dest_f) {
        if (tmp_f) fclose(tmp_f);
        if (dest_f) fclose(dest_f);
        remove(TMP_ZIP);
        return false;
    }

    uint8_t buf[READ_CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), tmp_f)) > 0)
        fwrite(buf, 1, n, dest_f);

    fclose(tmp_f);
    fclose(dest_f);
    remove(TMP_ZIP);
    return true;
}

// ===================== 备份逻辑 =====================
static void do_backup() {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = true;
        g_backup_state.current_status = "初始化备份...";
        g_backup_state.last_result = BackupState::NONE;
        g_backup_state.recent_logs.clear();
    }

    log("=== 开始全量备份 ===");

    try {
        if (!std::filesystem::exists(BASE_DIR)) {
            log("游戏目录不存在");
            throw std::runtime_error("dir not exist");
        }
        std::filesystem::create_directories(ROOT_DIR);
        FILE* test = fopen((std::string(ROOT_DIR) + "/test.tmp").c_str(), "wb");
        if (!test) { log("无存储权限"); throw std::runtime_error("permission"); }
        fclose(test); remove((std::string(ROOT_DIR) + "/test.tmp").c_str());
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "初始化失败";
        g_backup_state.last_result = BackupState::FAILED;
        return;
    }

    std::string addon_path = std::string(ROOT_DIR) + "/MC备份_资源包.mcaddon";
    int err = 0;
    zip_t* main_zip = zip_open(addon_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (main_zip) {
        const char* dirs[] = {DIRS_RESOURCE, DIRS_BEHAVIOR, DIRS_SKIN};
        for (auto dir : dirs) {
            std::string full = std::string(BASE_DIR) + dir;
            if (!std::filesystem::exists(full)) continue;
            for (auto& entry : std::filesystem::directory_iterator(full)) {
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                std::string tmp = std::string(TMP_ZIP);
                compress_folder(entry.path().string(), tmp);

                FILE* f = fopen(tmp.c_str(), "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                zip_source_t* s = zip_source_filep(main_zip, f, 0, sz);
                if (s) zip_file_add(main_zip, (name + ".mcpack").c_str(), s, ZIP_FL_OVERWRITE);
                remove(tmp.c_str());
            }
        }
        zip_close(main_zip);
    }

    std::string world_root = std::string(BASE_DIR) + DIRS_WORLD;
    if (std::filesystem::exists(world_root)) {
        for (auto& entry : std::filesystem::directory_iterator(world_root)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            std::string dst = std::string(ROOT_DIR) + "/" + name + ".mcworld";
            compress_folder(entry.path().string(), dst);
            log("导出地图: " + name);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "备份完成";
        g_backup_state.last_result = BackupState::SUCCESS;
    }
    log("=== 备份完成 ===");
}

// ===================== UI =====================
static void DrawUI() {
    if (g_ChineseFont) ImGui::PushFont(g_ChineseFont);

    ImGuiIO& io = ImGui::GetIO();
    const float pad = 20;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(380, 0), ImVec2(io.DisplaySize.x*0.92f, io.DisplaySize.y*0.85f));

    ImGui::Begin("MC一键备份工具", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("MC世界一键备份工具").x) * 0.5f);
    ImGui::TextColored(ImVec4(0.2f,0.5f,0.1f,1), "MC世界一键备份工具");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0,10));

    BackupState st;
    { std::lock_guard<std::mutex> l(g_state_mutex); st = g_backup_state; }

    if (st.is_backing_up) {
        ImGui::BeginDisabled();
        ImGui::Button("正在备份...", ImVec2(-1,60));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("✅ 一键全量备份", ImVec2(-1,60))) {
            std::thread(do_backup).detach();
        }
    }

    ImGui::Text("状态: ");
    ImGui::SameLine();
    if (st.is_backing_up)
        ImGui::TextColored(ImVec4(0.8f,0.6f,0,1), "%s", st.current_status.c_str());
    else if (st.last_result == BackupState::SUCCESS)
        ImGui::TextColored(ImVec4(0.2f,0.7f,0.2f,1), "备份完成");
    else if (st.last_result == BackupState::FAILED)
        ImGui::TextColored(ImVec4(0.9f,0.2f,0.2f,1), "备份失败");
    else
        ImGui::Text("等待操作");

    ImGui::Separator();
    ImGui::Text("日志:");
    ImGui::BeginChild("log", ImVec2(-1,260), true);
    for (auto& s : st.recent_logs) ImGui::TextWrapped("%s", s.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 15) ImGui::SetScrollHereY(1);
    ImGui::EndChild();

    ImGui::End();
    if (g_ChineseFont) ImGui::PopFont();
}

// ===================== GL状态 =====================
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

// ===================== 样式：黄绿色主题（真正生效版） =====================
static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // 黄绿色主题
    c[ImGuiCol_WindowBg] = ImVec4(0.92f, 0.98f, 0.82f, 0.94f);
    c[ImGuiCol_Text] = ImVec4(0.15f, 0.35f, 0.05f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.65f, 0.85f, 0.3f, 0.9f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.95f, 0.4f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.75f, 0.2f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.85f, 0.95f, 0.75f, 0.9f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.8f, 0.9f, 0.7f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.75f, 0.85f, 0.65f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.65f, 0.85f, 0.3f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.95f, 0.4f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.75f, 0.2f, 0.8f);
    c[ImGuiCol_Separator] = ImVec4(0.3f,0.5f,0.1f,0.5f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.85f,0.95f,0.75f,0.5f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.65f,0.85f,0.3f,0.6f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.75f,0.95f,0.4f,0.8f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f,0.75f,0.2f,0.8f);

    s.WindowRounding = 16;
    s.FrameRounding = 10;
    s.GrabRounding = 10;
    s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(16,16);
    s.FramePadding = ImVec2(12,10);
    s.TouchExtraPadding = ImVec2(8,8);
}

// ===================== 初始化 =====================
static void Setup() {
    if (g_Initialized || g_Width <=0 || g_Height <=0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    float scale = (float)g_Height / 720.0f;
    scale = std::clamp(scale, 1.6f, 4.0f);

    // 中文字体
    const char* paths[] = {
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/DroidSansFallback.ttf",
        "/system/fonts/MiSans-Regular.ttf",
        "/system/fonts/HarmonyOS_Sans_SC_Regular.ttf"
    };

    ImFontConfig cfg;
    cfg.SizePixels = 36 * scale;
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    for (auto p : paths) {
        if (access(p, F_OK) == 0) {
            g_ChineseFont = io.Fonts->AddFontFromFileTTF(p, cfg.SizePixels, &cfg, io.Fonts->GetGlyphRangesChineseFull());
            if (g_ChineseFont) break;
        }
    }
    if (!g_ChineseFont) g_ChineseFont = io.Fonts->AddFontDefault(&cfg);

    // 初始化后端
    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // ✅ 这里只设置一次颜色，真正生效
    SetupStyle();

    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s; SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1,1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ===================== EGL Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d,s);
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d,s);

    EGLint w=0,h=0;
    eglQuerySurface(d,s,EGL_WIDTH,&w);
    eglQuerySurface(d,s,EGL_HEIGHT,&h);
    if (w<500||h<500) return orig_eglSwapBuffers(d,s);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d,s,EGL_RENDER_BUFFER,&buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
        }
    }

    if (ctx != g_TargetContext || s != g_TargetSurface)
        return orig_eglSwapBuffers(d,s);

    g_Width = w; g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(d,s);
}

// ===================== 输入Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz,a1,a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz,a1,a2,a3,a4,e) : 0;
    if (r == 0 && e && *e && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void HookInput() {
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== 启动 =====================
static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle egl = GlossOpen("libEGL.so");
    if (!egl) return nullptr;
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
