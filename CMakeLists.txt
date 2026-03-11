cmake_minimum_required(VERSION 3.18)
project(MCbackup LANGUAGES C CXX)

# ===================== 【必加】安卓NDK交叉编译核心适配 =====================
# 解决安卓SO编译的架构、路径、权限兼容问题，避免编译后产物异常
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_ANDROID_API_MIN 24)
set(CMAKE_ANDROID_API 24)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # 安卓SO必须开启位置无关代码
set(CMAKE_BUILD_TYPE Release CACHE INTERNAL "强制Release编译，确保优化生效")

# ===================== C++标准配置 =====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===================== 编译优化与链接配置 =====================
# 新增-fPIC确保安卓SO兼容，保留原有优化配置
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

# ===================== 屏蔽CMake弃用警告（兼容高低版本CMake） =====================
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

# ===================== 1. fmt库（固定官方稳定版10.2.1，和你提供的仓库一致） =====================
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(fmt)

# ===================== 2. preloader-android（固定官方稳定版0.1.14） =====================
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.1.14
    GIT_SHALLOW ON
)
FetchContent_GetProperties(preloader_android)
if(NOT preloader_android_POPULATED)
    FetchContent_Populate(preloader_android)
    
    # 【修复】增加文件存在性判断，避免文件不存在导致的编译中断
    set(LOGGER_FILE "${preloader_android_SOURCE_DIR}/src/pl/Logger.h")
    if(EXISTS ${LOGGER_FILE})
        file(READ "${LOGGER_FILE}" LOGGER_CONTENT)
        string(REPLACE "#include <format>" "#include <fmt/format.h>" LOGGER_CONTENT "${LOGGER_CONTENT}")
        string(REPLACE "std::vformat" "fmt::vformat" LOGGER_CONTENT "${LOGGER_CONTENT}")
        string(REPLACE "std::make_format_args" "fmt::make_format_args" LOGGER_CONTENT "${LOGGER_CONTENT}")
        file(WRITE "${LOGGER_FILE}" "${LOGGER_CONTENT}")
    endif()
    
    # 【修复】添加EXCLUDE_FROM_ALL，避免编译不必要的测试目标
    add_subdirectory(${preloader_android_SOURCE_DIR} ${preloader_android_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
target_link_libraries(preloader PUBLIC fmt::fmt)

# ===================== 3. libzip库（固定官方稳定版v1.10.1，彻底解决链接问题） =====================
FetchContent_Declare(
    libzip
    GIT_REPOSITORY https://github.com/nih-at/libzip.git
    GIT_TAG v1.10.1
    GIT_SHALLOW ON
)

# 极致精简配置：只保留核心zip打包功能，关闭所有无关项，加快编译+减小体积
set(BUILD_TOOLS OFF CACHE INTERNAL "")
set(BUILD_REGRESS OFF CACHE INTERNAL "")
set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(BUILD_DOC OFF CACHE INTERNAL "")
# 关闭所有额外压缩格式，只保留zlib
set(ENABLE_BZIP2 OFF CACHE INTERNAL "")
set(ENABLE_LZMA OFF CACHE INTERNAL "")
set(ENABLE_ZSTD OFF CACHE INTERNAL "")
# 彻底关闭加密功能，消除警告+无需额外加密库
set(ENABLE_COMMONCRYPTO OFF CACHE INTERNAL "")
set(ENABLE_GNUTLS OFF CACHE INTERNAL "")
set(ENABLE_MBEDTLS OFF CACHE INTERNAL "")
set(ENABLE_OPENSSL OFF CACHE INTERNAL "")
set(ENABLE_WINDOWS_CRYPTO OFF CACHE INTERNAL "")
# 强制静态编译，直接打包到最终SO，无需额外依赖
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
# 安卓交叉编译适配
set(DISABLE_FSEEKO OFF CACHE INTERNAL "")
set(LIBZIP_DO_INSTALL OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(libzip)

# ===================== 【核心修复】统一头文件目录（主目标+preloader都添加） =====================
# 之前的版本只给preloader加了头文件，主目标MCbackup没加，导致编译找不到zip.h/ImGui头文件
set(PROJECT_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
    ${preloader_android_SOURCE_DIR}/src
    # libzip头文件（源码+生成的配置头文件）
    ${libzip_SOURCE_DIR}/lib
    ${libzip_BINARY_DIR}
)
include_directories(${PROJECT_INCLUDE_DIRS})

# ===================== 源文件配置 =====================
set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

# ===================== 生成最终SO库 =====================
add_library(MCbackup SHARED ${SOURCES})

# ===================== 【核心修复】链接目标修正 =====================
# 静态编译libzip时，正确的目标名是zip-static，不是zip！之前的版本这里错误导致链接失败
target_link_libraries(MCbackup
    preloader
    fmt::fmt
    zip-static # 修复：静态libzip的正确链接目标
    log
    android
    EGL
    GLESv3
    GLESv2
    z # NDK自带zlib，libzip核心依赖
)

# ===================== 【核心修复】强制固定产物输出路径（和你的Workflow100%匹配） =====================
# 编译后直接生成在 build/libMCbackup.so，和你Workflow里的上传路径完全一致，不会再出现找不到文件的警告
set_target_properties(MCbackup PROPERTIES
    # 强制输出到build根目录，和Workflow的path: build/libMCbackup.so匹配
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}
    # 强制固定文件名：libMCbackup.so，安卓标准命名规范
    OUTPUT_NAME "MCbackup"
    PREFIX "lib"
)

# ===================== 编译后打印产物路径，方便日志排查 =====================
add_custom_command(TARGET MCbackup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "============================================="
    COMMAND ${CMAKE_COMMAND} -E echo "✅ 编译完成！产物路径：${CMAKE_BINARY_DIR}/libMCbackup.so"
    COMMAND ${CMAKE_COMMAND} -E echo "📦 产物大小：$(du -h ${CMAKE_BINARY_DIR}/libMCbackup.so | awk '{print $1}')"
    COMMAND ${CMAKE_COMMAND} -E echo "============================================="
)
