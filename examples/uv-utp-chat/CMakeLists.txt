# uv-utp-chat：libuv + libutp + TBB 多客户端聊天示例
#
# 依赖：libuv（pkg-config 或 CMake config）、oneTBB（CMake config）。
# 任一缺失时给出警告并跳过，不影响主工程构建。

find_package(TBB CONFIG QUIET)

find_package(libuv CONFIG QUIET)
if(libuv_FOUND)
    if(TARGET libuv::uv_a)
        set(UVCHAT_LIBUV libuv::uv_a)
    else()
        set(UVCHAT_LIBUV libuv::uv)
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
        if(LIBUV_FOUND)
            set(UVCHAT_LIBUV PkgConfig::LIBUV)
        endif()
    endif()
endif()

if(NOT TBB_FOUND OR NOT DEFINED UVCHAT_LIBUV)
    message(WARNING "跳过 uv-utp-chat 示例：缺少 "
        "$<IF:$<BOOL:${TBB_FOUND}>,,TBB >$<IF:$<BOOL:${UVCHAT_LIBUV}>,,libuv>"
        "（Debian/Ubuntu: apt install libuv1-dev libtbb-dev）")
    return()
endif()

# 共用胶水层（UtpUvNode + TbbBridge + 协议分帧）
add_library(uvchat_common STATIC common/utp_uv.cpp)
target_include_directories(uvchat_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uvchat_common PUBLIC utp ${UVCHAT_LIBUV} TBB::tbb)
target_compile_features(uvchat_common PUBLIC cxx_std_23)
if(NOT WIN32)
    target_compile_definitions(uvchat_common PUBLIC POSIX)
endif()

add_executable(utp_chat_server server.cpp)
target_link_libraries(utp_chat_server PRIVATE uvchat_common)

add_executable(utp_chat_client client.cpp)
target_link_libraries(utp_chat_client PRIVATE uvchat_common)

# 自检模式注册为 ctest 用例：同进程起 1 server + 3 client 互发消息并验证
if(BUILD_TESTING)
    add_test(NAME uv_utp_chat_selftest COMMAND utp_chat_server --self-test)
    set_tests_properties(uv_utp_chat_selftest PROPERTIES TIMEOUT 60)
endif()
