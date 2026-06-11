// 序列号缓冲区测试
// 验证循环缓冲区的正确性，用于乱序到达的数据包重组
// 确保索引回绕、读写操作、所有权转移和边界条件的正确处理

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include "utp/sequence_buffer.hpp"

namespace {
std::unique_ptr<int> boxed(int v) { return std::make_unique<int>(v); }
}

TEST_CASE("SequenceBuffer 初始状态", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.get(0) == nullptr);
}

TEST_CASE("SequenceBuffer 初始化和存取", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);
    REQUIRE(buf.size() == 16);
    REQUIRE(buf.mask() == 15);

    buf.put(5, boxed(42));
    REQUIRE(buf.get(5) != nullptr);
    REQUIRE(*buf.get(5) == 42);
    REQUIRE(buf.get(0) == nullptr);
}

TEST_CASE("SequenceBuffer 索引回绕", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);
    buf.put(21, boxed(99));  // 21 & 15 = 5
    REQUIRE(buf.get(21) != nullptr);
    REQUIRE(*buf.get(21) == 99);
    REQUIRE(buf.get(5) == buf.get(21));
}

TEST_CASE("SequenceBuffer 顺序读写", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);

    for (int i = 0; i < 4; ++i)
        buf.put(i, boxed((i + 1) * 100));

    for (int i = 0; i < 4; ++i) {
        REQUIRE(buf.get(i) != nullptr);
        REQUIRE(*buf.get(i) == (i + 1) * 100);
    }
}

TEST_CASE("SequenceBuffer 元素覆盖自动释放旧元素", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);

    buf.put(5, boxed(111));
    REQUIRE(*buf.get(5) == 111);

    buf.put(5, boxed(222));
    REQUIRE(*buf.get(5) == 222);
}

TEST_CASE("SequenceBuffer take 转移所有权并置空槽位", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);

    buf.put(7, boxed(77));
    std::unique_ptr<int> taken = buf.take(7);
    REQUIRE(taken != nullptr);
    REQUIRE(*taken == 77);
    REQUIRE(buf.get(7) == nullptr);

    // 空槽位 take 返回 nullptr
    REQUIRE(buf.take(7) == nullptr);
}

TEST_CASE("SequenceBuffer 未写入位置返回 nullptr", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(16);

    REQUIRE(buf.get(0) == nullptr);
    REQUIRE(buf.get(1) == nullptr);
    REQUIRE(buf.get(15) == nullptr);
}

TEST_CASE("SequenceBuffer 大小和掩码", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(8);
    REQUIRE(buf.size() == 8);
    REQUIRE(buf.mask() == 7);

    buf.initialize(32);
    REQUIRE(buf.size() == 32);
    REQUIRE(buf.mask() == 31);
}

TEST_CASE("SequenceBuffer ensure_size 扩容保持环形偏移", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    buf.initialize(4);

    // 写满连续 4 个序号
    for (int s = 10; s <= 13; ++s)
        buf.put(s, boxed(s));

    // 触发扩容：item=14（下一个写入位置），index=4（在飞数量），窗口 [10,14)
    buf.ensure_size(14, 4);
    REQUIRE(buf.size() >= 8);

    // 扩容后原数据仍可按原序号取到
    for (int s = 10; s <= 13; ++s) {
        REQUIRE(buf.get(s) != nullptr);
        REQUIRE(*buf.get(s) == s);
    }
}

TEST_CASE("SequenceBuffer 默认初始化", "[seqbuf]") {
    utp::SequenceBuffer<int> buf;
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.mask() == 0);
    REQUIRE(buf.get(0) == nullptr);
}
