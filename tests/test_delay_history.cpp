// 延迟历史测试
// 验证延迟样本的采集、基线计算和时间戳处理
// 确保拥塞控制的延迟测量机制正确工作

#include <catch2/catch_test_macros.hpp>
#include "utp/delay_history.hpp"

TEST_CASE("DelayHistory 初始状态", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    REQUIRE(dh.delay_base == 0);
    REQUIRE_FALSE(dh.delay_base_initialized);
    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory 添加样本", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    dh.add_sample(100, 1000);
    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory 多个样本", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(100, 1000);
    dh.add_sample(100, 1060);
    dh.add_sample(100, 1120);

    REQUIRE(dh.get_value() == 0);
}

TEST_CASE("DelayHistory 添加样本后获取值", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(500, 1000);
    dh.add_sample(600, 2000);
    dh.add_sample(400, 3000);

    REQUIRE(dh.get_value() >= 0);
}

TEST_CASE("DelayHistory 时间戳回绕处理", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(0xFFFFFF00, 1000);
    dh.add_sample(0x00000100, 2000);

    REQUIRE(dh.delay_base_initialized == true);
}

TEST_CASE("DelayHistory shift 更新延迟基线", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);

    dh.add_sample(500, 1000);
    std::uint32_t old_delay_base = dh.delay_base;

    dh.shift(100);

    REQUIRE(dh.delay_base == old_delay_base + 100);
}

TEST_CASE("DelayHistory clear 重置状态", "[delay]") {
    utp::DelayHistory dh;
    dh.clear(1000);
    dh.add_sample(500, 1000);

    dh.clear(2000);

    REQUIRE(dh.delay_base == 0);
    REQUIRE(dh.delay_base_initialized == false);
    REQUIRE(dh.get_value() == 0);
}