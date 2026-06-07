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

// =============================================================================
// 模块说明
// -----------------------------------------------------------------------------
// 本文件实现 uTP（Micro Transport Protocol，Micro 传输协议）的 Socket 层核心逻辑。
// uTP 是一种在 UDP 之上提供可靠、有序字节流传输的协议，由 BitTorrent 扩展协议
// BEP-29 定义，底层采用 LEDBAT 拥塞控制算法以尽量降低对其他应用的影响。
//
// 本文件包含以下主要功能：
//   1. 数据包的发送与封装：包括 DATA / FIN / STATE / RESET / SYN 等类型。
//   2. 接收窗口与发送窗口管理：判断发送窗口是否已满以控制数据流入。
//   3. ACK 处理：包括普通 ACK 和选择性 ACK（EACK/SACK），用于乱序与丢包检测。
//   4. 超时与重传：基于 RTO（重传超时）的丢包恢复与快速重传。
//   5. Path MTU Discovery（路径 MTU 发现）：通过二分搜索探测最佳 MTU。
//   6. 保活（KeepAlive）和连接复位（RST）机制。
//
// 主要数据结构：
//   - UtpSocket：表示一个 uTP 连接，包含收发缓冲区、状态机、拥塞控制等。
//   - OutgoingPacket / InboundPacket：出站 / 入站数据包对象。
//   - MtuDiscovery：MTU 探测的状态机（floor/ceiling/last + 在途探测包）。
//   - LedbatController：LEDBAT 拥塞控制器（延迟基线的 cwnd 调节）。
//
// 设计要点：
//   - 延迟 ACK：通过 ack_sockets_ 列表在指定时机统一发送 ACK，节约带宽。
//   - 快速重传：连续 DUPLICATE_ACKS_BEFORE_RESEND 次重复 ACK 触发。
//   - MTU 探测：嵌入在普通数据报文中，通过 ACK 中是否携带 EACK 反馈。
// =============================================================================

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <vector>
#include <memory>

#include "utp_socket.hpp"
#include "utp_callbacks.h"

using utp::wrapping_compare_less;
using utp::wire::PacketFormatV1;
using utp::wire::PacketFormatAckV1;
using utp::zeromem;
using std::min;
using std::max;
using std::clamp;

char addrbuf[65];

enum {
	ST_DATA = 0,
	ST_FIN = 1,
	ST_STATE = 2,
	ST_RESET = 3,
	ST_SYN = 4,
	ST_NUM_STATES,
};

static const cstr flagnames[] = {
	"ST_DATA","ST_FIN","ST_STATE","ST_RESET","ST_SYN"
};

static const cstr statenames[] = {
	"UNINITIALIZED", "IDLE","SYN_SENT", "SYN_RECV", "CONNECTED","CONNECTED_FULL","DESTROY_DELAY","RESET","DESTROY"
};

// -----------------------------------------------------------------------------
// 函数：remove_socket_from_ack_list
// 功能：将指定 socket 从全局延迟 ACK 列表（ctx->ack_sockets_）中移除。
//
// 上下文：
//   uTP 实现了延迟 ACK 机制，需要发送 ACK 的 socket 会被 push 到 ack_sockets_
//   列表中等待定时器统一处理。本函数在 socket 关闭、ACK 已发送、或不再需要
//   延迟 ACK 时被调用。
//
// 参数：
//   conn - 指向需要从延迟 ACK 列表中移除的 UtpSocket 指针。
//
// 算法说明（"swap-pop" 优化）：
//   为避免从 vector 中部删除元素导致 O(n) 的元素移动，本实现采用经典的
//   "swap with last + pop_back" 技巧：
//     1. 取列表最后一个元素 last。
//     2. 将 conn 所在的位置用 last 覆盖。
//     3. 更新 last 的 ida（位置索引）指向 conn 原来的位置。
//     4. 将 conn 的 ida 置为 -1，标记其不在列表中。
//     5. 弹出末尾元素。
//   ida 字段是 socket 在列表中的位置下标，用于 O(1) 定位以便快速移除。
//
// 副作用：
//   - 修改 conn->ida（置为 -1 表示不在列表中）。
//   - 修改 ctx->ack_sockets_ 容器内容。
// -----------------------------------------------------------------------------
void remove_socket_from_ack_list(UtpSocket *conn)
{
	if (conn->ida >= 0)
	{
		// 取出列表末尾元素，用于填补待删除元素的位置
		UtpSocket *last = conn->ctx->ack_sockets_.back();

		// 防御性检查：确保 ida 索引与容器实际状态一致
		assert(last->ida < (int)(conn->ctx->ack_sockets_.size()));
		assert(conn->ctx->ack_sockets_[last->ida] == last);

		// 用 last 覆盖待删除元素的位置，并同步更新 last 的 ida
		last->ida = conn->ida;
		conn->ctx->ack_sockets_[conn->ida] = last;

		// 标记当前 socket 已不在列表中
		conn->ida = -1;

		// 弹出末尾元素（swap-pop 完成）
		conn->ctx->ack_sockets_.pop_back();
	}
}

// -----------------------------------------------------------------------------
// 函数：utp_register_sent_packet
// 功能：将本次发送的 UDP 数据包按尺寸分类，累加到上下文统计计数器中。
//
// 上下文：
//   维护 utp_context 中的原始（raw）发送统计。统计按尺寸分为 5 个桶：
//   EMPTY / SMALL / MID / BIG / HUGE，用于上层分析流量特征。
//
// 参数：
//   ctx    - 指向 utp_context 全局上下文的指针。
//   length - 本次发送数据包的字节数。
//
// 桶边界（典型值，参见 utp::wire::packet_size_from_bucket）：
//   EMPTY  ≤ 23B
//   SMALL  ≤ 373B
//   MID    ≤ 723B
//   BIG    ≤ 1400B
//   HUGE   > 1400B
//
// 注意：
//   该函数仅在 UTP_ENABLE_STATS 编译选项启用时被调用，目前总是被调用
//   （由 send_to_addr 调用），通过条件编译控制编译产物。
// -----------------------------------------------------------------------------
static void utp_register_sent_packet(utp_context *ctx, size_t length)
{
	// 尺寸 < MID 区间：进一步细分为 EMPTY / SMALL / MID 三个桶
	if (length <= PACKET_SIZE_MID) {
		if (length <= PACKET_SIZE_EMPTY) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (length <= PACKET_SIZE_SMALL) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_MID_BUCKET]++;
	} else {
		// 尺寸 ≥ MID 区间：细分为 BIG / HUGE 两个桶
		if (length <= PACKET_SIZE_BIG) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_BIG_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

// -----------------------------------------------------------------------------
// 函数：send_to_addr
// 功能：通过 utp 回调接口将一段字节流作为 UDP 数据报发送到指定对端地址。
//
// 上下文：
//   这是 uTP 所有出站数据包的最终发送出口。utp_call_sendto 是一个由调用方
//   注册的回调函数（参见 utp_callbacks.h），实际执行 sendto() 系统调用。
//
// 参数：
//   ctx    - 指向 utp_context 全局上下文的指针。
//   p      - 指向待发送数据（含 uTP 头）的缓冲区起始地址。
//   len    - 待发送的字节数。
//   addr   - 目标对端的 utp::Address 套接字地址。
//   flags  - 透传给底层 sendto 的额外标志（如 UTP_UDP_DONTFRAG 表示
//            在 MTU 探测时禁止分片）。
//
// 算法说明：
//   1. 将 utp::Address 内部存储转换为平台原生 sockaddr 结构。
//   2. 记录本次发送的长度到 raw 统计。
//   3. 调用 sendto 回调完成实际发送。
// -----------------------------------------------------------------------------
void send_to_addr(utp_context *ctx, const byte *p, size_t len, const utp::Address &addr, int flags = 0)
{
	socklen_t tolen;
	// 取出平台原生 sockaddr 表示
	SOCKADDR_STORAGE to = addr.get_sockaddr_storage(&tolen);
	// 计入发送统计
	utp_register_sent_packet(ctx, len);
	// 通过注册的回调执行真正的 sendto
	utp_call_sendto(ctx, NULL, p, len, (const struct sockaddr *)&to, tolen, flags);
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::schedule_ack
// 功能：将当前 socket 标记为"需要发送 ACK"，加入全局延迟 ACK 处理列表。
//
// 上下文：
//   uTP 实现延迟 ACK 机制。当收到数据时不会立即回复 ACK，而是将 socket
//   加入 ctx->ack_sockets_ 列表，由定时器统一触发 send_ack()。这样做的好处：
//     - 减少 ACK 包的数量（多个数据包可合并到一个 ACK）。
//     - 减少空载报文开销，提高带宽利用率。
//   已被调度（ida != -1）的 socket 不会被重复加入。
//
// 参数：无
//
// 算法说明：
//   ida == -1 表示当前 socket 不在延迟 ACK 列表中；否则说明已加入，无需重复。
//   加入时记录其在 vector 中的下标到 ida 字段，以便 remove_socket_from_ack_list
//   可以 O(1) 移除。
// -----------------------------------------------------------------------------
void UtpSocket::schedule_ack()
{
	if (ida == -1){
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack");
		#endif
		// 当前未在列表中：加入列表尾部，并记录其下标到 ida
		ctx->ack_sockets_.push_back(this); ida = ctx->ack_sockets_.size() - 1;
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack: already in list");
		#endif
	}
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::send_data
// 功能：填充 uTP 数据包头部的元数据并执行实际发送。
//
// 上下文：
//   这是 UtpSocket 内所有出站数据包（DATA / FIN / STATE / SYN 等）共用的
//   发送入口。负责：
//     - 写入时间戳（用于 RTT 测量）。
//     - 写入延迟反馈（reply_micro，告知对端我方测得的延迟）。
//     - 维护 last_sent_packet_（保活逻辑依赖该时间戳）。
//     - 上报带宽统计。
//     - 真正调用 send_to_addr 完成 UDP 发送。
//     - 从延迟 ACK 列表中移除自己（因为 ACK 已随包发出）。
//
// 参数：
//   b      - 指向已分配好的 PacketFormatV1 头部 + payload 的缓冲区。
//   length - 数据包总长度（头部 + payload）。
//   type   - 带宽统计分类（payload / overhead / retransmit / connect / close / ack）。
//   flags  - 透传给底层 sendto 的额外标志。
//
// 算法说明：
//   1. 读取当前微秒时间戳并写入 tv_usec 字段。
//   2. 写入 reply_micro_ 反馈给对端的延迟测量值。
//   3. 报告上层统计。
//   4. 调试日志输出数据包概要。
//   5. send_to_addr 真正发送。
//   6. remove_socket_from_ack_list 清理延迟 ACK 调度。
// -----------------------------------------------------------------------------
void UtpSocket::send_data(byte* b, size_t length, BandwidthType type, uint32 flags)
{
	// 取当前微秒时间戳，填入包头（对端用于 RTT 测量）
	uint64 time = utp_call_get_microseconds(ctx, this);

	PacketFormatV1* b1 = (PacketFormatV1*)b;
	b1->tv_usec = (uint32)time;
	// reply_micro：发送方测得的延迟反馈，对端 LEDBAT 控制器将以此为目标延迟基线
	b1->reply_micro = reply_micro_;

	// 记录最近一次发送时间，用于 KEEPALIVE_INTERVAL 触发保活
	last_sent_packet_ = ctx->current_ms_;

	#ifdef _DEBUG
	// 调试模式下累计发送字节与包数
	stats_.nbytes_xmit += length;
	++stats_.nxmit;
	#endif

	if (true) {
		size_t n;
		// 区分 payload 与 overhead：payload 上报两次（先 payload 再 overhead）
		if (type == payload_bandwidth) {
			type = header_overhead;
			n = get_overhead();
		} else {
			n = length + get_udp_overhead();
		}
		// 上报带宽统计：true 表示发送方向
		utp_call_on_overhead_statistics(ctx, this, true, n, type);
	}
#if UTP_DEBUG_LOGGING
	// 调试日志：解码包头字段输出
	int flags2 = b1->type();
	uint16 seq_nr_ = b1->seq_nr;
	uint16 ack_nr_ = b1->ack_nr;
	log(UTP_LOG_DEBUG, "send %s len:%u id:%u timestamp:" I64u " reply_micro_:%u flags:%s seq_nr_:%u ack_nr_:%u",
		addrfmt(addr, addrbuf), (uint)length, conn_id_send_, time, reply_micro_, flagnames[flags2],
		seq_nr_, ack_nr_);
#endif
	// 通过回调真正发送 UDP 报文
	send_to_addr(ctx, b, length, addr, flags);
	// 数据包已发出，移除自己可能存在的延迟 ACK 调度
	remove_socket_from_ack_list(this);
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::send_ack
// 功能：构造并发送一个 STATE 类型的 ACK 包，支持普通 ACK 与选择性 ACK（EACK）。
//
// 上下文：
//   收到对端数据包后，本 socket 会 schedule_ack 调度延迟 ACK，最终通过本函数
//   将 ACK 发出。当检测到乱序包时，会附加 EACK（Selective Acknowledgment）
//   位图，告知对端哪些后续序号已收到。
//
// 参数：
//   synack - true 表示这是对 SYN 的 ACK（不允许携带 EACK 扩展）。
//
// 算法说明：
//   1. 计算当前接收窗口大小 last_rcv_win_。
//   2. 填充 PacketFormatV1 通用字段（版本、类型、连接 ID、ack_nr、seq_nr、
//      windowsize）。
//   3. 若 reorder_count_ != 0 且尚未完成 FIN 接收：
//        - 启用 EACK 扩展（pf.ext = 1）。
//        - 扫描接收缓冲区中 ack_nr_+2 ~ ack_nr_+window 范围，
//          把已收到的序号对应的 bit 置 1。
//        - 限制 EACK 位图最大为 30 bits（4 字节 + 14 头部窗口）。
//   4. 通过 send_data 发送，并通过 ack_overhead 类别上报带宽统计。
//   5. 从延迟 ACK 列表移除（ACK 已发出）。
//
// 注意：
//   - EACK 范围 14+16 = 30 位（4 字节 32 位中有 2 位是扩展头）。
//   - 序号 ack_nr_+1 应为空（连续），这是 TCP-like 累积 ACK 的语义。
// -----------------------------------------------------------------------------
void UtpSocket::send_ack(bool synack)
{
	PacketFormatAckV1 pfa;
	zeromem(&pfa);

	size_t len;
	// 取当前接收窗口大小并记录到 last_rcv_win_
	last_rcv_win_ = get_rcv_window();
	pfa.pf.set_version(1);
	pfa.pf.set_type(ST_STATE);
	pfa.pf.ext = 0;
	pfa.pf.connid = conn_id_send_;
	pfa.pf.ack_nr = ack_nr_;
	pfa.pf.seq_nr = seq_nr_;
	pfa.pf.windowsize = (uint32)last_rcv_win_;
	len = sizeof(PacketFormatV1);

	// 存在乱序包且未完成 FIN：构造选择性 ACK（EACK）扩展
	if (reorder_count_ != 0 && !got_fin_reached_) {
		// SYN-ACK 不应携带 EACK
		assert(!synack);
		pfa.pf.ext = 1;          // 启用扩展
		pfa.ext_next = 0;         // 后续无更多扩展
		pfa.ext_len = 4;          // EACK 位图长度 4 字节
		uint m = 0;               // EACK 位图（共 32 位）

		// ack_nr_+1 应为空（保证 ACK 累积语义）
		assert(inbuf_.get(ack_nr_ + 1) == NULL);
		// 限制窗口扫描最大为 14+16 = 30 位
		size_t window = min<size_t>(14+16, inbuf_.size());
		for (size_t i = 0; i < window; i++) {
			// ack_nr_+i+2 处是否有包：bit i 表示 ack_nr_+i+2
			if (inbuf_.get(ack_nr_ + i + 2) != NULL) {
				m |= 1 << i;

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "EACK packet [%u]", ack_nr_ + i + 2);
				#endif
			}
		}
		// 写入位图（小端）
		pfa.acks[0] = (byte)m;
		pfa.acks[1] = (byte)(m >> 8);
		pfa.acks[2] = (byte)(m >> 16);
		pfa.acks[3] = (byte)(m >> 24);
		// 长度增加：4 字节位图 + 2 字节扩展头
		len += 4 + 2;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending EACK %u [%u] bits:[%032b]", ack_nr_, conn_id_send_, m);
		#endif
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending ACK %u [%u]", ack_nr_, conn_id_send_);
		#endif
	}

	// 发送 ACK（ack_overhead 类别计入统计）
	send_data((byte*)&pfa, len, ack_overhead);
	// 清理延迟 ACK 调度
	remove_socket_from_ack_list(this);
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::send_keep_alive
// 功能：发送保活包（Keep-Alive），用于防止长空闲期间链路被中间设备关闭。
//
// 上下文：
//   当连接已建立但长时间无数据交互时（KEEPALIVE_INTERVAL 触发），需主动向
//   对端发送一个保活包确认链路通畅。uTP 复用了 STATE 类型（纯 ACK）作为
//   保活机制。
//
// 算法说明：
//   关键技巧：先临时把 ack_nr_ 减 1，让 send_ack 报出的 ack_nr 等于
//   "上一序号"，然后再恢复。这样对端能够区分这是"保活"而非"有数据要确认"。
//
// 参数：无
// -----------------------------------------------------------------------------
void UtpSocket::send_keep_alive()
{
	// 临时将 ack_nr_ 减 1，让 send_ack 发出"过期一个序号"的 ACK（保活语义）
	ack_nr_--;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Sending KeepAlive ACK %u [%u]", ack_nr_, conn_id_send_);
	#endif
	// 复用 send_ack 发送 STATE 类型保活 ACK
	send_ack();
	// 恢复 ack_nr_ 到真实值
	ack_nr_++;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::send_rst
// 功能：发送一个 RST（Reset）复位包，强制关闭一个 uTP 连接。
//
// 上下文：
//   RST 用于异常断开（如收到非法包、协议错误、连接被对端拒绝等）。
//   这是个静态方法，可在 UtpSocket 尚未完全构造或已经销毁的情况下调用，
//   只需给定对端地址与连接号即可发送。
//
// 参数：
//   ctx            - utp 上下文，用于回调与统计。
//   addr           - 目标对端地址。
//   conn_id_send_  - 发送方向的连接 ID（对端用于识别）。
//   ack_nr_        - 当前确认序号（通常为收到的对端 seq）。
//   seq_nr_        - 当前发送序号。
// -----------------------------------------------------------------------------
void UtpSocket::send_rst(utp_context *ctx,
	const utp::Address &addr, uint32 conn_id_send_, uint16 ack_nr_, uint16 seq_nr_)
{
	PacketFormatV1 pf1;
	zeromem(&pf1);

	size_t len;
	pf1.set_version(1);
	pf1.set_type(ST_RESET);    // 标记为 RESET 类型
	pf1.ext = 0;
	pf1.connid = conn_id_send_;
	pf1.ack_nr = ack_nr_;
	pf1.seq_nr = seq_nr_;
	pf1.windowsize = 0;        // 复位包窗口大小无意义
	len = sizeof(PacketFormatV1);

	// 直接经 send_to_addr 发出，不经过 send_data（避免修改 last_sent_packet_ 等状态）
	send_to_addr(ctx, (const byte*)&pf1, len, addr);
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::send_packet
// 功能：发送一个出站数据包。设置 ACK 号、处理 MTU 探测标记、调用 send_data 实际发送。
//
// 参数：
//   pkt - 待发送的出站数据包指针。
//
// 算法说明：
//   1. 若是首次发送或需要重传，将 payload 加入在飞字节计数。
//   2. 填充当前 ack_nr_ 到包头。
//   3. 检查是否适合作为 MTU 探测包（满足 floor < size <= ceiling、无在途探测等条件）。
//   4. 递增传输计数并调用 send_data。
// -----------------------------------------------------------------------------
void UtpSocket::send_packet(OutgoingPacket *pkt)
{
	time_t cur_time = utp_call_get_milliseconds(this->ctx, this);

	if (pkt->transmissions == 0 || pkt->need_resend) {
		cc_.add_in_flight(pkt->payload);
	}

	pkt->need_resend = false;

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
	p1->ack_nr = ack_nr_;
	pkt->time_sent = utp_call_get_microseconds(this->ctx, this);

	bool use_as_mtu_probe = false;

 	if (mtu_.should_rediscover((uint64)cur_time)) {
		mtu_.reset((uint32)get_udp_mtu(), (uint64)cur_time);
	}

 	if (mtu_.floor() < mtu_.ceiling()
		&& pkt->length > mtu_.floor()
		&& pkt->length <= mtu_.ceiling()
		&& !mtu_.probe_seq()
		&& seq_nr_ != 1
		&& pkt->transmissions == 0) {

 		mtu_.set_probe((seq_nr_ - 1) & ACK_NR_MASK, (uint32)pkt->length);
		assert(pkt->length >= mtu_.floor());
		assert(pkt->length <= mtu_.ceiling());
 		use_as_mtu_probe = true;
		log(UTP_LOG_MTU, "MTU [PROBE] floor:%d ceiling:%d current:%d"
			, mtu_.floor(), mtu_.ceiling(), mtu_.probe_size());
  	}

	pkt->transmissions++;
	send_data((byte*)pkt->data.data(), pkt->length,
		(state_ == CS_SYN_SENT) ? connect_overhead
		: (pkt->transmissions == 1) ? payload_bandwidth
		: retransmit_overhead, use_as_mtu_probe ? UTP_UDP_DONTFRAG : 0);
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::is_full
// 功能：判断发送窗口是否已满（考虑拥塞窗口、用户缓冲区、在飞包数量）。
//
// 参数：
//   bytes - 本次欲发送的字节数；<0 时以当前 packet_size 计算。
//
// 返回值：
//   true  - 发送窗口已满，应暂停写入。
//   false - 发送窗口仍有空间。
//
// 算法说明：
//   1. 取 min(拥塞窗口, 用户发送缓冲区, 用户最大窗口) 作为上限 max_send。
//   2. 若在飞包数已达 OUTGOING_BUFFER_MAX_SIZE - 1，则窗口已满。
//   3. 否则若 cur_window + bytes > max_send，则窗口已满。
//   4. 窗口满时调用 mark_window_full，用于 LEDBAT 延迟测量。
// -----------------------------------------------------------------------------
bool UtpSocket::is_full(int bytes)
{
	size_t packet_size = get_packet_size();
	if (bytes < 0) bytes = packet_size;
	else if (bytes > (int)packet_size) bytes = (int)packet_size;
	size_t max_send = min(min(cc_.max_window(), opt_sndbuf_), max_window_user_);

	if (cur_window_packets_ >= OUTGOING_BUFFER_MAX_SIZE - 1) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "is_full:false cur_window_packets_:%d MAX:%d", cur_window_packets_, OUTGOING_BUFFER_MAX_SIZE - 1);
		#endif

		cc_.mark_window_full(ctx->current_ms_);
		return true;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "is_full:%s. cur_window_:%u pkt:%u max:%u cur_window_packets_:%u max_window_:%u"
		, (cc_.cur_window() + bytes > max_send) ? "true" : "false"
		, (uint)cc_.cur_window(), bytes, max_send, cur_window_packets_
		, (uint)cc_.max_window());
	#endif

	if (cc_.cur_window() + bytes > max_send) {
		cc_.mark_window_full(ctx->current_ms_);
		return true;
	}
	return false;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::flush_packets
// 功能：刷新待发送的数据包队列，将需要发送或重传的包逐个发出。
//
// 返回值：
//   true  - 发送窗口已满，尚有包未发出。
//   false - 队列已清空或无需发送。
//
// 算法说明：
//   1. 遍历 seq_nr_ - cur_window_packets_ 到 seq_nr_ 范围内的所有包。
//   2. 跳过已发送且无需重传的包。
//   3. 若发送窗口已满（is_full），立即返回 true。
//   4. 对最后一个包，仅在它是唯一在飞包或 payload 已达 packet_size 时才发送，
//      以便后续可能追加更多数据。
// -----------------------------------------------------------------------------
bool UtpSocket::flush_packets()
{
	size_t packet_size = get_packet_size();
	for (uint16 i = seq_nr_ - cur_window_packets_; i != seq_nr_; ++i) {
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(i);
		if (pkt == 0 || (pkt->transmissions > 0 && pkt->need_resend == false)) continue;
		if (is_full()) return true;

		if (i != ((seq_nr_ - 1) & ACK_NR_MASK) ||
			cur_window_packets_ == 1 ||
			pkt->payload >= packet_size) {
			send_packet(pkt);
		}
	}
	return false;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::write_outgoing_packet
// 功能：构造并写入一个出站数据包（支持 payload 追加到已有包或创建新包）。
//
// 参数：
//   payload     - 本次要写入的总 payload 字节数。
//   flags       - 包类型标志（ST_DATA 或 ST_FIN）。
//   iovec       - 指向 utp_iovec 数组，包含待发送的用户数据。
//   num_iovecs  - iovec 数组长度。
//
// 算法说明：
//   1. 若当前无在飞包，初始化 RTO。
//   2. 尝试将数据追加到最后一个未发送且未满的包中（减少小包数量）。
//   3. 若无法追加，创建新的 OutgoingPacket。
//   4. 从 iovec 中拷贝数据到包 payload 区。
//   5. 填充 uTP 头部（版本、类型、连接 ID、窗口大小、ack_nr）。
//   6. 若是新包，加入 outbuf_ 并递增 seq_nr_ 和 cur_window_packets_。
//   7. 循环直到所有 payload 处理完毕，最后调用 flush_packets 发送。
// -----------------------------------------------------------------------------
void UtpSocket::write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs)
{
	if (cur_window_packets_ == 0) {
		cc_.set_initial_rto(ctx->current_ms_);
		assert(cc_.cur_window() == 0);
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

		if (payload && pkt && !pkt->transmissions && pkt->payload < packet_size) {
			added = min(payload + pkt->payload, max<size_t>(packet_size, pkt->payload)) - pkt->payload;
			pkt->data.resize(header_size + pkt->payload + added);
			outbuf_.put(seq_nr_ - 1, pkt);
			append = false;
			assert(!pkt->need_resend);
		} else {
			added = payload;
			pkt = new OutgoingPacket();
			pkt->data.resize(header_size + added);
			pkt->payload = 0;
			pkt->transmissions = 0;
			pkt->need_resend = false;
		}

		if (added) {
			assert(flags == ST_DATA);

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
				iovec[i].iov_base = (byte*)iovec[i].iov_base + num;
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

// -----------------------------------------------------------------------------
// 函数：UtpSocket::check_invariant
// 功能：DEBUG 模式下检查内部状态一致性。
//
// 算法说明：
//   1. 若存在乱序包（reorder_count_ > 0），则 ack_nr_ + 1 处必须为空（累积 ACK 语义）。
//   2. 遍历所有在飞包，累加已发送且无需重传的 payload 字节数。
//   3. 断言 outstanding_bytes 等于拥塞控制器的 cur_window()。
// -----------------------------------------------------------------------------
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
	assert(outstanding_bytes == cc_.cur_window());
}
#endif

// -----------------------------------------------------------------------------
// 函数：UtpSocket::check_timeouts
// 功能：检查超时：RTO 超时重传、零窗口恢复、保活发送、窗口状态变更。
//
// 算法说明：
//   1. DEBUG 模式下先调用 check_invariant 检查状态一致性。
//   2. 调用 flush_packets 尝试发送待发送包。
//   3. 若零窗口时间已到且用户窗口为 0，恢复为 PACKET_SIZE。
//   4. 若 RTO 超时：
//        - 处理 MTU 探测超时（若仅有一个在飞包且是探测包，忽略丢包并收缩 ceiling）。
//        - SYN_RECV 超时直接销毁；重传次数过多则进入 RESET/DESTROY。
//        - 否则更新 RTO 超时时间，标记所有在飞包需要重传，重传第一个包。
//   5. 若状态为 CONNECTED_FULL 且窗口不再满，转为 CONNECTED 并通知可写。
//   6. 若连接已建立且长时间未发包，发送保活包。
// -----------------------------------------------------------------------------
void UtpSocket::check_timeouts()
{
	#ifdef _DEBUG
	check_invariant();
	#endif

	assert(cur_window_packets_ == 0 || outbuf_.get(seq_nr_ - cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "CheckTimeouts timeout:%d max_window_:%u cur_window_:%u "
			 "state:%s cur_window_packets_:%u",
			 (int)(cc_.rto_timeout() - ctx->current_ms_), (uint)cc_.max_window(), (uint)cc_.cur_window(),
			 statenames[state_], cur_window_packets_);
	#endif
	if (state_ != CS_DESTROY) flush_packets();

	switch (state_) {
	case CS_SYN_SENT:
	case CS_SYN_RECV:
	case CS_CONNECTED_FULL:
	case CS_CONNECTED: {

		if ((int)(ctx->current_ms_ - cc_.zerowindow_time()) >= 0 && max_window_user_ == 0) {
			max_window_user_ = PACKET_SIZE;
		}

		if (cc_.is_rto_expired(ctx->current_ms_)) {

			bool ignore_loss = mtu_.handle_probe_timeout(
				(seq_nr_ - 1) & ACK_NR_MASK, cur_window_packets_, ctx->current_ms_);
			if (ignore_loss) {
				log(UTP_LOG_MTU, "MTU [PROBE-TIMEOUT] floor:%d ceiling:%d current:%d"
					, mtu_.floor(), mtu_.ceiling(), mtu_.last());
			}
			log(UTP_LOG_MTU, "MTU [TIMEOUT]");

			/*
			OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);

			// If there were a lot of retransmissions, force recomputation of round trip time
			if (pkt->transmissions >= 4)
				rtt = 0;
			*/
			const uint new_timeout = ignore_loss ? cc_.retransmit_timeout() : cc_.retransmit_timeout() * 2;

			if (state_ == CS_SYN_RECV) {
				state_ = CS_DESTROY;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			if (cc_.retransmit_count() >= 4 || (state_ == CS_SYN_SENT && cc_.retransmit_count() >= 2)) {
				if (close_requested_)
					state_ = CS_DESTROY;
				else
					state_ = CS_RESET;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			cc_.set_retransmit_timeout(new_timeout);
			cc_.set_rto_timeout(ctx->current_ms_ + new_timeout);

			if (!ignore_loss) {
				duplicate_ack_ = 0;

				int packet_size = get_packet_size();
				cc_.on_rto_timeout(ctx->current_ms_, (size_t)packet_size, cur_window_packets_);
			}
			for (int i = 0; i < cur_window_packets_; ++i) {
				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - i - 1);
				if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
				pkt->need_resend = true;
				cc_.remove_in_flight(pkt->payload);
			}
			if (cur_window_packets_ > 0) {
				cc_.increment_retransmit_count();
				log(UTP_LOG_NORMAL, "Packet timeout. Resend. seq_nr_:%u. timeout:%u "
					"max_window_:%u cur_window_packets_:%d"
					, seq_nr_ - cur_window_packets_, cc_.retransmit_timeout()
					, (uint)cc_.max_window(), int(cur_window_packets_));

				fast_timeout_ = true;
				timeout_seq_nr_ = seq_nr_;

				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);
				assert(pkt);
				send_packet(pkt);
			}
		}

		if (state_ == CS_CONNECTED_FULL && !is_full()) {
			state_ = CS_CONNECTED;
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
				(uint)cc_.max_window(), (uint)cc_.cur_window(), (uint)get_packet_size());
			#endif
			utp_call_on_state_change(this->ctx, this, UTP_STATE_WRITABLE);
		}
		if (state_ >= CS_CONNECTED && !fin_sent) {
			if ((int)(ctx->current_ms_ - last_sent_packet_) >= KEEPALIVE_INTERVAL) {
				send_keep_alive();
			}
		}
		break;
	}

	case CS_UNINITIALIZED:
	case CS_IDLE:
	case CS_RESET:
	case CS_DESTROY:
		break;
	}
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::ack_packet
// 功能：处理一个被 ACK 确认的出站包（RTT 更新、在飞字节数减去、重传计数清零）。
//
// 参数：
//   seq - 被确认的序号。
//
// 返回值：
//   0 - 正常确认。
//   1 - 包不存在（已确认或从未发送）。
//   2 - 包未发送过。
//
// 算法说明：
//   1. 从 outbuf_ 取出对应序号的包。
//   2. 若包不存在或 transmissions == 0，返回错误码。
//   3. 从 outbuf_ 移除该包。
//   4. 若是首次传输（transmissions == 1），根据发送时间计算 RTT 并更新。
//   5. 否则重置重传超时为当前 RTO。
//   6. 若包无需重传，从在飞字节数中减去 payload。
//   7. 删除包对象，清零重传计数。
// -----------------------------------------------------------------------------
int UtpSocket::ack_packet(uint16 seq)
{
	OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq);

	if (pkt == NULL) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "got ack for:%u (already acked, or never sent)", seq);
		#endif

		return 1;
	}

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

	if (pkt->transmissions == 1) {
		const uint32 ertt = (uint32)((utp_call_get_microseconds(this->ctx, this) - pkt->time_sent) / 1000);
		cc_.update_rtt(ertt, ctx->current_ms_);

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "rtt:%u avg:%u var:%u rto:%u",
			ertt, cc_.rtt_ms(), cc_.rtt_var(), cc_.rto_ms());
		#endif

	} else {
		cc_.set_retransmit_timeout(cc_.rto_ms());
		cc_.set_rto_timeout(ctx->current_ms_ + cc_.rto_ms());
	}
	if (!pkt->need_resend) {
		cc_.remove_in_flight(pkt->payload);
	}
	delete pkt;
	cc_.set_retransmit_count(0);
	return 0;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::selective_ack_bytes
// 功能：从 SACK 位图计算被确认的字节数和最小 RTT。
//
// 参数：
//   base     - SACK 位图起始序号（通常为 ack_nr_ + 1）。
//   mask     - SACK 位图字节数组。
//   len      - 位图字节长度。
//   min_rtt  - 输出参数，记录被确认包中的最小 RTT（微秒）。
//
// 返回值：
//   被 SACK 确认的总 payload 字节数。
//
// 算法说明：
//   1. 遍历位图中每一位，从高位到低位对应 base + bits 序号。
//   2. 跳过不在当前发送窗口范围内的序号。
//   3. 若对应位为 1 且包已发送，累加 payload 到 acked_bytes。
//   4. 同时更新 min_rtt 为所有被确认包中的最小发送延迟。
// -----------------------------------------------------------------------------
size_t UtpSocket::selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt)
{
	if (cur_window_packets_ == 0) return 0;

	size_t acked_bytes = 0;
	int bits = len * 8;
	uint64 now = utp_call_get_microseconds(this->ctx, this);

	do {
		uint v = base + bits;

		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0)
			continue;

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

// -----------------------------------------------------------------------------
// 函数：UtpSocket::selective_ack
// 功能：处理选择性 ACK（SACK），确认已收到包并触发丢失包快速重传。
//
// 参数：
//   base - SACK 位图起始序号（通常为 ack_nr_ + 1）。
//   mask - SACK 位图字节数组。
//   len  - 位图字节长度。
//
// 算法说明：
//   1. 遍历位图每一位，统计连续已确认包数量 count。
//   2. 对已确认的包调用 ack_packet 处理。
//   3. 对未确认但满足快速重传条件的包（count >= DUPLICATE_ACKS_BEFORE_RESEND
//      且序号在 fast_resend_seq_nr_ 之后），加入重传列表 resends。
//   4. 从重传列表尾部开始重传（优先重传序号较大的包），最多重传 4 个。
//   5. 若发生重传，调用 maybe_decay_win 衰减窗口。
// -----------------------------------------------------------------------------
void UtpSocket::selective_ack(uint base, const byte *mask, byte len)
{
	if (cur_window_packets_ == 0) return;

	int bits = len * 8 - 1;

	int count = 0;

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
		uint v = base + bits;

		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		bool bit_set = bits >= 0 && mask[bits>>3] & (1 << (bits & 7));

		if (bit_set) count++;

		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "skipping %u. pkt:%08x transmissions:%u %s",
				v, pkt, pkt?pkt->transmissions:0, pkt?"(not sent yet?)":"(already acked?)");
			#endif
			continue;
		}

		if (bit_set) {
			assert((v & outbuf_.mask()) != ((seq_nr_ - cur_window_packets_) & outbuf_.mask()));
			ack_packet(v);
			continue;
		}

		if (((v - fast_resend_seq_nr_) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
			count >= DUPLICATE_ACKS_BEFORE_RESEND) {
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
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);

		if (!pkt) continue;

		log(UTP_LOG_NORMAL, "Packet %u lost. Resending", v);

		back_off = true;

		#ifdef _DEBUG
		++stats_.rexmit;
		#endif

		send_packet(pkt);
		fast_resend_seq_nr_ = (v + 1) & ACK_NR_MASK;

		if (++i >= 4) break;
	}

	if (back_off)
		cc_.maybe_decay_win(ctx->current_ms_);

	duplicate_ack_ = count;
}

size_t UtpSocket::get_packet_size() const
{
	return mtu_.effective_mtu(get_header_size());
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::UtpSocket
// 功能：初始化 socket 所有成员变量。
//
// 参数：
//   _ctx - 指向 utp_context 全局上下文的指针。
//
// 算法说明：
//   1. 初始化地址、上下文指针、ida（延迟 ACK 列表索引）等基础字段。
//   2. 初始化拥塞控制相关字段（cur_window_packets_、max_window_user_ 等）。
//   3. 初始化状态机（state_ = CS_UNINITIALIZED，seq_nr_ = 1）。
//   4. 初始化缓冲区（inbuf_、outbuf_）。
//   5. 初始化 MTU 探测器和拥塞控制器。
//   6. DEBUG 模式下清零统计结构体。
// -----------------------------------------------------------------------------
UtpSocket::UtpSocket(utp_context* _ctx)
	: addr()
	, ctx(_ctx)
	, ida(-1)
	, reorder_count_(0)
	, duplicate_ack_(0)
	, cur_window_packets_(0)
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
	, eof_pkt_(0)
	, ack_nr_(0)
	, seq_nr_(1)
	, timeout_seq_nr_(0)
	, fast_resend_seq_nr_(1)
	, reply_micro_(0)
	, last_got_packet_(0)
	, last_sent_packet_(0)
	, last_measured_delay_(0)
	, userdata_(NULL)
	, conn_seed_(0)
	, conn_id_recv_(0)
	, conn_id_send_(0)
	, last_rcv_win_(0)
	, extensions_()
	, mtu_(this)
	, cc_()
{
	inbuf_.initialize(16);
	outbuf_.initialize(16);
	cc_.set_ssthresh(_ctx->opt_sndbuf_);
	memset(extensions_, 0, sizeof(extensions_));
	#ifdef _DEBUG
	memset(&stats_, 0, sizeof(utp_socket_stats));
	#endif
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::~UtpSocket
// 功能：销毁 socket：通知应用、从哈希表移除、释放缓冲区。
//
// 算法说明：
//   1. 通知上层应用 socket 正在销毁（UTP_STATE_DESTROYING）。
//   2. 若当前 socket 是上下文的 last_utp_socket_，清空该指针。
//   3. 从全局 sockets_ 哈希表中移除本 socket。
//   4. 从延迟 ACK 列表中移除本 socket。
//   5. 释放所有入站包和出站包的内存。
// -----------------------------------------------------------------------------
UtpSocket::~UtpSocket()
{
	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Killing socket");
	#endif

	utp_call_on_state_change(ctx, this, UTP_STATE_DESTROYING);

	if (ctx->last_utp_socket_ == this) {
		ctx->last_utp_socket_ = NULL;
	}

	auto erased = ctx->sockets_.erase(UtpSocketKey(addr, conn_id_recv_));
	assert(erased == 1);

	remove_socket_from_ack_list(this);

	for (size_t i = 0; i < inbuf_.buf_size(); i++) {
		delete (InboundPacket*)inbuf_.element(i);
	}
 	for (size_t i = 0; i < outbuf_.buf_size(); i++) {
		delete (OutgoingPacket*)outbuf_.element(i);
	}
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::reset
// 功能：重置 MTU 搜索范围。
//
// 参数：
//   udp_mtu     - 当前 UDP MTU 值（作为 ceiling 初始值）。
//   current_ms  - 当前毫秒时间戳。
//
// 算法说明：
//   1. 将 ceiling 设为当前 UDP MTU。
//   2. 将 floor 设为 576（IPv4 最小重组缓冲区大小）。
//   3. 断言 floor <= ceiling。
//   4. 设置下次探测时间为 30 分钟后。
// -----------------------------------------------------------------------------
void MtuDiscovery::reset(uint32 udp_mtu, uint64 current_ms)
{
	mtu_ceiling_ = udp_mtu;
	mtu_floor_ = 576;
	owner_->log(UTP_LOG_MTU, "MTU [RESET] floor:%d ceiling:%d current:%d"
		, mtu_floor_, mtu_ceiling_, mtu_last_);
	assert(mtu_floor_ <= mtu_ceiling_);
	mtu_discover_time_ = current_ms + 30 * 60 * 1000;
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::search_update
// 功能：推进 MTU 二分搜索。
//
// 参数：
//   current_ms - 当前毫秒时间戳。
//
// 算法说明：
//   1. 断言 floor <= ceiling。
//   2. 将 last 设为 (floor + ceiling) / 2。
//   3. 清除当前探测状态（probe_seq、probe_size 清零）。
//   4. 若 ceiling - floor <= 16，认为搜索完成：
//        - 将 last 设为 floor。
//        - 将 ceiling 设为 floor。
//        - 设置下次探测时间为 30 分钟后。
// -----------------------------------------------------------------------------
void MtuDiscovery::search_update(uint64 current_ms)
{
	assert(mtu_floor_ <= mtu_ceiling_);

	mtu_last_ = (mtu_floor_ + mtu_ceiling_) / 2;

	mtu_probe_seq_ = mtu_probe_size_ = 0;

	if (mtu_ceiling_ - mtu_floor_ <= 16) {
		mtu_last_ = mtu_floor_;
		owner_->log(UTP_LOG_MTU, "MTU [DONE] floor:%d ceiling:%d current:%d"
			, mtu_floor_, mtu_ceiling_, mtu_last_);
		mtu_ceiling_ = mtu_floor_;
		assert(mtu_floor_ <= mtu_ceiling_);
		mtu_discover_time_ = current_ms + 30 * 60 * 1000;
	}
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::handle_probe_ack
// 功能：处理探测包被 ACK。
//
// 参数：
//   seq        - 被 ACK 的探测包序号。
//   current_ms - 当前毫秒时间戳。
//
// 返回值：
//   true  - 该 ACK 确实对应一个 MTU 探测包，已更新搜索范围。
//   false - 该 ACK 不是探测包的确认。
//
// 算法说明：
//   1. 检查 seq 是否匹配当前在途的探测包。
//   2. 若是，将 floor 提升到探测包大小（说明该大小可行）。
//   3. 调用 search_update 推进二分搜索。
// -----------------------------------------------------------------------------
bool MtuDiscovery::handle_probe_ack(uint32 seq, uint64 current_ms)
{
	if (is_probe(seq)) {
		mtu_floor_ = mtu_probe_size_;
		search_update(current_ms);
		return true;
	}
	return false;
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::handle_probe_timeout
// 功能：处理探测包超时。
//
// 参数：
//   outstanding_seq     - 当前最早未确认包的序号。
//   cur_window_packets  - 当前在飞包数量。
//   current_ms          - 当前毫秒时间戳。
//
// 返回值：
//   true  - 超时的是探测包，已忽略丢包并收缩搜索范围。
//   false - 超时的是普通包。
//
// 算法说明：
//   1. 若只有一个在飞包且它是探测包，说明探测失败：
//        - 将 ceiling 收缩到 probe_size - 1。
//        - 调用 search_update 推进二分搜索。
//        - 返回 true 表示忽略此次丢包（不触发拥塞控制）。
//   2. 清除探测状态。
// -----------------------------------------------------------------------------
bool MtuDiscovery::handle_probe_timeout(uint32 outstanding_seq, uint32 cur_window_packets, uint64 current_ms)
{
	bool ignore_loss = false;
	if (cur_window_packets == 1 && is_probe(outstanding_seq)) {
		mtu_ceiling_ = mtu_probe_size_ - 1;
		search_update(current_ms);
		ignore_loss = true;
	}
	clear_probe();
	return ignore_loss;
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::handle_probe_loss
// 功能：处理探测包丢失。
//
// 参数：
//   current_ms - 当前毫秒时间戳。
//
// 算法说明：
//   1. 将 ceiling 收缩到 probe_size - 1（说明该大小不可行）。
//   2. 调用 search_update 推进二分搜索。
//   3. 清除探测状态。
// -----------------------------------------------------------------------------
void MtuDiscovery::handle_probe_loss(uint64 current_ms)
{
	mtu_ceiling_ = mtu_probe_size_ - 1;
	search_update(current_ms);
	clear_probe();
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::handle_icmp_fragmentation
// 功能：处理 ICMP 分片通知（"Packet Too Big"）。
//
// 参数：
//   next_hop_mtu - ICMP 消息中报告的下一跳 MTU。
//   current_ms   - 当前毫秒时间戳。
//
// 算法说明：
//   1. 将 ceiling 收缩到 min(next_hop_mtu, 当前 ceiling)。
//   2. 调用 search_update 推进二分搜索。
//   3. 将 last 直接设为 ceiling（信任 ICMP 报告的 MTU）。
// -----------------------------------------------------------------------------
void MtuDiscovery::handle_icmp_fragmentation(uint16 next_hop_mtu, uint64 current_ms)
{
	mtu_ceiling_ = std::min<uint32>(next_hop_mtu, mtu_ceiling_);
	search_update(current_ms);
	mtu_last_ = mtu_ceiling_;
}

// -----------------------------------------------------------------------------
// 函数：MtuDiscovery::handle_icmp_unknown
// 功能：处理未知 ICMP MTU 通知（无法获取具体 MTU 值时）。
//
// 参数：
//   current_ms - 当前毫秒时间戳。
//
// 算法说明：
//   1. 将 ceiling 收缩到 (floor + ceiling) / 2（保守估计）。
//   2. 调用 search_update 推进二分搜索。
// -----------------------------------------------------------------------------
void MtuDiscovery::handle_icmp_unknown(uint64 current_ms)
{
	mtu_ceiling_ = (mtu_floor_ + mtu_ceiling_) / 2;
	search_update(current_ms);
}
