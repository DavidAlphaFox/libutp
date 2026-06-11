// libFuzzer harness：把不可信字节流喂给 uTP 报文解析入口。
//
// 覆盖面设计：
//   - 输入被分帧成多个"报文"，依次喂给同一个 context，使会话状态机
//     （SYN 接受、连接查找、ACK/SACK 处理、FIN/RESET、重组缓冲）被持续推进，
//     而不是每次只解析一个孤立包。
//   - 每帧首字节是路由选择子：UDP 报文 / ICMP need-frag / ICMP 错误 /
//     推进假时钟并触发超时检查。
//   - 回调全部确定性（假时钟、计数器随机数、发送即丢弃），无 I/O、无系统时间，
//     保证崩溃可由输入字节精确复现。
//
// Debug 构建保留 assert：协议可达状态触发断言即为真实发现。

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>

#include "utp.h"

#include <netinet/in.h>

namespace {

uint64_t g_now_ms;
uint32_t g_rng_state;

uint64 cb_sendto(utp_callback_arguments*)            { return 0; }  // 丢弃出站报文
uint64 cb_on_read(utp_callback_arguments* a)         { utp_read_drained(a->socket); return 0; }
uint64 cb_state_change(utp_callback_arguments*)      { return 0; }
uint64 cb_on_error(utp_callback_arguments*)          { return 0; }
uint64 cb_on_accept(utp_callback_arguments*)         { return 0; }
uint64 cb_firewall(utp_callback_arguments*)          { return 0; }  // 接受入站连接，打开 SYN 路径
uint64 cb_read_buffer_size(utp_callback_arguments*)  { return 0; }
uint64 cb_get_ms(utp_callback_arguments*)            { return g_now_ms; }
uint64 cb_get_us(utp_callback_arguments*)            { return g_now_ms * 1000; }
uint64 cb_get_random(utp_callback_arguments*) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state >> 8;
}

sockaddr_in peer_addr() {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(6881);
    a.sin_addr.s_addr = htonl(0x7F000002u);
    return a;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    g_now_ms = 1'000'000;
    g_rng_state = 0x5EED5EEDu;

    utp_context* ctx = utp_init(2);
    if (!ctx) return 0;

    utp_set_callback(ctx, UTP_SENDTO, &cb_sendto);
    utp_set_callback(ctx, UTP_ON_READ, &cb_on_read);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE, &cb_state_change);
    utp_set_callback(ctx, UTP_ON_ERROR, &cb_on_error);
    utp_set_callback(ctx, UTP_ON_ACCEPT, &cb_on_accept);
    utp_set_callback(ctx, UTP_ON_FIREWALL, &cb_firewall);
    utp_set_callback(ctx, UTP_GET_READ_BUFFER_SIZE, &cb_read_buffer_size);
    utp_set_callback(ctx, UTP_GET_MILLISECONDS, &cb_get_ms);
    utp_set_callback(ctx, UTP_GET_MICROSECONDS, &cb_get_us);
    utp_set_callback(ctx, UTP_GET_RANDOM, &cb_get_random);

    const sockaddr_in from = peer_addr();

    // 分帧：[1B 选择子][1B 长度 n][n 字节负载]，循环直到输入耗尽
    size_t pos = 0;
    while (pos + 2 <= size) {
        const uint8_t selector = data[pos];
        const size_t n = data[pos + 1];
        pos += 2;
        const size_t avail = size - pos;
        const size_t len = n < avail ? n : avail;
        const uint8_t* payload = data + pos;
        pos += len;

        switch (selector & 0x3) {
            case 0:
            case 1:  // 偏向 UDP 报文路径
                utp_process_udp(ctx, payload, len, (const sockaddr*)&from, sizeof(from));
                break;
            case 2:
                if (len >= 2) {
                    const uint16_t mtu = (uint16_t)(payload[0] | (payload[1] << 8));
                    utp_process_icmp_fragmentation(ctx, payload + 2, len - 2,
                                                   (const sockaddr*)&from, sizeof(from), mtu);
                } else {
                    utp_process_icmp_error(ctx, payload, len, (const sockaddr*)&from, sizeof(from));
                }
                break;
            case 3:  // 推进时钟，触发重传/保活/销毁路径
                g_now_ms += 500 + selector * 37;
                utp_check_timeouts(ctx);
                utp_issue_deferred_acks(ctx);
                break;
        }
    }

    // 收尾：把时间推远，逼出超时销毁路径，再销毁 context（覆盖批量析构）
    for (int i = 0; i < 4; ++i) {
        g_now_ms += 30'000;
        utp_check_timeouts(ctx);
    }
    utp_destroy(ctx);
    return 0;
}
