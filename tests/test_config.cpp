// 配置常量测试
// 验证协议配置参数的正确性和合理性
// 确保超时、缓冲区、窗口大小等关键配置符合预期

#include <catch2/catch_test_macros.hpp>
#include "utp/config.hpp"

TEST_CASE("配置常量具有预期值", "[config]") {
    REQUIRE(utp::config::CUR_DELAY_SIZE == 3);
    REQUIRE(utp::config::DELAY_BASE_HISTORY == 13);
    REQUIRE(utp::config::PACKET_SIZE == 1435);
    REQUIRE(utp::config::MIN_WINDOW_SIZE == 10);
    REQUIRE(utp::config::CCONTROL_TARGET == 100000);
    REQUIRE(utp::config::TIMESTAMP_MASK == 0xFFFFFFFF);
}

TEST_CASE("配置超时常量为正数", "[config]") {
    REQUIRE(utp::config::TIMEOUT_CHECK_INTERVAL > 0);
    REQUIRE(utp::config::RST_INFO_TIMEOUT > 0);
    REQUIRE(utp::config::KEEPALIVE_INTERVAL > 0);
}

TEST_CASE("配置缓冲区大小为正数", "[config]") {
    REQUIRE(utp::config::REORDER_BUFFER_SIZE > 0);
    REQUIRE(utp::config::REORDER_BUFFER_MAX_SIZE >= utp::config::REORDER_BUFFER_SIZE);
    REQUIRE(utp::config::OUTGOING_BUFFER_MAX_SIZE > 0);
}

TEST_CASE("配置数据包大小常量一致", "[config]") {
    REQUIRE(utp::config::PACKET_SIZE_EMPTY_BUCKET == 0);
    REQUIRE(utp::config::PACKET_SIZE_SMALL_BUCKET == 1);
    REQUIRE(utp::config::PACKET_SIZE_MID_BUCKET == 2);
    REQUIRE(utp::config::PACKET_SIZE_BIG_BUCKET == 3);
    REQUIRE(utp::config::PACKET_SIZE_HUGE_BUCKET == 4);

    REQUIRE(utp::config::PACKET_SIZE_EMPTY == 23);
    REQUIRE(utp::config::PACKET_SIZE_SMALL == 373);
    REQUIRE(utp::config::PACKET_SIZE_MID == 723);
    REQUIRE(utp::config::PACKET_SIZE_BIG == 1400);
}

TEST_CASE("配置窗口常量为正数", "[config]") {
    REQUIRE(utp::config::MIN_WINDOW_SIZE > 0);
    REQUIRE(utp::config::DUPLICATE_ACKS_BEFORE_RESEND > 0);
    REQUIRE(utp::config::ACK_NR_ALLOWED_WINDOW == utp::config::DUPLICATE_ACKS_BEFORE_RESEND);
    REQUIRE(utp::config::RST_INFO_LIMIT > 0);
}

TEST_CASE("配置位掩码具有预期值", "[config]") {
    REQUIRE(utp::config::SEQ_NR_MASK == 0xFFFF);
    REQUIRE(utp::config::ACK_NR_MASK == 0xFFFF);
    REQUIRE(utp::config::TIMESTAMP_MASK == 0xFFFFFFFF);
}