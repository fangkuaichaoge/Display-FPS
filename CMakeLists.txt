cmake_minimum_required(VERSION 3.18)
# 核心修改1：项目名从DisplayFPS改为MCbackup
project(MCbackup LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译优化与链接配置（保留原有配置，适配安卓NDK）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

# 屏蔽FetchContent弃用警告（适配CMake 3.31+，解决你之前日志里的警告）
cmake_policy(SET CMP0169 OLD)

include(FetchContent)

# 1. fmt库配置（固定官方稳定版10.2.1，和你提供的仓库版本一致）
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(fmt)

# 2. preloader-android配置（固定官方最新稳定版0.1.14，替换main分支保证编译稳定）
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.1.14
    GIT_SHALLOW ON
)
FetchContent_GetProperties(preloader_android)
if(NOT preloader_android_POPULATED)
    FetchContent_Populate(preloader_android)
    
    # 修复preloader的format依赖：把std::format替换为fmt库，和原有逻辑一致
    file(READ "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" LOGGER_CONTENT)
    string(REPLACE "#include <format>" "#include <fmt/format.h>" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::vformat" "fmt::vformat" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::make_format_args" "fmt::make_format_args" LOGGER_CONTENT "${LOGGER_CONTENT}")
    file(WRITE "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" "${LOGGER_CONTENT}")
    
    add_subdirectory(${preloader_android_SOURCE_DIR} ${preloader_android_BINARY_DIR})
endif()

# 链接fmt到preloader
target_link_libraries(preloader PUBLIC fmt::fmt)

# 头文件目录配置（保留原有ImGui路径，适配项目结构）
target_include_directories(preloader PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
)

# 源文件配置
set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

# 核心修改2：生成的so库名改为libMCbackup.so
add_library(MCbackup SHARED ${SOURCES})

# 核心修改3：链接配置，适配MC备份功能（新增zlib/zip依赖，适配压缩功能）
target_link_libraries(MCbackup
    preloader
    fmt::fmt
    log
    android
    EGL
    GLESv3
    GLESv2
    z       # 压缩功能依赖的zlib，NDK自带
    zip     # 备份功能的zip压缩依赖，NDK自带
)

# 可选：强制输出so库路径，方便你提取编译产物
set_target_properties(MCbackup PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
