cmake_minimum_required(VERSION 3.16)
project(ClipLink LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# ClipLink Windows MVP
# 工具链要求：MinGW-w64 / g++（C++17）
# 产物：单个 ClipLink.exe（静态链接运行时）
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "ClipLink 当前仅支持 MinGW-w64 g++ 工具链，"
        "当前编译器: ${CMAKE_CXX_COMPILER_ID}")
endif()

# 静态运行时（libgcc / libstdc++ / winpthread），保证单 EXE 无外部运行时 DLL 依赖
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

add_compile_options(-Wall -Wextra)

# Win32 宽字符：MAKEINTRESOURCE/IDC_ARROW 等宏随 UNICODE 走 W 系语义
add_compile_definitions(UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# sqlite3 静态编译（amalgamation，third_party/sqlite）
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_TEMP_STORE=2
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(MINGW)
    target_link_libraries(sqlite3 PUBLIC m)
endif()
# 第三方代码不参与 -Wall -Wextra 项目级告警门禁（最终产物 0 error 0 warning）
target_compile_options(sqlite3 PRIVATE -w)

# ---------------------------------------------------------------------------
# 主程序：收集 src/ 下全部 .cpp（后续模块新增文件自动纳入构建）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CLIPLINK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(ClipLink WIN32 ${CLIPLINK_SOURCES})

target_include_directories(ClipLink PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
    # resource.rc 内 `1 ICON "icon.ico"` 的资源搜索路径（windres -I）
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(ClipLink PRIVATE
    sqlite3
    ws2_32
    comctl32
    shell32
    user32
    gdi32
    ole32
    advapi32
    uuid
    pthread
)

# 资源文件（图标 / 版本信息），存在时自动参与编译
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.rc")
    target_sources(ClipLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.rc")
endif()

set_target_properties(ClipLink PROPERTIES OUTPUT_NAME "ClipLink")
