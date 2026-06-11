// LEDBAT 拥塞控制器单元测试
// 独立驱动 LedbatController：RTT/RTO 估算（RFC 6298）、慢启动与拥塞避免、
// 窗口衰减、RTO 超时收缩、发送窗口配额检查。

#include <catch2/catch_test_macros.hpp>
#include "utp/ledbat.hpp"

namespace {

constexpr uint64_t T0 = 10'000'000;       // 任意起始毫秒时间戳
constexpr size_t   SNDBUF = 1024 * 1024;  // 发送缓冲上限
constexpr size_t   TARGET = 100'000;      // LEDBAT 目标排队延迟（微秒）
constexpr size_t   PKT = 1400;            // 单包大小

// 向 our_hist 填入稳定的排队延迟 D（微秒）：
// 首样本建立 delay_base，再写满 3 个 cur_delay 槽，使 get_value() == D
void fill_our_delay(LedbatController& cc, uint32_t delay_us, uint64_t now) {
    cc.our_hist().add_sample(1'000'000, now);
    for (int i = 0; i < 3; ++i)
        cc.our_hist().add_sample(1'000'000 + delay_us, now);
}

LedbatController make_cc(size_t max_window) {
    LedbatController cc;
    cc.init_timing(T0);
    cc.init_delay_histories(T0);
    cc.set_max_window(max_window);
    cc.set_ssthresh(SNDBUF);
    return cc;
}

}  // namespace

TEST_CASE("Ledbat update_rtt 首样本初始化（RFC 6298）", "[ledbat]") {
    LedbatController cc = make_cc(10000);

    cc.update_rtt(/*ertt=*/100, T0);
    REQUIRE(cc.rtt_ms() == 100);
    REQUIRE(cc.rtt_var() == 50);
    // rto = max(rtt + 4*var, 1000) = max(300, 1000)
    REQUIRE(cc.rto_ms() == 1000);
    REQUIRE(cc.retransmit_timeout() == 1000);
    REQUIRE(cc.rto_timeout() == T0 + 1000);
}

TEST_CASE("Ledbat update_rtt EWMA 平滑", "[ledbat]") {
    LedbatController cc = make_cc(10000);

    cc.update_rtt(100, T0);
    cc.update_rtt(200, T0 + 100);

    // delta = 100-200 = -100; var = 50 + (100-50)/4 = 62
    // rtt = 100 - 100/8 + 200/8 = 113
    REQUIRE(cc.rtt_ms() == 113);
    REQUIRE(cc.rtt_var() == 62);
    REQUIRE(cc.rto_ms() == 1000);  // 113 + 4*62 = 361 < 下界 1000
}

TEST_CASE("Ledbat update_rtt 高 RTT 时 RTO 超过下界", "[ledbat]") {
    LedbatController cc = make_cc(10000);

    cc.update_rtt(2000, T0);
    // rto = max(2000 + 4*1000, 1000) = 6000
    REQUIRE(cc.rto_ms() == 6000);
    REQUIRE(cc.is_rto_expired(T0 + 6000));
    REQUIRE_FALSE(cc.is_rto_expired(T0 + 5999));
}

TEST_CASE("Ledbat 在飞字节配额与 can_send", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/5000);

    REQUIRE(cc.can_send(5000, /*max_window_user=*/SNDBUF, SNDBUF));
    cc.add_in_flight(4000);
    REQUIRE(cc.cur_window() == 4000);
    REQUIRE(cc.can_send(1000, SNDBUF, SNDBUF));
    REQUIRE_FALSE(cc.can_send(1001, SNDBUF, SNDBUF));

    // 限幅取三者最小：用户窗口收紧到 4500 后只能再发 500
    REQUIRE_FALSE(cc.can_send(501, /*max_window_user=*/4500, SNDBUF));
    REQUIRE(cc.can_send(500, 4500, SNDBUF));

    cc.remove_in_flight(4000);
    REQUIRE(cc.cur_window() == 0);
}

TEST_CASE("Ledbat maybe_decay_win 减半并退出慢启动", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/8000);
    REQUIRE(cc.slow_start());

    // init_timing 已把 last_rwin_decay 设为 T0 - MAX_WINDOW_DECAY，可立即衰减
    cc.maybe_decay_win(T0);
    REQUIRE(cc.max_window() == 4000);
    REQUIRE_FALSE(cc.slow_start());
    REQUIRE(cc.ssthresh() == 4000);

    // 100ms 内不允许再次衰减
    cc.maybe_decay_win(T0 + 50);
    REQUIRE(cc.max_window() == 4000);

    // 过了衰减间隔后可再次减半
    cc.maybe_decay_win(T0 + LedbatController::MAX_WINDOW_DECAY);
    REQUIRE(cc.max_window() == 2000);
}

TEST_CASE("Ledbat maybe_decay_win 不低于最小窗口", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/12);
    cc.maybe_decay_win(T0);
    REQUIRE(cc.max_window() == LedbatController::MIN_WINDOW_SIZE);
}

TEST_CASE("Ledbat on_rto_timeout 空闲连接温和衰减", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/9000);

    cc.on_rto_timeout(T0, PKT, /*cur_window_packets=*/0);
    REQUIRE(cc.max_window() == 6000);  // 衰减到 2/3
    REQUIRE(cc.slow_start());          // 空闲衰减不重置慢启动

    // 不低于单包大小
    LedbatController cc2 = make_cc(/*max_window=*/PKT + 1);
    cc2.on_rto_timeout(T0, PKT, 0);
    REQUIRE(cc2.max_window() == PKT);
}

TEST_CASE("Ledbat on_rto_timeout 有在飞包时重置进入慢启动", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/9000);
    cc.maybe_decay_win(T0);  // 先退出慢启动
    REQUIRE_FALSE(cc.slow_start());

    cc.on_rto_timeout(T0, PKT, /*cur_window_packets=*/4);
    REQUIRE(cc.max_window() == PKT);
    REQUIRE(cc.slow_start());
}

TEST_CASE("Ledbat apply_ccontrol 低延迟时慢启动增窗", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/2 * PKT);
    cc.mark_window_full(T0);  // 最近触顶过，允许继续加窗

    // our_hist 无样本时 get_value()==0（cur_delay 槽初始为 0），
    // 配合 min_rtt=0 得 our_delay=0，off_target = target（最大增益方向）
    const size_t before = cc.max_window();
    int32_t our_delay = cc.apply_ccontrol(/*bytes_acked=*/PKT, /*actual_delay=*/50'000,
                                          /*min_rtt=*/0, T0, SNDBUF, TARGET, PKT);
    REQUIRE(our_delay == 0);
    REQUIRE(cc.max_window() > before);
    REQUIRE(cc.slow_start());
}

TEST_CASE("Ledbat apply_ccontrol 排队延迟接近目标时退出慢启动", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/4 * PKT);
    cc.mark_window_full(T0);
    fill_our_delay(cc, /*delay_us=*/95'000, T0);  // > 0.9 * target

    const size_t before = cc.max_window();
    cc.apply_ccontrol(PKT, 95'000, /*min_rtt=*/500'000, T0, SNDBUF, TARGET, PKT);
    REQUIRE_FALSE(cc.slow_start());
    REQUIRE(cc.ssthresh() == before);  // 退出时记录 ssthresh
}

TEST_CASE("Ledbat apply_ccontrol 超过目标延迟时收缩窗口", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/8 * PKT);
    cc.mark_window_full(T0);
    cc.maybe_decay_win(T0);  // 进入拥塞避免（slow_start = false）
    const size_t before = cc.max_window();

    fill_our_delay(cc, /*delay_us=*/200'000, T0);  // 2 倍目标延迟
    int32_t our_delay = cc.apply_ccontrol(PKT, 200'000, /*min_rtt=*/500'000,
                                          T0, SNDBUF, TARGET, PKT);
    REQUIRE(our_delay == 200'000);
    REQUIRE(cc.max_window() < before);
    REQUIRE(cc.max_window() >= LedbatController::MIN_WINDOW_SIZE);
}

TEST_CASE("Ledbat apply_ccontrol 长期未触顶则不再加窗", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/2 * PKT);
    cc.maybe_decay_win(T0);  // 进入拥塞避免，窗口减半为 PKT
    const size_t before = cc.max_window();

    // last_maxed_out_window 默认 0，距 T0 远超 1 秒 → 增益被置零
    cc.apply_ccontrol(PKT, 50'000, /*min_rtt=*/0, T0, SNDBUF, TARGET, PKT);
    REQUIRE(cc.max_window() == before);
}

TEST_CASE("Ledbat apply_ccontrol 窗口受 opt_sndbuf 上限约束", "[ledbat]") {
    LedbatController cc = make_cc(/*max_window=*/3000);
    cc.mark_window_full(T0);

    cc.apply_ccontrol(3000, 50'000, /*min_rtt=*/0, T0, /*opt_sndbuf=*/3100, TARGET, PKT);
    REQUIRE(cc.max_window() <= 3100);
}
