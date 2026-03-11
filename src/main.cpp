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
#define ROOT_DIR        "/storage/emulated/0/外部备份文件夹"
#define DIRS_BEHAVIOR   "behavior_packs"
#define DIRS_RESOURCE   "resource_packs"
#define DIRS_SKIN       "skin_packs"
#define DIRS_WORLD      "minecraftWorlds"
#define TMP_ZIP         "/storage/emulated/0/backup_tmp.zip"
#define MAX_LOG_COUNT   20

// ===================== 线程安全的备份状态管理 =====================
struct BackupState {
    bool is_backing_up = false;
    std::string current_status = "等待执行备份";
    std::vector<std::string> recent_logs;
    enum Result { NONE, SUCCESS, FAILED } last_result = NONE;
};
static BackupState g_backup_state;
static std::mutex g_state_mutex;

// ===================== 全局基础运行状态 =====================
static bool g_ImGui_Initialized = false;
static int g_Screen_Width = 0, g_Screen_Height = 0;
static EGLContext g_Target_Context = EGL_NO_CONTEXT;
static EGLSurface g_Target_Surface = EGL_NO_SURFACE;

// ===================== Hook函数指针声明 =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static const char* MAIN_WINDOW_NAME = "MC一键备份工具";

// ===================== 日志功能 =====================
static std::string get_log_path() {
    return std::string(ROOT_DIR) + "/backup_log.txt";
}

static void log(const std::string& msg) {
    // 写本地日志文件
    std::filesystem::create_directories(ROOT_DIR);
    FILE* f = fopen(get_log_path().c_str(), "a+");
    if (f) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char time_buf[32] = {0};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now_time));
        fprintf(f, "[%s] %s\n", time_buf, msg.c_str());
        fclose(f);
    }

    // 同步到UI显示
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_backup_state.recent_logs.push_back(msg);
    if (g_backup_state.recent_logs.size() > MAX_LOG_COUNT) {
        g_backup_state.recent_logs.erase(g_backup_state.recent_logs.begin());
    }
}

// ===================== 核心压缩功能 =====================
static bool compress_folder(const std::string& src_path, const std::string& dest_path) {
    remove(TMP_ZIP);

    int error = 0;
    zip_t* zip = zip_open(TMP_ZIP, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip) {
        log("创建临时压缩文件失败: 错误码" + std::to_string(error));
        return false;
    }

    bool all_ok = true;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            std::string full_path = entry.path().string();
            std::string relative_path = full_path.substr(src_path.length() + 1);

            if (entry.is_directory()) {
                if (!relative_path.empty()) zip_dir_add(zip, relative_path.c_str(), 0);
            } else if (entry.is_regular_file()) {
                FILE* f = fopen(full_path.c_str(), "rb");
                if (!f) {
                    log("跳过无法读取的文件: " + relative_path);
                    continue;
                }

                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);

                std::vector<uint8_t> buffer(file_size);
                if (fread(buffer.data(), 1, file_size, f) != file_size) {
                    fclose(f);
                    log("读取文件失败: " + relative_path);
                    continue;
                }
                fclose(f);

                zip_source_t* source = zip_source_buffer(zip, buffer.data(), file_size, 0);
                if (!source) {
                    log("创建zip源失败: " + relative_path);
                    continue;
                }

                if (zip_file_add(zip, relative_path.c_str(), source, ZIP_FL_OVERWRITE) < 0) {
                    zip_source_free(source);
                    log("添加文件到zip失败: " + relative_path);
                    all_ok = false;
                    continue;
                }
            }
        }
    } catch (const std::exception& e) {
        log("遍历文件夹失败: " + std::string(e.what()));
        all_ok = false;
    }

    if (zip_close(zip) < 0) {
        log("压缩文件关闭失败");
        zip_discard(zip);
        all_ok = false;
    }

    if (!all_ok) {
        remove(TMP_ZIP);
        return false;
    }

    // 复制到目标路径
    FILE* tmp_f = fopen(TMP_ZIP, "rb");
    FILE* dest_f = fopen(dest_path.c_str(), "wb");
    if (!tmp_f || !dest_f) {
        log("复制最终文件失败");
        if (tmp_f) fclose(tmp_f);
        if (dest_f) fclose(dest_f);
        remove(TMP_ZIP);
        return false;
    }

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), tmp_f)) > 0) {
        fwrite(buf, 1, n, dest_f);
    }

    fclose(tmp_f);
    fclose(dest_f);
    remove(TMP_ZIP);

    return true;
}

// ===================== 核心备份全流程 =====================
static void do_backup() {
    // 更新备份状态
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = true;
        g_backup_state.current_status = "正在初始化备份...";
        g_backup_state.last_result = BackupState::NONE;
        g_backup_state.recent_logs.clear();
    }

    log("=== 开始备份 ===");
    std::filesystem::create_directories(ROOT_DIR);
    remove(TMP_ZIP);

    // 1. 打包资源/行为/皮肤包为mcaddon
    std::string addon_path = std::string(ROOT_DIR) + "/外部备份_资源包+行为包+皮肤包.mcaddon";
    remove(addon_path.c_str());

    int error = 0;
    zip_t* main_zip = zip_open(addon_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!main_zip) {
        log("创建mcaddon文件失败: 错误码" + std::to_string(error));
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "备份失败，无法创建mcaddon";
        g_backup_state.last_result = BackupState::FAILED;
        return;
    }

    const char* pack_dirs[] = { DIRS_RESOURCE, DIRS_BEHAVIOR, DIRS_SKIN };
    for (const char* dir : pack_dirs) {
        std::string full_dir = std::string(BASE_DIR) + dir;
        if (!std::filesystem::exists(full_dir)) {
            log("跳过不存在的目录: " + std::string(dir));
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(full_dir)) {
            if (!entry.is_directory()) continue;

            std::string pack_path = entry.path().string();
            std::string pack_name = entry.path().filename().string();
            std::string tmp_pack_name = pack_name + ".mcpack";

            log("正在打包: " + tmp_pack_name);
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_backup_state.current_status = "正在打包: " + pack_name;
            }

            if (!compress_folder(pack_path, TMP_ZIP)) {
                log("打包失败，跳过: " + pack_name);
                continue;
            }

            // 读取临时mcpack添加到主文件
            FILE* tmp_f = fopen(TMP_ZIP, "rb");
            if (!tmp_f) continue;

            fseek(tmp_f, 0, SEEK_END);
            long file_size = ftell(tmp_f);
            fseek(tmp_f, 0, SEEK_SET);

            std::vector<uint8_t> buffer(file_size);
            fread(buffer.data(), 1, file_size, tmp_f);
            fclose(tmp_f);
            remove(TMP_ZIP);

            zip_source_t* source = zip_source_buffer(main_zip, buffer.data(), file_size, 0);
            if (source) zip_file_add(main_zip, tmp_pack_name.c_str(), source, ZIP_FL_OVERWRITE);
        }
    }

    if (zip_close(main_zip) < 0) {
        log("mcaddon文件打包失败");
    } else {
        log("资源包/行为包/皮肤包 → 合并为mcaddon完成");
    }

    // 2. 导出地图为mcworld
    std::string world_root = std::string(BASE_DIR) + DIRS_WORLD;
    if (std::filesystem::exists(world_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(world_root)) {
            if (!entry.is_directory()) continue;

            std::string world_path = entry.path().string();
            std::string world_name = entry.path().filename().string();
            std::string dest_path = std::string(ROOT_DIR) + "/" + world_name + ".mcworld";

            log("正在导出地图: " + world_name);
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_backup_state.current_status = "正在导出地图: " + world_name;
            }

            if (compress_folder(world_path, dest_path)) {
                log("地图导出成功: " + world_name);
            } else {
                log("地图导出失败: " + world_name);
            }
        }
    }

    // 备份完成
    log("=== 全部备份完成 ===");
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_backup_state.is_backing_up = false;
    g_backup_state.current_status = "备份完成";
    g_backup_state.last_result = BackupState::SUCCESS;
}

// ===================== ImGui UI绘制（一键备份核心界面） =====================
static void DrawUI() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec4 titleColor = ImVec4(0.15f, 0.35f, 0.05f, 1.0f);
    const ImVec4 successColor = ImVec4(0.12f, 0.65f, 0.12f, 1.0f);
    const ImVec4 failedColor = ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
    const ImVec4 warningColor = ImVec4(0.75f, 0.55f, 0.05f, 1.0f);

    // 窗口基础设置：默认左上角，可拖动
    const float pad = 18.0f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0), ImVec2(450.0f, io.DisplaySize.y * 0.8f));

    ImGui::Begin(MAIN_WINDOW_NAME, nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    // 可拖动标题区域
    ImVec2 titleSize = ImGui::CalcTextSize("☘︎ MC一键备份工具");
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
    ImGui::TextColored(titleColor, "☘︎ MC一键备份工具");
    
    ImVec2 titleMin = ImGui::GetItemRectMin();
    ImVec2 titleMax = ImGui::GetItemRectMax();
    ImGui::SetCursorScreenPos(titleMin);
    ImGui::InvisibleButton("##drag_zone", ImVec2(titleMax.x - titleMin.x, titleMax.y - titleMin.y));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 dragDelta = ImGui::GetMouseDragDelta(0);
        ImGui::SetWindowPos(MAIN_WINDOW_NAME, ImVec2(mousePos.x - dragDelta.x, mousePos.y - dragDelta.y));
    }

    ImGui::Separator();
    ImGui::Spacing(10);

    // 线程安全读取状态
    std::lock_guard<std::mutex> lock(g_state_mutex);

    // 核心一键备份按钮
    if (g_backup_state.is_backing_up) {
        ImGui::BeginDisabled();
        ImGui::Button("正在备份中，请稍候...", ImVec2(-1, 50));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("✅ 一键执行全量备份", ImVec2(-1, 50))) {
            std::thread backup_thread(do_backup);
            backup_thread.detach();
        }
    }

    ImGui::Spacing(8);

    // 状态显示
    ImGui::TextColored(titleColor, "当前状态：");
    ImGui::SameLine();
    if (g_backup_state.is_backing_up) {
        ImGui::TextColored(warningColor, "%s", g_backup_state.current_status.c_str());
    } else if (g_backup_state.last_result == BackupState::SUCCESS) {
        ImGui::TextColored(successColor, "✅ 备份完成，文件已保存到指定目录");
    } else if (g_backup_state.last_result == BackupState::FAILED) {
        ImGui::TextColored(failedColor, "❌ 备份失败，请查看日志");
    } else {
        ImGui::Text("等待执行备份");
    }

    ImGui::Spacing(8);
    ImGui::Separator();
    ImGui::Spacing(8);

    // 日志预览区域
    ImGui::TextColored(titleColor, "备份日志：");
    ImGui::BeginChild("##log_area", ImVec2(-1, 220), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const auto& log_line : g_backup_state.recent_logs) {
        ImGui::TextWrapped("%s", log_line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::End();
}

// ===================== GL状态保护（避免游戏花屏） =====================
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

// ===================== ImGui环境初始化 =====================
static void SetupImGui() {
    if (g_ImGui_Initialized || g_Screen_Width <= 0 || g_Screen_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 屏幕自动缩放适配
    float scale = (float)g_Screen_Height / 720.0f;
    scale = (scale < 1.6f) ? 1.6f : (scale > 4.0f ? 4.0f : scale);

    // 字体高清渲染
    ImFontConfig cfg;
    cfg.SizePixels = 36.0f * scale;
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    io.Fonts->AddFontDefault(&cfg);

    // 淡黄绿主题样式
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

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.92f, 0.98f, 0.82f, 0.85f);
    colors[ImGuiCol_Text] = ImVec4(0.08f, 0.12f, 0.03f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.45f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.85f, 0.95f, 0.70f, 0.95f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.80f, 0.92f, 0.65f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.75f, 0.90f, 0.60f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.65f, 0.85f, 0.35f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.90f, 0.45f, 0.98f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.75f, 0.25f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.60f, 0.80f, 0.30f, 0.95f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.88f, 0.40f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.50f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.45f, 0.10f, 0.6f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.25f, 0.45f, 0.10f, 0.75f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.25f, 0.45f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.88f, 0.95f, 0.75f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.85f, 0.93f, 0.70f, 1.00f);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    style.ScaleAllSizes(scale);

    g_ImGui_Initialized = true;
}

// ===================== EGL渲染Hook（每帧绘制UI） =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;

    // 获取屏幕宽高
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 400 || h < 400) return orig_eglSwapBuffers(dpy, surf);

    // 初始化目标渲染上下文
    if (g_Target_Context == EGL_NO_CONTEXT) {
        g_Target_Context = eglGetCurrentContext();
        g_Target_Surface = surf;
    }

    // 只在目标游戏窗口渲染
    if (eglGetCurrentContext() != g_Target_Context || surf != g_Target_Surface) {
        return orig_eglSwapBuffers(dpy, surf);
    }

    g_Screen_Width = w;
    g_Screen_Height = h;
    SetupImGui();

    // 渲染ImGui UI
    if (g_ImGui_Initialized) {
        GLState gl_state;
        SaveGL(gl_state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();

        DrawUI(); // 绘制备份UI

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        RestoreGL(gl_state);
    }

    return orig_eglSwapBuffers(dpy, surf);
}

// ===================== 触摸事件Hook（保证UI交互正常） =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_ImGui_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (event && *event && g_ImGui_Initialized) ImGui_ImplAndroid_HandleInputEvent(*event);
    return result;
}

// ===================== 输入事件Hook挂载 =====================
static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);

    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_12InputEventEblPjPSA_", nullptr);
    if (sym2) GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== SO注入入口（已删除3秒延迟） =====================
__attribute__((constructor))
void MCbackup_Init() {
    std::thread([=]() {
        // 已彻底删除sleep(3)延迟，SO加载后直接初始化
        GlossInit(true);
        GHandle hEGL = GlossOpen("libEGL.so");
        if (!hEGL) return;
        void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
        if (!swap) return;
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        HookInput();
    }).detach();
}

// ===================== 标准JNI入口（安卓SO加载规范） =====================
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    return JNI_VERSION_1_6;
}
