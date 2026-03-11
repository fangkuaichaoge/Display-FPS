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
#define TMP_ZIP         "/data/local/tmp/backup_tmp.zip" // 临时文件改到系统安全目录，避免权限闪退
#define MAX_LOG_COUNT   30
#define READ_CHUNK_SIZE 4096 // 分块读写，避免大文件OOM闪退

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
static ImFont* g_ChineseFont = nullptr; // 中文字体全局句柄

// ===================== Hook函数指针声明 =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== 日志功能（线程安全+异常保护） =====================
static std::string get_log_path() {
    return std::string(ROOT_DIR) + "/backup_log.txt";
}

static void log(const std::string& msg) {
    // 先写内存日志，避免文件操作阻塞UI
    std::string formatted_msg;
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char time_buf[32] = {0};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now_time));
        formatted_msg = "[" + std::string(time_buf) + "] " + msg;
    }

    // 线程安全更新UI日志
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.recent_logs.push_back(formatted_msg);
        if (g_backup_state.recent_logs.size() > MAX_LOG_COUNT) {
            g_backup_state.recent_logs.erase(g_backup_state.recent_logs.begin());
        }
    }

    // 异步写本地文件，异常捕获避免闪退
    try {
        std::filesystem::create_directories(ROOT_DIR);
        FILE* f = fopen(get_log_path().c_str(), "a+");
        if (f) {
            fprintf(f, "%s\n", formatted_msg.c_str());
            fclose(f);
        }
    } catch (...) {
        // 忽略文件写入异常，绝不闪退
    }
}

// ===================== 核心压缩功能（流式读写+全异常捕获，彻底解决闪退） =====================
static bool compress_folder(const std::string& src_path, const std::string& dest_path) {
    // 先清理临时文件
    remove(TMP_ZIP);

    int error = 0;
    zip_t* zip = zip_open(TMP_ZIP, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip) {
        log("创建临时压缩文件失败，错误码：" + std::to_string(error));
        return false;
    }

    bool all_ok = true;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            std::string full_path = entry.path().string();
            std::string relative_path = full_path.substr(src_path.length() + 1);

            // 跳过空路径和系统隐藏文件
            if (relative_path.empty() || relative_path[0] == '.') continue;

            if (entry.is_directory()) {
                zip_dir_add(zip, relative_path.c_str(), 0);
            } else if (entry.is_regular_file()) {
                FILE* f = fopen(full_path.c_str(), "rb");
                if (!f) {
                    log("跳过无法读取的文件：" + relative_path);
                    continue;
                }

                // 获取文件大小
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);

                // 流式分块读取，避免大文件一次性加载OOM闪退
                zip_source_t* source = zip_source_filep(zip, f, 0, file_size);
                if (!source) {
                    log("创建zip源失败：" + relative_path);
                    fclose(f);
                    all_ok = false;
                    continue;
                }

                if (zip_file_add(zip, relative_path.c_str(), source, ZIP_FL_OVERWRITE) < 0) {
                    zip_source_free(source);
                    fclose(f);
                    log("添加文件到zip失败：" + relative_path);
                    all_ok = false;
                    continue;
                }
                // 注意：zip_source_filep会自动关闭文件，无需手动fclose
            }
        }
    } catch (const std::exception& e) {
        log("遍历文件夹失败：" + std::string(e.what()));
        all_ok = false;
    } catch (...) {
        log("遍历文件夹发生未知错误");
        all_ok = false;
    }

    // 关闭zip文件，异常处理
    if (zip_close(zip) < 0) {
        log("压缩文件关闭失败");
        zip_discard(zip);
        all_ok = false;
    }

    if (!all_ok) {
        remove(TMP_ZIP);
        return false;
    }

    // 复制到最终目标路径，流式读写
    FILE* tmp_f = fopen(TMP_ZIP, "rb");
    FILE* dest_f = fopen(dest_path.c_str(), "wb");
    if (!tmp_f || !dest_f) {
        log("复制最终备份文件失败");
        if (tmp_f) fclose(tmp_f);
        if (dest_f) fclose(dest_f);
        remove(TMP_ZIP);
        return false;
    }

    uint8_t buf[READ_CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), tmp_f)) > 0) {
        fwrite(buf, 1, n, dest_f);
    }

    fclose(tmp_f);
    fclose(dest_f);
    remove(TMP_ZIP);

    return true;
}

// ===================== 核心备份全流程（全异常捕获+权限预检查，彻底解决闪退） =====================
static void do_backup() {
    // 线程安全更新状态，锁粒度最小化
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = true;
        g_backup_state.current_status = "正在初始化备份...";
        g_backup_state.last_result = BackupState::NONE;
        g_backup_state.recent_logs.clear();
    }

    log("=== 开始执行全量备份 ===");

    // 【关键修复】权限&路径预检查，避免非法访问闪退
    try {
        // 检查游戏目录是否存在
        if (!std::filesystem::exists(BASE_DIR)) {
            log("❌ 游戏目录不存在，请确认启动器路径正确");
            throw std::runtime_error("游戏目录不存在");
        }

        // 创建备份目录
        std::filesystem::create_directories(ROOT_DIR);
        // 测试目录是否可写
        FILE* test_f = fopen((std::string(ROOT_DIR) + "/test.tmp").c_str(), "wb");
        if (!test_f) {
            log("❌ 备份目录无写入权限，请给游戏开启【文件/存储】权限");
            throw std::runtime_error("无存储权限");
        }
        fclose(test_f);
        remove((std::string(ROOT_DIR) + "/test.tmp").c_str());
        log("✅ 存储权限检查通过，备份目录就绪");

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "备份初始化失败";
        g_backup_state.last_result = BackupState::FAILED;
        return;
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "备份初始化发生未知错误";
        g_backup_state.last_result = BackupState::FAILED;
        return;
    }

    // 1. 打包资源/行为/皮肤包为mcaddon
    std::string addon_path = std::string(ROOT_DIR) + "/MC备份_资源包+行为包+皮肤包.mcaddon";
    remove(addon_path.c_str());

    int error = 0;
    zip_t* main_zip = zip_open(addon_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!main_zip) {
        log("创建mcaddon文件失败，错误码：" + std::to_string(error));
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_backup_state.is_backing_up = false;
        g_backup_state.current_status = "备份失败，无法创建备份文件";
        g_backup_state.last_result = BackupState::FAILED;
        return;
    }

    const char* pack_dirs[] = { DIRS_RESOURCE, DIRS_BEHAVIOR, DIRS_SKIN };
    for (const char* dir : pack_dirs) {
        std::string full_dir = std::string(BASE_DIR) + dir;
        if (!std::filesystem::exists(full_dir)) {
            log("跳过不存在的目录：" + std::string(dir));
            continue;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(full_dir)) {
                if (!entry.is_directory()) continue;

                std::string pack_path = entry.path().string();
                std::string pack_name = entry.path().filename().string();
                std::string tmp_pack_name = pack_name + ".mcpack";

                log("正在打包：" + tmp_pack_name);
                {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    g_backup_state.current_status = "正在打包：" + pack_name;
                }

                if (!compress_folder(pack_path, TMP_ZIP)) {
                    log("打包失败，跳过：" + pack_name);
                    continue;
                }

                // 读取临时mcpack添加到主文件
                FILE* tmp_f = fopen(TMP_ZIP, "rb");
                if (!tmp_f) continue;

                fseek(tmp_f, 0, SEEK_END);
                long file_size = ftell(tmp_f);
                fseek(tmp_f, 0, SEEK_SET);

                zip_source_t* source = zip_source_filep(main_zip, tmp_f, 0, file_size);
                if (source) {
                    zip_file_add(main_zip, tmp_pack_name.c_str(), source, ZIP_FL_OVERWRITE);
                }
                remove(TMP_ZIP);
            }
        } catch (...) {
            log("扫描目录失败，跳过：" + std::string(dir));
            continue;
        }
    }

    if (zip_close(main_zip) < 0) {
        log("mcaddon文件打包失败");
        zip_discard(main_zip);
    } else {
        log("✅ 资源包/行为包/皮肤包打包完成");
    }

    // 2. 导出地图为mcworld
    std::string world_root = std::string(BASE_DIR) + DIRS_WORLD;
    if (std::filesystem::exists(world_root)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(world_root)) {
                if (!entry.is_directory()) continue;

                std::string world_path = entry.path().string();
                std::string world_name = entry.path().filename().string();
                std::string dest_path = std::string(ROOT_DIR) + "/" + world_name + ".mcworld";

                log("正在导出地图：" + world_name);
                {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    g_backup_state.current_status = "正在导出地图：" + world_name;
                }

                if (compress_folder(world_path, dest_path)) {
                    log("✅ 地图导出成功：" + world_name);
                } else {
                    log("❌ 地图导出失败：" + world_name);
                }
            }
        } catch (...) {
            log("扫描地图目录失败");
        }
    } else {
        log("跳过地图备份，地图目录不存在");
    }

    // 备份完成
    log("=== 全量备份全部完成 ===");
    log("📁 备份文件已保存到：" + std::string(ROOT_DIR));
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_backup_state.is_backing_up = false;
    g_backup_state.current_status = "备份全部完成";
    g_backup_state.last_result = BackupState::SUCCESS;
}

// ===================== 【定制黄绿主题】ImGui UI绘制 =====================
static void DrawUI() {
    // 强制使用中文字体
    if (g_ChineseFont) ImGui::PushFont(g_ChineseFont);

    ImGuiIO& io = ImGui::GetIO();
    // 主题配色定义（专属黄绿MC风格）
    const ImVec4 bgColor = ImVec4(0.94f, 0.98f, 0.86f, 0.92f); // 柔和黄绿色窗口背景
    const ImVec4 titleColor = ImVec4(0.18f, 0.42f, 0.08f, 1.0f); // 深绿标题色
    const ImVec4 successColor = ImVec4(0.15f, 0.7f, 0.15f, 1.0f); // 成功绿色
    const ImVec4 failedColor = ImVec4(0.88f, 0.18f, 0.18f, 1.0f); // 失败红色
    const ImVec4 warningColor = ImVec4(0.8f, 0.6f, 0.08f, 1.0f); // 警告黄色
    const ImVec4 btnNormal = ImVec4(0.68f, 0.88f, 0.32f, 0.95f); // 按钮正常黄绿
    const ImVec4 btnHover = ImVec4(0.78f, 0.93f, 0.42f, 1.0f); // 按钮hover高亮
    const ImVec4 btnActive = ImVec4(0.58f, 0.78f, 0.22f, 1.0f); // 按钮按下深绿
    const ImVec4 frameBg = ImVec4(0.88f, 0.96f, 0.76f, 0.9f); // 输入框/日志背景

    // 窗口基础设置：触屏优化，可拖动，不超出屏幕
    const float pad = 20.0f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(380.0f, 0), 
        ImVec2(io.DisplaySize.x * 0.92f, io.DisplaySize.y * 0.85f)
    );
    ImGui::SetNextWindowBgAlpha(bgColor.w);

    // 窗口样式重写，定制黄绿主题
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = bgColor;
    style.Colors[ImGuiCol_Text] = titleColor;
    style.Colors[ImGuiCol_FrameBg] = frameBg;
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.83f, 0.93f, 0.68f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.78f, 0.90f, 0.63f, 1.0f);
    style.Colors[ImGuiCol_Button] = btnNormal;
    style.Colors[ImGuiCol_ButtonHovered] = btnHover;
    style.Colors[ImGuiCol_ButtonActive] = btnActive;
    style.Colors[ImGuiCol_Header] = btnNormal;
    style.Colors[ImGuiCol_HeaderHovered] = btnHover;
    style.Colors[ImGuiCol_HeaderActive] = btnActive;
    style.Colors[ImGuiCol_Separator] = ImVec4(titleColor.x, titleColor.y, titleColor.z, 0.6f);
    style.Colors[ImGuiCol_ScrollbarBg] = frameBg;
    style.Colors[ImGuiCol_ScrollbarGrab] = btnNormal;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = btnHover;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = btnActive;
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.4f, 0.5f, 0.3f, 1.0f);

    ImGui::Begin("MC一键备份工具", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    // 可拖动标题区域（大点击区域，触屏好操作）
    ImVec2 titleSize = ImGui::CalcTextSize("☘︎ MC世界一键备份工具");
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
    ImGui::TextColored(titleColor, "☘︎ MC世界一键备份工具");
    
    ImVec2 titleMin = ImGui::GetItemRectMin();
    ImVec2 titleMax = ImGui::GetItemRectMax();
    ImGui::SetCursorScreenPos(titleMin);
    ImGui::InvisibleButton("##drag_zone", ImVec2(titleMax.x - titleMin.x, titleMax.y - titleMin.y + 10));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 dragDelta = ImGui::GetMouseDragDelta(0);
        ImVec2 newPos = ImVec2(mousePos.x - dragDelta.x, mousePos.y - dragDelta.y);
        // 限制窗口不拖出屏幕
        newPos.x = std::max(0.0f, std::min(newPos.x, io.DisplaySize.x - ImGui::GetWindowWidth()));
        newPos.y = std::max(0.0f, std::min(newPos.y, io.DisplaySize.y - ImGui::GetWindowHeight()));
        ImGui::SetWindowPos("MC一键备份工具", newPos);
    }

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 12));

    // 线程安全读取状态
    BackupState local_state;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        local_state = g_backup_state;
    }

    // 【大按钮触屏优化】核心一键备份按钮
    if (local_state.is_backing_up) {
        ImGui::BeginDisabled();
        ImGui::Button("📦 正在备份中，请稍候...", ImVec2(-1, 60));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("✅ 一键执行全量备份", ImVec2(-1, 60))) {
            std::thread backup_thread(do_backup);
            backup_thread.detach();
        }
    }

    ImGui::Dummy(ImVec2(0, 10));

    // 状态显示
    ImGui::TextColored(titleColor, "当前状态：");
    ImGui::SameLine();
    if (local_state.is_backing_up) {
        ImGui::TextColored(warningColor, "%s", local_state.current_status.c_str());
    } else if (local_state.last_result == BackupState::SUCCESS) {
        ImGui::TextColored(successColor, "✅ 备份全部完成，文件已保存");
    } else if (local_state.last_result == BackupState::FAILED) {
        ImGui::TextColored(failedColor, "❌ 备份失败，请查看日志");
    } else {
        ImGui::Text("等待执行备份操作");
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 10));

    // 日志预览区域（触屏优化，滚动顺滑）
    ImGui::TextColored(titleColor, "备份实时日志：");
    ImGui::BeginChild("##log_area", ImVec2(-1, 260), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const auto& log_line : local_state.recent_logs) {
        ImGui::TextWrapped("%s", log_line.c_str());
    }
    // 自动滚动到最新日志
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 15.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::End();

    // 弹出字体
    if (g_ChineseFont) ImGui::PopFont();
}

// ===================== GL状态保护（完整修复，避免花屏/渲染异常） =====================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst, bSrcA, bDstA;
    GLboolean blend, cull, depth, scissor, stencil, dither;
    GLint frontFace, polygonMode[2], activeTexture;
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
    // 完整保存混合模式，避免透明异常
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    // 完整保存GL状态
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
    glGetIntegerv(GL_POLYGON_MODE, s.polygonMode);
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
    // 完整恢复混合模式
    glBlendFuncSeparate(s.bSrc, s.bDst, s.bSrcA, s.bDstA);
    // 完整恢复GL状态
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glFrontFace(s.frontFace);
    glPolygonMode(GL_FRONT_AND_BACK, s.polygonMode[0]);
}

// ===================== 【修复中文乱码】ImGui环境初始化 =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 屏幕自动缩放适配，触屏优化
    float scale = (float)g_Height / 720.0f;
    scale = std::clamp(scale, 1.6f, 4.0f); // 限制缩放范围，避免字体过大/过小

    // 【关键修复】加载安卓系统中文字体，解决中文???乱码
    const char* font_paths[] = {
        "/system/fonts/NotoSansSC-Regular.otf", // 安卓原生思源黑体（绝大多数机型）
        "/system/fonts/NotoSansCJK-Regular.ttc", // 老版本安卓
        "/system/fonts/DroidSansFallback.ttf", // 安卓4.x+兼容
        "/system/fonts/MiSans-Regular.ttf", // 小米机型
        "/system/fonts/HarmonyOS_Sans_SC_Regular.ttf" // 华为机型
    };

    ImFontConfig font_cfg;
    font_cfg.SizePixels = 36.0f * scale;
    font_cfg.OversampleH = font_cfg.OversampleV = 2; // 高清渲染，避免字体模糊
    font_cfg.PixelSnapH = true;
    font_cfg.GlyphOffset = ImVec2(0, 1.0f); // 字体垂直居中

    // 遍历字体路径，找到可用的中文字体
    for (const char* path : font_paths) {
        if (access(path, F_OK) == 0) {
            g_ChineseFont = io.Fonts->AddFontFromFileTTF(path, font_cfg.SizePixels, &font_cfg, io.Fonts->GetGlyphRangesChineseFull());
            if (g_ChineseFont) break;
        }
    }

    // 兜底：找不到中文字体用默认字体，至少英文能正常显示
    if (!g_ChineseFont) {
        g_ChineseFont = io.Fonts->AddFontDefault(&font_cfg);
    }

    // 初始化ImGui后端
    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // 全局样式优化，触屏适配
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 16.0f;
    style.FrameRounding = 10.0f;
    style.GrabRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 8.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 10);
    style.ItemSpacing = ImVec2(10, 12);
    style.ItemInnerSpacing = ImVec2(8, 8);
    style.TouchExtraPadding = ImVec2(8, 8); // 触屏点击额外padding，更容易点中
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ScaleAllSizes(scale);

    g_Initialized = true;
}

// ===================== ImGui渲染核心流程 =====================
static void Render() {
    if (!g_Initialized) return;
    GLState s;
    SaveGL(s);

    // 强制设置ImGui显示尺寸
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // 帧循环
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();

    DrawUI(); // 绘制定制UI

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ===================== EGL渲染Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);

    // 获取屏幕宽高，过滤无效表面
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);

    // 只初始化游戏的主渲染窗口
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }

    // 只在目标游戏窗口渲染
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);

    // 更新宽高，初始化ImGui，执行渲染
    g_Width = w;
    g_Height = h;
    Setup();
    Render();

    return orig_eglSwapBuffers(dpy, surf);
}

// ===================== 触摸事件Hook（双符号兜底，确保交互正常） =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (sym2) {
        GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
    }
}

// ===================== 主线程初始化（保留3秒延迟，确保游戏环境就绪） =====================
static void* MainThread(void*) {
    sleep(3); // 保留3秒延迟，等待游戏完全加载，避免Hook失败
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    HookInput();
    return nullptr;
}

// ===================== SO注入入口 =====================
__attribute__((constructor))
void MCbackup_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
