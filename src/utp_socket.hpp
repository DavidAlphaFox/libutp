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
	uint16 windowsize = 0;          // advertised window
};

// =============================================================================
// 数据分组 struct：将 UtpSocket 的 ~40 个字段按职责归入 4 个纯数据分组。
// 这些 struct 没有行为方法——协议逻辑天然跨 send/receive 边界，
// 因此方法留在 UtpSocket 上，struct 仅用于认知分组和初始化。
// =============================================================================

// 连接标识 + 全局状态
struct ConnectionId {
	utp::Address addr;              // 对端地址
	UtpContext* ctx = nullptr;      // 所属上下文
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

class UtpSocket {
public:
	UtpSocket(UtpContext* _ctx);
	~UtpSocket();

	// ── 内联工具方法 ──────────────────────────────────

	void log(int level, char const *fmt, ...)
	{
		va_list va;
		char buf[4096], buf2[4096];

		if (!conn_.ctx->would_log(level)) {
			return;
		}

		va_start(va, fmt);
		vsnprintf(buf, 4096, fmt, va);
		va_end(va);
		buf[4095] = '\0';

		snprintf(buf2, 4096, "%p %s %06u %s", this, addrfmt(conn_.addr, addrbuf), conn_.conn_id_recv, buf);
		buf2[4095] = '\0';

		conn_.ctx->log_unchecked(this, buf2);
	}

	size_t get_rcv_window()
	{
		const size_t numbuf = utp_call_get_read_buffer_size(conn_.ctx, this);
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
		return utp_call_get_udp_mtu(conn_.ctx, this, (const struct sockaddr *)&sa, len);
	}

	size_t get_udp_overhead()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = conn_.addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_overhead(conn_.ctx, this, (const struct sockaddr *)&sa, len);
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
						 const utp::Address &addr, uint32 conn_id_send_,
						 uint16 ack_nr_, uint16 seq_nr_);

	size_t get_packet_size() const;

	#ifdef _DEBUG
	void check_invariant();
	#endif

	void check_timeouts();
	void remove_from_ack_list();
	void register_recv_packet(size_t len);
	size_t process_incoming(const byte *packet, size_t len, bool syn = false);
	int connect(const struct sockaddr *to, socklen_t tolen);
	void close();
	void shutdown(int how);
	void read_drained();

	// ── 友元：允许 UtpContext 和 C API 包装函数访问私有成员 ──
	friend class UtpContext;
	friend void utp_initialize_socket(utp_socket*, const struct sockaddr*, socklen_t, bool, uint32, uint32, uint32);
	friend ssize_t utp_writev(utp_socket*, struct utp_iovec*, size_t);
	friend utp_socket* utp_create_socket(utp_context*);
	friend int utp_getpeername(utp_socket*, struct sockaddr*, socklen_t*);
	friend utp_context* utp_get_context(utp_socket*);
	friend void* utp_set_userdata(utp_socket*, void*);
	friend void* utp_get_userdata(utp_socket*);
	friend utp_socket_stats* utp_get_stats(utp_socket*);
	friend int utp_setsockopt(UtpSocket*, int, int);
	friend int utp_getsockopt(UtpSocket*, int);
	friend int utp_get_delays(UtpSocket*, uint32*, uint32*, uint32*);

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
	MtuDiscovery    mtu_;           // MTU探测对象
	LedbatController cc_;           // LEDBAT拥塞控制对象

	#ifdef _DEBUG
	utp_socket_stats stats_;        // 统计信息（仅DEBUG模式）
	#endif

	// ── 兼容性别名（过渡期使用，最终删除）──────────────────
	utp::Address& addr = conn_.addr;
	UtpContext*& ctx = conn_.ctx;
	void*& userdata_ = conn_.userdata;
	uint32& conn_seed_ = conn_.conn_seed;
	uint32& conn_id_recv_ = conn_.conn_id_recv;
	uint32& conn_id_send_ = conn_.conn_id_send;
	CONN_STATE& state_ = conn_.state;
	size_t& target_delay_ = conn_.target_delay;
	byte (&extensions_)[8] = conn_.extensions;

	uint16& ack_nr_ = recv_.ack_nr;
	uint16& reorder_count_ = recv_.reorder_count;
	uint16& eof_pkt_ = recv_.eof_pkt;
	size_t& opt_rcvbuf_ = recv_.opt_rcvbuf;
	size_t& last_rcv_win_ = recv_.last_rcv_win;
	utp::RawSequenceBuffer& inbuf_ = recv_.inbuf;
	// got_fin / got_fin_reached_ / read_shutdown 是位域，直接使用 recv_.xxx

	uint16& seq_nr_ = send_.seq_nr;
	uint16& cur_window_packets_ = send_.cur_window_packets;
	size_t& opt_sndbuf_ = send_.opt_sndbuf;
	size_t& max_window_user_ = send_.max_window_user;
	utp::RawSequenceBuffer& outbuf_ = send_.outbuf;
	// 注意：fin_sent / fin_sent_acked_ / close_requested_ 是位域，使用 send_.xxx

	uint32& reply_micro_ = timing_.reply_micro;
	uint64& last_measured_delay_ = timing_.last_measured_delay;
	uint64& last_got_packet_ = timing_.last_got_packet;
	uint64& last_sent_packet_ = timing_.last_sent_packet;
	uint16& timeout_seq_nr_ = timing_.timeout_seq_nr;
	uint16& fast_resend_seq_nr_ = timing_.fast_resend_seq_nr;
	// 注意：fast_timeout_ 是位域，使用 timing_.fast_timeout
};
