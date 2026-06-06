// 字节序转换测试
// 验证大端字节序（big_endian）模板类的正确性
// 确保在网络传输中正确处理多字节数据的字节序

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include "utp/endian.hpp"

TEST_CASE("big_endian 32位往返转换", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0x12345678;
    REQUIRE(static_cast<uint32_t>(be) == 0x12345678);
}

TEST_CASE("big_endian 16位往返转换", "[endian]") {
    utp::big_endian<uint16_t> be;
    be = 0xABCD;
    REQUIRE(static_cast<uint16_t>(be) == 0xABCD);
}

TEST_CASE("big_endian 32位有符号数往返转换", "[endian]") {
    utp::int32_big be;
    be = -42;
    REQUIRE(static_cast<int32_t>(be) == -42);
}

TEST_CASE("big_endian 32位最大值", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0xFFFFFFFF;
    REQUIRE(static_cast<uint32_t>(be) == 0xFFFFFFFF);
}

TEST_CASE("big_endian 32位往返转换一致性", "[endian]") {
    utp::big_endian<uint32_t> be;
    be = 0x12345678;
    uint32_t val = be;
    utp::big_endian<uint32_t> be2;
    be2 = val;
    REQUIRE(static_cast<uint32_t>(be2) == 0x12345678);
}

TEST_CASE("big_endian 16位零值", "[endian]") {
    utp::big_endian<uint16_t> be;
    be = 0;
    REQUIRE(static_cast<uint16_t>(be) == 0);
}

TEST_CASE("big_endian 32位负数最大值", "[endian]") {
    utp::int32_big be;
    be = -1;
    REQUIRE(static_cast<int32_t>(be) == -1);
}