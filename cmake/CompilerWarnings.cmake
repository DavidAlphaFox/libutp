# Compiler warning flags for libutp

function(utp_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /wd4200  # zero-length array in struct (used by utp_hash_t)
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-sign-compare
        )
    endif()
endfunction()
