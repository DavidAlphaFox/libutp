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

class UtpSocket {
public:
	UtpSocket(utp_context* _ctx);
	~UtpSocket();

	utp::Address addr;              // 对端地址
	utp_context *ctx;               // 所属上下文

	int ida;                        // 延迟ACK列表索引（用于批量发送ACK）

	uint16 reorder_count_;          // 乱序包计数（收到非期望序列号的包）
	byte duplicate_ack_;            // 重复ACK计数（用于快速重传检测）
	uint16 cur_window_packets_;     // 当前在飞（未确认）的包数量

	size_t opt_sndbuf_;             // 发送缓冲区大小（用户设定）
	size_t opt_rcvbuf_;             // 接收缓冲区大小（用户设定）

	size_t target_delay_;           // 目标延迟（LEDBAT拥塞控制目标，微秒）

	bool got_fin:1;                 // 是否收到对端的FIN包
	bool got_fin_reached_:1;        // 是否已处理到FIN包位置（所有前置数据已接收）

	bool fin_sent:1;                // 是否已发送FIN包
	bool fin_sent_acked_:1;         // 已发送的FIN是否已被确认

	bool read_shutdown_:1;          // 读端是否已关闭（收到FIN且数据读完）
	bool close_requested_:1;        // 用户是否请求关闭连接

	bool fast_timeout_:1;           // 是否启用快速超时（缩短重传超时时间）

	size_t max_window_user_;        // 用户设定的最大发送窗口
	CONN_STATE state_;              // 当前连接状态

	uint16 eof_pkt_;                // FIN包的序列号（end-of-file packet）

	uint16 ack_nr_;                 // 期望收到的下一个序列号（已确认的最大序列号+1）
	uint16 seq_nr_;                 // 下一个待发送包的序列号

	uint16 timeout_seq_nr_;         // 超时检查时记录的序列号（用于区分超时包）

	uint16 fast_resend_seq_nr_;     // 快速重传起始序列号

	uint32 reply_micro_;            // 延迟反馈值（对端计算的单向延迟，微秒）

	uint64 last_got_packet_;        // 最后一次收到数据包的时间戳（微秒）
	uint64 last_sent_packet_;       // 最后一次发送数据包的时间戳（微秒）
	uint64 last_measured_delay_;    // 最后一次测量延迟的时间戳（微秒）

	void *userdata_;                // 用户数据指针（由应用层设置）

	uint32 conn_seed_;              // 连接种子（用于生成连接ID）
	uint32 conn_id_recv_;           // 接收方向连接ID（用于匹配入站包）
	uint32 conn_id_send_;           // 发送方向连接ID（用于匹配出站包）
	size_t last_rcv_win_;           // 最后通告给对端的接收窗口大小

	byte extensions_[8];            // 扩展位标志（支持选择性确认等扩展）

	MtuDiscovery mtu_;              // MTU探测对象（用于路径MTU发现）
	LedbatController cc_;           // LEDBAT拥塞控制对象

	utp::RawSequenceBuffer inbuf_, outbuf_;  // 入站/出站序列缓冲区（按序列号索引）

	#ifdef _DEBUG
	utp_socket_stats stats_;        // 统计信息（仅DEBUG模式）
	#endif

	// 记录日志（带格式化），自动添加socket标识信息
	void log(int level, char const *fmt, ...)
	{
		va_list va;
		char buf[4096], buf2[4096];

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

	// 调度发送ACK（将本socket加入延迟ACK列表，批量确认）
	void schedule_ack();

	// 获取当前接收窗口大小（接收缓冲区剩余空间）
	size_t get_rcv_window()
	{
		const size_t numbuf = utp_call_get_read_buffer_size(this->ctx, this);
		assert((int)numbuf >= 0);
		return opt_rcvbuf_ > numbuf ? opt_rcvbuf_ - numbuf : 0;
	}

	// 获取包头大小（uTP V1包格式固定大小）
	size_t get_header_size() const
	{
		return sizeof(utp::wire::PacketFormatV1);
	}

	// 获取UDP MTU（通过回调查询底层UDP的MTU）
	size_t get_udp_mtu()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_mtu(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	// 获取UDP开销（IP层+UDP层头部大小）
	size_t get_udp_overhead()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_overhead(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	// 获取总开销（UDP开销 + uTP包头大小）
	size_t get_overhead()
	{
		return get_udp_overhead() + get_header_size();
	}

	// 发送原始数据到对端（通过UDP回调）
	void send_data(byte* b, size_t length, BandwidthType type, uint32 flags = 0);

	// 发送ACK确认包（synack=true时发送SYNACK）
	void send_ack(bool synack = false);

	// 发送保活包（防止连接因空闲而超时断开）
	void send_keep_alive();

	// 发送RST重置包（强制关闭连接，静态方法无需socket实例）
	static void send_rst(utp_context *ctx,
						 const utp::Address &addr, uint32 conn_id_send_,
						 uint16 ack_nr_, uint16 seq_nr_);

	// 发送一个已构造好的出站数据包（处理重传逻辑和带宽统计）
	void send_packet(OutgoingPacket *pkt);

	// 检查发送窗口是否已满（bytes=-1时使用默认包大小）
	bool is_full(int bytes = -1);
	// 刷新发送队列（尽可能发送缓冲区中的包）
	bool flush_packets();
	// 构造并发送出站数据包（从iovec填充数据）
	void write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs);

	#ifdef _DEBUG
	// 检查socket内部状态一致性（调试用不变量检查）
	void check_invariant();
	#endif

	// 检查并处理超时（重传、连接超时、保活等）
	void check_timeouts();
	// 确认指定序列号的包（从outbuf_中移除，更新窗口）
	int ack_packet(uint16 seq);
	// 计算选择性确认(SACK)覆盖的字节数（用于拥塞控制）
	size_t selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt);
	// 处理选择性确认(SACK)（标记已确认的包，触发快速重传）
	void selective_ack(uint base, const byte *mask, byte len);
	// 获取当前包大小（考虑MTU探测结果）
	size_t get_packet_size() const;
};
