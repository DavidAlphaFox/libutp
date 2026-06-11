# uTP (Micro Transport Protocol) 库根构建配置
# libutp: 在 UDP 之上实现的 TCP 协议，用于 P2P 传输
# 使用 CMake 3.25+，支持 C 和 C++（C23 和 C++23 标准）

cmake_minimum_required(VERSION 3.25)

project(libutp
    VERSION     1.0.0
    LANGUAGES   C CXX
    DESCRIPTION "uTP (Micro Transport Protocol) library"
)

# ── C++ 标准 ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── 构建选项 ───────────────────────────────────────────────────
option(UTP_BUILD_SHARED     "构建动态库而非静态库" OFF)
option(UTP_BUILD_EXAMPLES   "构建示例程序（ucat）"         ON)
option(UTP_DEBUG_LOGGING    "启用详细调试日志"          OFF)
option(UTP_ENABLE_STATS     "启用 utp_get_stats() 和额外检查" OFF)
option(UTP_SANITIZE         "启用 AddressSanitizer + UBSan（作用于全部目标）" OFF)

# ── Sanitizers ────────────────────────────────────────────
if(UTP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── 库 ───────────────────────────────────────────────────
add_subdirectory(src)

# ── 示例 ──────────────────────────────────────────────────
if(UTP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 测试 ───────────────────────────────────────────────────
option(BUILD_TESTING "构建测试" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
