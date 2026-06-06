# ── Source files ───────────────────────────────────────────────
set(UTP_SOURCES
    utp_api.cpp
    utp_callbacks.cpp
    utp_internal.cpp
    utp_packedsockaddr.cpp
    utp_utils.cpp
)

set(UTP_HEADERS_PUBLIC
    ${CMAKE_SOURCE_DIR}/include/utp.h
)

set(UTP_HEADERS_INTERNAL
    utp_callbacks.h
    utp_internal.h
    utp_packedsockaddr.h
    utp_types.h
    utp_utils.h
)

# Windows-specific source (only compiled on WIN32)
if(WIN32)
    list(APPEND UTP_SOURCES libutp_inet_ntop.cpp)
    list(APPEND UTP_HEADERS_INTERNAL libutp_inet_ntop.h)
endif()

# ── Library target ────────────────────────────────────────────
add_library(utp ${UTP_SOURCES})

set_target_properties(utp PROPERTIES
    OUTPUT_NAME         utp
    VERSION             ${PROJECT_VERSION}
    SOVERSION           ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

# Shared vs static
if(UTP_BUILD_SHARED)
    set_target_properties(utp PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ── Include directories ───────────────────────────────────────
# PUBLIC:  include/   → consumers use #include "utp.h"
# PRIVATE: src/       → internal headers like "utp_types.h"
target_include_directories(utp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Compile definitions ───────────────────────────────────────
target_compile_definitions(utp
    PRIVATE
        $<$<NOT:$<PLATFORM_ID:Windows>>:POSIX>
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<BOOL:${UTP_DEBUG_LOGGING}>:UTP_DEBUG_LOGGING>
)

# ── Link libraries ────────────────────────────────────────────
if(NOT WIN32)
    include(CheckLibraryExists)
    check_library_exists(rt clock_gettime "" HAVE_RT)
    if(HAVE_RT)
        target_link_libraries(utp PRIVATE rt)
    endif()
endif()

# ── Warnings ──────────────────────────────────────────────────
include(${CMAKE_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
utp_set_warnings(utp)

# ── Compiler-specific options ─────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp PRIVATE -fno-exceptions -fno-rtti)
endif()
