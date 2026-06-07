# TASK: UtpSocket 数据分组 + 独立函数归位重构

## 目标

1. 将 UtpSocket 的 ~40 个成员变量归入 4 个纯数据 struct，实现认知分组
2. 将散落在 utp_api.cpp / utp_process.cpp / utp_socket.cpp 中的独立函数转为 UtpSocket / UtpContext 的成员函数
3. C API 层变为薄委托层（1-3 行），保持二进制兼容

## 约束

- 公共 C API (`include/utp.h`) 二进制兼容不变
- `LedbatController` 和 `MtuDiscovery` 已提取完毕，保持不动
- 纯数据 struct 无行为方法（协议逻辑天然跨 send/receive 边界）

---

## Phase A: 数据 struct 分组

### 新增 4 个内部 struct（定义在 utp_socket.hpp 中）

```cpp
struct ConnectionId {
    utp::Address addr;
    utp_context* ctx = nullptr;
    uint32 conn_seed = 0;
    uint32 conn_id_recv = 0;
    uint32 conn_id_send = 0;
    void* userdata = nullptr;
    CONN_STATE state = CS_UNINITIALIZED;
    byte extensions[8] = {};
};

struct ReceiveState {
    uint16 ack_nr = 0;
    uint16 reorder_count = 0;
    uint16 eof_pkt = 0;
    bool got_fin : 1 = false;
    bool got_fin_reached : 1 = false;
    bool read_shutdown : 1 = false;
    size_t opt_rcvbuf = 0;
    size_t last_rcv_win = 0;
    utp::RawSequenceBuffer inbuf;
};

struct SendState {
    uint16 seq_nr = 1;
    uint16 cur_window_packets = 0;
    bool fin_sent : 1 = false;
    bool fin_sent_acked : 1 = false;
    bool close_requested : 1 = false;
    size_t opt_sndbuf = 0;
    size_t max_window_user = 0;
    utp::RawSequenceBuffer outbuf;
};

struct TimingState {
    uint32 reply_micro = 0;
    uint64 last_measured_delay = 0;
    uint64 last_got_packet = 0;
    uint64 last_sent_packet = 0;
    bool fast_timeout : 1 = false;
    uint16 timeout_seq_nr = 0;
    uint16 fast_resend_seq_nr = 1;
};
```

### UtpSocket 替换后布局

```cpp
class UtpSocket {
    ConnectionId  conn_;
    ReceiveState  recv_;
    SendState     send_;
    TimingState   timing_;

    int   ida_ = -1;
    byte  duplicate_ack_ = 0;

    MtuDiscovery    mtu_;
    LedbatController cc_;
};
```

### 字段重命名映射

| 旧名 | 新名 |
|-------|------|
| `addr` | `conn_.addr` |
| `ctx` | `conn_.ctx` |
| `conn_seed_` | `conn_.conn_seed` |
| `conn_id_recv_` | `conn_.conn_id_recv` |
| `conn_id_send_` | `conn_.conn_id_send` |
| `userdata_` | `conn_.userdata` |
| `state_` | `conn_.state` |
| `extensions_` | `conn_.extensions` |
| `ack_nr_` | `recv_.ack_nr` |
| `reorder_count_` | `recv_.reorder_count` |
| `eof_pkt_` | `recv_.eof_pkt` |
| `got_fin` | `recv_.got_fin` |
| `got_fin_reached_` | `recv_.got_fin_reached` |
| `read_shutdown_` | `recv_.read_shutdown` |
| `opt_rcvbuf_` | `recv_.opt_rcvbuf` |
| `last_rcv_win_` | `recv_.last_rcv_win` |
| `inbuf_` | `recv_.inbuf` |
| `seq_nr_` | `send_.seq_nr` |
| `cur_window_packets_` | `send_.cur_window_packets` |
| `fin_sent` | `send_.fin_sent` |
| `fin_sent_acked_` | `send_.fin_sent_acked` |
| `close_requested_` | `send_.close_requested` |
| `opt_sndbuf_` | `send_.opt_sndbuf` |
| `max_window_user_` | `send_.max_window_user` |
| `outbuf_` | `send_.outbuf` |
| `reply_micro_` | `timing_.reply_micro` |
| `last_measured_delay_` | `timing_.last_measured_delay` |
| `last_got_packet_` | `timing_.last_got_packet` |
| `last_sent_packet_` | `timing_.last_sent_packet` |
| `fast_timeout_` | `timing_.fast_timeout` |
| `timeout_seq_nr_` | `timing_.timeout_seq_nr` |
| `fast_resend_seq_nr_` | `timing_.fast_resend_seq_nr` |
| `target_delay_` | `conn_.target_delay` (新增到此 struct) |

---

## Phase B: 独立函数 → 成员函数

### UtpSocket 新增成员函数

| 旧自由函数 | 新成员函数 | 源文件 |
|-----------|-----------|--------|
| `utp_initialize_socket()` | `UtpSocket::initialize()` | utp_api.cpp |
| `utp_connect()` | `UtpSocket::connect()` | utp_api.cpp |
| `utp_writev()` | `UtpSocket::writev()` | utp_api.cpp |
| `utp_read_drained()` | `UtpSocket::read_drained()` | utp_api.cpp |
| `utp_close()` | `UtpSocket::close()` | utp_api.cpp |
| `utp_shutdown()` | `UtpSocket::shutdown()` | utp_api.cpp |
| `utp_getpeername()` | `UtpSocket::get_peername()` | utp_api.cpp |
| `utp_get_delays()` | `UtpSocket::get_delays()` | utp_api.cpp |
| `utp_setsockopt()` | `UtpSocket::set_option()` | utp_api.cpp |
| `utp_getsockopt()` | `UtpSocket::get_option()` | utp_api.cpp |
| `utp_process_incoming()` | `UtpSocket::process_incoming()` | utp_process.cpp |
| `remove_socket_from_ack_list()` | `UtpSocket::remove_from_ack_list()` | utp_socket.cpp |
| `utp_register_recv_packet()` | `UtpSocket::register_recv_packet()` | utp_process.cpp |
| `parse_icmp_payload()` | `UtpContext::find_socket_by_icmp()` | utp_process.cpp |

### UtpContext 新增成员函数

| 旧自由函数 | 新成员函数 | 源文件 |
|-----------|-----------|--------|
| `utp_check_timeouts()` | `UtpContext::check_timeouts()` | utp_api.cpp |
| `utp_issue_deferred_acks()` | `UtpContext::issue_deferred_acks()` | utp_api.cpp |
| `utp_process_udp()` | `UtpContext::process_udp()` | utp_process.cpp |
| `utp_process_icmp_fragmentation()` | `UtpContext::process_icmp_fragmentation()` | utp_process.cpp |
| `utp_process_icmp_error()` | `UtpContext::process_icmp_error()` | utp_process.cpp |
| `send_to_addr()` | `UtpContext::send_to_addr()` | utp_socket.cpp |
| `utp_register_sent_packet()` | `UtpContext::register_sent_packet()` | utp_socket.cpp |

### C API 变为薄委托

```cpp
extern "C" void utp_close(utp_socket* s) {
    UtpSocket* conn = static_cast<UtpSocket*>(s);
    if (conn) conn->close();
}
```

---

## 执行顺序

1. **Phase A** — 定义 4 个 struct + 批量字段重命名
2. 编译验证
3. **Phase B** — 独立函数转为成员函数 + C API 薄委托
4. 编译验证
5. 提交

## 风险与注意事项

- 位域分组到不同 struct 后布局变化 → 安全（无 memcpy），但检查 sizeof
- `ida` 保留在 UtpSocket 顶层（仅 2 字段，不归入任何 struct）
- `duplicate_ack_` 保留在 UtpSocket 顶层（跨 send/recv 边界）
- `target_delay_` 移入 ConnectionId（per-socket 配置）
