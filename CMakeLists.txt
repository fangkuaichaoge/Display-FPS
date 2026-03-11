cmake_minimum_required(VERSION 3.18)
project(MCbackup LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译优化与链接配置（适配安卓NDK）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

# 屏蔽CMake弃用警告，适配GitHub Actions高版本CMake
cmake_policy(SET CMP0169 OLD)
cmake_policy(SET CMP0135 NEW)

include(FetchContent)

# ===================== 1. fmt库（固定稳定版） =====================
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(fmt)

# ===================== 2. preloader-android（固定稳定版） =====================
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.1.14
    GIT_SHALLOW ON
)
FetchContent_GetProperties(preloader_android)
if(NOT preloader_android_POPULATED)
    FetchContent_Populate(preloader_android)
    
    # 修复preloader的format依赖，替换为fmt库
    file(READ "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" LOGGER_CONTENT)
    string(REPLACE "#include <format>" "#include <fmt/format.h>" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::vformat" "fmt::vformat" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::make_format_args" "fmt::make_format_args" LOGGER_CONTENT "${LOGGER_CONTENT}")
    file(WRITE "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" "${LOGGER_CONTENT}")
    
    add_subdirectory(${preloader_android_SOURCE_DIR} ${preloader_android_BINARY_DIR})
endif()
target_link_libraries(preloader PUBLIC fmt::fmt)

# ===================== 3. 修复版libzip（Git拉取，彻底解决哈希问题） =====================
FetchContent_Declare(
    libzip
    GIT_REPOSITORY https://github.com/nih-at/libzip.git
    GIT_TAG v1.10.1
    GIT_SHALLOW ON
)

# 核心配置：关闭所有不需要的功能，只保留基础zip打包，适配安卓交叉编译
set(BUILD_TOOLS OFF CACHE INTERNAL "")
set(BUILD_REGRESS OFF CACHE INTERNAL "")
set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(BUILD_DOC OFF CACHE INTERNAL "")
# 关闭所有额外压缩格式，只保留zlib
set(ENABLE_BZIP2 OFF CACHE INTERNAL "")
set(ENABLE_LZMA OFF CACHE INTERNAL "")
set(ENABLE_ZSTD OFF CACHE INTERNAL "")
# 彻底关闭加密功能，消除警告，不需要额外加密库
set(ENABLE_COMMONCRYPTO OFF CACHE INTERNAL "")
set(ENABLE_GNUTLS OFF CACHE INTERNAL "")
set(ENABLE_MBEDTLS OFF CACHE INTERNAL "")
set(ENABLE_OPENSSL OFF CACHE INTERNAL "")
set(ENABLE_WINDOWS_CRYPTO OFF CACHE INTERNAL "")
# 编译静态库，直接打包到最终so里
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
# 适配安卓交叉编译，禁用系统库检测
set(DISABLE_FSEEKO OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(libzip)

# ===================== 项目头文件与源文件配置 =====================
target_include_directories(preloader PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
    # 强制添加libzip头文件目录，确保能找到zip.h
    ${libzip_SOURCE_DIR}/lib
    ${libzip_BINARY_DIR}
)

set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

# 生成最终的so库
add_library(MCbackup SHARED ${SOURCES})

# 链接所有依赖库
target_link_libraries(MCbackup
    preloader
    fmt::fmt
    zip
    log
    android
    EGL
    GLESv3
    GLESv2
    z
)

# 强制输出编译产物到output目录，方便提取
set_target_properties(MCbackup PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
