# 吞吐基准（内存回环，无真实网络）。仅在 UTP_BUILD_BENCH=ON 时构建。
add_executable(bench_loopback bench_loopback.cpp)
target_link_libraries(bench_loopback PRIVATE utp)
target_compile_definitions(bench_loopback PRIVATE POSIX)
target_compile_features(bench_loopback PRIVATE cxx_std_23)
