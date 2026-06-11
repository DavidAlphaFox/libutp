// MTU 探测（Path MTU Discovery）单元测试
// 通过假 ILogger 独立驱动 MtuDiscovery 的二分搜索：
// 探测确认提升下界、探测丢失收紧上界、ICMP 处理、收敛与重发现调度。

#include <catch2/catch_test_macros.hpp>
#include "utp/mtu_discovery.hpp"

namespace {

// 静默日志：MtuDiscovery 只依赖这个窄接口
struct NullLogger : utp::ILogger {
    void vlog(int, const char*, va_list) override {}
};

constexpr uint64_t T0 = 1'000'000;  // 任意起始毫秒时间戳

MtuDiscovery make_mtu(NullLogger& logger, uint32_t udp_mtu = 1500) {
    MtuDiscovery mtu(&logger);
    mtu.reset(udp_mtu, T0);
    return mtu;
}

}  // namespace

TEST_CASE("MtuDiscovery reset 设定搜索区间与重发现时间", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    REQUIRE(mtu.floor() == 576);
    REQUIRE(mtu.ceiling() == 1500);
    // 30 分钟内不应触发重发现
    REQUIRE_FALSE(mtu.should_rediscover(T0));
    REQUIRE_FALSE(mtu.should_rediscover(T0 + 29 * 60 * 1000));
    REQUIRE(mtu.should_rediscover(T0 + 30 * 60 * 1000 + 1));
}

TEST_CASE("MtuDiscovery search_update 取中点并清除探测", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    mtu.set_probe(42, 1200);
    mtu.search_update(T0);

    REQUIRE(mtu.last() == (576 + 1500) / 2);
    REQUIRE(mtu.probe_seq() == 0);
    REQUIRE(mtu.probe_size() == 0);
}

TEST_CASE("MtuDiscovery effective_mtu 扣除包头", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    mtu.set_last_to_ceiling();
    REQUIRE(mtu.raw_mtu() == 1500);
    REQUIRE(mtu.effective_mtu(20) == 1480);
}

TEST_CASE("MtuDiscovery 探测确认提升下界", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    mtu.set_probe(7, 1038);
    REQUIRE(mtu.is_probe(7));
    REQUIRE_FALSE(mtu.is_probe(8));

    // 非探测序号的 ACK 不影响状态
    REQUIRE_FALSE(mtu.handle_probe_ack(8, T0));
    REQUIRE(mtu.floor() == 576);

    // 探测序号被确认：floor 提升到探测大小并推进搜索
    REQUIRE(mtu.handle_probe_ack(7, T0));
    REQUIRE(mtu.floor() == 1038);
    REQUIRE(mtu.last() == (1038 + 1500) / 2);
    REQUIRE(mtu.probe_seq() == 0);
}

TEST_CASE("MtuDiscovery 探测超时收紧上界", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    SECTION("唯一在飞包且为探测：忽略丢包并降 ceiling") {
        mtu.set_probe(9, 1200);
        REQUIRE(mtu.handle_probe_timeout(9, /*cur_window_packets=*/1, T0));
        REQUIRE(mtu.ceiling() == 1199);
        REQUIRE(mtu.probe_seq() == 0);
    }

    SECTION("有多个在飞包：不忽略丢包，但仍清除探测") {
        mtu.set_probe(9, 1200);
        REQUIRE_FALSE(mtu.handle_probe_timeout(9, /*cur_window_packets=*/3, T0));
        REQUIRE(mtu.ceiling() == 1500);
        REQUIRE(mtu.probe_seq() == 0);
    }
}

TEST_CASE("MtuDiscovery 探测丢失（DUPACK）收紧上界", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);

    mtu.set_probe(5, 1400);
    mtu.handle_probe_loss(T0);
    REQUIRE(mtu.ceiling() == 1399);
    REQUIRE(mtu.probe_seq() == 0);
}

TEST_CASE("MtuDiscovery ICMP 处理", "[mtu]") {
    NullLogger logger;

    SECTION("need-frag 携带有效 next_hop_mtu：直接采纳") {
        MtuDiscovery mtu = make_mtu(logger, 1500);
        mtu.handle_icmp_fragmentation(1100, T0);
        REQUIRE(mtu.ceiling() == 1100);
        REQUIRE(mtu.last() == 1100);
    }

    SECTION("next_hop_mtu 大于当前 ceiling：不放大") {
        MtuDiscovery mtu = make_mtu(logger, 1500);
        mtu.handle_icmp_fragmentation(9000, T0);
        REQUIRE(mtu.ceiling() == 1500);
    }

    SECTION("无有效 next_hop_mtu：二分取中点为新 ceiling") {
        MtuDiscovery mtu = make_mtu(logger, 1500);
        mtu.handle_icmp_unknown(T0);
        REQUIRE(mtu.ceiling() == (576 + 1500) / 2);
    }
}

TEST_CASE("MtuDiscovery 二分搜索收敛到真实路径 MTU", "[mtu]") {
    NullLogger logger;
    MtuDiscovery mtu = make_mtu(logger, 1500);
    constexpr uint32_t TRUE_MTU = 1104;  // 假设的真实路径 MTU

    mtu.search_update(T0);  // 开始搜索：last = 中点

    uint32_t seq = 1;
    int rounds = 0;
    while (mtu.ceiling() != mtu.floor()) {
        REQUIRE(++rounds < 32);  // 二分必定快速收敛，防死循环

        const uint32_t probe_size = mtu.last();
        mtu.set_probe(seq, probe_size);
        if (probe_size <= TRUE_MTU)
            mtu.handle_probe_ack(seq, T0);   // 探测包能通过：提升 floor
        else
            mtu.handle_probe_loss(T0);       // 探测包过大被丢：收紧 ceiling
        ++seq;
    }

    // 收敛精度：搜索在区间 <= 16 时取 floor 终止
    REQUIRE(mtu.raw_mtu() <= TRUE_MTU);
    REQUIRE(mtu.raw_mtu() >= TRUE_MTU - 16);
    // 完成后安排了下一次重发现
    REQUIRE_FALSE(mtu.should_rediscover(T0));
}
