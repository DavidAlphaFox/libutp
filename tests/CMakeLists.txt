set(CATCH2_ROOT "/tmp/catch2_src" CACHE PATH "Path to Catch2 source")

add_subdirectory(${CATCH2_ROOT} catch2_binary)

add_executable(utp_tests
    test_endian.cpp
    test_wire_format.cpp
    test_config.cpp
    test_sequence_buffer.cpp
    test_delay_history.cpp
)

target_include_directories(utp_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(utp_tests PUBLIC cxx_std_23)

target_link_libraries(utp_tests PRIVATE Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(utp_tests)