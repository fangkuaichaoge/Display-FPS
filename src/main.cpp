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

// ===================== Path Config (World Only Backup) =====================
#define GAME_BASE_DIR   "/storage/emulated/0/你的MC文件夹"
#define BACKUP_DIR      "/storage/emulated/0/MCBackup"
#define TEMP_DIR        "/storage/emulated/0/这个是缓存文件夹/cache/"
#define WORLD_FOLDER    "minecraftWorlds"
#define MAX_LOG_COUNT   30
#define READ_CHUNK_SIZE 8192

// ===================== Backup State Management =====================
struct BackupState {
    bool is_backing_up = false;
    std::string current_status = "Ready";
    std::vector<std::string> recent_logs;
    enum Result { NONE, SUCCESS, FAILED } last_result = NONE;
};
static BackupState g_backup_state;
static std::mutex g_state_mutex;

// ===================== ImGui Render Global State =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;
static ImFont* g_UIFont = nullptr;

// ===================== Hook Function Pointers =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== Utility Functions =====================
static bool ensure_dir(const std::string& path) {
    try {
        std::filesystem::create_directories(path);
        return std::filesystem::exists(path) && std::filesystem::is_directory(path);
    } catch (...) {
        return false;
    }
}

static std::string get_unique_temp_path() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::string(TEMP_DIR) + "world_backup_" + std::to_string(timestamp) + ".zip";
}

// ===================== Log Function =====================
static std::string get_log_path() {
    return std::string(BACKUP_DIR) + "/backup_log.txt";
}

static void log(const std::string& msg) {
    std::string formatted_msg;
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char time_buf[32] = {0};
        strftime(time_buf, sizeof(time_buf), "[%Y-%m-%d %H:%M:%S] ", localtime(&now_time));
        formatted_msg = std::string(time_buf) + msg;
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.recent_logs.push_back(formatted_msg);
        if (g_backup_state.recent_logs.size() > MAX_LOG_COUNT)
            g_backup_state.recent_logs.erase(g_backup_state.recent_logs.begin());
    }

    ensure_dir(BACKUP_DIR);
    try {
        FILE* f = fopen(get_log_path().c_str(), "a+");
        if (f) {
            fprintf(f, "%s\n", formatted_msg.c_str());
            fclose(f);
        }
    } catch (...) {}
}

// ===================== Core Compress Function (Bug Fixed) =====================
static bool compress_folder(const std::string& src_path, const std::string& dest_path) {
    remove(dest_path.c_str());

    if (!std::filesystem::exists(src_path) || !std::filesystem::is_directory(src_path)) {
        log("❌ World directory not found: " + src_path);
        return false;
    }

    std::string temp_path = get_unique_temp_path();
    remove(temp_path.c_str());

    int zip_error = 0;
    zip_t* zip = zip_open(temp_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (!zip) {
        log("❌ Failed to create zip, error code: " + std::to_string(zip_error));
        return false;
    }

    int file_count = 0;
    bool all_ok = true;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            std::string full_path = entry.path().string();
            std::string relative_path = full_path.substr(src_path.length() + 1);

            if (relative_path.empty() || relative_path[0] == '.') continue;

            if (entry.is_directory()) {
                zip_dir_add(zip, relative_path.c_str(), ZIP_FL_ENC_UTF_8);
                continue;
            }

            if (entry.is_regular_file()) {
                FILE* f = fopen(full_path.c_str(), "rb");
                if (!f) {
                    log("⚠️ Skip unreadable file: " + relative_path);
                    continue;
                }

                fseek(f, 0, SEEK_END);
                zip_int64_t file_size = ftell(f);
                fseek(f, 0, SEEK_SET);

                if (file_size <= 0) {
                    fclose(f);
                    continue;
                }

                zip_source_t* source = zip_source_file(zip, full_path.c_str(), 0, file_size);
                if (!source) {
                    fclose(f);
                    log("⚠️ Failed to create zip source: " + relative_path);
                    continue;
                }

                if (zip_file_add(zip, relative_path.c_str(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
                    zip_source_free(source);
                    fclose(f);
                    log("⚠️ Failed to add file to zip: " + relative_path);
                    all_ok = false;
                    continue;
                }

                fclose(f);
                file_count++;
            }
        }
    } catch (const std::exception& e) {
        log("❌ Failed to scan world directory: " + std::string(e.what()));
        all_ok = false;
    } catch (...) {
        log("❌ Unknown error when scanning world directory");
        all_ok = false;
    }

    if (zip_close(zip) < 0) {
        zip_error_t* err = zip_get_error(zip);
        log("❌ Failed to close zip: " + std::string(zip_error_strerror(err)));
        zip_discard(zip);
        remove(temp_path.c_str());
        return false;
    }

    if (!std::filesystem::exists(temp_path) || std::filesystem::file_size(temp_path) <= 0) {
        log("❌ Temp zip file is invalid or empty");
        remove(temp_path.c_str());
        return false;
    }

    try {
        std::filesystem::copy_file(temp_path, dest_path, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        log("❌ Failed to copy final backup file: " + std::string(e.what()));
        remove(temp_path.c_str());
        return false;
    }

    remove(temp_path.c_str());

    if (!std::filesystem::exists(dest_path) || std::filesystem::file_size(dest_path) <= 0) {
        log("❌ Final backup file is invalid: " + dest_path);
        remove(dest_path.c_str());
        return false;
    }

    log("✅ World backup success: " + dest_path + " | Files: " + std::to_string(file_count) + " | Size: " + std::to_string(std::filesystem::file_size(dest_path)/1024) + " KB");
    return true;
}

// ===================== Main World Backup Workflow =====================
static void do_backup() {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = true;
        g_backup_state.current_status = "Initializing...";
        g_backup_state.last_result = BackupState::NONE;
        g_backup_state.recent_logs.clear();
    }

    log("==================== World Backup Start ====================");
    log("📂 Target backup directory: " + std::string(BACKUP_DIR));

    if (!ensure_dir(BACKUP_DIR)) {
        log("❌ Failed to create backup directory!");
        log("💡 Please enable 'Manage all files' permission for the game in system settings");
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "No Permission";
        g_backup_state.last_result = BackupState::FAILED;
        log("==================== Backup Aborted ====================");
        return;
    }
    log("✅ Backup directory created/verified successfully");

    if (!ensure_dir(TEMP_DIR)) {
        log("❌ Failed to create temp directory!");
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "Init Failed";
        g_backup_state.last_result = BackupState::FAILED;
        log("==================== Backup Aborted ====================");
        return;
    }

    std::string world_root = std::string(GAME_BASE_DIR) + WORLD_FOLDER;
    try {
        if (!std::filesystem::exists(world_root)) {
            log("❌ Game world directory not found! Please enter the game first and try again");
            throw std::runtime_error("world dir missing");
        }
        log("✅ Game world directory found: " + world_root);

        FILE* test = fopen((std::string(BACKUP_DIR) + "/write_test.tmp").c_str(), "wb");
        if (!test) {
            log("❌ Storage permission denied! Cannot write to backup directory");
            log("💡 Please enable 'Manage all files' permission in system settings");
            throw std::runtime_error("permission denied");
        }
        fclose(test);
        remove((std::string(BACKUP_DIR) + "/write_test.tmp").c_str());
        log("✅ Storage permission check passed");

    } catch (...) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "Init Failed";
        g_backup_state.last_result = BackupState::FAILED;
        log("==================== Backup Aborted ====================");
        return;
    }

    int success_world_count = 0;
    int total_world_count = 0;

    for (auto& entry : std::filesystem::directory_iterator(world_root)) {
        if (!entry.is_directory()) continue;
        total_world_count++;
        std::string world_id = entry.path().filename().string();
        std::string dest_path = std::string(BACKUP_DIR) + "/" + world_id + ".mcworld";

        log("Exporting world: " + world_id);
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_backup_state.current_status = "Exporting: " + world_id;
        }

        if (compress_folder(entry.path().string(), dest_path)) {
            success_world_count++;
        } else {
            log("❌ Failed to export world: " + world_id);
        }
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(TEMP_DIR)) {
            if (entry.path().filename().string().find("world_backup_") == 0) {
                remove(entry.path());
            }
        }
    } catch (...) {}

    log("==================== World Backup Completed ====================");
    log("📊 Total worlds: " + std::to_string(total_world_count) + " | Success: " + std::to_string(success_world_count));
    log("📂 All world backups saved to: " + std::string(BACKUP_DIR));

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "Backup Completed";
        g_backup_state.last_result = success_world_count > 0 ? BackupState::SUCCESS : BackupState::FAILED;
    }
}

// ===================== Yellow-Green Theme Style =====================
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

// ===================== UI Interface (Full English) =====================
static void DrawUI() {
    ForceStyle();
    if (g_UIFont) ImGui::PushFont(g_UIFont);

    ImGuiIO& io = ImGui::GetIO();
    const float pad = 20;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(380, 0), ImVec2(io.DisplaySize.x*0.92f, io.DisplaySize.y*0.85f));

    ImGui::Begin("MC Backup Tool", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("MC World Backup").x) * 0.5f);
    ImGui::TextColored(ImVec4(0.2f,0.5f,0.1f,1), "MC World Backup");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0,10));

    BackupState st;
    { std::lock_guard<std::mutex> l(g_state_mutex); st = g_backup_state; }

    if (st.is_backing_up) {
        ImGui::BeginDisabled();
        ImGui::Button("Backing up worlds...", ImVec2(-1,60));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Start World Backup", ImVec2(-1,60))) {
            std::thread(do_backup).detach();
        }
    }

    ImGui::Text("Status: ");
    ImGui::SameLine();
    if (st.is_backing_up)
        ImGui::TextColored(ImVec4(0.8f,0.6f,0,1), "%s", st.current_status.c_str());
    else if (st.last_result == BackupState::SUCCESS)
        ImGui::TextColored(ImVec4(0.2f,0.7f,0.2f,1), "Backup Completed");
    else if (st.last_result == BackupState::FAILED)
        ImGui::TextColored(ImVec4(0.9f,0.2f,0.2f,1), "Backup Failed");
    else
        ImGui::Text("Ready");

    ImGui::Separator();
    ImGui::Text("Log:");
    ImGui::BeginChild("log", ImVec2(-1,260), true);
    for (auto& s : st.recent_logs) ImGui::TextWrapped("%s", s.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 15) ImGui::SetScrollHereY(1);
    ImGui::EndChild();

    ImGui::End();
    if (g_UIFont) ImGui::PopFont();
}

// ===================== GL State Protection =====================
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

// ===================== ImGui Initialization =====================
static void Setup() {
    if (g_Initialized || g_Width <=0 || g_Height <=0) return;
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

// ===================== EGL Render Hook =====================
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

// ===================== Input Hook =====================
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

// ===================== Main Thread =====================
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
