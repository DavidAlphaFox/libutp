# libFuzzer 模糊测试目标（需 Clang：-fsanitize=fuzzer）
# 用法：
#   cmake -S . -B build/fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DUTP_BUILD_FUZZERS=ON -DUTP_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
#   ./build/fuzz/fuzz/fuzz_process_udp -max_total_time=60 fuzz/corpus
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "UTP_BUILD_FUZZERS 需要 Clang（libFuzzer）；当前编译器：${CMAKE_CXX_COMPILER_ID}")
endif()

add_executable(fuzz_process_udp fuzz_process_udp.cpp)
target_link_libraries(fuzz_process_udp PRIVATE utp)
target_compile_options(fuzz_process_udp PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_process_udp PRIVATE -fsanitize=fuzzer)
target_compile_definitions(fuzz_process_udp PRIVATE POSIX)

# 被链接的 utp 库本身需要插桩才能给 fuzzer 提供覆盖率反馈，
# 由顶层在 UTP_BUILD_FUZZERS=ON 时统一加 -fsanitize=fuzzer-no-link。
