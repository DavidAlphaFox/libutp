#include <catch2/catch_test_macros.hpp>
#include "utp/sequence_buffer.hpp"

TEST_CASE("RawSequenceBuffer initial state", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.get(0) == nullptr);
}

TEST_CASE("RawSequenceBuffer initialize and put/get", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(16);
    REQUIRE(buf.size() == 16);
    REQUIRE(buf.mask() == 15);

    int val = 42;
    buf.put(5, &val);
    REQUIRE(buf.get(5) == &val);
    REQUIRE(buf.get(0) == nullptr);
}

TEST_CASE("RawSequenceBuffer wrapping index", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(16);
    int val = 99;
    buf.put(21, &val);  // 21 & 15 = 5
    REQUIRE(buf.get(21) == &val);
    REQUIRE(buf.get(5) == &val);
}

TEST_CASE("RawSequenceBuffer sequential writes and reads", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(16);

    int vals[4] = {100, 200, 300, 400};
    buf.put(0, &vals[0]);
    buf.put(1, &vals[1]);
    buf.put(2, &vals[2]);
    buf.put(3, &vals[3]);

    REQUIRE(buf.get(0) == &vals[0]);
    REQUIRE(buf.get(1) == &vals[1]);
    REQUIRE(buf.get(2) == &vals[2]);
    REQUIRE(buf.get(3) == &vals[3]);
}

TEST_CASE("RawSequenceBuffer element overwrite", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(16);

    int val1 = 111;
    int val2 = 222;

    buf.put(5, &val1);
    REQUIRE(buf.get(5) == &val1);

    buf.put(5, &val2);
    REQUIRE(buf.get(5) == &val2);
}

TEST_CASE("RawSequenceBuffer get returns nullptr for unwritten slots", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(16);

    REQUIRE(buf.get(0) == nullptr);
    REQUIRE(buf.get(1) == nullptr);
    REQUIRE(buf.get(15) == nullptr);
}

TEST_CASE("RawSequenceBuffer size and mask", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    buf.initialize(8);
    REQUIRE(buf.size() == 8);
    REQUIRE(buf.mask() == 7);

    buf.initialize(32);
    REQUIRE(buf.size() == 32);
    REQUIRE(buf.mask() == 31);
}

TEST_CASE("RawSequenceBuffer default initialization", "[seqbuf]") {
    utp::RawSequenceBuffer buf;
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.mask() == 0);
    REQUIRE(buf.get(0) == nullptr);
}