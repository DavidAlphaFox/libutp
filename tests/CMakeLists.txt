# Catch2 测试框架路径配置
set(CATCH2_ROOT "/tmp/catch2_src" CACHE PATH "Catch2 源码路径")

# 添加 Catch2 子目录
add_subdirectory(${CATCH2_ROOT} catch2_binary)

# 定义测试可执行文件，包含所有测试源文件
add_executable(utp_tests
    test_endian.cpp          # 测试大端字节序转换
    test_wire_format.cpp     # 测试协议格式解析
    test_config.cpp          # 测试配置常量
    test_sequence_buffer.cpp # 测试序列号缓冲区
    test_delay_history.cpp   # 测试延迟历史记录
)

# 包含内部头文件目录
target_include_directories(utp_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# 要求 C++23 特性
target_compile_features(utp_tests PUBLIC cxx_std_23)

# 链接 Catch2 主库
target_link_libraries(utp_tests PRIVATE Catch2::Catch2WithMain)

# 启用 CTest 和自动发现测试
include(CTest)
include(Catch)
catch_discover_tests(utp_tests)