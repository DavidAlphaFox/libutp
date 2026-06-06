cmake_minimum_required(VERSION 3.25)

project(libutp
    VERSION     1.0.0
    LANGUAGES   C CXX
    DESCRIPTION "uTP (Micro Transport Protocol) library"
)

# ── C++ standard ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────
option(UTP_BUILD_SHARED     "Build shared library instead of static" OFF)
option(UTP_BUILD_EXAMPLES   "Build example programs (ucat)"         ON)
option(UTP_DEBUG_LOGGING    "Enable verbose debug logging"          OFF)
option(UTP_ENABLE_STATS     "Enable utp_get_stats() and extra checks" OFF)

# ── Library ───────────────────────────────────────────────────
add_subdirectory(src)

# ── Examples ──────────────────────────────────────────────────
if(UTP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
