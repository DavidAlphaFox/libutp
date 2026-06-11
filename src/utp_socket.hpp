/*
 * Copyright (c) 2010-2013 BitTorrent, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

// 本文件定义 uTP（Micro Transport Protocol）Socket 类及相关数据结构
// 包括：UtpSocket 类定义、收发缓冲区数据结构、连接状态枚举、带宽类型枚举等

#include "utp_internal.h"
#include "utp/config.hpp"
#include "utp/ledbat.hpp"
#include "utp/mtu_discovery.hpp"
#include "utp/sequence_buffer.hpp"
#include "utp/delay_history.hpp"
#include "utp/wire_format.hpp"
#include "utp/connection_state.hpp"
#include "utp_callbacks.h"

using namespace utp::config;

enum BandwidthType {
	payload_bandwidth,   // 有效载荷带宽（用户数据）
	connect_overhead,    // 连接建立开销（SYN/SYNACK）
	close_overhead,      // 连接关闭开销（FIN/RST）
	ack_overhead,        // ACK 确认开销
	header_overhead,     // 包头开销（非数据部分）
	retransmit_overhead  // 重传开销
};

enum CONN_STATE {
	CS_UNINITIALIZED = 0,  // 未初始化
	CS_IDLE,               // 空闲（已分配但未连接）
	CS_SYN_SENT,           // 已发送 SYN（主动连接中）
	CS_SYN_RECV,           // 已收到 SYN（被动连接中）
	CS_CONNECTED,          // 已连接
	CS_CONNECTED_FULL,     // 已连接且发送窗口已满
	CS_RESET,              // 已收到 RST，连接被重置
	CS_DESTROY             // 待销毁
};

// 调试日志用的名称表（与 utp::wire::PacketType / CONN_STATE 一一对应）
inline constexpr const char* flagnames[] = {
	"ST_DATA","ST_FIN","ST_STATE","ST_RESET","ST_SYN"
};

inline constexpr const char* statenames[] = {
	"UNINITIALIZED","IDLE","SYN_SENT","SYN_RECV","CONNECTED","CONNECTED_FULL","RESET","DESTROY"
};

struct OutgoingPacket {
	size_t length = 0;             // 数据包总长度（含包头）
	size_t payload = 0;            // 有效载荷长度（用户数据）
	uint64 time_sent = 0;          // 发送时间戳（微秒）
	uint transmissions:31 = 0;     // 已发送次数（含重传）
	bool need_resend:1 = false;    // 是否需要重传
	std::vector<uint8_t> data;     // 数据包内容（含包头和载荷）
};

struct InboundPacket {
	uint32_t size = 0;             // 数据包大小（含包头）
	std::vector<uint8_t> data;     // 数据包内容
};

struct ParsedPacket {
	const byte* payload = nullptr;   // data start (after header + extensions)
	const byte* end = nullptr;       // packet end
	const byte* selack = nullptr;    // SACK extension pointer (NULL if none)
	uint16 seq_nr = 0;              // packet sequence number
	uint16 ack_nr = 0;              // packet ack number
	uint8  type = 0;                // packet type (ST_DATA, ST_STATE, etc.)
	uint32 timestamp = 0;           // sender timestamp (tv_usec)
	uint32 reply_micro = 0;         // our delay as measured by remote
	uint32 windowsize = 0;          // advertised window（线上字段为 32 位，勿用 uint16 截断）
};

// =============================================================================
// 数据分组 struct：将 UtpSocket 的 ~40 个字段按职责归入 4 个纯数据分组。
// 这些 struct 没有行为方法——协议逻辑天然跨 send/receive 边界，
// 因此方法留在 UtpSocket 上，struct 仅用于认知分组和初始化。
// =============================================================================

// 连接标识 + 全局状态
struct ConnectionId {
	utp::Address addr;              // 对端地址
	ISocketHost* host = nullptr;      // 宿主环境(依赖倒置)，由 UtpContext 实现
	uint32 conn_seed = 0;           // 连接种子（用于生成连接ID）
	uint32 conn_id_recv = 0;        // 接收方向连接ID
	uint32 conn_id_send = 0;        // 发送方向连接ID
	void* userdata = nullptr;       // 用户数据指针
	CONN_STATE state = CS_UNINITIALIZED;  // 当前连接状态
	size_t target_delay = 0;        // LEDBAT 目标延迟（微秒）
	byte extensions[8] = {};        // 扩展位标志
};

// 接收侧状态
struct ReceiveState {
	uint16 ack_nr = 0;              // 期望收到的下一个序列号
	uint16 reorder_count = 0;       // 乱序包计数
	uint16 eof_pkt = 0;             // FIN 包的序列号
	bool got_fin : 1 = false;       // 是否收到对端的 FIN 包
	bool got_fin_reached_ : 1 = false;  // 是否已处理到 FIN 包位置
	bool read_shutdown : 1 = false;    // 读端是否已关闭
	size_t opt_rcvbuf = 0;          // 接收缓冲区大小
	size_t last_rcv_win = 0;        // 最后通告的接收窗口大小
	utp::RawSequenceBuffer inbuf;   // 入站序列缓冲区
};

// 发送侧状态
struct SendState {
	uint16 seq_nr = 1;              // 下一个待发送包的序列号
	uint16 cur_window_packets = 0;  // 当前在飞（未确认）的包数量
	bool fin_sent : 1 = false;      // 是否已发送 FIN 包
	bool fin_sent_acked_ : 1 = false;  // 已发送的 FIN 是否已被确认
	bool close_requested_ : 1 = false;  // 用户是否请求关闭连接
	size_t opt_sndbuf = 0;          // 发送缓冲区大小
	size_t max_window_user = 0;     // 用户设定的最大发送窗口
	utp::RawSequenceBuffer outbuf;  // 出站序列缓冲区
};

// 超时 / 延迟测量状态
struct TimingState {
	uint32 reply_micro = 0;         // 延迟反馈值（微秒）
	uint64 last_measured_delay = 0; // 最后一次测量延迟的时间戳
	uint64 last_got_packet = 0;     // 最后一次收到数据包的时间戳
	uint64 last_sent_packet = 0;    // 最后一次发送数据包的时间戳
	bool fast_timeout_ : 1 = false;  // 是否启用快速超时
	uint16 timeout_seq_nr = 0;      // 超时检查时记录的序列号
	uint16 fast_resend_seq_nr = 1;  // 快速重传起始序列号
};

class UtpSocket : public utp::ILogger {
public:
	UtpSocket(ISocketHost* _host);
	~UtpSocket() override;

	// ── 内联工具方法 ──────────────────────────────────

	// ILogger 实现：按 level 过滤，加 "指针 地址 连接ID" 前缀后转发给 context。
	// 变参版 log(level, fmt, ...) 由基类 utp::ILogger 提供，自动转调此处。
	void vlog(int level, char const *fmt, va_list va) override
	{
		char buf[4096], buf2[4096];

		if (!conn_.host->would_log(level)) {
			return;
		}

		vsnprintf(buf, 4096, fmt, va);
		buf[4095] = '\0';

		snprintf(buf2, 4096, "%p %s %06u %s", static_cast<void*>(this), addrfmt(conn_.addr, addrbuf), conn_.conn_id_recv, buf);
		buf2[4095] = '\0';

		// 直接经回调把日志送出（已自行加好前缀，无需再过 log_unchecked）
		utp_call_log(conn_.host->handle(), this, (const byte *)buf2);
	}

	size_t get_rcv_window()
	{
		const size_t numbuf = utp_call_get_read_buffer_size(conn_.host->handle(), this);
		assert((int)numbuf >= 0);
		return recv_.opt_rcvbuf > numbuf ? recv_.opt_rcvbuf - numbuf : 0;
	}

	size_t get_header_size() const
	{
		return sizeof(utp::wire::PacketFormatV1);
	}

	size_t get_udp_mtu()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = conn_.addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_mtu(conn_.host->handle(), this, (const struct sockaddr *)&sa, len);
	}

	size_t get_udp_overhead()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = conn_.addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_overhead(conn_.host->handle(), this, (const struct sockaddr *)&sa, len);
	}

	size_t get_overhead()
	{
		return get_udp_overhead() + get_header_size();
	}

	// ── 外部接口方法（C API 包装 / UtpContext 调用）──

	void schedule_ack();

	void send_ack(bool synack = false);
	void send_keep_alive();
	static void send_rst(UtpContext *ctx,
						 const utp::Address &addr, uint32 conn_id_send,
						 uint16 ack_nr, uint16 seq_nr);

	size_t get_packet_size() const;

	#ifdef _DEBUG
	void check_invariant();
	#endif

	void check_timeouts();
	void remove_from_ack_list();
	void register_recv_packet(size_t len);
	size_t process_incoming(const byte *packet, size_t len, bool syn = false);
	void initialize(const struct sockaddr *addr, socklen_t addrlen,
					bool need_seed_gen, uint32 seed, uint32 id_recv, uint32 id_send);
	int connect(const struct sockaddr *to, socklen_t tolen);
	ssize_t writev(struct utp_iovec *iovec_input, size_t num_iovecs);
	void close();
	void shutdown(int how);
	void read_drained();
	int get_peername(struct sockaddr *addr_out, socklen_t *addrlen);
	int set_option(int opt, int val);
	int get_option(int opt);
	int get_delays(uint32 *ours, uint32 *theirs, uint32 *age);
	utp_socket_stats* get_stats();

	utp_context* context() { return conn_.host->handle(); }
	void* userdata() { return conn_.userdata; }
	void set_userdata(void *userdata) { conn_.userdata = userdata; }

	// ── 供宿主(UtpContext)调用的公开 API（取代原 friend 直接访问私有成员）──

	CONN_STATE state() const { return conn_.state; }
	uint32 conn_id_send() const { return conn_.conn_id_send; }
	uint32 recv_conn_id() const { return conn_.conn_id_recv; }
	const utp::Address& peer_addr() const { return conn_.addr; }

	// ── 连接状态机（State 模式）：按当前状态取无数据状态单例进行行为分派 ──
	static IConnectionState* state_descriptor(CONN_STATE s);
	IConnectionState& fsm() const { return *state_descriptor(conn_.state); }

	// 状态对象使用的公开原语（仅暴露所需操作，避免 friend 直访私有成员）
	void mark_read_shutdown() { recv_.read_shutdown = true; }
	void request_close()      { send_.close_requested_ = true; }
	bool fin_sent() const     { return send_.fin_sent; }
	bool fin_acked() const    { return send_.fin_sent_acked_; }
	void set_conn_state(CONN_STATE s) { conn_.state = s; }
	void send_fin();                 // 标记 fin_sent 并发出 ST_FIN（.cpp）
	void backoff_rto_for_close();    // 关闭时对 SYN_SENT 做 RTO 退避（.cpp）

	// 延迟 ACK 列表：宿主持有容器，通过这两个访问器读写 socket 自身的列表下标
	int  ack_index() const { return ida_; }
	void set_ack_index(int i) { ida_ = i; }

	// 被动接受 SYN：记录对端序号、生成随机发送序号、进入 CS_SYN_RECV
	void accept_syn(uint16 peer_seq) {
		recv_.ack_nr = peer_seq;
		send_.seq_nr = utp_call_get_random(conn_.host->handle(), NULL);
		timing_.fast_resend_seq_nr = send_.seq_nr;
		conn_.state = CS_SYN_RECV;
	}

	// 收到对端 RST：按是否已请求关闭决定 DESTROY/RESET，并上报开销与错误
	void on_reset(size_t recv_len) {
		conn_.state = send_.close_requested_ ? CS_DESTROY : CS_RESET;
		utp_call_on_overhead_statistics(conn_.host->handle(), this, false, recv_len + get_udp_overhead(), close_overhead);
		const int err = (conn_.state == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
		utp_call_on_error(conn_.host->handle(), this, err);
	}

	// ICMP「需要分片」：调整 MTU 搜索并记录日志
	void on_icmp_fragmentation(uint16 next_hop_mtu) {
		if (next_hop_mtu >= 576 && next_hop_mtu < 0x2000)
			mtu_.handle_icmp_fragmentation(next_hop_mtu, conn_.host->current_ms());
		else
			mtu_.handle_icmp_unknown(conn_.host->current_ms());
		log(UTP_LOG_MTU, "MTU [ICMP] floor:%d ceiling:%d current:%d",
			mtu_.floor(), mtu_.ceiling(), mtu_.last());
	}

	// ICMP 错误：CS_IDLE 忽略；否则按是否已关闭转 DESTROY/RESET 并上报错误
	void on_icmp_error() {
		if (conn_.state == CS_IDLE) return;
		const int err = (conn_.state == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
		conn_.state = send_.close_requested_ ? CS_DESTROY : CS_RESET;
		utp_call_on_error(conn_.host->handle(), this, err);
	}

private:
	// ── 内部实现方法（仅被自身成员函数调用）──
	void send_data(byte* b, size_t length, BandwidthType type, uint32 flags = 0);
	void send_packet(OutgoingPacket *pkt);
	bool is_full(int bytes = -1);
	bool flush_packets();
	void write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs);
	int ack_packet(uint16 seq);
	size_t selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt);
	void selective_ack(uint base, const byte *mask, byte len);
	bool parse_packet(const byte* packet, size_t len, ParsedPacket& pp);
	size_t process_acks(const ParsedPacket& pp, int acks, uint64 time);
	void advance_send_window(const ParsedPacket& pp, int acks);
	size_t deliver_data(const ParsedPacket& pp, uint seqnr);

	// ── 数据分组 ──────────────────────────────────────────
	ConnectionId  conn_;            // 连接标识 + 全局状态
	ReceiveState  recv_;            // 接收侧状态
	SendState     send_;            // 发送侧状态
	TimingState   timing_;          // 超时 / 延迟测量

	// ACK 管理（仅 2 字段，跨 send/recv 边界，保留在顶层）
	int   ida_ = -1;                // 延迟ACK列表索引
	byte  duplicate_ack_ = 0;       // 重复ACK计数

	// 已提取的子组件
	MtuDiscovery    mtu_;                            // MTU探测对象
	std::unique_ptr<ICongestionController> cc_;      // 拥塞控制策略（默认 LEDBAT，可替换）

	#ifdef _DEBUG
	utp_socket_stats stats_;        // 统计信息（仅DEBUG模式）
	#endif

};
