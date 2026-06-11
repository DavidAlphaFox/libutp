# Catch2 测试框架：优先使用系统安装，否则用 FetchContent 拉取固定版本
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# 定义测试可执行文件，包含所有测试源文件
add_executable(utp_tests
    test_endian.cpp          # 测试大端字节序转换
    test_wire_format.cpp     # 测试协议格式解析
    test_config.cpp          # 测试配置常量
    test_sequence_buffer.cpp # 测试序列号缓冲区
    test_delay_history.cpp   # 测试延迟历史记录
    test_ledbat.cpp          # 测试 LEDBAT 拥塞控制器
    test_mtu_discovery.cpp   # 测试 MTU 二分探测
    test_loopback.cpp        # 内存回环集成测试（双 context + 故障注入）
)

# 包含内部头文件目录（utp 库以 PUBLIC 传递 include/）
target_include_directories(utp_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# 要求 C++23 特性
target_compile_features(utp_tests PUBLIC cxx_std_23)

# 与库一致的平台宏（utp.h/utp_types.h 需要）
if(NOT WIN32)
    target_compile_definitions(utp_tests PRIVATE POSIX)
endif()

# 链接被测库与 Catch2 主库
target_link_libraries(utp_tests PRIVATE utp Catch2::Catch2WithMain)

# 启用 CTest 和自动发现测试
include(CTest)
include(Catch)
catch_discover_tests(utp_tests)
