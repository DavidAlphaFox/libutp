#include <catch2/catch_test_macros.hpp>
#include "utp/delay_history.hpp"

TEST_CASE("DelayHistory initial state", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    REQUIRE(dh.delay_base == 0);
    REQUIRE_FALSE(dh.delay_base_initialized);
    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory add_sample", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    dh.add_sample(100, 1000);
    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory multiple samples", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(100, 1000);
    dh.add_sample(100, 1060);
    dh.add_sample(100, 1120);

    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory get_value after samples", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(500, 1000);
    dh.add_sample(600, 2000);
    dh.add_sample(400, 3000);

    REQUIRE(dh.get_value() >= 0);
}

TEST_CASE("DelayHistory wrapping timestamp handling", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(0xFFFFFF00, 1000);
    dh.add_sample(0x00000100, 2000);

    REQUIRE(dh.delay_base_initialized == true);
}

TEST_CASE("DelayHistory shift updates delay base", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(500, 1000);
    std::uint32_t old_delay_base = dh.delay_base;

    dh.shift(100);

    REQUIRE(dh.delay_base == old_delay_base + 100);
}

TEST_CASE("DelayHistory clear resets state", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    dh.add_sample(500, 1000);

    dh.clear(2000);

    REQUIRE(dh.delay_base == 0);
    REQUIRE(dh.delay_base_initialized == false);
    REQUIRE(dh.get_value() == 0);
}