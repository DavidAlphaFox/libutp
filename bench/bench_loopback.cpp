// 内存回环吞吐基准
// 双 UtpContext 经内存队列互递报文（无真实 socket），假时钟驱动协议、
// 真实 wall-clock 计量吞吐。同时通过替换全局 operator new/delete 统计堆分配，
// 输出 MB/s 与 每 MB 分配次数，用于判断包对象分配是否值得池化。
//
// 用法：bench_loopback [总 MB 数，默认 256]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <deque>
#include <new>
#include <vector>

#include "utp.h"

#include <netinet/in.h>

// ── 全局分配计数（基准专用，统计协议栈+测试本身的所有堆操作）──
static size_t g_alloc_count = 0;
static size_t g_alloc_bytes = 0;

void* operator new(size_t n) {
    ++g_alloc_count;
    g_alloc_bytes += n;
    if (void* p = malloc(n)) return p;
    abort();
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

namespace {

uint64_t g_now_ms = 1'000'000;

struct Endpoint {
    utp_context* ctx = nullptr;
    utp_socket* sock = nullptr;
    Endpoint* peer = nullptr;
    sockaddr_in addr{};
    std::deque<std::vector<unsigned char>> inbox;
    size_t received_bytes = 0;
    bool connected = false;
    bool got_eof = false;
    bool destroyed = false;
};

uint64 cb_sendto(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->peer->inbox.emplace_back(a->buf, a->buf + a->len);
    return 0;
}
uint64 cb_state_change(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    if (a->state == UTP_STATE_CONNECT)    self->connected = true;
    if (a->state == UTP_STATE_EOF)        self->got_eof = true;
    if (a->state == UTP_STATE_DESTROYING) self->destroyed = true;
    return 0;
}
uint64 cb_on_read(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->received_bytes += a->len;  // 只计数不存储，避免基准自身失真
    utp_read_drained(a->socket);
    return 0;
}
uint64 cb_on_accept(utp_callback_arguments* a) {
    auto* self = (Endpoint*)utp_context_get_userdata(a->context);
    self->sock = a->socket;
    self->connected = true;
    return 0;
}
uint64 cb_get_ms(utp_callback_arguments*) { return g_now_ms; }
uint64 cb_get_us(utp_callback_arguments*) { return g_now_ms * 1000; }
uint64 cb_get_random(utp_callback_arguments*) {
    static uint64_t s = 0x5EED5EEDULL;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(s >> 33);
}
uint64 cb_zero(utp_callback_arguments*) { return 0; }

void install(utp_context* ctx) {
    utp_set_callback(ctx, UTP_SENDTO, &cb_sendto);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE, &cb_state_change);
    utp_set_callback(ctx, UTP_ON_READ, &cb_on_read);
    utp_set_callback(ctx, UTP_ON_ACCEPT, &cb_on_accept);
    utp_set_callback(ctx, UTP_ON_FIREWALL, &cb_zero);
    utp_set_callback(ctx, UTP_GET_READ_BUFFER_SIZE, &cb_zero);
    utp_set_callback(ctx, UTP_GET_MILLISECONDS, &cb_get_ms);
    utp_set_callback(ctx, UTP_GET_MICROSECONDS, &cb_get_us);
    utp_set_callback(ctx, UTP_GET_RANDOM, &cb_get_random);
}

void deliver(Endpoint& e) {
    while (!e.inbox.empty()) {
        auto& pkt = e.inbox.front();
        utp_process_udp(e.ctx, pkt.data(), pkt.size(),
                        (const sockaddr*)&e.peer->addr, sizeof(e.peer->addr));
        e.inbox.pop_front();
    }
    utp_issue_deferred_acks(e.ctx);
}

}  // namespace

int main(int argc, char** argv) {
    const size_t total_mb = argc > 1 ? (size_t)atoll(argv[1]) : 256;
    const size_t total_bytes = total_mb * 1024 * 1024;

    Endpoint client, server;
    client.ctx = utp_init(2);
    server.ctx = utp_init(2);
    client.peer = &server;
    server.peer = &client;

    client.addr.sin_family = AF_INET;
    client.addr.sin_port = htons(5001);
    client.addr.sin_addr.s_addr = htonl(0x7F000001u);
    server.addr = client.addr;
    server.addr.sin_port = htons(5002);
    server.addr.sin_addr.s_addr = htonl(0x7F000002u);

    utp_context_set_userdata(client.ctx, &client);
    utp_context_set_userdata(server.ctx, &server);
    install(client.ctx);
    install(server.ctx);

    // 64KB 发送源块（内容不重要）
    std::vector<unsigned char> chunk(64 * 1024, 0xA5);

    client.sock = utp_create_socket(client.ctx);
    utp_connect(client.sock, (const sockaddr*)&server.addr, sizeof(server.addr));

    const size_t alloc_count_0 = g_alloc_count;
    const size_t alloc_bytes_0 = g_alloc_bytes;
    const auto t0 = std::chrono::steady_clock::now();

    size_t sent = 0;
    bool close_sent = false;
    while (!(client.destroyed && server.destroyed)) {
        g_now_ms += 5;

        deliver(server);
        deliver(client);

        if (client.connected && !close_sent) {
            while (sent < total_bytes) {
                const size_t want = chunk.size() < total_bytes - sent ? chunk.size() : total_bytes - sent;
                ssize_t n = utp_write(client.sock, chunk.data(), want);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            if (sent == total_bytes) {
                utp_close(client.sock);
                close_sent = true;
            }
        }
        if (server.got_eof && server.sock) {
            utp_close(server.sock);
            server.sock = nullptr;
        }

        utp_check_timeouts(client.ctx);
        utp_check_timeouts(server.ctx);
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const size_t allocs = g_alloc_count - alloc_count_0;
    const size_t abytes = g_alloc_bytes - alloc_bytes_0;

    if (server.received_bytes != total_bytes) {
        fprintf(stderr, "FAIL: received %zu != %zu\n", server.received_bytes, total_bytes);
        return 1;
    }

    printf("transferred  : %zu MB\n", total_mb);
    printf("wall time    : %.3f s\n", secs);
    printf("throughput   : %.1f MB/s\n", (double)total_mb / secs);
    printf("heap allocs  : %zu total, %.1f per MB\n", allocs, (double)allocs / (double)total_mb);
    printf("heap bytes   : %.1f MB total, %.2f bytes alloc'd per payload byte\n",
           (double)abytes / 1048576.0, (double)abytes / (double)total_bytes);

    utp_destroy(client.ctx);
    utp_destroy(server.ctx);
    return 0;
}
