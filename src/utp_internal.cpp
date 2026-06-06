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

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h> // for UINT_MAX
#include <time.h>
#include <vector>
#include <memory>

#include "utp_types.h"
#include "utp_internal.h"
#include "utp/sequence_buffer.hpp"
#include "utp/delay_history.hpp"
#include "utp/wire_format.hpp"

using utp::wrapping_compare_less;
using utp::wire::PacketFormatV1;
using utp::wire::PacketFormatAckV1;
using utp::zeromem;
using std::min;
using std::max;
using std::clamp;

// =============================================================================
// 模块说明: uTP (uTorrent Transport Protocol) 核心协议实现
// -----------------------------------------------------------------------------
// uTP 是 BEP-29 定义的一种基于 UDP 的可靠传输协议,提供类似 TCP 的有序字节流
// 语义,同时通过 LEDBAT 拥塞控制算法尽量少占带宽,把延迟留给交互式应用。
//
// 主要功能模块:
//   1. 连接管理: UtpSocket 状态机 (CS_IDLE / SYN_SENT / SYN_RECV / CONNECTED /
//      CONNECTED_FULL / RESET / DESTROY),SYN/SYN-ACK/FIN 握手和关闭流程。
//   2. 数据收发: 发送侧的 OutgoingPacket 队列、接收侧 InboundPacket 乱序重排
//      缓冲、按序向上层交付 (utp_call_on_read)。
//   3. 拥塞控制: LEDBAT 算法 (apply_ccontrol) 基于排队延迟调整 CWIN,
//      慢启动 -> 拥塞避免 -> RTO 超时降窗的三段式逻辑。
//   4. 超时与重传: RTT/RTTVAR 估算 (RFC 6298 风格),RTO 翻倍,基于 SACK 的
//      快速重传,以及零窗口探测定时器。
//   5. MTU 探测: 二分搜索 mtu_floor_/mtu_ceiling_,根据 ICMP need-frag 反馈收紧。
//
// 关键设计思路:
//   - 单线程事件驱动: 所有数据收发、超时、ACK 调度都通过回调触发,
//     整个库非线程安全,但通过外部加锁可在多线程环境使用。
//   - LEDBAT 拥塞控制: 目标延迟 target_delay_ (默认 100ms),基于"我们测量到
//     的对端延迟 - 我们到对端的延迟"来估算排队延迟,以比 TCP 更"礼貌"的方式
//     让出带宽。
//   - 接收端使用延迟 ACK 合并策略,避免大量纯 ACK 包。
//   - 通过 ICMP need-frag 与主动 MTU 探测结合,在 576~1500 字节间搜索最优 MTU。
// =============================================================================

#define	TIMEOUT_CHECK_INTERVAL	500

// number of bytes to increase max window size by, per RTT. This is
// scaled down linearly proportional to off_target. i.e. if all packets
// in one window have 0 delay, window size will increase by this number.
// Typically it's less. TCP increases one MSS per RTT, which is 1500
#define MAX_CWND_INCREASE_BYTES_PER_RTT 3000
#define CUR_DELAY_SIZE 3
// experiments suggest that a clock skew of 10 ms per 325 seconds
// is not impossible. Reset delay_base every 13 minutes. The clock
// skew is dealt with by observing the delay base in the other
// direction, and adjusting our own upwards if the opposite direction
// delay base keeps going down
#define DELAY_BASE_HISTORY 13
#define MAX_WINDOW_DECAY 100 // ms

#define REORDER_BUFFER_SIZE 32
#define REORDER_BUFFER_MAX_SIZE 1024
#define OUTGOING_BUFFER_MAX_SIZE 1024

#define PACKET_SIZE 1435

// this is the minimum max_window_ value. It can never drop below this
#define MIN_WINDOW_SIZE 10

// if we receive 4 or more duplicate acks, we resend the packet
// that hasn't been acked yet
#define DUPLICATE_ACKS_BEFORE_RESEND 3

// Allow a reception window of at least 3 ack_nr_s behind seq_nr_
// A non-SYN packet with an ack_nr_ difference greater than this is
// considered suspicious and ignored
#define ACK_NR_ALLOWED_WINDOW DUPLICATE_ACKS_BEFORE_RESEND

#define RST_INFO_TIMEOUT 10000
#define RST_INFO_LIMIT 1000
// 29 seconds determined from measuring many home NAT devices
#define KEEPALIVE_INTERVAL 29000


#define SEQ_NR_MASK 0xFFFF
#define ACK_NR_MASK 0xFFFF
#define TIMESTAMP_MASK 0xFFFFFFFF

#define DIV_ROUND_UP(num, denom) ((num + denom - 1) / denom)

// The totals are derived from the following data:
//  45: IPv6 address including embedded IPv4 address
//  11: Scope Id
//   2: Brackets around IPv6 address when port is present
//   6: Port (including colon)
//   1: Terminating null byte
char addrbuf[65];
#define addrfmt(x, s) x.fmt(s, sizeof(s))


// these packet sizes are including the uTP header wich
// is either 20 or 23 bytes depending on version
#define PACKET_SIZE_EMPTY_BUCKET 0
#define PACKET_SIZE_EMPTY 23
#define PACKET_SIZE_SMALL_BUCKET 1
#define PACKET_SIZE_SMALL 373
#define PACKET_SIZE_MID_BUCKET 2
#define PACKET_SIZE_MID 723
#define PACKET_SIZE_BIG_BUCKET 3
#define PACKET_SIZE_BIG 1400
#define PACKET_SIZE_HUGE_BUCKET 4

enum {
	ST_DATA = 0,		// Data packet.
	ST_FIN = 1,			// Finalize the connection. This is the last packet.
	ST_STATE = 2,		// State packet. Used to transmit an ACK with no data.
	ST_RESET = 3,		// Terminate connection forcefully.
	ST_SYN = 4,			// Connect SYN
	ST_NUM_STATES,		// used for bounds checking
};

static const cstr flagnames[] = {
	"ST_DATA","ST_FIN","ST_STATE","ST_RESET","ST_SYN"
};

// CONN_STATE: uTP 套接字状态机。
// 转换关系 (主要路径):
//   CS_UNINITIALIZED (初始) -> utp_initialize_socket -> CS_IDLE
//   CS_IDLE -> utp_connect()  -> CS_SYN_SENT
//   CS_IDLE <- 收到 SYN      -> CS_SYN_RECV
//   CS_SYN_SENT 收到 SYN-ACK  -> CS_CONNECTED
//   CS_SYN_RECV 收到首个 DATA  -> CS_CONNECTED
//   CS_CONNECTED (拥塞窗口满) -> CS_CONNECTED_FULL (反之亦然)
//   CS_CONNECTED* 调用 close  -> CS_DESTROY (释放资源)
//   任何状态收到 RST 或严重错误 -> CS_RESET (回调错误后转为 CS_DESTROY)
enum CONN_STATE {
	CS_UNINITIALIZED = 0,
	CS_IDLE,
	CS_SYN_SENT,
	CS_SYN_RECV,
	CS_CONNECTED,
	CS_CONNECTED_FULL,
	CS_RESET,
	CS_DESTROY
};

static const cstr statenames[] = {
	"UNINITIALIZED", "IDLE","SYN_SENT", "SYN_RECV", "CONNECTED","CONNECTED_FULL","DESTROY_DELAY","RESET","DESTROY"
};

// OutgoingPacket: 发送缓冲区中的一个 uTP 包单元。data 包含 uTP 头 + 负载,
// length 为总长度,payload 为负载长度 (用于流量统计 / CWIN 增减)。
// transmissions 记录发送次数 (首次发送或重传后递增),time_sent 记录最近一次
// 发送的微秒时间戳,RTT 估算和超时重传都依赖它。need_resend 用于标记在
// RTO 触发时已被认定"丢失"、等待下次 flush 时被重新发送。
struct OutgoingPacket {
	size_t length = 0;
	size_t payload = 0;
	uint64 time_sent = 0; // microseconds
	uint transmissions:31 = 0;
	bool need_resend:1 = false;
	std::vector<uint8_t> data;
};

// InboundPacket: 接收侧重排缓冲区中的一个乱序包,等待 ack_nr_ 推进后按序交付。
// data 中只保存包体 (不含 uTP 头),size 为其字节数。
// 这些包只在乱序到达时分配;按序到达的包会直接调用 on_read 而不入队。
struct InboundPacket {
	uint32_t size = 0;
	std::vector<uint8_t> data;
};

// =============================================================================
// UtpSocket: 一次 uTP 连接的全部运行时状态,包含协议字段、缓冲、计时器、拥塞
// 控制参数等。结构体非常"扁平",以便单线程事件循环中快速访问。
// 下面的成员按职责分组 (与字段在结构体中出现的顺序基本一致):
//   - 标识/地址
//   - ACK 列表 / 重传 / 乱序 计数
//   - 发送窗口 / 接收缓冲上限 / 目标延迟
//   - 连接状态 / FIN / 关闭相关布尔位
//   - 对端通告窗口 / 状态机当前状态
//   - 序列号 (ack_nr_ / seq_nr_ / eof_pkt_ / fast_resend_seq_nr_)
//   - 延迟采样 (reply_micro_ / last_* / clock_drift_ / average_delay_)
//   - RTT 估算 (rtt / rtt_var / rto / rto_timeout_)
//   - 拥塞控制 (slow_start_ / ssthresh_ / max_window_)
//   - 接收/发送环形缓冲区 (inbuf_ / outbuf_)
//   - MTU 探测 (mtu_*)
//   - 用户数据指针 (userdata_) / 调试统计 (stats_)
// =============================================================================
class UtpSocket {
public:
	UtpSocket(utp_context* _ctx);
	~UtpSocket();

	// -- 标识/地址: 对端压缩地址 + 所属上下文 --
	utp::Address addr;
	utp_context *ctx;

	// ida: 套接字在 ctx->ack_sockets_ 列表中的下标,-1 表示不在列表中。
	// 删除套接字时通过"与末尾元素交换"实现 O(1) 摘除,因此需要记录位置。
	int ida; //for ack socket list

	// -- 计数: 重传、乱序、重复 ACK --
	uint16 retransmit_count_;

	// reorder_count_: 接收侧等待重排 (尚未按序交付) 的包数,用于决定 ACK 是否带 EACK。
	uint16 reorder_count_;
	// duplicate_ack_: 收到的连续重复 ACK 计数,达到阈值时触发快速重传。
	byte duplicate_ack_;
	// 当前需要重新被发送的数据包数量,最旧的数据包应为seq_nr_ - cur_window_packets_
	// the number of packets in the send queue. Packets that haven't
	// yet been sent count as well as packets marked as needing resend
	// the oldest un-acked packet in the send queue is seq_nr_ - cur_window_packets_
	uint16 cur_window_packets_;

	// -- 发送/接收窗口: 字节数级别 + 上限配置 --
	// how much of the window is used, number of bytes in-flight
	// packets that have not yet been sent do not count, packets
	// that are marked as needing to be re-sent (due to a timeout)
	// don't count either
	size_t cur_window_;
	// maximum window size, in bytes
	size_t max_window_;
	// UTP_SNDBUF setting, in bytes
	size_t opt_sndbuf_;
	// UTP_RCVBUF setting, in bytes
	size_t opt_rcvbuf_;

	// -- LEDBAT 目标延迟 (微秒) --
	// this is the target delay, in microseconds
	// for this socket. defaults to 100000.
	size_t target_delay_;

	// -- FIN / 关闭 状态位: 用于优雅关闭流程 --
	// Is a FIN packet in the reassembly buffer?
	bool got_fin:1;
	// Have we reached the FIN?
	bool got_fin_reached_:1;

	// Have we sent our FIN?
	bool fin_sent:1;
	// Has our fin been ACKed?
	bool fin_sent_acked_:1;

	// Reading is disabled
	bool read_shutdown_:1;
	// User called utp_close()
	bool close_requested_:1;

	// Timeout procedure
	bool fast_timeout_:1;

	// -- 接收端"对方允许的窗口" / 当前状态机 / 上次降窗时间 --
	// max receive window for other end, in bytes
	size_t max_window_user_;
	CONN_STATE state_;
	// TickCount when we last decayed window (wraps)
	int64 last_rwin_decay_;

	// -- 序列号空间: 16 位回绕 --
	// the sequence number of the FIN packet. This field is only set
	// when we have received a FIN, and the flag field has the FIN flag set.
	// it is used to know when it is safe to destroy the socket, we must have
	// received all packets up to this sequence number first.
	uint16 eof_pkt_;

	// All sequence numbers up to including this have been properly received
	// by us
	uint16 ack_nr_;
	// This is the sequence number for the next packet to be sent.
	uint16 seq_nr_;

	uint16 timeout_seq_nr_;

	// This is the sequence number of the next packet we're allowed to
	// do a fast resend with. This makes sure we only do a fast-resend
	// once per packet. We can resend the packet with this sequence number
	// or any later packet (with a higher sequence number).
	uint16 fast_resend_seq_nr_;

	// -- 延迟采样与时间戳 --
	// reply_micro_: 最近一次收到包时测得的"对端到我们"单向延迟,会回填到下个
	// 出向包的 reply_micro_ 字段,供对端计算反向延迟。
	uint32 reply_micro_;

	// last_got_packet_ / last_sent_packet_: 最近一次收/发包的时间,用于 keep-alive。
	uint64 last_got_packet_;
	uint64 last_sent_packet_;
	uint64 last_measured_delay_;

	// timestamp of the last time the cwnd was full
	// this is used to prevent the congestion window
	// from growing when we're not sending at capacity
	mutable uint64 last_maxed_out_window_;

	// userdata_: 用户通过 utp_set_userdata 关联的自定义数据,供回调使用。
	void *userdata_;

	// -- RTT 估算 (单位: 毫秒) 与 RTO 计时器 --
	// Round trip time
	uint rtt;
	// Round trip time variance
	uint rtt_var;
	// Round trip timeout
	uint rto;
	utp::DelayHistory rtt_hist_;
	// retransmit_timeout_: 当前 RTO 重传退避值,首次超时后翻倍。
	uint retransmit_timeout_;
	// The RTO timer will timeout here.
	uint64 rto_timeout_;
	// When the window size is set to zero, start this timer. It will send a new packet every 30secs.
	uint64 zerowindow_time_;

	// -- 连接标识 --
	uint32 conn_seed_;
	// Connection ID for packets I receive
	uint32 conn_id_recv_;
	// Connection ID for packets I send
	uint32 conn_id_send_;
	// Last rcv window we advertised, in bytes
	size_t last_rcv_win_;

	// -- 双向延迟历史 (用于 LEDBAT 排队延迟估算) --
	// our_hist_: 对方测得的"我们到对方"的延迟;their_hist_: 我们测得的"对方到我们"的延迟。
	// 两者相减可估计"我们方向上的排队延迟",作为 LEDBAT 调窗的反馈信号。
	utp::DelayHistory our_hist_;
	utp::DelayHistory their_hist_;

	// extension bytes from SYN packet
	byte extensions_[8];

	// -- MTU 探测 (Path MTU Discovery) --
	// mtu_discover_time_: 下一次重置 MTU 搜索的截止时间。
	// time when we should restart the MTU discovery
	uint64 mtu_discover_time_;
	// ceiling and floor of binary search. last is the mtu size
	// we're currently using
	uint32 mtu_ceiling_, mtu_floor_, mtu_last_;
	// we only ever have a single probe in flight at any given time.
	// this is the sequence number of that probe, and the size of
	// that packet
	uint32 mtu_probe_seq_, mtu_probe_size_;

	// -- 长时窗延迟统计 (5 秒粒度), 用于检测时钟漂移 --
	// this is the average delay samples, as compared to the initial
	// sample. It's averaged over 5 seconds
	int32 average_delay_;
	// this is the sum of all the delay samples
	// we've made recently. The important distinction
	// of these samples is that they are all made compared
	// to the initial sample, this is to deal with
	// wrapping in a simple way.
	int64 current_delay_sum_;
	// number of sample ins current_delay_sum_
	int current_delay_samples_;
	// initialized to 0, set to the first raw delay sample
	// each sample that's added to current_delay_sum_
	// is subtracted from the value first, to make it
	// a delay relative to this sample
	uint32 average_delay_base_;
	// the next time we should add an average delay
	// sample into average_delay_hist
	uint64 average_sample_time_;
	// the estimated clock drift between our computer
	// and the endpoint computer. The unit is microseconds
	// per 5 seconds
	int32 clock_drift_;
	// just used for logging
	int32 clock_drift_raw_;

	// -- 收发环形缓冲 (乱序重排 + 发送重传) --
	// inbuf_: 接收侧乱序缓冲,按 16 位 seq_nr_ 索引;outbuf_: 发送侧包队列。
	utp::RawSequenceBuffer inbuf_, outbuf_;

	#ifdef _DEBUG
	// Public per-socket statistics, returned by utp_get_stats()
	utp_socket_stats stats_;
	#endif

	// -- 拥塞控制状态 --
	// slow_start_: 是否处于慢启动 (指数增长) 阶段;一旦排队延迟接近目标或窗口
	// 超过 ssthresh_,则切换到 LEDBAT 拥塞避免阶段 (线性增长 / 减窗)。
	// true if we're in slow-start (exponential growth) phase
	bool slow_start_;

	// ssthresh_: 慢启动阈值,首次超时或重复 ACK 检测到丢包时被更新。
	// the slow-start threshold, in bytes
	size_t ssthresh_;

	void log(int level, char const *fmt, ...)
	{
		va_list va;
		char buf[4096], buf2[4096];

		// don't bother with vsnprintf() etc calls if we're not going to log.
		if (!ctx->would_log(level)) {
			return;
		}

		va_start(va, fmt);
		vsnprintf(buf, 4096, fmt, va);
		va_end(va);
		buf[4095] = '\0';

		snprintf(buf2, 4096, "%p %s %06u %s", this, addrfmt(addr, addrbuf), conn_id_recv_, buf);
		buf2[4095] = '\0';

		ctx->log_unchecked(this, buf2);
	}

	void schedule_ack();

	// called every time mtu_floor_ or mtu_ceiling_ are adjusted
	void mtu_search_update();
	void mtu_reset();

	// Calculates the current receive window
	size_t get_rcv_window()
	{
		// Trim window down according to what's already in buffer.
		const size_t numbuf = utp_call_get_read_buffer_size(this->ctx, this);
		assert((int)numbuf >= 0);
		return opt_rcvbuf_ > numbuf ? opt_rcvbuf_ - numbuf : 0;
	}

	// Test if we're ready to decay max_window_
	// XXX this breaks when spaced by > INT_MAX/2, which is 49
	// days; the failure mode in that case is we do an extra decay
	// or fail to do one when we really shouldn't.
	bool can_decay_win(int64 msec) const
	{
                return (msec - last_rwin_decay_) >= MAX_WINDOW_DECAY;
	}

	// If we can, decay max window, returns true if we actually did so
	void maybe_decay_win(uint64 current_ms)
	{
		if (can_decay_win(current_ms)) {
			// TCP uses 0.5
			max_window_ = (size_t)(max_window_ * .5);
			last_rwin_decay_ = current_ms;
			if (max_window_ < MIN_WINDOW_SIZE)
				max_window_ = MIN_WINDOW_SIZE;
			slow_start_ = false;
			ssthresh_ = max_window_;
		}
	}

	size_t get_header_size() const
	{
		return sizeof(PacketFormatV1);
	}

	size_t get_udp_mtu()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_mtu(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	size_t get_udp_overhead()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_overhead(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	size_t get_overhead()
	{
		return get_udp_overhead() + get_header_size();
	}

	void send_data(byte* b, size_t length, BandwidthType type, uint32 flags = 0);

	void send_ack(bool synack = false);

	void send_keep_alive();

	static void send_rst(utp_context *ctx,
						 const utp::Address &addr, uint32 conn_id_send_,
						 uint16 ack_nr_, uint16 seq_nr_);

	void send_packet(OutgoingPacket *pkt);

	bool is_full(int bytes = -1);
	bool flush_packets();
	void write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs);

	#ifdef _DEBUG
	void check_invariant();
	#endif

	void check_timeouts();
	int ack_packet(uint16 seq);
	size_t selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt);
	void selective_ack(uint base, const byte *mask, byte len);
	void apply_ccontrol(size_t bytes_acked, uint32 actual_delay, int64 min_rtt);
	size_t get_packet_size() const;
};

void remove_socket_from_ack_list(UtpSocket *conn)
{
	if (conn->ida >= 0)
	{
		UtpSocket *last = conn->ctx->ack_sockets_.back();

		assert(last->ida < (int)(conn->ctx->ack_sockets_.size()));
		assert(conn->ctx->ack_sockets_[last->ida] == last);
		last->ida = conn->ida;
		conn->ctx->ack_sockets_[conn->ida] = last;
		conn->ida = -1;

		// Decrease the count
		conn->ctx->ack_sockets_.pop_back();
	}
}

static void utp_register_sent_packet(utp_context *ctx, size_t length)
{
	if (length <= PACKET_SIZE_MID) {
		if (length <= PACKET_SIZE_EMPTY) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (length <= PACKET_SIZE_SMALL) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_MID_BUCKET]++;
	} else {
		if (length <= PACKET_SIZE_BIG) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_BIG_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

void send_to_addr(utp_context *ctx, const byte *p, size_t len, const utp::Address &addr, int flags = 0)
{
	socklen_t tolen;
	SOCKADDR_STORAGE to = addr.get_sockaddr_storage(&tolen);
	utp_register_sent_packet(ctx, len);
	utp_call_sendto(ctx, NULL, p, len, (const struct sockaddr *)&to, tolen, flags);
}

void UtpSocket::schedule_ack()
{
	//将自己放到发送序列中去就结束了
	if (ida == -1){
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack");
		#endif
		ctx->ack_sockets_.push_back(this); ida = ctx->ack_sockets_.size() - 1;
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack: already in list");
		#endif
	}
}

void UtpSocket::send_data(byte* b, size_t length, BandwidthType type, uint32 flags)
{
	// time stamp this packet with local time, the stamp goes into
	// the header of every packet at the 8th byte for 8 bytes :
	// two integers, check packet.h for more
	uint64 time = utp_call_get_microseconds(ctx, this);

	PacketFormatV1* b1 = (PacketFormatV1*)b;
	b1->tv_usec = (uint32)time;
	b1->reply_micro = reply_micro_;

	last_sent_packet_ = ctx->current_ms_;

	#ifdef _DEBUG
	stats_.nbytes_xmit += length;
	++stats_.nxmit;
	#endif

	if (ctx->callbacks_[UTP_ON_OVERHEAD_STATISTICS]) {
		size_t n;
		if (type == payload_bandwidth) {
			// if this packet carries payload, just
			// count the header as overhead
			type = header_overhead;
			n = get_overhead();
		} else {
			n = length + get_udp_overhead();
		}
		utp_call_on_overhead_statistics(ctx, this, true, n, type);
	}
#if UTP_DEBUG_LOGGING
	int flags2 = b1->type();
	uint16 seq_nr_ = b1->seq_nr;
	uint16 ack_nr_ = b1->ack_nr;
	log(UTP_LOG_DEBUG, "send %s len:%u id:%u timestamp:" I64u " reply_micro_:%u flags:%s seq_nr_:%u ack_nr_:%u",
		addrfmt(addr, addrbuf), (uint)length, conn_id_send_, time, reply_micro_, flagnames[flags2],
		seq_nr_, ack_nr_);
#endif
	send_to_addr(ctx, b, length, addr, flags);
	remove_socket_from_ack_list(this);
}

void UtpSocket::send_ack(bool synack)
{
	PacketFormatAckV1 pfa;
	zeromem(&pfa);

	size_t len;
	last_rcv_win_ = get_rcv_window();
	pfa.pf.set_version(1);
	pfa.pf.set_type(ST_STATE);
	pfa.pf.ext = 0;
	pfa.pf.connid = conn_id_send_;
	pfa.pf.ack_nr = ack_nr_;
	pfa.pf.seq_nr = seq_nr_;
	pfa.pf.windowsize = (uint32)last_rcv_win_;
	len = sizeof(PacketFormatV1);

	// we never need to send EACK for connections
	// that are shutting down
	if (reorder_count_ != 0 && !got_fin_reached_) {
		// if reorder count > 0, send an EACK.
		// reorder count should always be 0
		// for synacks, so this should not be
		// as synack
		assert(!synack);
		pfa.pf.ext = 1;
		pfa.ext_next = 0;
		pfa.ext_len = 4;
		uint m = 0;

		// reorder count should only be non-zero
		// if the packet ack_nr_ + 1 has not yet
		// been received
		assert(inbuf_.get(ack_nr_ + 1) == NULL);
		size_t window = min<size_t>(14+16, inbuf_.size());
		// Generate bit mask of segments received.
		for (size_t i = 0; i < window; i++) {
			if (inbuf_.get(ack_nr_ + i + 2) != NULL) {
				m |= 1 << i;

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "EACK packet [%u]", ack_nr_ + i + 2);
				#endif
			}
		}
		pfa.acks[0] = (byte)m;
		pfa.acks[1] = (byte)(m >> 8);
		pfa.acks[2] = (byte)(m >> 16);
		pfa.acks[3] = (byte)(m >> 24);
		len += 4 + 2;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending EACK %u [%u] bits:[%032b]", ack_nr_, conn_id_send_, m);
		#endif
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending ACK %u [%u]", ack_nr_, conn_id_send_);
		#endif
	}

	send_data((byte*)&pfa, len, ack_overhead);
	remove_socket_from_ack_list(this);
}

void UtpSocket::send_keep_alive()
{
	ack_nr_--;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Sending KeepAlive ACK %u [%u]", ack_nr_, conn_id_send_);
	#endif
	//发送一个ack包
	send_ack();
	ack_nr_++;
}

void UtpSocket::send_rst(utp_context *ctx,
	const utp::Address &addr, uint32 conn_id_send_, uint16 ack_nr_, uint16 seq_nr_)
{
	PacketFormatV1 pf1;
	zeromem(&pf1);

	size_t len;
	pf1.set_version(1);
	pf1.set_type(ST_RESET);
	pf1.ext = 0;
	pf1.connid = conn_id_send_;
	pf1.ack_nr = ack_nr_;
	pf1.seq_nr = seq_nr_;
	pf1.windowsize = 0;
	len = sizeof(PacketFormatV1);

//	LOG_DEBUG("%s: Sending RST id:%u seq_nr_:%u ack_nr_:%u", addrfmt(addr, addrbuf), conn_id_send_, seq_nr_, ack_nr_);
//	LOG_DEBUG("send %s len:%u id:%u", addrfmt(addr, addrbuf), (uint)len, conn_id_send_);
	send_to_addr(ctx, (const byte*)&pf1, len, addr);
}

// send_packet: 把 outbuf_ 中已就绪的一个 OutgoingPacket 通过 send_data 真正发出。
// 关键逻辑:
//   - 首次发送或被标记 need_resend 时,才把负载计入 cur_window_ (CWIN 配额)。
//   - 把当前 ack_nr_ 写入包头, 同时记录 time_sent, 供 ack_packet 中做 RTT 估算。
//   - 如果在 MTU 探测窗口内, 可顺便将该包作为探针 (DON'T FRAGMENT) 发出。
//   - transmissions 计数递增, 用于 RTO 计算和日志统计。
void UtpSocket::send_packet(OutgoingPacket *pkt)
{
	// only count against the quota the first time we
	// send the packet. Don't enforce quota when closing
	// a socket. Only enforce the quota when we're sending
	// at slow rates (max window < packet size)

	//size_t max_send = min(max_window_, opt_sndbuf_, max_window_user_);
	time_t cur_time = utp_call_get_milliseconds(this->ctx, this);

	if (pkt->transmissions == 0 || pkt->need_resend) {
		cur_window_ += pkt->payload;
	}

	pkt->need_resend = false;

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
	p1->ack_nr = ack_nr_;
	pkt->time_sent = utp_call_get_microseconds(this->ctx, this);

	//socklen_t salen;
	//SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&salen);
	bool use_as_mtu_probe = false;

	// TODO: this is subject to nasty wrapping issues! Below as well
 	if (mtu_discover_time_ < (uint64)cur_time) {
		// it's time to reset our MTU assupmtions
		// and trigger a new search
		mtu_reset();
	}

	// don't use packets that are larger then mtu_ceiling_
	// as probes, since they were probably used as probes
	// already and failed, now we need it to fragment
	// just to get it through
	// if seq_nr_ == 1, the probe would end up being 0
	// which is a magic number representing no-probe
	// that why we don't send a probe for a packet with
	// sequence number 0
 	if (mtu_floor_ < mtu_ceiling_
		&& pkt->length > mtu_floor_
		&& pkt->length <= mtu_ceiling_
		&& mtu_probe_seq_ == 0
		&& seq_nr_ != 1
		&& pkt->transmissions == 0) {

		// we've already incremented seq_nr_
		// for this packet
 		mtu_probe_seq_ = (seq_nr_ - 1) & ACK_NR_MASK;
 		mtu_probe_size_ = pkt->length;
		assert(pkt->length >= mtu_floor_);
		assert(pkt->length <= mtu_ceiling_);
 		use_as_mtu_probe = true;
		log(UTP_LOG_MTU, "MTU [PROBE] floor:%d ceiling:%d current:%d"
			, mtu_floor_, mtu_ceiling_, mtu_probe_size_);
 	}

	pkt->transmissions++;
	send_data((byte*)pkt->data.data(), pkt->length,
		(state_ == CS_SYN_SENT) ? connect_overhead
		: (pkt->transmissions == 1) ? payload_bandwidth
		: retransmit_overhead, use_as_mtu_probe ? UTP_UDP_DONTFRAG : 0);
}

bool UtpSocket::is_full(int bytes)
{
	size_t packet_size = get_packet_size();
	if (bytes < 0) bytes = packet_size;
	else if (bytes > (int)packet_size) bytes = (int)packet_size;
	size_t max_send = min(min(max_window_, opt_sndbuf_), max_window_user_);

	// subtract one to save space for the FIN packet
	if (cur_window_packets_ >= OUTGOING_BUFFER_MAX_SIZE - 1) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "is_full:false cur_window_packets_:%d MAX:%d", cur_window_packets_, OUTGOING_BUFFER_MAX_SIZE - 1);
		#endif

		last_maxed_out_window_ = ctx->current_ms_;
		return true;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "is_full:%s. cur_window_:%u pkt:%u max:%u cur_window_packets_:%u max_window_:%u"
		, (cur_window_ + bytes > max_send) ? "true" : "false"
		, cur_window_, bytes, max_send, cur_window_packets_
		, max_window_);
	#endif

	if (cur_window_ + bytes > max_send) {
		last_maxed_out_window_ = ctx->current_ms_;
		return true;
	}
	return false;
}

bool UtpSocket::flush_packets()
{
	size_t packet_size = get_packet_size();
	//全力向外发送
	// send packets that are waiting on the pacer to be sent
	// i has to be an unsigned 16 bit counter to wrap correctly
	// signed types are not guaranteed to wrap the way you expect
	for (uint16 i = seq_nr_ - cur_window_packets_; i != seq_nr_; ++i) {
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(i);
		if (pkt == 0 || (pkt->transmissions > 0 && pkt->need_resend == false)) continue;
		// have we run out of quota?
		if (is_full()) return true;

		// Nagle check
		// don't send the last packet if we have one packet in-flight
		// and the current packet is still smaller than packet_size.
		if (i != ((seq_nr_ - 1) & ACK_NR_MASK) ||
			cur_window_packets_ == 1 ||
			pkt->payload >= packet_size) {
			send_packet(pkt);
		}
	}
	return false;
}

// @payload: number of bytes to send
// @flags: either ST_DATA, or ST_FIN
// @iovec: base address of iovec array
// @num_iovecs: number of iovecs in array
void UtpSocket::write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs)
{
	// Setup initial timeout timer
	if (cur_window_packets_ == 0) {
		retransmit_timeout_ = rto;
		rto_timeout_ = ctx->current_ms_ + retransmit_timeout_;
		assert(cur_window_ == 0);
	}

	size_t packet_size = get_packet_size();
	do {
		assert(cur_window_packets_ < OUTGOING_BUFFER_MAX_SIZE);
		assert(flags == ST_DATA || flags == ST_FIN);

		size_t added = 0;

		OutgoingPacket *pkt = NULL;

		if (cur_window_packets_ > 0) {
			pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - 1);
		}

		const size_t header_size = get_header_size();
		bool append = true;

		// if there's any room left in the last packet in the window
		// and it hasn't been sent yet, fill that frame first
		if (payload && pkt && !pkt->transmissions && pkt->payload < packet_size) {
			// Use the previous unsent packet
			added = min(payload + pkt->payload, max<size_t>(packet_size, pkt->payload)) - pkt->payload;
			pkt->data.resize(header_size + pkt->payload + added);
			outbuf_.put(seq_nr_ - 1, pkt);
			append = false;
			assert(!pkt->need_resend);
		} else {
			// Create the packet to send.
			added = payload;
			pkt = new OutgoingPacket();
			pkt->data.resize(header_size + added);
			pkt->payload = 0;
			pkt->transmissions = 0;
			pkt->need_resend = false;
		}

		if (added) {
			assert(flags == ST_DATA);

			// Fill it with data from the upper layer.
			unsigned char *p = pkt->data.data() + header_size + pkt->payload;
			size_t needed = added;

			/*
			while (needed) {
				*p = *(char*)iovec[0].iov_base;
				p++;
				iovec[0].iov_base = (char *)iovec[0].iov_base + 1;
				needed--;
			}
			*/

			for (size_t i = 0; i < num_iovecs && needed; i++) {
				if (iovec[i].iov_len == 0)
					continue;

				size_t num = min<size_t>(needed, iovec[i].iov_len);
				memcpy(p, iovec[i].iov_base, num);

				p += num;

				iovec[i].iov_len -= num;
				iovec[i].iov_base = (byte*)iovec[i].iov_base + num;	// iovec[i].iov_base += num, but without void* pointers
				needed -= num;
			}

			assert(needed == 0);
		}
		pkt->payload += added;
		pkt->length = header_size + pkt->payload;

		last_rcv_win_ = get_rcv_window();

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
		p1->set_version(1);
		p1->set_type(flags);
		p1->ext = 0;
		p1->connid = conn_id_send_;
		p1->windowsize = (uint32)last_rcv_win_;
		p1->ack_nr = ack_nr_;

		if (append) {
			// Remember the message in the outgoing queue.
			outbuf_.ensure_size(seq_nr_, cur_window_packets_);
			outbuf_.put(seq_nr_, pkt);
			p1->seq_nr = seq_nr_;
			seq_nr_++;
			cur_window_packets_++;
		}

		payload -= added;

	} while (payload);

	flush_packets();
}

#ifdef _DEBUG
void UtpSocket::check_invariant()
{
	if (reorder_count_ > 0) {
		assert(inbuf_.get(ack_nr_ + 1) == NULL);
	}

	size_t outstanding_bytes = 0;
	for (int i = 0; i < cur_window_packets_; ++i) {
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - i - 1);
		if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
		outstanding_bytes += pkt->payload;
	}
	assert(outstanding_bytes == cur_window_);
}
#endif

// check_timeouts: 由 utp_check_timeouts (context 级别, 每 500ms 一次) 调用,
// 负责检查单个套接字上的超时、keep-alive、可写性事件, 是协议"心跳"的核心。
// 主要职责:
//   1. 若 RTO 到期: RTO 翻倍、判定所有在飞包为丢失、降窗、必要时销毁套接字
//      (SYN_SENT 失败 2 次 / 已连接 4 次重传都算失败) 并立即重发最老包。
//   2. 处理零窗口定时器 (zerowindow_time_): 15s 探测一次。
//   3. 若窗口出现空余, 把 CONNECTED_FULL 回退到 CONNECTED 并回调 on_state_change。
//   4. 距上次发包超过 29s, 发送 keep-alive ACK 以维持 NAT 映射。
//   5. MTU 探测包自身超时时, 收紧 mtu_ceiling_ 但不计入丢包, 加速二分搜索。
void UtpSocket::check_timeouts()
{
	#ifdef _DEBUG
	check_invariant();
	#endif

	// this invariant should always be true
	assert(cur_window_packets_ == 0 || outbuf_.get(seq_nr_ - cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "CheckTimeouts timeout:%d max_window_:%u cur_window_:%u "
			 "state:%s cur_window_packets_:%u",
			 (int)(rto_timeout_ - ctx->current_ms_), (uint)max_window_, (uint)cur_window_,
			 statenames[state_], cur_window_packets_);
	#endif
	//如果是DSTROY了，清理包
	if (state_ != CS_DESTROY) flush_packets();

	switch (state_) {
	case CS_SYN_SENT:
	case CS_SYN_RECV:
	case CS_CONNECTED_FULL:
	case CS_CONNECTED: {

		// Reset max window...
		// 如果出现zerowindow了，重新设置接收方的Window
		if ((int)(ctx->current_ms_ - zerowindow_time_) >= 0 && max_window_user_ == 0) {
			max_window_user_ = PACKET_SIZE;
		}

		if ((int)(ctx->current_ms_ - rto_timeout_) >= 0
			&& rto_timeout_ > 0) { //需要进行重传

			bool ignore_loss = false;
			// mtu的探测包丢失了
			if (cur_window_packets_ == 1
				&& ((seq_nr_ - 1) & ACK_NR_MASK) == mtu_probe_seq_
				&& mtu_probe_seq_ != 0) {
				// we only had  a single outstanding packet that timed out, and it was the probe
				mtu_ceiling_ = mtu_probe_size_ - 1;
				mtu_search_update();
				// this packet was most likely dropped because the packet size being
				// too big and not because congestion. To accelerate the binary search for
				// the MTU, resend immediately and don't reset the window size
				ignore_loss = true;
				log(UTP_LOG_MTU, "MTU [PROBE-TIMEOUT] floor:%d ceiling:%d current:%d"
					, mtu_floor_, mtu_ceiling_, mtu_last_);
			}
			// we dropepd the probe, clear these fields to
			// allow us to send a new one
			mtu_probe_seq_ = mtu_probe_size_ = 0;
			log(UTP_LOG_MTU, "MTU [TIMEOUT]");

			/*
			OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);

			// If there were a lot of retransmissions, force recomputation of round trip time
			if (pkt->transmissions >= 4)
				rtt = 0;
			*/
			//如果是mtu探测包，就不进行RTO翻倍
			// Increase RTO
			const uint new_timeout = ignore_loss ? retransmit_timeout_ : retransmit_timeout_ * 2;

			// They initiated the connection but failed to respond before the rto. 
			// A malicious client can also spoof the destination address of a ST_SYN bringing us to this state.
			// Kill the connection and do not notify the upper layer
			if (state_ == CS_SYN_RECV) { //链接建立失败了，因为在RTO时间内，我们还没收到DATA
				state_ = CS_DESTROY;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			//我们建立的链接，但是已经重传了2次，或者进行了4次数据重传
			// We initiated the connection but the other side failed to respond before the rto
			if (retransmit_count_ >= 4 || (state_ == CS_SYN_SENT && retransmit_count_ >= 2)) {
				// 4 consecutive transmissions have timed out. Kill it. If we
				// haven't even connected yet, give up after only 2 consecutive
				// failed transmissions.
				if (close_requested_) //根据是否关闭，我们改变状态
					state_ = CS_DESTROY;
				else
					state_ = CS_RESET;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			//更新RTO的时间
			retransmit_timeout_ = new_timeout;
			rto_timeout_ = ctx->current_ms_ + new_timeout;

			if (!ignore_loss) {
				// On Timeout
				duplicate_ack_ = 0;

				int packet_size = get_packet_size();

				if ((cur_window_packets_ == 0) && ((int)max_window_ > packet_size)) {
					// we don't have any packets in-flight, even though
					// we could. This implies that the connection is just
					// idling. No need to be aggressive about resetting the
					// congestion window. Just let it decay by a 3:rd.
					// don't set it any lower than the packet size though
					// 没有数据发送，我们将减小发送窗口
					max_window_ = max(max_window_ * 2 / 3, size_t(packet_size));
				} else {
					// our delay was so high that our congestion window
					// was shrunk below one packet, preventing us from
					// sending anything for one time-out period. Now, reset
					// the congestion window to fit one packet, to start over
					// again
					// 重置窗口，进行慢启动
					max_window_ = packet_size;
					slow_start_ = true;
				}
			}
			// 我们可以认为所有的packet都已经丢失了
			// every packet should be considered lost
			for (int i = 0; i < cur_window_packets_; ++i) {
				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - i - 1);
				if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
				pkt->need_resend = true;
				assert(cur_window_ >= pkt->payload);
				cur_window_ -= pkt->payload;
			}
			//如果当前窗口不为0，将重传数量增加
			if (cur_window_packets_ > 0) {
				retransmit_count_++;
				// used in parse_log.py
				log(UTP_LOG_NORMAL, "Packet timeout. Resend. seq_nr_:%u. timeout:%u "
					"max_window_:%u cur_window_packets_:%d"
					, seq_nr_ - cur_window_packets_, retransmit_timeout_
					, (uint)max_window_, int(cur_window_packets_));

				fast_timeout_ = true;
				timeout_seq_nr_ = seq_nr_;

				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);
				assert(pkt);
				//立刻重传最老的一个包
				// Re-send the packet.
				send_packet(pkt);
			}
		}

		// Mark the socket as writable. If the cwnd has grown, or if the number of
		// bytes in-flight is lower than cwnd, we need to make the socket writable again
		// in case it isn't
		if (state_ == CS_CONNECTED_FULL && !is_full()) {
			state_ = CS_CONNECTED;
			//我们从全满状态，恢复到常态
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
				(uint)max_window_, (uint)cur_window_, (uint)get_packet_size());
			#endif
			utp_call_on_state_change(this->ctx, this, UTP_STATE_WRITABLE);
		}
		// 发送心跳包
		if (state_ >= CS_CONNECTED && !fin_sent) {
			if ((int)(ctx->current_ms_ - last_sent_packet_) >= KEEPALIVE_INTERVAL) {
				send_keep_alive();
			}
		}
		break;
	}

	// prevent warning
	case CS_UNINITIALIZED:
	case CS_IDLE:
	case CS_RESET:
	case CS_DESTROY:
		break;
	}
}

// this should be called every time we change mtu_floor_ or mtu_ceiling_
void UtpSocket::mtu_search_update()
{
	assert(mtu_floor_ <= mtu_ceiling_);

	// binary search
	mtu_last_ = (mtu_floor_ + mtu_ceiling_) / 2;

	// enable a new probe to be sent
	mtu_probe_seq_ = mtu_probe_size_ = 0;

	// if the floor and ceiling are close enough, consider the
	// MTU binary search complete. We set the current value
	// to floor since that's the only size we know can go through
	// also set the ceiling to floor to terminate the searching
	if (mtu_ceiling_ - mtu_floor_ <= 16) {
		mtu_last_ = mtu_floor_;
		log(UTP_LOG_MTU, "MTU [DONE] floor:%d ceiling:%d current:%d"
			, mtu_floor_, mtu_ceiling_, mtu_last_);
		mtu_ceiling_ = mtu_floor_;
		assert(mtu_floor_ <= mtu_ceiling_);
		// Do another search in 30 minutes
		mtu_discover_time_ = utp_call_get_milliseconds(this->ctx, this) + 30 * 60 * 1000;
	}
}

void UtpSocket::mtu_reset()
{
	mtu_ceiling_ = get_udp_mtu();
	// Less would not pass TCP...
	mtu_floor_ = 576;
	log(UTP_LOG_MTU, "MTU [RESET] floor:%d ceiling:%d current:%d"
		, mtu_floor_, mtu_ceiling_, mtu_last_);
	assert(mtu_floor_ <= mtu_ceiling_);
	mtu_discover_time_ = utp_call_get_milliseconds(this->ctx, this) + 30 * 60 * 1000;
}

// returns:
// 0: the packet was acked.
// 1: it means that the packet had already been acked
// 2: the packet has not been sent yet
// ack_packet: 处理一个被对端确认的发送包 (通过累积 ACK 或 SACK/EACK)。
// 返回值:
//   0 - 包被本次 ACK 确认
//   1 - 包已被 ACK 过 (重复)
//   2 - 包尚未发送, 不应被 ACK (异常情况, 调用方应停止后续 ACK 推进)
// 关键作用:
//   - 若 transmissions == 1, 用 (now - time_sent) 估算 RTT, 更新 rtt/rtt_var
//     与 rto (RFC 6298 风格 EWMA)。
//   - 将 cur_window_ 减去该包负载 (释放 CWIN 配额)。
//   - 复位 RTO 计时器和 retransmit_count_。
//   - 释放 OutgoingPacket 对象。
int UtpSocket::ack_packet(uint16 seq)
{
	OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq);

	// the packet has already been acked (or not sent)
	if (pkt == NULL) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "got ack for:%u (already acked, or never sent)", seq);
		#endif

		return 1;
	}

	// can't ack packets that haven't been sent yet!
	if (pkt->transmissions == 0) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "got ack for:%u (never sent, pkt_size:%u need_resend:%u)",
			seq, (uint)pkt->payload, pkt->need_resend);
		#endif

		return 2;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "got ack for:%u (pkt_size:%u need_resend:%u)",
		seq, (uint)pkt->payload, pkt->need_resend);
	#endif

	outbuf_.put(seq, nullptr);

	// if we never re-sent the packet, update the RTT estimate
	if (pkt->transmissions == 1) {
		// Estimate the round trip time.
		const uint32 ertt = (uint32)((utp_call_get_microseconds(this->ctx, this) - pkt->time_sent) / 1000);
		if (rtt == 0) {
			// First round trip time sample
			rtt = ertt;
			rtt_var = ertt / 2;
			// sanity check. rtt should never be more than 6 seconds
//			assert(rtt < 6000);
		} else {
			// Compute new round trip times
			const int delta = (int)rtt - ertt;
			rtt_var = rtt_var + (int)(abs(delta) - rtt_var) / 4;
			rtt = rtt - rtt/8 + ertt/8;
			// sanity check. rtt should never be more than 6 seconds
//			assert(rtt < 6000);
			rtt_hist_.add_sample(ertt, ctx->current_ms_);
		}
		rto = max<uint>(rtt + rtt_var * 4, 1000);

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "rtt:%u avg:%u var:%u rto:%u",
			ertt, rtt, rtt_var, rto);
		#endif

	}
	retransmit_timeout_ = rto;
	rto_timeout_ = ctx->current_ms_ + rto;
	// if need_resend is set, this packet has already
	// been considered timed-out, and is not included in
	// the cur_window_ anymore
	if (!pkt->need_resend) {
		assert(cur_window_ >= pkt->payload);
		cur_window_ -= pkt->payload;
	}
	delete pkt;
	retransmit_count_ = 0;
	return 0;
}

// count the number of bytes that were acked by the EACK header
size_t UtpSocket::selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt)
{
	if (cur_window_packets_ == 0) return 0;

	size_t acked_bytes = 0;
	int bits = len * 8;
	uint64 now = utp_call_get_microseconds(this->ctx, this);

	do {
		uint v = base + bits;

		// ignore bits that haven't been sent yet
		// see comment in UtpSocket::selective_ack
		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		// ignore bits that represents packets we haven't sent yet
		// or packets that have already been acked
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0)
			continue;

		// Count the number of segments that were successfully received past it.
		if (bits >= 0 && mask[bits>>3] & (1 << (bits & 7))) {
			assert((int)(pkt->payload) >= 0);
			acked_bytes += pkt->payload;
			if (pkt->time_sent < now)
				min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
			else
				min_rtt = min<int64>(min_rtt, 50000);
			continue;
		}
	} while (--bits >= -1);
	return acked_bytes;
}

enum { MAX_EACK = 128 };

void UtpSocket::selective_ack(uint base, const byte *mask, byte len)
{
	if (cur_window_packets_ == 0) return;

	// the range is inclusive [0, 31] bits
	int bits = len * 8 - 1;

	int count = 0;

	// resends is a stack of sequence numbers we need to resend. Since we
	// iterate in reverse over the acked packets, at the end, the top packets
	// are the ones we want to resend
	int resends[MAX_EACK];
	int nr = 0;

#if UTP_DEBUG_LOGGING
	char bitmask[1024] = {0};
	int counter = bits;
	for (int i = 0; i <= bits; ++i) {
		bool bit_set = counter >= 0 && mask[counter>>3] & (1 << (counter & 7));
		bitmask[i] = bit_set ? '1' : '0';
		--counter;
	}

	log(UTP_LOG_DEBUG, "Got EACK [%s] base:%u", bitmask, base);
#endif

	do {
		// we're iterating over the bits from higher sequence numbers
		// to lower (kind of in reverse order, wich might not be very
		// intuitive)
		uint v = base + bits;

		// ignore bits that haven't been sent yet
		// and bits that fall below the ACKed sequence number
		// this can happen if an EACK message gets
		// reordered and arrives after a packet that ACKs up past
		// the base for thie EACK message

		// this is essentially the same as:
		// if v >= seq_nr_ || v <= seq_nr_ - cur_window_packets_
		// but it takes wrapping into account

		// if v == seq_nr_ the -1 will make it wrap. if v > seq_nr_
		// it will also wrap (since it will fall further below 0)
		// and be > cur_window_packets_.
		// if v == seq_nr_ - cur_window_packets_, the result will be
		// seq_nr_ - (seq_nr_ - cur_window_packets_) - 1
		// == seq_nr_ - seq_nr_ + cur_window_packets_ - 1
		// == cur_window_packets_ - 1 which will be caught by the
		// test. If v < seq_nr_ - cur_window_packets_ the result will grow
		// fall furhter outside of the cur_window_packets_ range.

		// sequence number space:
		//
		//     rejected <   accepted   > rejected
		// <============+--------------+============>
		//              ^              ^
		//              |              |
		//        (seq_nr_-wnd)         seq_nr_

		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		// this counts as a duplicate ack, even though we might have
		// received an ack for this packet previously (in another EACK
		// message for instance)
		bool bit_set = bits >= 0 && mask[bits>>3] & (1 << (bits & 7));

		// if this packet is acked, it counts towards the duplicate ack counter
		if (bit_set) count++;

		// ignore bits that represents packets we haven't sent yet
		// or packets that have already been acked
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "skipping %u. pkt:%08x transmissions:%u %s",
				v, pkt, pkt?pkt->transmissions:0, pkt?"(not sent yet?)":"(already acked?)");
			#endif
			continue;
		}

		// Count the number of segments that were successfully received past it.
		if (bit_set) {
			// the selective ack should never ACK the packet we're waiting for to decrement cur_window_packets_
			assert((v & outbuf_.mask()) != ((seq_nr_ - cur_window_packets_) & outbuf_.mask()));
			ack_packet(v);
			continue;
		}

		// Resend segments
		// if count is less than our re-send limit, we haven't seen enough
		// acked packets in front of this one to warrant a re-send.
		// if count == 0, we're still going through the tail of zeroes
		if (((v - fast_resend_seq_nr_) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
			count >= DUPLICATE_ACKS_BEFORE_RESEND) {
			// resends is a stack, and we're mostly interested in the top of it
			// if we're full, just throw away the lower half
			if (nr >= MAX_EACK - 2) {
				memmove(resends, &resends[MAX_EACK/2], MAX_EACK/2 * sizeof(resends[0]));
				nr -= MAX_EACK / 2;
			}
			resends[nr++] = v;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "no ack for %u", v);
			#endif

		} else {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "not resending %u count:%d dup_ack:%u fast_resend_seq_nr_:%u",
				v, count, duplicate_ack_, fast_resend_seq_nr_);
			#endif
		}
	} while (--bits >= -1);

	if (((base - 1 - fast_resend_seq_nr_) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
		count >= DUPLICATE_ACKS_BEFORE_RESEND) {
		// if we get enough duplicate acks to start
		// resending, the first packet we should resend
		// is base-1
		resends[nr++] = (base - 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "no ack for %u", (base - 1) & ACK_NR_MASK);
		#endif

	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "not resending %u count:%d dup_ack:%u fast_resend_seq_nr_:%u",
			base - 1, count, duplicate_ack_, fast_resend_seq_nr_);
		#endif
	}

	bool back_off = false;
	int i = 0;
	while (nr > 0) {
		uint v = resends[--nr];
		// don't consider the tail of 0:es to be lost packets
		// only unacked packets with acked packets after should
		// be considered lost
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);

		// this may be an old (re-ordered) packet, and some of the
		// packets in here may have been acked already. In which
		// case they will not be in the send queue anymore
		if (!pkt) continue;

		// used in parse_log.py
		log(UTP_LOG_NORMAL, "Packet %u lost. Resending", v);

		// On Loss
		back_off = true;

		#ifdef _DEBUG
		++stats_.rexmit;
		#endif

		send_packet(pkt);
		fast_resend_seq_nr_ = (v + 1) & ACK_NR_MASK;

		// Re-send max 4 packets.
		if (++i >= 4) break;
	}

	if (back_off)
		maybe_decay_win(ctx->current_ms_);

	duplicate_ack_ = count;
}

// apply_ccontrol: LEDBAT 拥塞控制核心,每收到一个 ACK 都会调用一次。
// 算法思路:
//   - 用 our_delay = min(对端测得的本端到对端延迟, 当前 RTT) 作为"我方方向上
//     引入的排队延迟"估计。
//   - off_target = target_delay_ - our_delay 表示距离目标延迟还有多少余量;
//     正值 (我们还有余量) -> 加窗, 负值 (我们引入的排队过多) -> 减窗。
//   - scaled_gain = MAX_CWND_INCREASE_BYTES_PER_RTT * window_factor * delay_factor,
//     把"每个 RTT 至多增加 X 字节"按 ACK 所占窗口比例和延迟比例缩放。
//   - 慢启动阶段: 窗口每次按 packet_size 翻倍式增长, 超过 ssthresh_ 或排队
//     延迟达到 target 的 90% 时, 切到 LEDBAT 线性 (减/增) 模式。
//   - 防"作弊": 时钟漂移 < -200000 (即对端故意让时钟变慢) 时追加 penalty,
//     抑制其获得过多带宽。
//   - 若 1 秒以上未触达 CWIN 上限 (受应用层速率限制), 不再继续加窗, 防止
//     窗口无限膨胀。
void UtpSocket::apply_ccontrol(size_t bytes_acked, uint32 actual_delay, int64 min_rtt)
{
	// the delay can never be greater than the rtt. The min_rtt
	// variable is the RTT in microseconds

	assert(min_rtt >= 0);
	int32 our_delay = min<uint32>(our_hist_.get_value(), uint32(min_rtt));
	assert(our_delay != INT_MAX);
	assert(our_delay >= 0);

	utp_call_on_delay_sample(this->ctx, this, our_delay / 1000);

	// This test the connection under heavy load from foreground
	// traffic. Pretend that our delays are very high to force the
	// connection to use sub-packet size window sizes
	//our_delay *= 4;

	// target is microseconds
	int target = target_delay_;
	if (target <= 0) target = 100000;

	// this is here to compensate for very large clock drift that affects
	// the congestion controller into giving certain endpoints an unfair
	// share of the bandwidth. We have an estimate of the clock drift
	// (clock_drift_). The unit of this is microseconds per 5 seconds.
	// empirically, a reasonable cut-off appears to be about 200000
	// (which is pretty high). The main purpose is to compensate for
	// people trying to "cheat" uTP by making their clock run slower,
	// and this definitely catches that without any risk of false positives
	// if clock_drift_ < -200000 start applying a penalty delay proportional
	// to how far beoynd -200000 the clock drift is
	int32 penalty = 0;
	if (clock_drift_ < -200000) {
		penalty = (-clock_drift_ - 200000) / 7;
		our_delay += penalty;
	}

	double off_target = target - our_delay;

	// this is the same as:
	//
	//    (min(off_target, target) / target) * (bytes_acked / max_window_) * MAX_CWND_INCREASE_BYTES_PER_RTT
	//
	// so, it's scaling the max increase by the fraction of the window this ack represents, and the fraction
	// of the target delay the current delay represents.
	// The min() around off_target protects against crazy values of our_delay, which may happen when th
	// timestamps wraps, or by just having a malicious peer sending garbage. This caps the increase
	// of the window size to MAX_CWND_INCREASE_BYTES_PER_RTT per rtt.
	// as for large negative numbers, this direction is already capped at the min packet size further down
	// the min around the bytes_acked protects against the case where the window size was recently
	// shrunk and the number of acked bytes exceeds that. This is considered no more than one full
	// window, in order to keep the gain within sane boundries.

	assert(bytes_acked > 0);
	double window_factor = (double)min(bytes_acked, max_window_) / (double)max(max_window_, bytes_acked);

	double delay_factor = off_target / target;
	double scaled_gain = MAX_CWND_INCREASE_BYTES_PER_RTT * window_factor * delay_factor;

	// since MAX_CWND_INCREASE_BYTES_PER_RTT is a cap on how much the window size (max_window_)
	// may increase per RTT, we may not increase the window size more than that proportional
	// to the number of bytes that were acked, so that once one window has been acked (one rtt)
	// the increase limit is not exceeded
	// the +1. is to allow for floating point imprecision
	assert(scaled_gain <= 1. + MAX_CWND_INCREASE_BYTES_PER_RTT * (double)min(bytes_acked, max_window_) / (double)max(max_window_, bytes_acked));

	if (scaled_gain > 0 && ctx->current_ms_ - last_maxed_out_window_ > 1000) {
		// if it was more than 1 second since we tried to send a packet
		// and stopped because we hit the max window, we're most likely rate
		// limited (which prevents us from ever hitting the window size)
		// if this is the case, we cannot let the max_window_ grow indefinitely
		scaled_gain = 0;
	}

	size_t ledbat_cwnd = (max_window_ + scaled_gain < MIN_WINDOW_SIZE) ? MIN_WINDOW_SIZE : (size_t)(max_window_ + scaled_gain);

	// 慢启动 vs 拥塞避免 分支:
	//   - 慢启动: 每个 ACK 把窗口按 packet_size 比例往上推 (近似指数增长),
	//     直到 ss_cwnd > ssthresh_ 或排队延迟达到 target_delay_ 的 90%。
	//   - 拥塞避免: 改用 LEDBAT 公式 (线性/亚线性) 计算 ledbat_cwnd。
	// 退出慢启动的同时会把当前 max_window_ 记为新的 ssthresh_, 与 TCP 类似。
	if (slow_start_) {
		size_t ss_cwnd = (size_t)(max_window_ + window_factor*get_packet_size());
		if (ss_cwnd > ssthresh_) {
			slow_start_ = false;
		} else if (our_delay > target*0.9) {
			// even if we're a little under the target delay, we conservatively
			// discontinue the slow start phase
			slow_start_ = false;
			ssthresh_ = max_window_;
		} else {
			max_window_ = max(ss_cwnd, ledbat_cwnd);
		}
	} else {
		max_window_ = ledbat_cwnd;
	}


	// make sure that the congestion window is below max
	// make sure that we don't shrink our window too small
	max_window_ = clamp<size_t>(max_window_, MIN_WINDOW_SIZE, opt_sndbuf_);

	// used in parse_log.py
	log(UTP_LOG_NORMAL, "actual_delay:%u our_delay:%d their_delay:%u off_target:%d max_window_:%u "
			"delay_base:%u delay_sum:%d target_delay_:%d acked_bytes:%u cur_window_:%u "
			"scaled_gain:%f rtt:%u rate:%u wnduser:%u rto:%u timeout:%d get_microseconds:" I64u " "
			"cur_window_packets_:%u packet_size:%u their_delay_base:%u their_actual_delay:%u "
			"average_delay_:%d clock_drift_:%d clock_drift_raw_:%d delay_penalty:%d current_delay_sum_:" I64u
			"current_delay_samples_:%d average_delay_base_:%d last_maxed_out_window_:" I64u " opt_sndbuf_:%d "
			"current_ms:" I64u "",
			actual_delay, our_delay / 1000, their_hist_.get_value() / 1000,
			int(off_target / 1000), uint(max_window_), uint32(our_hist_.delay_base),
			int((our_delay + their_hist_.get_value()) / 1000), int(target / 1000), uint(bytes_acked),
			(uint)(cur_window_ - bytes_acked), (float)(scaled_gain), rtt,
			(uint)(max_window_ * 1000 / (rtt_hist_.delay_base?rtt_hist_.delay_base:50)),
			(uint)max_window_user_, rto, (int)(rto_timeout_ - ctx->current_ms_),
			utp_call_get_microseconds(this->ctx, this), cur_window_packets_, (uint)get_packet_size(),
			their_hist_.delay_base, their_hist_.delay_base + their_hist_.get_value(),
			average_delay_, clock_drift_, clock_drift_raw_, penalty / 1000,
			current_delay_sum_, current_delay_samples_, average_delay_base_,
			uint64(last_maxed_out_window_), int(opt_sndbuf_), uint64(ctx->current_ms_));
}

static void utp_register_recv_packet(UtpSocket *conn, size_t len)
{
	#ifdef _DEBUG
	++conn->stats_.nrecv;
	conn->stats_.nbytes_recv += len;
	#endif

	if (len <= PACKET_SIZE_MID) {
		if (len <= PACKET_SIZE_EMPTY) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (len <= PACKET_SIZE_SMALL) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_MID_BUCKET]++;
	} else {
		if (len <= PACKET_SIZE_BIG) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_BIG_BUCKET]++;
		} else
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

// returns the max number of bytes of payload the uTP
// connection is allowed to send
size_t UtpSocket::get_packet_size() const
{
	int header_size = sizeof(PacketFormatV1);
	size_t mtu = mtu_last_ ? mtu_last_ : mtu_ceiling_;
	return mtu - header_size;
}

// Process an incoming packet
// syn is true if this is the first packet received. It will cut off parsing
// as soon as the header is done
// utp_process_incoming: 一个 uTP 数据包的核心处理函数, 涵盖了协议栈大半逻辑。
// 处理流程:
//   1. 验证 ack_nr_ 是否在合法窗口内, 防止伪造/重放。
//   2. 解析扩展头 (SACK/EACK、extension bits), 拿到 selective ack 位图。
//   3. 计算本端接收延迟 (their_delay), 写入 reply_micro_ 用于下个回包。
//   4. 计算本次 ACK 覆盖的字节数, 调用 ack_packet 释放对应 in-flight 包,
//      收集 min_rtt 用于延迟基线校准。
//   5. 处理 SACK/EACK: selective_ack 触发快速重传 (4 个重复 ACK)。
//   6. 更新 max_window_user_ (对端通告窗口), 处理零窗口探测。
//   7. 推进连接状态: SYN_SENT->CONNECTED、SYN_RECV->CONNECTED, FIN 处理。
//   8. 若 seqnr == 0 (按序): 投递到上层 on_read, 并 flush 重排缓冲;
//      否则放入 inbuf_ 等待按序交付 (并立刻发 SACK)。
//   9. 调用 apply_ccontrol 完成 LEDBAT 调窗。
// syn=true 时仅解析头即返回, 用于初始 SYN 路径。
size_t utp_process_incoming(UtpSocket *conn, const byte *packet, size_t len, bool syn = false)
{
	//统计信息
	utp_register_recv_packet(conn, len);
	//记录当前的ms
	conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);

	const PacketFormatV1 *pf1 = (PacketFormatV1*)packet;
	const byte *packet_end = packet + len;

	uint16 pk_seq_nr_ = pf1->seq_nr;
	uint16 pk_ack_nr_ = pf1->ack_nr;
	uint8 pk_flags   = pf1->type();

	if (pk_flags >= ST_NUM_STATES) return 0;

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "Got %s. seq_nr_:%u ack_nr_:%u state:%s timestamp:" I64u " reply_micro_:%u"
		, flagnames[pk_flags], pk_seq_nr_, pk_ack_nr_, statenames[conn->state_]
		, uint64(pf1->tv_usec), (uint32)(pf1->reply_micro));
	#endif

	// mark receipt time
	uint64 time = utp_call_get_microseconds(conn->ctx, conn);
	// 计算当前窗口大小，从而确定ACK的范围
	// window packets size is used to calculate a minimum
	// permissible range for received acks. connections with acks falling
	// out of this range are dropped
	const uint16 curr_window = max<uint16>(conn->cur_window_packets_ + ACK_NR_ALLOWED_WINDOW, ACK_NR_ALLOWED_WINDOW);

	// ignore packets whose ack_nr_ is invalid. This would imply a spoofed address
	// or a malicious attempt to attach the uTP implementation.
	// acking a packet that hasn't been sent yet!
	// SYN packets have an exception, since there are no previous packets
	if ((pk_flags != ST_SYN || conn->state_ != CS_SYN_RECV) && 
		(wrapping_compare_less(conn->seq_nr_ - 1, pk_ack_nr_, ACK_NR_MASK)
		|| wrapping_compare_less(pk_ack_nr_, conn->seq_nr_ - 1 - curr_window, ACK_NR_MASK))) {
#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "Invalid ack_nr_: %u. our seq_nr_: %u last unacked: %u"
	, pk_ack_nr_, conn->seq_nr_, (conn->seq_nr_ - conn->cur_window_packets_) & ACK_NR_MASK);
#endif
	//非SYN，且状态不是CS_SYN_RECV
	//链接的seq_nr_要比包中的ack_nr_小
	//包中的ack_nr_要比链接中seq_nr_ - curr_window小，说明已经包已经被ACK了，因此就无需再次处理
		return 0;
	}

	// RSTs are handled earlier, since the connid matches the send id not the recv id
	assert(pk_flags != ST_RESET);

	// TODO: maybe send a ST_RESET if we're in CS_RESET?

	const byte *selack_ptr = NULL;

	// Unpack UTP packet options
	// Data pointer
	const byte *data = (const byte*)pf1 + conn->get_header_size();
	if (conn->get_header_size() > len) {

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Invalid packet size (less than header size)");
		#endif

		return 0;
	}
	// Skip the extension headers
	uint extension = pf1->ext;
	//处理扩展
	if (extension != 0) {
		do {
			// Verify that the packet is valid.
			data += 2;

			if ((int)(packet_end - data) < 0 || (int)(packet_end - data) < data[-1]) {

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "Invalid len of extensions_");
				#endif

				return 0;
			}

			switch(extension) {
			case 1: // Selective Acknowledgment
				selack_ptr = data;
				break;
			case 2: // extension bits
				if (data[-1] != 8) {

					#if UTP_DEBUG_LOGGING
					conn->log(UTP_LOG_DEBUG, "Invalid len of extension bits header");
					#endif

					return 0;
				}
				memcpy(conn->extensions_, data, 8);

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "got extension bits:%02x%02x%02x%02x%02x%02x%02x%02x",
					conn->extensions_[0], conn->extensions_[1], conn->extensions_[2], conn->extensions_[3],
					conn->extensions_[4], conn->extensions_[5], conn->extensions_[6], conn->extensions_[7]);
				#endif
			}
			extension = data[-2];
			data += data[-1];
		} while (extension);
	}

	if (conn->state_ == CS_SYN_SENT) {
		// if this is a syn-ack, initialize our ack_nr_
		// to match the sequence number we got from
		// the other end
		// 如果我们是处在CS_SYN_SENT，那么我们需要设置我们链接的ACK_NR
		conn->ack_nr_ = (pk_seq_nr_ - 1) & SEQ_NR_MASK;
	}
	//我们最后一次得到数据包的时间
	conn->last_got_packet_ = conn->ctx->current_ms_;
	//如果是SYN包，处理到此结束
	if (syn) {
		return 0;
	}

	// seqnr is the number of packets past the expected
	// packet this is. ack_nr_ is the last acked, seq_nr_ is the
	// current. Subtracring 1 makes 0 mean "this is the next
	// expected packet".
	const uint seqnr = (pk_seq_nr_ - conn->ack_nr_ - 1) & SEQ_NR_MASK;
	//  65528   =       65535 - 6 - 1
	//  65532   =       65535 - 2 - 1
	// Getting an invalid sequence number?
	if (seqnr >= REORDER_BUFFER_MAX_SIZE) { //超过乱序队列的大小
		if (seqnr >= (SEQ_NR_MASK + 1) - REORDER_BUFFER_MAX_SIZE && pk_flags != ST_STATE) {
			//        65532 = 65535 + 1 - 4
			conn->schedule_ack();
		}

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "    Got old Packet/Ack (%u/%u)=%u"
			, pk_seq_nr_, conn->ack_nr_, seqnr);
		#endif
		return 0;
	}

	// Process acknowledgment
	// acks is the number of packets that was acked
	// 计算对方已经收到了多少个包
	int acks = (pk_ack_nr_ - (conn->seq_nr_ - 1 - conn->cur_window_packets_)) & ACK_NR_MASK;

	// this happens when we receive an old ack nr
	if (acks > conn->cur_window_packets_) acks = 0;

	// if we get the same ack_nr_ as in the last packet
	// increase the duplicate_ack_ counter, otherwise reset
	// it to 0.
	// It's important to only count ACKs in ST_STATE packets. Any other
	// packet (primarily ST_DATA) is likely to have been sent because of the
	// other end having new outgoing data, not in response to incoming data.
	// For instance, if we're receiving a steady stream of payload with no
	// outgoing data, and we suddently have a few bytes of payload to send (say,
	// a bittorrent HAVE message), we're very likely to see 3 duplicate ACKs
	// immediately after sending our payload packet. This effectively disables
	// the fast-resend on duplicate-ack logic for bi-directional connections
	// (except in the case of a selective ACK). This is in line with BSD4.4 TCP
	// implementation.
	if (conn->cur_window_packets_ > 0) {
		if (pk_ack_nr_ == ((conn->seq_nr_ - conn->cur_window_packets_ - 1) & ACK_NR_MASK)
			&& conn->cur_window_packets_ > 0
			&& pk_flags == ST_STATE) {//STATE包，curr_window > 0 且都应答了
			++conn->duplicate_ack_;
			if (conn->duplicate_ack_ == DUPLICATE_ACKS_BEFORE_RESEND && conn->mtu_probe_seq_) {
				// It's likely that the probe was rejected due to its size, but we haven't got an
				// ICMP report back yet
				if (pk_ack_nr_ == ((conn->mtu_probe_seq_ - 1) & ACK_NR_MASK)) {
					conn->mtu_ceiling_ = conn->mtu_probe_size_ - 1;
					conn->mtu_search_update();
					conn->log(UTP_LOG_MTU, "MTU [DUPACK] floor:%d ceiling:%d current:%d"
						, conn->mtu_floor_, conn->mtu_ceiling_, conn->mtu_last_);
				} else {
					// A non-probe was blocked before our probe.
					// Can't conclude much, send a new probe
					conn->mtu_probe_seq_ = conn->mtu_probe_size_ = 0;
				}
			}
		} else {
			conn->duplicate_ack_ = 0;
		}

		// TODO: if duplicate_ack_ == DUPLICATE_ACK_BEFORE_RESEND
		// and fast_resend_seq_nr_ <= ack_nr_ + 1
		//    resend ack_nr_ + 1
		// also call maybe_decay_win()
	}

	// figure out how many bytes were acked
	size_t acked_bytes = 0;

	// the minimum rtt of all acks
	// this is the upper limit on the delay we get back
	// from the other peer. Our delay cannot exceed
	// the rtt of the packet. If it does, clamp it.
	// this is done in apply_ledbat_ccontrol()
	int64 min_rtt = INT64_MAX;

	uint64 now = utp_call_get_microseconds(conn->ctx, conn);

	for (int i = 0; i < acks; ++i) {
		int seq = (conn->seq_nr_ - conn->cur_window_packets_ + i) & ACK_NR_MASK;
		OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(seq);
		if (pkt == 0 || pkt->transmissions == 0) continue;
		assert((int)(pkt->payload) >= 0);
		acked_bytes += pkt->payload; //计算已经被acked的字节数
		if (conn->mtu_probe_seq_ && seq == conn->mtu_probe_seq_) {
			conn->mtu_floor_ = conn->mtu_probe_size_;
			conn->mtu_search_update();
			conn->log(UTP_LOG_MTU, "MTU [ACK] floor:%d ceiling:%d current:%d"
				, conn->mtu_floor_, conn->mtu_ceiling_, conn->mtu_last_);
		}
		//计算最小的rtt
		// in case our clock is not monotonic
		if (pkt->time_sent < now)
			min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
		else
			min_rtt = min<int64>(min_rtt, 50000);
	}

	// count bytes acked by EACK
	if (selack_ptr != NULL) {
		acked_bytes += conn->selective_ack_bytes((pk_ack_nr_ + 2) & ACK_NR_MASK,
												 selack_ptr, selack_ptr[-1], min_rtt);
	}

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%d cur_window_:%u cur_window_packets_:%u relative_seqnr:%u max_window_:%u min_rtt:%u rtt:%u",
		acks, (uint)acked_bytes, conn->seq_nr_, (uint)conn->cur_window_, conn->cur_window_packets_,
		seqnr, (uint)conn->max_window_, (uint)(min_rtt / 1000), conn->rtt);
	#endif

	uint64 p = pf1->tv_usec;

	conn->last_measured_delay_ = conn->ctx->current_ms_;

	// get delay in both directions
	// record the delay to report back
	const uint32 their_delay = (uint32)(p == 0 ? 0 : time - p); //计算到对方的延迟，time是收到包的时间，p是对方的发包时间
	conn->reply_micro_ = their_delay;
	uint32 prev_delay_base = conn->their_hist_.delay_base;
	if (their_delay != 0) conn->their_hist_.add_sample(their_delay, conn->ctx->current_ms_);

	// if their new delay base is less than their previous one
	// we should shift our delay base in the other direction in order
	// to take the clock skew into account
	if (prev_delay_base != 0 &&
		wrapping_compare_less(conn->their_hist_.delay_base, prev_delay_base, TIMESTAMP_MASK)) {
		// never adjust more than 10 milliseconds
		if (prev_delay_base - conn->their_hist_.delay_base <= 10000) {
			conn->our_hist_.shift(prev_delay_base - conn->their_hist_.delay_base);
		}
	}
	//如果对面没有设置reply_micro_
	const uint32 actual_delay = (uint32(pf1->reply_micro) == INT_MAX ? 0 : uint32(pf1->reply_micro));

	// if the actual delay is 0, it means the other end
	// hasn't received a sample from us yet, and doesn't
	// know what it is. We can't update out history unless
	// we have a true measured sample
	if (actual_delay != 0) {
		conn->our_hist_.add_sample(actual_delay, conn->ctx->current_ms_);

		// this is keeping an average of the delay samples
		// we've recevied within the last 5 seconds. We sum
		// all the samples and increase the count in order to
		// calculate the average every 5 seconds. The samples
		// are based off of the average_delay_base_ to deal with
		// wrapping counters.
		if (conn->average_delay_base_ == 0) conn->average_delay_base_ = actual_delay;
		int64 average_delay_sample = 0;
		// distance walking from lhs to rhs, downwards
		const uint32 dist_down = conn->average_delay_base_ - actual_delay;
		// distance walking from lhs to rhs, upwards
		const uint32 dist_up = actual_delay - conn->average_delay_base_;

		if (dist_down > dist_up) {
//			assert(dist_up < INT_MAX / 4);
			// average_delay_base_ < actual_delay, we should end up
			// with a positive sample
			average_delay_sample = dist_up;
		} else {
//			assert(-int64(dist_down) < INT_MAX / 4);
			// average_delay_base_ >= actual_delay, we should end up
			// with a negative sample
			average_delay_sample = -int64(dist_down);
		}
		conn->current_delay_sum_ += average_delay_sample;
		++conn->current_delay_samples_;

		if (conn->ctx->current_ms_ > conn->average_sample_time_) {

			int32 prev_average_delay_ = conn->average_delay_;

			assert(conn->current_delay_sum_ / conn->current_delay_samples_ < INT_MAX);
			assert(conn->current_delay_sum_ / conn->current_delay_samples_ > -INT_MAX);
			// write the new average
			conn->average_delay_ = (int32)(conn->current_delay_sum_ / conn->current_delay_samples_);
			// each slot represents 5 seconds
			conn->average_sample_time_ += 5000;

			conn->current_delay_sum_ = 0;
			conn->current_delay_samples_ = 0;

			// this makes things very confusing when logging the average delay
//#if !g_log_utp
			// normalize the average samples
			// since we're only interested in the slope
			// of the curve formed by the average delay samples,
			// we can cancel out the actual offset to make sure
			// we won't have problems with wrapping.
			int min_sample = min(prev_average_delay_, conn->average_delay_);
			int max_sample = max(prev_average_delay_, conn->average_delay_);

			// normalize around zero. Try to keep the min <= 0 and max >= 0
			int adjust = 0;
			if (min_sample > 0) {
				// adjust all samples (and the baseline) down by min_sample
				adjust = -min_sample;
			} else if (max_sample < 0) {
				// adjust all samples (and the baseline) up by -max_sample
				adjust = -max_sample;
			}
			if (adjust) {
				conn->average_delay_base_ -= adjust;
				conn->average_delay_ += adjust;
				prev_average_delay_ += adjust;
			}
//#endif

			// update the clock drift estimate
			// the unit is microseconds per 5 seconds
			// what we're doing is just calculating the average of the
			// difference between each slot. Since each slot is 5 seconds
			// and the timestamps unit are microseconds, we'll end up with
			// the average slope across our history. If there is a consistent
			// trend, it will show up in this value

			//int64 slope = 0;
			int32 drift = conn->average_delay_ - prev_average_delay_;

			// clock_drift_ is a rolling average
			conn->clock_drift_ = (int64(conn->clock_drift_) * 7 + drift) / 8;
			conn->clock_drift_raw_ = drift;
		}
	}

	// if our new delay base is less than our previous one
	// we should shift the other end's delay base in the other
	// direction in order to take the clock skew into account
	// This is commented out because it creates bad interactions
	// with our adjustment in the other direction. We don't really
	// need our estimates of the other peer to be very accurate
	// anyway. The problem with shifting here is that we're more
	// likely shift it back later because of a low latency. This
	// second shift back would cause us to shift our delay base
	// which then get's into a death spiral of shifting delay bases
/*	if (prev_delay_base != 0 &&
		wrapping_compare_less(conn->our_hist_.delay_base, prev_delay_base)) {
		// never adjust more than 10 milliseconds
		if (prev_delay_base - conn->our_hist_.delay_base <= 10000) {
			conn->their_hist_.Shift(prev_delay_base - conn->our_hist_.delay_base);
		}
	}
*/

	// if the delay estimate exceeds the RTT, adjust the base_delay to
	// compensate
	assert(min_rtt >= 0);
	if (int64(conn->our_hist_.get_value()) > min_rtt) {
		conn->our_hist_.shift((uint32)(conn->our_hist_.get_value() - min_rtt));
	}

	// only apply the congestion controller on acks
	// if we don't have a delay measurement, there's
	// no point in invoking the congestion control
	if (actual_delay != 0 && acked_bytes >= 1) //如果我们确认过的数据大于1，我们就进行流控
		conn->apply_ccontrol(acked_bytes, actual_delay, min_rtt);

	// sanity check, the other end should never ack packets
	// past the point we've sent
	if (acks <= conn->cur_window_packets_) { //如果acks的数量等于或小于当前窗口
		conn->max_window_user_ = pf1->windowsize; //记录对端的窗口

		// If max user window is set to 0, then we startup a timer
		// That will reset it to 1 after 15 seconds.
		if (conn->max_window_user_ == 0) //对端窗口满了，我们需要停下15s
			// Reset max_window_user_ to 1 every 15 seconds.
			conn->zerowindow_time_ = conn->ctx->current_ms_ + 15000;

		// Respond to connect message
		// Switch to CONNECTED state.
		// If this is an ack and we're in still handshaking
		// transition over to the connected state.
		// 真正的数据进入，变更状态
		// Incoming connection completion
		if (pk_flags == ST_DATA && conn->state_ == CS_SYN_RECV) {
			conn->state_ = CS_CONNECTED;
		}
		// 链接成功了，变更状态
		// Outgoing connection completion
		if (pk_flags == ST_STATE && conn->state_ == CS_SYN_SENT)	{
			conn->state_ = CS_CONNECTED;
		
			// If the user has defined the ON_CONNECT callback, use that to
			// notify the user that the socket is now connected.  If ON_CONNECT
			// has not been defined, notify the user via ON_STATE_CHANGE.
			if (conn->ctx->callbacks_[UTP_ON_CONNECT])
				utp_call_on_connect(conn->ctx, conn);
			else
				utp_call_on_state_change(conn->ctx, conn, UTP_STATE_CONNECT);

		// We've sent a fin, and everything was ACKed (including the FIN).
		// cur_window_packets_ == acks means that this packet acked all 
		// the remaining packets that were in-flight.
		} else if (conn->fin_sent && conn->cur_window_packets_ == acks) {
			conn->fin_sent_acked_ = true; //如果发送了FIN，并且ACKS的包和窗口相同，我们直接设置FIN_SENT_ACKED
			if (conn->close_requested_) { //如果已经要求关闭，那么直接进入DSTROY
				conn->state_ = CS_DESTROY;
			}
		}
		// 更新快速重传的序列号
		// Update fast resend counter
		if (wrapping_compare_less(conn->fast_resend_seq_nr_
			, (pk_ack_nr_ + 1) & ACK_NR_MASK, ACK_NR_MASK))
			conn->fast_resend_seq_nr_ = (pk_ack_nr_ + 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "fast_resend_seq_nr_:%u", conn->fast_resend_seq_nr_);
		#endif
		//对包进行ack
		for (int i = 0; i < acks; ++i) {
			int ack_status = conn->ack_packet(conn->seq_nr_ - conn->cur_window_packets_);
			// if ack_status is 0, the packet was acked.
			// if acl_stauts is 1, it means that the packet had already been acked
			// if it's 2, the packet has not been sent yet
			// We need to break this loop in the latter case. This could potentially
			// happen if we get an ack_nr_ that does not exceed what we have stuffed
			// into the outgoing buffer, but does exceed what we have sent
			if (ack_status == 2) {
				#ifdef _DEBUG
				OutgoingPacket* pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_);
				assert(pkt->transmissions == 0);
				#endif

				break;
			}
			conn->cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", conn->cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (conn->cur_window_packets_ == 0)
			assert(conn->cur_window_ == 0);
		#endif

		// packets in front of this may have been acked by a
		// selective ack (EACK). Keep decreasing the window packet size
		// until we hit a packet that is still waiting to be acked
		// in the send queue
		// this is especially likely to happen when the other end
		// has the EACK send bug older versions of uTP had
		// 处理掉所有被EACK所ACK掉的包
		while (conn->cur_window_packets_ > 0 && !conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_)) {
			conn->cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", conn->cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (conn->cur_window_packets_ == 0)
			assert(conn->cur_window_ == 0);
		#endif

		// this invariant should always be true
		assert(conn->cur_window_packets_ == 0 || conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_));
		// 尝试快速重传第一个包
		// flush Nagle
		if (conn->cur_window_packets_ == 1) {
			OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - 1);
			// do we still have quota?
			if (pkt->transmissions == 0) {
				conn->send_packet(pkt);
			}
		}

		// Fast timeout-retry
		if (conn->fast_timeout_) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Fast timeout %u,%u,%u?", (uint)conn->cur_window_, conn->seq_nr_ - conn->timeout_seq_nr_, conn->timeout_seq_nr_);
			#endif

			// if the fast_resend_seq_nr_ is not pointing to the oldest outstanding packet, it suggests that we've already
			// resent the packet that timed out, and we should leave the fast-timeout mode.
			if (((conn->seq_nr_ - conn->cur_window_packets_) & ACK_NR_MASK) != conn->fast_resend_seq_nr_) {
				conn->fast_timeout_ = false;
			} else {
				// resend the oldest packet and increment fast_resend_seq_nr_
				// to not allow another fast resend on it again
				// 快速重传最老的包
				OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_);
				if (pkt && pkt->transmissions > 0) {

					#if UTP_DEBUG_LOGGING
					conn->log(UTP_LOG_DEBUG, "Packet %u fast timeout-retry.", conn->seq_nr_ - conn->cur_window_packets_);
					#endif

					#ifdef _DEBUG
					++conn->stats_.fastrexmit;
					#endif

					conn->fast_resend_seq_nr_++;
					conn->send_packet(pkt);
				}
			}
		}
	}
	// 处理SACK
	// Process selective acknowledgent
	if (selack_ptr != NULL) {
		conn->selective_ack(pk_ack_nr_ + 2, selack_ptr, selack_ptr[-1]);
	}

	// this invariant should always be true
	assert(conn->cur_window_packets_ == 0 || conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%u cur_window_:%u cur_window_packets_:%u ",
		acks, (uint)acked_bytes, conn->seq_nr_, (uint)conn->cur_window_, conn->cur_window_packets_);
	#endif

	// In case the ack dropped the current window below
	// the max_window_ size, Mark the socket as writable
	if (conn->state_ == CS_CONNECTED_FULL && !conn->is_full()) {
		conn->state_ = CS_CONNECTED;
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
			(uint)conn->max_window_, (uint)conn->cur_window_, (uint)conn->get_packet_size());
		#endif
		utp_call_on_state_change(conn->ctx, conn, UTP_STATE_WRITABLE);
	}
	//如果是个STATE的包，处理到此为止
	if (pk_flags == ST_STATE) {
		// This is a state packet only.
		return 0;
	}
	//链接尚未完全建立，处理到此为止
	// The connection is not in a state that can accept data?
	if (conn->state_ != CS_CONNECTED &&
		conn->state_ != CS_CONNECTED_FULL) {
		return 0;
	}

	// Is this a finalize packet?
	if (pk_flags == ST_FIN && !conn->got_fin) {
		//FIN包，并且我们之前并没见到FIN包
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Got FIN eof_pkt_:%u", pk_seq_nr_);
		#endif
		//标记我们收到FIN包了
		conn->got_fin = true;
		conn->eof_pkt_ = pk_seq_nr_;
		// at this point, it is possible for the
		// other end to have sent packets with
		// sequence numbers higher than seq_nr_.
		// if this is the case, our reorder_count_
		// is out of sync. This case is dealt with
		// when we re-order and hit the eof_pkt_.
		// we'll just ignore any packets with
		// sequence numbers past this
	}

	// Getting an in-order packet?
	if (seqnr == 0) {
		size_t count = packet_end - data;
		if (count > 0 && !conn->read_shutdown_) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Got Data len:%u (rb:%u)", (uint)count, (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif

			// Post bytes to the upper layer
			utp_call_on_read(conn->ctx, conn, data, count);
		}
		conn->ack_nr_++;

		// Check if the next packet has been received too, but waiting
		// in the reorder buffer.
		for (;;) {
			//如果我们收到FIN包，并且ACK_NR和eof_pkt_相等
			if (!conn->got_fin_reached_ && conn->got_fin && conn->eof_pkt_ == conn->ack_nr_) {
				conn->got_fin_reached_ = true; //我们已经响应了FIN包
				conn->rto_timeout_ = conn->ctx->current_ms_ + min<uint>(conn->rto * 3, 60); //更新RTO_timeout时间戳

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "Posting EOF");
				#endif

				utp_call_on_state_change(conn->ctx, conn, UTP_STATE_EOF);
				// 发送FIN的ACK，我们可以开始丢弃发送数据了
				// if the other end wants to close, ack
				conn->send_ack();

				// reorder_count_ is not necessarily 0 at this point.
				// even though it is most of the time, the other end
				// may have sent packets with higher sequence numbers
				// than what later end up being eof_pkt_
				// since we have received all packets up to eof_pkt_
				// just ignore the ones after it.
				conn->reorder_count_ = 0;
			}

			// Quick get-out in case there is nothing to reorder
			if (conn->reorder_count_ == 0)
				break;

			// Check if there are additional buffers in the reorder buffers
			// that need delivery.
			auto *pkt = (InboundPacket*)conn->inbuf_.get(conn->ack_nr_+1);
			if (pkt == NULL)
				break;
			conn->inbuf_.put(conn->ack_nr_+1, NULL);
			count = pkt->size;
			if (count > 0 && !conn->read_shutdown_) {
				// Pass the bytes to the upper layer
				utp_call_on_read(conn->ctx, conn, pkt->data.data(), count);
			}
			delete pkt;
			conn->ack_nr_++;

			assert(conn->reorder_count_ > 0);
			conn->reorder_count_--;
		}
		//发送一个ACK
		conn->schedule_ack();
	} else {
		// Getting an out of order packet.
		// The packet needs to be remembered and rearranged later.

		// if we have received a FIN packet, and the EOF-sequence number
		// is lower than the sequence number of the packet we just received
		// something is wrong.
		if (conn->got_fin && pk_seq_nr_ > conn->eof_pkt_) {
			//收到FIN包，且新包的序列号大于了EOF
			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Got an invalid packet sequence number, past EOF "
				"reorder_count_:%u len:%u (rb:%u)",
				conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif
			return 0;
		}

		// if the sequence number is entirely off the expected
		// one, just drop it. We can't allocate buffer space in
		// the inbuf_ entirely based on untrusted input
		if (seqnr > 0x3ff) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "0x%08x: Got an invalid packet sequence number, too far off "
				"reorder_count_:%u len:%u (rb:%u)",
				conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif
			return 0;
		}

		// we need to grow the circle buffer before we
		// check if the packet is already in here, so that
		// we don't end up looking at an older packet (since
		// the indices wraps around).
		conn->inbuf_.ensure_size(pk_seq_nr_ + 1, seqnr + 1);

		// Has this packet already been received? (i.e. a duplicate)
		// If that is the case, just discard it.
		if (conn->inbuf_.get(pk_seq_nr_) != NULL) {
			#ifdef _DEBUG
			++conn->stats_.nduprecv;
			#endif

			return 0;
		}

		// Allocate memory to fit the packet that needs to re-ordered
		auto *pkt = new InboundPacket;
		pkt->size = (uint32_t)(packet_end - data);
		pkt->data.assign(data, packet_end);

		// Insert into reorder buffer and increment the count
		// of # of packets to be reordered.
		// we add one to seqnr in order to leave the last
		// entry empty, that way the assert in send_ack
		// is valid. we have to add one to seqnr too, in order
		// to make the circular buffer grow around the correct
		// point (which is conn->ack_nr_ + 1).
		assert(conn->inbuf_.get(pk_seq_nr_) == NULL);
		assert((pk_seq_nr_ & conn->inbuf_.mask()) != ((conn->ack_nr_+1) & conn->inbuf_.mask()));
		conn->inbuf_.put(pk_seq_nr_, pkt);
		conn->reorder_count_++;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "0x%08x: Got out of order data reorder_count_:%u len:%u (rb:%u)",
			conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
		#endif

		conn->schedule_ack();
	}

	return (size_t)(packet_end - data);
}

inline byte UTP_Version(PacketFormatV1 const* pf)
{
	return (pf->type() < ST_NUM_STATES && pf->ext < 3 ? pf->version() : 0);
}

// UtpSocket 构造函数: 对每个新创建的套接字设定一组保守的初始值。
// 关键初值含义:
//   - state_ = CS_UNINITIALIZED: 尚未调用 utp_initialize_socket, 不在哈希表中。
//   - seq_nr_ = 1, ack_nr_ = 0: 留出 0 作为"非法/初始"占位, 与 libutp 习惯一致。
//   - rtt = 0, rtt_var = 800, rto = 3000: 未拿到样本时使用 3s 兜底 RTO。
//   - max_window_ = 0: 真实窗口会在 utp_initialize_socket 中根据 mtu 设置。
//   - max_window_user_ = 255 * PACKET_SIZE: 假定的对端最大接收窗口 (255 个满包)。
//   - slow_start_ = true, ssthresh_ = opt_sndbuf_: 新连接默认走慢启动。
//   - inbuf_ / outbuf_ 初始化为 16 项的环形缓冲, 之后按需扩张。
UtpSocket::UtpSocket(utp_context* _ctx)
	: addr()
	, ctx(_ctx)
	, ida(-1)
	, retransmit_count_(0)
	, reorder_count_(0)
	, duplicate_ack_(0)
	, cur_window_packets_(0)
	, cur_window_(0)
	, max_window_(0)
	, opt_sndbuf_(_ctx->opt_sndbuf_)
	, opt_rcvbuf_(_ctx->opt_rcvbuf_)
	, target_delay_(_ctx->target_delay_)
	, got_fin(false)
	, got_fin_reached_(false)
	, fin_sent(false)
	, fin_sent_acked_(false)
	, read_shutdown_(false)
	, close_requested_(false)
	, fast_timeout_(false)
	, max_window_user_(255 * PACKET_SIZE)
	, state_(CS_UNINITIALIZED)
	, last_rwin_decay_(0)
	, eof_pkt_(0)
	, ack_nr_(0)
	, seq_nr_(1)
	, timeout_seq_nr_(0)
	, fast_resend_seq_nr_(1)
	, reply_micro_(0)
	, last_got_packet_(0)
	, last_sent_packet_(0)
	, last_measured_delay_(0)
	, last_maxed_out_window_(0)
	, userdata_(NULL)
	, rtt(0)
	, rtt_var(800)
	, rto(3000)
	, retransmit_timeout_(0)
	, rto_timeout_(0)
	, zerowindow_time_(0)
	, conn_seed_(0)
	, conn_id_recv_(0)
	, conn_id_send_(0)
	, last_rcv_win_(0)
	, extensions_()
	, mtu_discover_time_(0)
	, mtu_ceiling_(0)
	, mtu_floor_(0)
	, mtu_last_(0)
	, mtu_probe_seq_(0)
	, mtu_probe_size_(0)
	, average_delay_(0)
	, current_delay_sum_(0)
	, current_delay_samples_(0)
	, average_delay_base_(0)
	, average_sample_time_(0)
	, clock_drift_(0)
	, clock_drift_raw_(0)
	, slow_start_(true)
	, ssthresh_(_ctx->opt_sndbuf_)
{
	inbuf_.initialize(16);
	outbuf_.initialize(16);
	our_hist_.clear(0);
	their_hist_.clear(0);
	rtt_hist_.clear(0);
	memset(extensions_, 0, sizeof(extensions_));
	#ifdef _DEBUG
	memset(&stats_, 0, sizeof(utp_socket_stats));
	#endif
}

UtpSocket::~UtpSocket()
{
	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Killing socket");
	#endif

	utp_call_on_state_change(ctx, this, UTP_STATE_DESTROYING);

	if (ctx->last_utp_socket_ == this) {
		ctx->last_utp_socket_ = NULL;
	}

	// Remove object from the global hash table
	auto erased = ctx->sockets_.erase(UtpSocketKey(addr, conn_id_recv_));
	assert(erased == 1);

	// remove the socket from ack_sockets if it was there also
	remove_socket_from_ack_list(this);

	// Free all memory occupied by the socket object.
	for (size_t i = 0; i < inbuf_.buf_size(); i++) {
		delete (InboundPacket*)inbuf_.element(i);
	}
	for (size_t i = 0; i < outbuf_.buf_size(); i++) {
		delete (OutgoingPacket*)outbuf_.element(i);
	}
}

void utp_initialize_socket(	utp_socket *conn,
							const struct sockaddr *addr,
							socklen_t addrlen,
							bool need_seed_gen,
							uint32 conn_seed_,
							uint32 conn_id_recv_,
							uint32 conn_id_send_)
{
	utp::Address psaddr = utp::Address((const SOCKADDR_STORAGE*)addr, addrlen);

	if (need_seed_gen) {
		do {
			conn_seed_ = utp_call_get_random(conn->ctx, conn);
			// we identify v1 and higher by setting the first two bytes to 0x0001
			conn_seed_ &= 0xffff;
		} while (conn->ctx->sockets_.count(UtpSocketKey(psaddr, conn_seed_)));

		conn_id_recv_ += conn_seed_;
		conn_id_send_ += conn_seed_;
	}

	conn->state_					= CS_IDLE;
	conn->conn_seed_				= conn_seed_;
	conn->conn_id_recv_			= conn_id_recv_;
	conn->conn_id_send_			= conn_id_send_;
	conn->addr					= psaddr;
	conn->ctx->current_ms_		= utp_call_get_milliseconds(conn->ctx, NULL);
	conn->last_got_packet_		= conn->ctx->current_ms_;
	conn->last_sent_packet_		= conn->ctx->current_ms_;
	conn->last_measured_delay_	= conn->ctx->current_ms_ + 0x70000000;
	conn->average_sample_time_	= conn->ctx->current_ms_ + 5000;
	conn->last_rwin_decay_		= conn->ctx->current_ms_ - MAX_WINDOW_DECAY;

	conn->our_hist_.clear(conn->ctx->current_ms_);
	conn->their_hist_.clear(conn->ctx->current_ms_);
	conn->rtt_hist_.clear(conn->ctx->current_ms_);

	// initialize MTU floor and ceiling
	conn->mtu_reset();
	conn->mtu_last_ = conn->mtu_ceiling_;

	conn->ctx->sockets_[UtpSocketKey(conn->addr, conn->conn_id_recv_)] = conn;

	// we need to fit one packet in the window when we start the connection
	conn->max_window_ = conn->get_packet_size();

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP socket initialized");
	#endif
}
//创建utp的socket
utp_socket*	utp_create_socket(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return NULL;

	UtpSocket *conn = new UtpSocket(ctx);
	conn->fast_resend_seq_nr_ = conn->seq_nr_;

	return conn;
}

int utp_context_set_option(utp_context *ctx, int opt, int val)
{
	assert(ctx);
	if (!ctx) return -1;

	switch (opt) {
    	case UTP_LOG_NORMAL:
			ctx->log_normal_ = val ? true : false;
			return 0;

    	case UTP_LOG_MTU:
			ctx->log_mtu_ = val ? true : false;
			return 0;

    	case UTP_LOG_DEBUG:
			ctx->log_debug_ = val ? true : false;
			return 0;

    	case UTP_TARGET_DELAY:
			ctx->target_delay_ = val;
			return 0;

		case UTP_SNDBUF:
			assert(val >= 1);
			ctx->opt_sndbuf_ = val;
			return 0;

		case UTP_RCVBUF:
			assert(val >= 1);
			ctx->opt_rcvbuf_ = val;
			return 0;
	}
	return -1;
}

int utp_context_get_option(utp_context *ctx, int opt)
{
	assert(ctx);
	if (!ctx) return -1;

	switch (opt) {
    	case UTP_LOG_NORMAL:	return ctx->log_normal_ ? 1 : 0;
    	case UTP_LOG_MTU:		return ctx->log_mtu_    ? 1 : 0;
    	case UTP_LOG_DEBUG:		return ctx->log_debug_  ? 1 : 0;
    	case UTP_TARGET_DELAY:	return ctx->target_delay_;
		case UTP_SNDBUF:		return ctx->opt_sndbuf_;
		case UTP_RCVBUF:		return ctx->opt_rcvbuf_;
	}
	return -1;
}


int utp_setsockopt(UtpSocket* conn, int opt, int val)
{
	assert(conn);
	if (!conn) return -1;

	switch (opt) {

	case UTP_SNDBUF:
		assert(val >= 1);
		conn->opt_sndbuf_ = val;
		return 0;

	case UTP_RCVBUF:
		assert(val >= 1);
		conn->opt_rcvbuf_ = val;
		return 0;

	case UTP_TARGET_DELAY:
		conn->target_delay_ = val;
		return 0;
	}

	return -1;
}

int utp_getsockopt(UtpSocket* conn, int opt)
{
	assert(conn);
	if (!conn) return -1;

	switch (opt) {
		case UTP_SNDBUF:		return conn->opt_sndbuf_;
		case UTP_RCVBUF:		return conn->opt_rcvbuf_;
		case UTP_TARGET_DELAY:	return conn->target_delay_;
	}

	return -1;
}

// Try to connect to a specified host.
// utp_connect: 主动发起一次 uTP 连接 (TCP 的 connect 等价物)。
// 流程:
//   1. 校验状态必须为 CS_UNINITIALIZED, 否则直接销毁 (防止重复 connect)。
//   2. 调用 utp_initialize_socket 生成随机 conn_seed_ 与 conn_id_*, 状态 -> CS_SYN_SENT。
//   3. seq_nr_ 重新随机化 (避免被攻击者猜出初始 seq)。
//   4. 构造一个 ST_SYN 包 (只含头, 无负载) 放入 outbuf_, 设置初始 RTO (3s) 并发送。
//   5. 超时由 check_timeouts 处理: SYN_SENT 连续 2 次重传即判定失败。
int utp_connect(utp_socket *conn, const struct sockaddr *to, socklen_t tolen)
{
	assert(conn);
	if (!conn) return -1;

	assert(conn->state_ == CS_UNINITIALIZED);
	if (conn->state_ != CS_UNINITIALIZED) {
		conn->state_ = CS_DESTROY;
		return -1;
	}

	utp_initialize_socket(conn, to, tolen, true, 0, 0, 1);

	assert(conn->cur_window_packets_ == 0);
	assert(conn->outbuf_.get(conn->seq_nr_) == NULL);
	assert(sizeof(PacketFormatV1) == 20);

	conn->state_ = CS_SYN_SENT;
	conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);

	// Create and send a connect message

	// used in parse_log.py
	conn->log(UTP_LOG_NORMAL, "UTP_Connect conn_seed_:%u packet_size:%u (B) "
			"target_delay_:%u (ms) delay_history:%u "
			"delay_base_history:%u (minutes)",
			conn->conn_seed_, PACKET_SIZE, conn->target_delay_ / 1000,
			CUR_DELAY_SIZE, DELAY_BASE_HISTORY);

	// Setup initial timeout timer.
	conn->retransmit_timeout_ = 3000;
	conn->rto_timeout_ = conn->ctx->current_ms_ + conn->retransmit_timeout_;
	conn->last_rcv_win_ = conn->get_rcv_window();

	// if you need compatibiltiy with 1.8.1, use this. it increases attackability though.
	//conn->seq_nr_ = 1;
	conn->seq_nr_ = utp_call_get_random(conn->ctx, conn);

	// Create the connect packet.
	const size_t header_size = sizeof(PacketFormatV1);

	OutgoingPacket *pkt = new OutgoingPacket();
	pkt->data.resize(header_size);
	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();

	memset(p1, 0, header_size);
	// SYN packets are special, and have the receive ID in the connid field,
	// instead of conn_id_send_.
	p1->set_version(1);
	p1->set_type(ST_SYN);
	p1->ext = 0;
	p1->connid = conn->conn_id_recv_;
	p1->windowsize = (uint32)conn->last_rcv_win_;
	p1->seq_nr = conn->seq_nr_;
	pkt->transmissions = 0;
	pkt->length = header_size;
	pkt->payload = 0;

	/*
	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "Sending connect %s [%u].",
			addrfmt(conn->addr, addrbuf), conn_seed_);
	#endif
	*/

	// Remember the message in the outgoing queue.
	conn->outbuf_.ensure_size(conn->seq_nr_, conn->cur_window_packets_);
	conn->outbuf_.put(conn->seq_nr_, pkt);
	conn->seq_nr_++;
	conn->cur_window_packets_++;

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "incrementing cur_window_packets_:%u", conn->cur_window_packets_);
	#endif

	conn->send_packet(pkt);
	return 0;
}

// Returns 1 if the UDP payload was recognized as a UTP packet, or 0 if it was not
// utp_process_udp: 上层从 UDP socket 收到数据后调用的入口, 负责分派数据包。
// 返回 1 表示"识别为 uTP 协议", 0 表示"非 uTP 协议" (忽略)。
// 分派逻辑:
//   - 长度/版本校验失败直接返回。
//   - ST_RESET: 查找匹配连接并设置 CS_RESET/CS_DESTROY, 触发 on_error 回调。
//   - 非 SYN 包: 优先用 last_utp_socket 快路径查找, 否则走哈希表;
//                命中后调用 utp_process_incoming。
//   - SYN 包 (新建连接): 校验 ON_ACCEPT 回调、连接数限制、防火墙回调后,
//                创建新 UtpSocket, 状态 CS_SYN_RECV, 回 SYN-ACK, 触发 on_accept。
//   - 其它无法识别的包: 缓存到 rst_info, 按需回送 RST (抑制放大攻击)。
int utp_process_udp(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(ctx);
	if (!ctx) return 0;

	assert(buffer);
	if (!buffer) return 0;

	assert(to);
	if (!to) return 0;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u too small", addrfmt(addr, addrbuf), (uint)len);
		#endif
		return 0;
	}

	const PacketFormatV1 *pf1 = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf1);
	const uint32 id = uint32(pf1->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u version:%u unsupported version", addrfmt(addr, addrbuf), (uint)len, version);
		#endif

		return 0;
	}

	#if UTP_DEBUG_LOGGING
	ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u id:%u", addrfmt(addr, addrbuf), (uint)len, id);
	ctx->log(UTP_LOG_DEBUG, NULL, "recv id:%u seq_nr:%u ack_nr:%u", id, (uint)pf1->seq_nr, (uint)pf1->ack_nr);
	#endif
	//得到包的类型
	const byte flags = pf1->type();

	if (flags == ST_RESET) {
		// id is either our recv id or our send id
		// if it's our send id, and we initiated the connection, our recv id is id + 1
		// if it's our send id, and we did not initiate the connection, our recv id is id - 1
		// we have to check every case
		//在connection的列表中寻找我们自己
		UtpSocket* conn = nullptr;
		if (auto it = ctx->sockets_.find(UtpSocketKey(addr, id)); it != ctx->sockets_.end()) {
			conn = it->second;
		} else if (auto it2 = ctx->sockets_.find(UtpSocketKey(addr, id + 1)); it2 != ctx->sockets_.end() && it2->second->conn_id_send_ == id) {
			conn = it2->second;
		} else if (auto it3 = ctx->sockets_.find(UtpSocketKey(addr, id - 1)); it3 != ctx->sockets_.end() && it3->second->conn_id_send_ == id) {
			conn = it3->second;
		}
		if (conn) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv RST for existing connection");
			#endif
			//如果connection的状态是close_requested_，那么直接进入DESTROY
			if (conn->close_requested_)
				conn->state_ = CS_DESTROY;
			else
				conn->state_ = CS_RESET;

			utp_call_on_overhead_statistics(conn->ctx, conn, false, len + conn->get_udp_overhead(), close_overhead);
			const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET; //设置错误码
			utp_call_on_error(conn->ctx, conn, err);
		}
		else {
			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv RST for unknown connection");
			#endif
		}
		return 1;
	}
	else if (flags != ST_SYN) { //如果不是SYN包
		UtpSocket* conn = NULL;
		//获得utp链接信息，这里面使用了缓存
		if (ctx->last_utp_socket_ && ctx->last_utp_socket_->addr == addr && ctx->last_utp_socket_->conn_id_recv_ == id) {
			conn = ctx->last_utp_socket_;
		} else {
			auto it = ctx->sockets_.find(UtpSocketKey(addr, id));
			if (it != ctx->sockets_.end()) {
				conn = it->second;
				ctx->last_utp_socket_ = conn;
			}
		}

		if (conn) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv processing");
			#endif
			//处理utp包
			const size_t read = utp_process_incoming(conn, buffer, len);
			utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
			return 1;
		}
	}

	// We have not found a matching utp_socket, and this isn't a SYN.  Reject it.
	const uint32 seq_nr_ = pf1->seq_nr;
	if (flags != ST_SYN) { //非链接包
		ctx->current_ms_ = utp_call_get_milliseconds(ctx, NULL);
		//已经发送了一个RST包
		for (size_t i = 0; i < ctx->rst_info_.size(); i++) {
			if ((ctx->rst_info_[i].connid == id)   &&
				(ctx->rst_info_[i].addr   == addr) &&
				(ctx->rst_info_[i].ack_nr == seq_nr_))
			{
				ctx->rst_info_[i].timestamp = ctx->current_ms_;

				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (stored)");
				#endif

				return 1;
			}
		}

		if (ctx->rst_info_.size() > RST_INFO_LIMIT) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (limit at %u stored)", (uint)ctx->rst_info_.size());
			#endif

			return 1;
		}

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv send RST to non-SYN (%u stored)", (uint)ctx->rst_info_.size());
		#endif

		ctx->rst_info_.emplace_back(); RstInfo &r = ctx->rst_info_.back();
		r.addr = addr;
		r.connid = id;
		r.ack_nr = seq_nr_;
		r.timestamp = ctx->current_ms_;
		//发送RST信息
		UtpSocket::send_rst(ctx, addr, id, seq_nr_, utp_call_get_random(ctx, NULL));
		return 1;
	}

	if (ctx->callbacks_[UTP_ON_ACCEPT]) {

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Incoming connection from %s", addrfmt(addr, addrbuf));
		#endif

		if (ctx->sockets_.count(UtpSocketKey(addr, id + 1))) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, connection already exists");
			#endif

			return 1;
		}

		if (ctx->sockets_.size() > 3000) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, too many uTP sockets %zu", ctx->sockets_.size());
			#endif

			return 1;
		}
		// true means yes, block connection.  false means no, don't block.
		if (utp_call_on_firewall(ctx, to, tolen)) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, firewall callback returned true");
			#endif

			return 1;
		}
		//创建一个全新的链接
		// Create a new UTP socket to handle this new connection
		UtpSocket *conn = utp_create_socket(ctx);
		utp_initialize_socket(conn, to, tolen, false, id, id+1, id); //初始化该链接
		conn->ack_nr_ = seq_nr_;
		conn->seq_nr_ = utp_call_get_random(ctx, NULL);
		conn->fast_resend_seq_nr_ = conn->seq_nr_;
		conn->state_ = CS_SYN_RECV; //更新状态
		//处理SYN包
		const size_t read = utp_process_incoming(conn, buffer, len, true);

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv send connect ACK");
		#endif
		//发送ACK
		conn->send_ack(true);
		//链接成功，回调相应函数
		utp_call_on_accept(ctx, conn, to, tolen);

		// we report overhead after on_accept(), because the callbacks are setup now
		utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead); // SYN
		utp_call_on_overhead_statistics(conn->ctx, conn, true,  conn->get_overhead(),                    ack_overhead);    // SYNACK
	}
	else {

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, UTP_ON_ACCEPT callback not set");
		#endif

	}

	return 1;
}

// Called by utp_process_icmp_fragmentation() and utp_process_icmp_error() below
static UtpSocket* parse_icmp_payload(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(ctx);
	if (!ctx) return NULL;

	assert(buffer);
	if (!buffer) return NULL;

	assert(to);
	if (!to) return NULL;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	// ICMP packets are only required to quote the first 8 bytes of the layer4
	// payload.  The UDP payload is 8 bytes, and the UTP header is another 20
	// bytes.  So, in order to find the entire UTP header, we need the ICMP
	// packet to quote 28 bytes.
	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: runt length %d", addrfmt(addr, addrbuf), len);
		#endif
		return NULL;
	}

	const PacketFormatV1 *pf = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf);
	const uint32 id = uint32(pf->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: not UTP version 1", addrfmt(addr, addrbuf));
		#endif
		return NULL;
	}

	UtpSocket* conn = nullptr;
	if (auto it = ctx->sockets_.find(UtpSocketKey(addr, id)); it != ctx->sockets_.end()) {
		conn = it->second;
	} else if (auto it2 = ctx->sockets_.find(UtpSocketKey(addr, id + 1)); it2 != ctx->sockets_.end() && it2->second->conn_id_send_ == id) {
		conn = it2->second;
	} else if (auto it3 = ctx->sockets_.find(UtpSocketKey(addr, id - 1)); it3 != ctx->sockets_.end() && it3->second->conn_id_send_ == id) {
		conn = it3->second;
	}
	if (conn) {
		return conn;
	}

	#if UTP_DEBUG_LOGGING
	ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: No matching connection found for id %u", addrfmt(addr, addrbuf), id);
	#endif
	return NULL;
}

// Should be called when an ICMP Type 3, Code 4 packet (fragmentation needed) is received, to adjust the MTU
//
// Returns 1 if the UDP payload (delivered in the ICMP packet) was recognized as a UTP packet, or 0 if it was not
//
// @ctx: utp_context
// @buf: Contents of the original UDP payload, which the ICMP packet quoted.  *Not* the ICMP packet itself.
// @len: buffer length
// @to: destination address of the original UDP pakcet
// @tolen: address length
// @next_hop_mtu: 
int utp_process_icmp_fragmentation(utp_context *ctx, const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu)
{
	UtpSocket* conn = parse_icmp_payload(ctx, buffer, len, to, tolen);
	if (!conn) return 0;

	// Constrain the next_hop_mtu to sane values.  It might not be initialized or sent properly
	if (next_hop_mtu >= 576 && next_hop_mtu < 0x2000) {
		conn->mtu_ceiling_ = min<uint32>(next_hop_mtu, conn->mtu_ceiling_);
		conn->mtu_search_update();
		// this is something of a speecial case, where we don't set mtu_last_
		// to the value in between the floor and the ceiling. We can update the
		// floor, because there might be more network segments after the one
		// that sent this ICMP with smaller MTUs. But we want to test this
		// MTU size first. If the next probe gets through, mtu_floor_ is updated
		conn->mtu_last_ = conn->mtu_ceiling_;
	} else {
		// Otherwise, binary search. At this point we don't actually know
		// what size the packet that failed was, and apparently we can't
		// trust the next hop mtu either. It seems reasonably conservative
		// to just lower the ceiling. This should not happen on working networks
		// anyway.
		conn->mtu_ceiling_ = (conn->mtu_floor_ + conn->mtu_ceiling_) / 2;
		conn->mtu_search_update();
	}

	conn->log(UTP_LOG_MTU, "MTU [ICMP] floor:%d ceiling:%d current:%d", conn->mtu_floor_, conn->mtu_ceiling_, conn->mtu_last_);
	return 1;
}

// Should be called when an ICMP message is received that should tear down the connection.
//
// Returns 1 if the UDP payload (delivered in the ICMP packet) was recognized as a UTP packet, or 0 if it was not
//
// @ctx: utp_context
// @buf: Contents of the original UDP payload, which the ICMP packet quoted.  *Not* the ICMP packet itself.
// @len: buffer length
// @to: destination address of the original UDP pakcet
// @tolen: address length
int utp_process_icmp_error(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	UtpSocket* conn = parse_icmp_payload(ctx, buffer, len, to, tolen);
	if (!conn) return 0;

	const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	switch(conn->state_) {
		// Don't pass on errors for idle/closed connections
		case CS_IDLE:
			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s in state CS_IDLE, ignoring", addrfmt(addr, addrbuf));
			#endif
			return 1;

		default:
			if (conn->close_requested_) {
				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s after close, setting state to CS_DESTROY and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_DESTROY;
			} else {
				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s, setting state to CS_RESET and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_RESET;
			}
			break;
	}

	utp_call_on_error(conn->ctx, conn, err);
	return 1;
}

// Write bytes to the UTP socket.  Returns the number of bytes written.
// 0 indicates the socket is no longer writable, -1 indicates an error
ssize_t utp_writev(utp_socket *conn, struct utp_iovec *iovec_input, size_t num_iovecs)
{
	static utp_iovec iovec[UTP_IOV_MAX];

	assert(conn);
	if (!conn) return -1;

	assert(iovec_input);
	if (!iovec_input) return -1;

	assert(num_iovecs);
	if (!num_iovecs) return -1;

	if (num_iovecs > UTP_IOV_MAX)
		num_iovecs = UTP_IOV_MAX;

	memcpy(iovec, iovec_input, sizeof(struct utp_iovec)*num_iovecs);

	size_t bytes = 0;
	size_t sent = 0;
	for (size_t i = 0; i < num_iovecs; i++)
		bytes += iovec[i].iov_len;

	#if UTP_DEBUG_LOGGING
	size_t param = bytes;
	#endif

	if (conn->state_ != CS_CONNECTED) {
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "UTP_Write %u bytes = false (not CS_CONNECTED)", (uint)bytes);
		#endif
		return 0;
	}

	if (conn->fin_sent) {
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "UTP_Write %u bytes = false (fin_sent already)", (uint)bytes);
		#endif
		return 0;
	}

	conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);

	// don't send unless it will all fit in the window
	size_t packet_size = conn->get_packet_size();
	size_t num_to_send = min<size_t>(bytes, packet_size);
	while (!conn->is_full(num_to_send)) {
		// Send an outgoing packet.
		// Also add it to the outgoing of packets that have been sent but not ACKed.

		bytes -= num_to_send;
		sent  += num_to_send;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Sending packet. seq_nr_:%u ack_nr_:%u wnd:%u/%u/%u rcv_win:%u size:%u cur_window_packets_:%u",
			conn->seq_nr_, conn->ack_nr_,
			(uint)(conn->cur_window_ + num_to_send),
			(uint)conn->max_window_, (uint)conn->max_window_user_,
			(uint)conn->last_rcv_win_, num_to_send,
			conn->cur_window_packets_);
		#endif
		conn->write_outgoing_packet(num_to_send, ST_DATA, iovec, num_iovecs);
		num_to_send = min<size_t>(bytes, packet_size);

		if (num_to_send == 0) {
			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "UTP_Write %u bytes = true", (uint)param);
			#endif
			return sent;
		}
	}

	bool full = conn->is_full();
	if (full) {
		// mark the socket as not being writable.
		conn->state_ = CS_CONNECTED_FULL;
	}

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP_Write %u bytes = %s", (uint)bytes, full ? "false" : "true");
	#endif

	// returns whether or not the socket is still writable
	// if the congestion window is not full, we can still write to it
	//return !full;
	return sent;
}

void utp_read_drained(utp_socket *conn)
{
	assert(conn);
	if (!conn) return;

	assert(conn->state_ != CS_UNINITIALIZED);
	if (conn->state_ == CS_UNINITIALIZED) return;

	const size_t rcvwin = conn->get_rcv_window();

	if (rcvwin > conn->last_rcv_win_) {
		// If last window was 0 send ACK immediately, otherwise should set timer
		if (conn->last_rcv_win_ == 0) {
			conn->send_ack();
		} else {
			conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);
			conn->schedule_ack();
		}
	}
}

// Should be called each time the UDP socket is drained
void utp_issue_deferred_acks(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return;

	for (size_t i = 0; i < ctx->ack_sockets_.size(); i++) {
		UtpSocket *conn = ctx->ack_sockets_[i];
		conn->send_ack();
		i--;
	}
}

// Should be called every 500ms
// utp_check_timeouts: 应用层必须按 TIMEOUT_CHECK_INTERVAL (500ms) 节拍调用,
// 负责驱动整个 context 下的所有套接字的心跳与超时。
// 流程:
//   1. 节流: 距上次调用不足 500ms 直接返回。
//   2. 清理过期的 rst_info 缓存 (超过 RST_INFO_TIMEOUT = 10s), 收缩 vector。
//   3. 先把哈希表里的套接字指针快照到 vector, 再依次调用 check_timeouts:
//      拷贝到 vector 是为了在 check_timeouts 内部 delete 套接字时不会使迭代器失效。
//   4. 对状态为 CS_DESTROY 的套接字, 在遍历后统一 delete 释放资源。
void utp_check_timeouts(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return;
	//更新时间戳
	ctx->current_ms_ = utp_call_get_milliseconds(ctx, NULL);

	if (ctx->current_ms_ - ctx->last_check_ < TIMEOUT_CHECK_INTERVAL)
		return;

	ctx->last_check_ = ctx->current_ms_;

	for (size_t i = 0; i < ctx->rst_info_.size(); i++) {
		if ((int)(ctx->current_ms_ - ctx->rst_info_[i].timestamp) >= RST_INFO_TIMEOUT) {
			ctx->rst_info_[i] = std::move(ctx->rst_info_.back());
			ctx->rst_info_.pop_back();
			i--;
		}
	}
	if (ctx->rst_info_.size() != ctx->rst_info_.capacity()) {
		ctx->rst_info_.shrink_to_fit();
	}
	std::vector<UtpSocket*> sockets;
	sockets.reserve(ctx->sockets_.size());
	for (auto& [key, socket] : ctx->sockets_) {
		sockets.push_back(socket);
	}
	for (auto* conn : sockets) {
		conn->check_timeouts();

		if (conn->state_ == CS_DESTROY) {
			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Destroying");
			#endif
			delete conn;
		}
	}
}

int utp_getpeername(utp_socket *conn, struct sockaddr *addr, socklen_t *addrlen)
{
	assert(addr);
	if (!addr) return -1;

	assert(addrlen);
	if (!addrlen) return -1;

	assert(conn);
	if (!conn) return -1;

	assert(conn->state_ != CS_UNINITIALIZED);
	if (conn->state_ == CS_UNINITIALIZED) return -1;

	socklen_t len;
	const SOCKADDR_STORAGE sa = conn->addr.get_sockaddr_storage(&len);
	*addrlen = min(len, *addrlen);
	memcpy(addr, &sa, *addrlen);
	return 0;
}

int utp_get_delays(UtpSocket *conn, uint32 *ours, uint32 *theirs, uint32 *age)
{
	assert(conn);
	if (!conn) return -1;

	assert(conn->state_ != CS_UNINITIALIZED);
	if (conn->state_ == CS_UNINITIALIZED) {
		if (ours)   *ours   = 0;
		if (theirs) *theirs = 0;
		if (age)    *age    = 0;
		return -1;
	}

	if (ours)   *ours   = conn->our_hist_.get_value();
	if (theirs) *theirs = conn->their_hist_.get_value();
	if (age)    *age    = (uint32)(conn->ctx->current_ms_ - conn->last_measured_delay_);
	return 0;
}

// Close the UTP socket.
// It is not valid for the upper layer to refer to socket after it is closed.
// Data will keep to try being delivered after the close.
void utp_close(UtpSocket *conn)
{  //上层直接关闭utp的socket
	assert(conn);
	if (!conn) return;

	assert(conn->state_ != CS_UNINITIALIZED
		&& conn->state_ != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP_Close in state:%s", statenames[conn->state_]);
	#endif

	switch(conn->state_) {
	case CS_CONNECTED:
	case CS_CONNECTED_FULL:
		conn->read_shutdown_ = true; //关闭读状态
		conn->close_requested_ = true; //记录关闭请求
		if (!conn->fin_sent) { //如果没发送FIN包，立刻发送一个FIN包
			conn->fin_sent = true;
			conn->write_outgoing_packet(0, ST_FIN, NULL, 0);
		} else if (conn->fin_sent_acked_) {
			conn->state_ = CS_DESTROY; //如果FIN包已经被应答了，则进入DESTROY状态
		}
		break;

	case CS_SYN_SENT: //刚刚发送了SYN包，那么立刻更新RTO，然后进入DESTROY状态
		conn->rto_timeout_ = utp_call_get_milliseconds(conn->ctx, conn) + min<uint>(conn->rto * 2, 60);
		// fall through
	case CS_SYN_RECV:
		// fall through
	default:
		conn->state_ = CS_DESTROY;
		break;
	}

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP_Close end in state:%s", statenames[conn->state_]);
	#endif
}

void utp_shutdown(UtpSocket *conn, int how)
{
	assert(conn);
	if (!conn) return;

	assert(conn->state_ != CS_UNINITIALIZED
		&& conn->state_ != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP_shutdown(%d) in state:%s", how, statenames[conn->state_]);
	#endif

	if (how != SHUT_WR) {
		conn->read_shutdown_ = true;
	}
	if (how != SHUT_RD) {
		switch(conn->state_) {
		case CS_CONNECTED:
		case CS_CONNECTED_FULL:
			if (!conn->fin_sent) {
				conn->fin_sent = true;
				conn->write_outgoing_packet(0, ST_FIN, NULL, 0);
			}
			break;
		case CS_SYN_SENT:
			conn->rto_timeout_ = utp_call_get_milliseconds(conn->ctx, conn) + min<uint>(conn->rto * 2, 60);
		default:
			break;
		}
	}
}

utp_context* utp_get_context(utp_socket *socket) {
	assert(socket);
	return socket ? socket->ctx : NULL;
}

void* utp_set_userdata(utp_socket *socket, void *userdata_) {
	assert(socket);
	if (socket) socket->userdata_ = userdata_;
	return socket ? socket->userdata_ : NULL;
}

void* utp_get_userdata(utp_socket *socket) {
	assert(socket);
	return socket ? socket->userdata_ : NULL;
}

void UtpContext::log(int level, utp_socket *socket, char const *fmt, ...)
{
	if (!would_log(level)) {
		return;
	}

	va_list va;
	va_start(va, fmt);
	log_unchecked(socket, fmt, va);
	va_end(va);
}

void UtpContext::log_unchecked(utp_socket *socket, char const *fmt, ...)
{
	va_list va;
	char buf[4096];

	va_start(va, fmt);
	vsnprintf(buf, 4096, fmt, va);
	buf[4095] = '\0';
	va_end(va);

	utp_call_log(this, socket, (const byte *)buf);
}

inline bool UtpContext::would_log(int level)
{
	if (level == UTP_LOG_NORMAL) return log_normal_;
	if (level == UTP_LOG_MTU) return log_mtu_;
	if (level == UTP_LOG_DEBUG) return log_debug_;
	return true;
}

utp_socket_stats* utp_get_stats(utp_socket *socket)
{
	#ifdef _DEBUG
		assert(socket);
		if (!socket) return NULL;
		socket->stats_.mtu_guess = socket->mtu_last_ ? socket->mtu_last_ : socket->mtu_ceiling_;
		return &socket->stats_;
	#else
		return NULL;
	#endif
}
