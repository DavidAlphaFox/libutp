// 内存回环集成测试
// 两个 UtpContext 在进程内通过内存队列互递 UDP 报文，不开真实 socket：
//   - 假时钟（UTP_GET_MILLISECONDS/MICROSECONDS 回调）驱动重传/超时，测试瞬间完成
//   - 固定种子的确定性 PRNG（UTP_GET_RANDOM + 故障注入），失败可复现
//   - 链路层故障注入：按概率丢包、乱序、重复
// 覆盖：握手、批量传输、FIN/EOF 关闭、丢包重传、乱序重组、重复包去重。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <deque>
#include <vector>

#include "utp.h"

#ifdef POSIX
#include <netinet/in.h>
#endif

namespace {

// 确定性 PRNG（splitmix64 变体），保证测试可复现
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return (uint32_t)((z ^ (z >> 31)) >> 16);
    }
    bool percent(int p) { return (int)(next() % 100) < p; }
};

uint64_t g_now_ms = 1'000'000;  // 假时钟（毫秒）
Rng* g_rng = nullptr;           // 供 UTP_GET_RANDOM 使用

struct Endpoint {
    utp_context* ctx = nullptr;
    utp_socket* sock = nullptr;   // 客户端的连接 socket / 服务端 accept 到的 socket
    Endpoint* peer = nullptr;
    sockaddr_in addr{};           // 本端假地址

    std::deque<std::vector<unsigned char>> inbox;  // 待本端处理的入站报文
    std::vector<unsigned char> received;           // 应用层收到的数据

    bool connected = false;
    bool got_eof = false;
    bool destroyed = false;
    int error = -1;
};

uint64 cb_sendto(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    // 报文先进对端收件箱，故障注入在投递时做（模拟链路而非发送端）
    self->peer->inbox.emplace_back(a->buf, a->buf + a->len);
    return 0;
}

uint64 cb_state_change(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    switch (a->state) {
        case UTP_STATE_CONNECT:    self->connected = true; break;
        case UTP_STATE_WRITABLE:   break;
        case UTP_STATE_EOF:        self->got_eof = true; break;
        case UTP_STATE_DESTROYING: self->destroyed = true; break;
    }
    return 0;
}

uint64 cb_on_read(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->received.insert(self->received.end(), a->buf, a->buf + a->len);
    utp_read_drained(a->socket);
    return 0;
}

uint64 cb_on_accept(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->sock = a->socket;
    self->connected = true;
    return 0;
}

uint64 cb_on_error(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->error = a->error_code;
    return 0;
}

uint64 cb_get_ms(utp_callback_arguments*)     { return g_now_ms; }
uint64 cb_get_us(utp_callback_arguments*)     { return g_now_ms * 1000; }
uint64 cb_get_random(utp_callback_arguments*) { return g_rng->next(); }
uint64 cb_firewall(utp_callback_arguments*)   { return 0; }  // 0 = 接受入站连接
uint64 cb_read_buffer_size(utp_callback_arguments*) { return 0; }

void install_callbacks(utp_context* ctx) {
    utp_set_callback(ctx, UTP_SENDTO, &cb_sendto);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE, &cb_state_change);
    utp_set_callback(ctx, UTP_ON_READ, &cb_on_read);
    utp_set_callback(ctx, UTP_ON_ACCEPT, &cb_on_accept);
    utp_set_callback(ctx, UTP_ON_ERROR, &cb_on_error);
    utp_set_callback(ctx, UTP_ON_FIREWALL, &cb_firewall);
    utp_set_callback(ctx, UTP_GET_READ_BUFFER_SIZE, &cb_read_buffer_size);
    utp_set_callback(ctx, UTP_GET_MILLISECONDS, &cb_get_ms);
    utp_set_callback(ctx, UTP_GET_MICROSECONDS, &cb_get_us);
    utp_set_callback(ctx, UTP_GET_RANDOM, &cb_get_random);
}

sockaddr_in make_addr(uint32_t host_be_last_octet, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000000u | host_be_last_octet);  // 127.0.0.x
    return a;
}

// 链路故障参数
struct LinkFaults {
    int loss_pct = 0;     // 丢包率（百分比）
    int dup_pct = 0;      // 重复投递率
    int reorder_pct = 0;  // 与队列中下一个报文交换的概率
};

// 把 e 收件箱中的报文投递给协议栈（带故障注入）。
// 注意 utp_process_udp 的地址参数是报文的“来源地址”，即对端地址。
void deliver_inbox(Endpoint& e, Rng& rng, const LinkFaults& f) {
    while (!e.inbox.empty()) {
        if (f.reorder_pct && e.inbox.size() >= 2 && rng.percent(f.reorder_pct)) {
            std::swap(e.inbox[0], e.inbox[1]);
        }
        std::vector<unsigned char> pkt = std::move(e.inbox.front());
        e.inbox.pop_front();

        if (f.loss_pct && rng.percent(f.loss_pct))
            continue;  // 链路丢弃

        const int copies = (f.dup_pct && rng.percent(f.dup_pct)) ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            utp_process_udp(e.ctx, pkt.data(), pkt.size(),
                            (const sockaddr*)&e.peer->addr, sizeof(e.peer->addr));
        }
    }
    utp_issue_deferred_acks(e.ctx);
}

// 端到端传输一段数据并完成关闭握手，返回是否在时限内完成
bool run_transfer(const std::vector<unsigned char>& payload, const LinkFaults& faults,
                  Endpoint& client, Endpoint& server, Rng& link_rng,
                  uint64_t max_sim_ms = 600'000) {
    size_t offset = 0;
    bool close_sent = false;
    const uint64_t deadline = g_now_ms + max_sim_ms;

    while (g_now_ms < deadline) {
        g_now_ms += 5;  // 5ms 一步推进假时钟

        deliver_inbox(server, link_rng, faults);
        deliver_inbox(client, link_rng, faults);

        // 客户端尽力写入剩余数据
        if (client.connected && !close_sent) {
            while (offset < payload.size()) {
                ssize_t n = utp_write(client.sock,
                                      (void*)(payload.data() + offset),
                                      payload.size() - offset);
                if (n <= 0) break;  // 发送窗口已满，等下一轮
                offset += (size_t)n;
            }
            if (offset == payload.size()) {
                utp_close(client.sock);  // 全部入队后发 FIN
                close_sent = true;
            }
        }

        // 服务端读到 EOF 后关闭自己的方向
        if (server.got_eof && server.sock) {
            utp_close(server.sock);
            server.sock = nullptr;
        }

        utp_check_timeouts(client.ctx);
        utp_check_timeouts(server.ctx);

        if (client.destroyed && server.destroyed)
            return true;
    }
    fprintf(stderr, "run_transfer stalled: offset=%zu/%zu close_sent=%d "
            "c{conn=%d eof=%d destroyed=%d err=%d} s{conn=%d eof=%d destroyed=%d err=%d}\n",
            offset, payload.size(), (int)close_sent,
            (int)client.connected, (int)client.got_eof, (int)client.destroyed, client.error,
            (int)server.connected, (int)server.got_eof, (int)server.destroyed, server.error);
    return false;
}

struct Harness {
    Endpoint client, server;
    Rng rand_cb{0x5EED5EEDULL};  // UTP_GET_RANDOM 用

    Harness() {
        g_rng = &rand_cb;
        client.ctx = utp_init(2);
        server.ctx = utp_init(2);
        REQUIRE(client.ctx != nullptr);
        REQUIRE(server.ctx != nullptr);

        client.peer = &server;
        server.peer = &client;
        client.addr = make_addr(1, 5001);
        server.addr = make_addr(2, 5002);

        utp_context_set_userdata(client.ctx, &client);
        utp_context_set_userdata(server.ctx, &server);
        install_callbacks(client.ctx);
        install_callbacks(server.ctx);

        client.sock = utp_create_socket(client.ctx);
        REQUIRE(client.sock != nullptr);
        REQUIRE(utp_connect(client.sock, (const sockaddr*)&server.addr,
                            sizeof(server.addr)) == 0);
    }

    ~Harness() {
        utp_destroy(client.ctx);
        utp_destroy(server.ctx);
        g_rng = nullptr;
    }
};

std::vector<unsigned char> make_payload(size_t n, uint64_t seed) {
    Rng rng(seed);
    std::vector<unsigned char> data(n);
    for (size_t i = 0; i < n; ++i) data[i] = (unsigned char)rng.next();
    return data;
}

}  // namespace

TEST_CASE("回环：无损链路批量传输逐字节一致", "[loopback]") {
    Harness h;
    Rng link(1);
    auto payload = make_payload(256 * 1024, 42);

    REQUIRE(run_transfer(payload, LinkFaults{}, h.client, h.server, link));
    REQUIRE(h.server.received == payload);
    REQUIRE(h.client.error == -1);
    // 服务端不做无错断言：uTP 无 TIME_WAIT，客户端 FIN 被确认后立即销毁，
    // 后关闭的服务端发出的 FIN 可能收到 RST（UTP_ECONNRESET），属协议固有关闭竞态
    REQUIRE((h.server.error == -1 || h.server.error == UTP_ECONNRESET));
}

TEST_CASE("回环：握手与连接事件", "[loopback]") {
    Harness h;
    Rng link(2);
    auto payload = make_payload(4 * 1024, 7);

    REQUIRE(run_transfer(payload, LinkFaults{}, h.client, h.server, link));
    REQUIRE(h.client.connected);   // 收到 SYN-ACK 触发 UTP_STATE_CONNECT
    REQUIRE(h.server.connected);   // SYN 触发 UTP_ON_ACCEPT
    REQUIRE(h.server.got_eof);     // FIN 触发 UTP_STATE_EOF
    REQUIRE(h.server.received == payload);
}

TEST_CASE("回环：5% 丢包下重传保证可靠交付", "[loopback]") {
    Harness h;
    Rng link(3);
    LinkFaults f;
    f.loss_pct = 5;
    auto payload = make_payload(128 * 1024, 99);

    REQUIRE(run_transfer(payload, f, h.client, h.server, link));
    REQUIRE(h.server.received == payload);
}

TEST_CASE("回环：乱序 + 重复包下重组与去重正确", "[loopback]") {
    Harness h;
    Rng link(4);
    LinkFaults f;
    f.reorder_pct = 25;
    f.dup_pct = 10;
    auto payload = make_payload(128 * 1024, 1234);

    REQUIRE(run_transfer(payload, f, h.client, h.server, link));
    REQUIRE(h.server.received == payload);
}

TEST_CASE("回环：丢包 + 乱序 + 重复组合故障", "[loopback]") {
    Harness h;
    Rng link(5);
    LinkFaults f;
    f.loss_pct = 3;
    f.reorder_pct = 15;
    f.dup_pct = 5;
    auto payload = make_payload(64 * 1024, 777);

    REQUIRE(run_transfer(payload, f, h.client, h.server, link));
    REQUIRE(h.server.received == payload);
}
