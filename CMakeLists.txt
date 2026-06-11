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
option(UTP_BUILD_FUZZERS    "构建 libFuzzer 模糊测试目标（需 Clang）" OFF)
option(UTP_BUILD_UV_EXAMPLE "构建 uv-utp-chat 示例（需 libuv + TBB）" OFF)

# ── Sanitizers ────────────────────────────────────────────
if(UTP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# fuzzer 覆盖率插桩作用于全部目标（库本体需要插桩反馈），
# 但只链接 fuzz 目标自身带 -fsanitize=fuzzer（含 main）
if(UTP_BUILD_FUZZERS)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# ── 测试开关（须在 examples 之前启用：uv-utp-chat 注册自检用例）──
option(BUILD_TESTING "构建测试" ON)
if(BUILD_TESTING)
    enable_testing()
endif()

# ── 库 ───────────────────────────────────────────────────
add_subdirectory(src)

# ── 示例 ──────────────────────────────────────────────────
if(UTP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 测试 ───────────────────────────────────────────────────
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ── 模糊测试 ────────────────────────────────────────────────
if(UTP_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# ── 基准测试 ────────────────────────────────────────────────
option(UTP_BUILD_BENCH "构建吞吐基准（bench_loopback）" OFF)
if(UTP_BUILD_BENCH)
    add_subdirectory(bench)
endif()
