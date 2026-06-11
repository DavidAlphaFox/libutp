# ── 源文件列表 ───────────────────────────────────────────────
set(UTP_SOURCES
    utp_api.cpp
    utp_callbacks.cpp
    utp_callbacks_defaults.cpp
    utp_process.cpp
    utp_socket.cpp
    utp/address.cpp
    utp/ledbat.cpp
)

set(UTP_HEADERS_PUBLIC
    ${CMAKE_SOURCE_DIR}/include/utp.h
)

set(UTP_HEADERS_INTERNAL
    utp/address.hpp
    utp_callbacks.h
    utp_callbacks.hpp
    utp/ledbat.hpp
    utp/congestion_control.hpp
    utp/mtu_discovery.hpp
    utp/logger.hpp
    utp/socket_host.hpp
    utp/connection_state.hpp
    utp_internal.h
    utp_socket.hpp
    utp_types.h
)

# Windows 特定源文件（仅在 WIN32 平台编译）
if(WIN32)
    list(APPEND UTP_SOURCES libutp_inet_ntop.cpp)
    list(APPEND UTP_HEADERS_INTERNAL libutp_inet_ntop.h)
endif()

# ── 库目标 ────────────────────────────────────────────
add_library(utp ${UTP_SOURCES})

set_target_properties(utp PROPERTIES
    OUTPUT_NAME         utp
    VERSION             ${PROJECT_VERSION}
    SOVERSION           ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

# 动态库 vs 静态库
if(UTP_BUILD_SHARED)
    set_target_properties(utp PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ── 包含目录 ───────────────────────────────────────
# PUBLIC:  include/   → 使用者使用 #include "utp.h"
# PRIVATE: src/       → 内部头文件如 "utp_types.h"
target_include_directories(utp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── 编译定义 ───────────────────────────────────────
target_compile_definitions(utp
    PRIVATE
        $<$<NOT:$<PLATFORM_ID:Windows>>:POSIX>
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<BOOL:${UTP_DEBUG_LOGGING}>:UTP_DEBUG_LOGGING>
)

# ── 链接库 ────────────────────────────────────────────
if(NOT WIN32)
    include(CheckLibraryExists)
    check_library_exists(rt clock_gettime "" HAVE_RT)
    if(HAVE_RT)
        target_link_libraries(utp PRIVATE rt)
    endif()
endif()

# ── 警告 ──────────────────────────────────────────────────
include(${CMAKE_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
utp_set_warnings(utp)

# ── 编译器特定选项 ─────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp PRIVATE -fno-exceptions)
endif()
