#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include "utp/endian.hpp"

TEST_CASE("big_endian roundtrip", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0x12345678;
    REQUIRE(static_cast<uint32_t>(be) == 0x12345678);
}

TEST_CASE("big_endian uint16 roundtrip", "[endian]") {
    utp::big_endian<uint16_t> be;
    be = 0xABCD;
    REQUIRE(static_cast<uint16_t>(be) == 0xABCD);
}

TEST_CASE("big_endian int32 roundtrip", "[endian]") {
    utp::int32_big be;
    be = -42;
    REQUIRE(static_cast<int32_t>(be) == -42);
}

TEST_CASE("big_endian uint32 max value", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0xFFFFFFFF;
    REQUIRE(static_cast<uint32_t>(be) == 0xFFFFFFFF);
}

TEST_CASE("big_endian uint32 roundtrip consistency", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0x12345678;
    uint32_t val = be;
    utp::big_endian<uint32_t> be2;
    be2 = val;
    REQUIRE(static_cast<uint32_t>(be2) == 0x12345678);
}

TEST_CASE("big_endian uint16 zero", "[endian]") {
    utp::big_endian<uint16_t> be;
    be = 0;
    REQUIRE(static_cast<uint16_t>(be) == 0);
}

TEST_CASE("big_endian int32 negative max", "[endian]") {
    utp::int32_big be;
    be = -1;
    REQUIRE(static_cast<int32_t>(be) == -1);
}