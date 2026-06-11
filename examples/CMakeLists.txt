# ── ucat example ──────────────────────────────────────────────

# ucat is a C program that links against libutp
add_executable(ucat ucat.c)
target_link_libraries(ucat PRIVATE utp)

# ucat uses POSIX-specific headers
if(NOT WIN32)
    target_compile_definitions(ucat PRIVATE POSIX)
endif()

# On Linux, ucat uses ICMP error queue
if(LINUX)
    target_compile_definitions(ucat PRIVATE __linux__)
endif()

# ── uv-utp-chat example（libuv + TBB，需 UTP_BUILD_UV_EXAMPLE=ON）─
if(UTP_BUILD_UV_EXAMPLE)
    add_subdirectory(uv-utp-chat)
endif()
