#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "utp/wire_format.hpp"

TEST_CASE("PacketFormatV1 is 20 bytes", "[wire]") {
    REQUIRE(sizeof(utp::wire::PacketFormatV1) == 20);
}

TEST_CASE("PacketFormatAckV1 is 26 bytes", "[wire]") {
    REQUIRE(sizeof(utp::wire::PacketFormatAckV1) == 26);
}

TEST_CASE("PacketFormatV1 ver_type field", "[wire]") {
    utp::wire::PacketFormatV1 pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.set_type(utp::wire::ST_SYN);
    pkt.set_version(1);
    REQUIRE(pkt.type() == utp::wire::ST_SYN);
    REQUIRE(pkt.version() == 1);
}

TEST_CASE("PacketFormatV1 all packet types", "[wire]") {
    utp::wire::PacketFormatV1 pkt;
    std::memset(&pkt, 0, sizeof(pkt));

    pkt.set_type(utp::wire::ST_DATA);
    REQUIRE(pkt.type() == utp::wire::ST_DATA);

    pkt.set_type(utp::wire::ST_FIN);
    REQUIRE(pkt.type() == utp::wire::ST_FIN);

    pkt.set_type(utp::wire::ST_STATE);
    REQUIRE(pkt.type() == utp::wire::ST_STATE);

    pkt.set_type(utp::wire::ST_RESET);
    REQUIRE(pkt.type() == utp::wire::ST_RESET);

    pkt.set_type(utp::wire::ST_SYN);
    REQUIRE(pkt.type() == utp::wire::ST_SYN);
}

TEST_CASE("PacketFormatV1 field offsets", "[wire]") {
    utp::wire::PacketFormatV1 pkt;
    std::memset(&pkt, 0, sizeof(pkt));

    pkt.connid = 0x1234;
    pkt.tv_usec = 0x5678ABCD;
    pkt.reply_micro = 0xDEADBEEF;
    pkt.windowsize = 0x12345678;
    pkt.seq_nr = 0xABCD;
    pkt.ack_nr = 0xEF01;

    REQUIRE(pkt.connid == 0x1234);
    REQUIRE(pkt.tv_usec == 0x5678ABCD);
    REQUIRE(pkt.reply_micro == 0xDEADBEEF);
    REQUIRE(pkt.windowsize == 0x12345678);
    REQUIRE(pkt.seq_nr == 0xABCD);
    REQUIRE(pkt.ack_nr == 0xEF01);
}

TEST_CASE("PacketFormatV1 ext field", "[wire]") {
    utp::wire::PacketFormatV1 pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.ext = 42;
    REQUIRE(pkt.ext == 42);
}

TEST_CASE("PacketFormatAckV1 fields", "[wire]") {
    utp::wire::PacketFormatAckV1 ack;
    std::memset(&ack, 0, sizeof(ack));

    ack.pf.set_type(utp::wire::ST_STATE);
    ack.pf.set_version(1);
    ack.ext_next = 1;
    ack.ext_len = 2;
    ack.acks[0] = 0x12;
    ack.acks[1] = 0x34;
    ack.acks[2] = 0x56;
    ack.acks[3] = 0x78;

    REQUIRE(ack.pf.type() == utp::wire::ST_STATE);
    REQUIRE(ack.pf.version() == 1);
    REQUIRE(ack.ext_next == 1);
    REQUIRE(ack.ext_len == 2);
    REQUIRE(ack.acks[0] == 0x12);
    REQUIRE(ack.acks[1] == 0x34);
    REQUIRE(ack.acks[2] == 0x56);
    REQUIRE(ack.acks[3] == 0x78);
}

TEST_CASE("PacketFormatV1 size matches static assert", "[wire]") {
    static_assert(sizeof(utp::wire::PacketFormatV1) == 20, "PacketFormatV1 must be 20 bytes");
    static_assert(sizeof(utp::wire::PacketFormatAckV1) == 26, "PacketFormatAckV1 must be 26 bytes");
    REQUIRE(sizeof(utp::wire::PacketFormatV1) == 20);
    REQUIRE(sizeof(utp::wire::PacketFormatAckV1) == 26);
}