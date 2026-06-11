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
using utp::wire::ST_DATA;
using utp::wire::ST_FIN;
using utp::wire::ST_STATE;
using utp::wire::ST_RESET;
using utp::wire::ST_SYN;
using utp::wire::ST_NUM_STATES;
using utp::zeromem;
using std::min;
using std::max;
using std::clamp;

// ============================================================================
// 连接状态机：State 模式具体状态（无数据单例）。
// 每个 CONN_STATE 对应一个状态类，封装该状态下 close/shutdown 的行为，以及
// "是否可写 / 是否参与超时" 两个策略位。状态对象只调用 UtpSocket 的公开原语，
// 不触碰其私有成员（不需要 friend），故不会重新引入跨类耦合。
// ============================================================================
namespace {

// 默认行为：关闭即销毁；半关闭仅置读端关闭。供非活动状态复用。
struct StateBase : IConnectionState {
	void close(UtpSocket& s) const override { s.set_conn_state(CS_DESTROY); }
	void shutdown(UtpSocket& s, int how) const override {
		if (how != SHUT_WR) s.mark_read_shutdown();
	}
};

struct StateUninitialized : StateBase { const char* name() const override { return "UNINITIALIZED"; } };
struct StateIdle          : StateBase { const char* name() const override { return "IDLE"; } };

struct StateSynSent : StateBase {
	const char* name() const override { return "SYN_SENT"; }
	bool timeout_active() const override { return true; }
	// 原 switch 中 SYN_SENT 先做 RTO 退避，再 fall-through 到 default 置 DESTROY
	void close(UtpSocket& s) const override {
		s.backoff_rto_for_close();
		s.set_conn_state(CS_DESTROY);
	}
	void shutdown(UtpSocket& s, int how) const override {
		if (how != SHUT_WR) s.mark_read_shutdown();
		if (how != SHUT_RD) s.backoff_rto_for_close();
	}
};

struct StateSynRecv : StateBase {
	const char* name() const override { return "SYN_RECV"; }
	bool timeout_active() const override { return true; }
	// close 用 StateBase 默认（置 DESTROY）
};

struct StateConnected : StateBase {
	const char* name() const override { return "CONNECTED"; }
	bool writable() const override { return true; }
	bool timeout_active() const override { return true; }
	void close(UtpSocket& s) const override {
		s.mark_read_shutdown();
		s.request_close();
		if (!s.fin_sent())      s.send_fin();
		else if (s.fin_acked()) s.set_conn_state(CS_DESTROY);
	}
	void shutdown(UtpSocket& s, int how) const override {
		if (how != SHUT_WR) s.mark_read_shutdown();
		if (how != SHUT_RD && !s.fin_sent()) s.send_fin();
	}
};

// 窗口已满：close/shutdown/超时行为与 CONNECTED 相同，但 writev 不可写
struct StateConnectedFull : StateConnected {
	const char* name() const override { return "CONNECTED_FULL"; }
	bool writable() const override { return false; }
};

struct StateReset   : StateBase { const char* name() const override { return "RESET"; } };
struct StateDestroy : StateBase { const char* name() const override { return "DESTROY"; } };

}  // namespace

// 按当前 CONN_STATE 返回对应的状态单例
IConnectionState* UtpSocket::state_descriptor(CONN_STATE s)
{
	static StateUninitialized  s_uninit;
	static StateIdle           s_idle;
	static StateSynSent        s_syn_sent;
	static StateSynRecv        s_syn_recv;
	static StateConnected      s_connected;
	static StateConnectedFull  s_connected_full;
	static StateReset          s_reset;
	static StateDestroy        s_destroy;

	switch (s) {
		case CS_UNINITIALIZED:  return &s_uninit;
		case CS_IDLE:           return &s_idle;
		case CS_SYN_SENT:       return &s_syn_sent;
		case CS_SYN_RECV:       return &s_syn_recv;
		case CS_CONNECTED:      return &s_connected;
		case CS_CONNECTED_FULL: return &s_connected_full;
		case CS_RESET:          return &s_reset;
		case CS_DESTROY:        return &s_destroy;
	}
	return &s_idle;  // 不可达
}

// 标记已发送 FIN 并发出 ST_FIN 包（供状态对象在关闭流程中调用）
void UtpSocket::send_fin()
{
	send_.fin_sent = true;
	write_outgoing_packet(0, ST_FIN, NULL, 0);
}

// 关闭 SYN_SENT 连接时的 RTO 退避（沿用原 close/shutdown 中的计算）
void UtpSocket::backoff_rto_for_close()
{
	cc_->set_rto_timeout(utp_call_get_milliseconds(conn_.host->handle(), this) + min<uint>(cc_->rto_ms() * 2, 60));
}

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
void UtpSocket::remove_from_ack_list()
{
	// 延迟 ACK 列表由宿主(UtpContext)持有，本 socket 只委托其将自己注销。
	conn_.host->remove_ack(this);
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
	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, ida_ == -1 ? "schedule_ack" : "schedule_ack: already in list");
	#endif
	// 延迟 ACK 列表由宿主(UtpContext)持有；重复登记由宿主依据 ack_index() 去重。
	conn_.host->schedule_ack(this);
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
	uint64 time = utp_call_get_microseconds(conn_.host->handle(), this);

	PacketFormatV1* b1 = (PacketFormatV1*)b;
	b1->tv_usec = (uint32)time;
	// reply_micro：发送方测得的延迟反馈，对端 LEDBAT 控制器将以此为目标延迟基线
	b1->reply_micro = timing_.reply_micro;

	// 记录最近一次发送时间，用于 KEEPALIVE_INTERVAL 触发保活
	timing_.last_sent_packet = conn_.host->current_ms();

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
		utp_call_on_overhead_statistics(conn_.host->handle(), this, true, n, type);
	}
#if UTP_DEBUG_LOGGING
	// 调试日志：解码包头字段输出
	int flags2 = b1->type();
	uint16 pkt_seq_nr = b1->seq_nr;
	uint16 pkt_ack_nr = b1->ack_nr;
	log(UTP_LOG_DEBUG, "send %s len:%u id:%u timestamp:" I64u " reply_micro_:%u flags:%s pkt_seq_nr:%u pkt_ack_nr:%u",
		addrfmt(conn_.addr), (uint)length, conn_.conn_id_send, time, timing_.reply_micro, flagnames[flags2],
		pkt_seq_nr, pkt_ack_nr);
#endif
	// 通过回调真正发送 UDP 报文
	conn_.host->send_to(b, length, conn_.addr, flags);
	// 数据包已发出，移除自己可能存在的延迟 ACK 调度
	remove_from_ack_list();
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
	recv_.last_rcv_win = get_rcv_window();
	pfa.pf.set_version(1);
	pfa.pf.set_type(ST_STATE);
	pfa.pf.ext = 0;
	pfa.pf.connid = conn_.conn_id_send;
	pfa.pf.ack_nr = recv_.ack_nr;
	pfa.pf.seq_nr = send_.seq_nr;
	pfa.pf.windowsize = (uint32)recv_.last_rcv_win;
	len = sizeof(PacketFormatV1);

	// 存在乱序包且未完成 FIN：构造选择性 ACK（EACK）扩展
	if (recv_.reorder_count != 0 && !recv_.got_fin_reached_) {
		// SYN-ACK 不应携带 EACK
		assert(!synack);
		pfa.pf.ext = 1;          // 启用扩展
		pfa.ext_next = 0;         // 后续无更多扩展
		pfa.ext_len = 4;          // EACK 位图长度 4 字节
		uint m = 0;               // EACK 位图（共 32 位）

		// ack_nr_+1 应为空（保证 ACK 累积语义）
		assert(recv_.inbuf.get(recv_.ack_nr + 1) == NULL);
		// 限制窗口扫描最大为 14+16 = 30 位
		size_t window = min<size_t>(14+16, recv_.inbuf.size());
		for (size_t i = 0; i < window; i++) {
			// ack_nr_+i+2 处是否有包：bit i 表示 ack_nr_+i+2
			if (recv_.inbuf.get(recv_.ack_nr + i + 2) != NULL) {
				m |= 1 << i;

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "EACK packet [%u]", recv_.ack_nr + i + 2);
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
		log(UTP_LOG_DEBUG, "Sending EACK %u [%u] bits:[%032b]", recv_.ack_nr, conn_.conn_id_send, m);
		#endif
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending ACK %u [%u]", recv_.ack_nr, conn_.conn_id_send);
		#endif
	}

	// 发送 ACK（ack_overhead 类别计入统计）
	send_data((byte*)&pfa, len, ack_overhead);
	// 清理延迟 ACK 调度
	remove_from_ack_list();
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
	recv_.ack_nr--;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Sending KeepAlive ACK %u [%u]", recv_.ack_nr, conn_.conn_id_send);
	#endif
	// 复用 send_ack 发送 STATE 类型保活 ACK
	send_ack();
	// 恢复 ack_nr_ 到真实值
	recv_.ack_nr++;
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
//   conn_id_send  - 发送方向的连接 ID（对端用于识别）。
//   ack_nr        - 当前确认序号（通常为收到的对端 seq）。
//   seq_nr        - 当前发送序号。
// -----------------------------------------------------------------------------
void UtpSocket::send_rst(UtpContext *ctx,
	const utp::Address &addr, uint32 conn_id_send, uint16 ack_nr, uint16 seq_nr)
{
	PacketFormatV1 pf1;
	zeromem(&pf1);

	size_t len;
	pf1.set_version(1);
	pf1.set_type(ST_RESET);    // 标记为 RESET 类型
	pf1.ext = 0;
	pf1.connid = conn_id_send;
	pf1.ack_nr = ack_nr;
	pf1.seq_nr = seq_nr;
	pf1.windowsize = 0;        // 复位包窗口大小无意义
	len = sizeof(PacketFormatV1);

	// 直接经 send_to_addr 发出，不经过 send_data（避免修改 last_sent_packet_ 等状态）
	ctx->send_to_addr_impl((const byte*)&pf1, len, addr);
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
	time_t cur_time = utp_call_get_milliseconds(this->conn_.host->handle(), this);

	if (pkt->transmissions == 0 || pkt->need_resend) {
		cc_->add_in_flight(pkt->payload);
	}

	pkt->need_resend = false;

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
	p1->ack_nr = recv_.ack_nr;
	pkt->time_sent = utp_call_get_microseconds(this->conn_.host->handle(), this);

	bool use_as_mtu_probe = false;

 	if (mtu_.should_rediscover((uint64)cur_time)) {
		mtu_.reset((uint32)get_udp_mtu(), (uint64)cur_time);
	}

 	if (mtu_.floor() < mtu_.ceiling()
		&& pkt->length > mtu_.floor()
		&& pkt->length <= mtu_.ceiling()
		&& !mtu_.probe_seq()
		&& send_.seq_nr != 1
		&& pkt->transmissions == 0) {

 		mtu_.set_probe((send_.seq_nr - 1) & ACK_NR_MASK, (uint32)pkt->length);
		assert(pkt->length >= mtu_.floor());
		assert(pkt->length <= mtu_.ceiling());
 		use_as_mtu_probe = true;
		log(UTP_LOG_MTU, "MTU [PROBE] floor:%d ceiling:%d current:%d"
			, mtu_.floor(), mtu_.ceiling(), mtu_.probe_size());
  	}

	pkt->transmissions++;
	send_data((byte*)pkt->data.data(), pkt->length,
		(conn_.state == CS_SYN_SENT) ? connect_overhead
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
	size_t max_send = min(min(cc_->max_window(), send_.opt_sndbuf), send_.max_window_user);

	if (send_.cur_window_packets >= OUTGOING_BUFFER_MAX_SIZE - 1) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "is_full:false cur_window_packets_:%d MAX:%d", send_.cur_window_packets, OUTGOING_BUFFER_MAX_SIZE - 1);
		#endif

		cc_->mark_window_full(conn_.host->current_ms());
		return true;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "is_full:%s. cur_window_:%u pkt:%u max:%u cur_window_packets_:%u max_window_:%u"
		, (cc_->cur_window() + bytes > max_send) ? "true" : "false"
		, (uint)cc_->cur_window(), bytes, max_send, send_.cur_window_packets
		, (uint)cc_->max_window());
	#endif

	if (cc_->cur_window() + bytes > max_send) {
		cc_->mark_window_full(conn_.host->current_ms());
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
	for (uint16 i = send_.seq_nr - send_.cur_window_packets; i != send_.seq_nr; ++i) {
		OutgoingPacket *pkt = send_.outbuf.get(i);
		if (pkt == 0 || (pkt->transmissions > 0 && pkt->need_resend == false)) continue;
		if (is_full()) return true;

		if (i != ((send_.seq_nr - 1) & ACK_NR_MASK) ||
			send_.cur_window_packets == 1 ||
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
	if (send_.cur_window_packets == 0) {
		cc_->set_initial_rto(conn_.host->current_ms());
		assert(cc_->cur_window() == 0);
	}

	size_t packet_size = get_packet_size();
	do {
		assert(send_.cur_window_packets < OUTGOING_BUFFER_MAX_SIZE);
		assert(flags == ST_DATA || flags == ST_FIN);

		size_t added = 0;

		OutgoingPacket *pkt = NULL;

		if (send_.cur_window_packets > 0) {
			pkt = send_.outbuf.get(send_.seq_nr - 1);
		}

		const size_t header_size = get_header_size();
		bool append = true;
		std::unique_ptr<OutgoingPacket> new_pkt;

		if (payload && pkt && !pkt->transmissions && pkt->payload < packet_size) {
			// 追加到缓冲区中最后一个未发送的包（包仍由 outbuf 持有）
			added = min(payload + pkt->payload, max<size_t>(packet_size, pkt->payload)) - pkt->payload;
			pkt->data.resize(header_size + pkt->payload + added);
			append = false;
			assert(!pkt->need_resend);
		} else {
			added = payload;
			new_pkt = std::make_unique<OutgoingPacket>();
			pkt = new_pkt.get();
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

		recv_.last_rcv_win = get_rcv_window();

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
		p1->set_version(1);
		p1->set_type(flags);
		p1->ext = 0;
		p1->connid = conn_.conn_id_send;
		p1->windowsize = (uint32)recv_.last_rcv_win;
		p1->ack_nr = recv_.ack_nr;

		if (append) {
			send_.outbuf.ensure_size(send_.seq_nr, send_.cur_window_packets);
			send_.outbuf.put(send_.seq_nr, std::move(new_pkt));
			p1->seq_nr = send_.seq_nr;
			send_.seq_nr++;
			send_.cur_window_packets++;
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
	if (recv_.reorder_count > 0) {
		assert(recv_.inbuf.get(recv_.ack_nr + 1) == NULL);
	}

	size_t outstanding_bytes = 0;
	for (int i = 0; i < send_.cur_window_packets; ++i) {
		OutgoingPacket *pkt = send_.outbuf.get(send_.seq_nr - i - 1);
		if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
		outstanding_bytes += pkt->payload;
	}
	assert(outstanding_bytes == cc_->cur_window());
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

	assert(send_.cur_window_packets == 0 || send_.outbuf.get(send_.seq_nr - send_.cur_window_packets));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "CheckTimeouts timeout:%d max_window_:%u cur_window_:%u "
			 "state:%s cur_window_packets_:%u",
			 (int)(cc_->rto_timeout() - conn_.host->current_ms()), (uint)cc_->max_window(), (uint)cc_->cur_window(),
			 statenames[conn_.state], send_.cur_window_packets);
	#endif
	if (conn_.state != CS_DESTROY) flush_packets();

	// 仅活动状态（SYN_SENT/SYN_RECV/CONNECTED/CONNECTED_FULL）运行 RTO/保活机制
	if (fsm().timeout_active()) {

		if ((int)(conn_.host->current_ms() - cc_->zerowindow_time()) >= 0 && send_.max_window_user == 0) {
			send_.max_window_user = PACKET_SIZE;
		}

		if (cc_->is_rto_expired(conn_.host->current_ms())) {

			bool ignore_loss = mtu_.handle_probe_timeout(
				(send_.seq_nr - 1) & ACK_NR_MASK, send_.cur_window_packets, conn_.host->current_ms());
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
			const uint new_timeout = ignore_loss ? cc_->retransmit_timeout() : cc_->retransmit_timeout() * 2;

			if (conn_.state == CS_SYN_RECV) {
				conn_.state = CS_DESTROY;
				utp_call_on_error(conn_.host->handle(), this, UTP_ETIMEDOUT);
				return;
			}
			if (cc_->retransmit_count() >= 4 || (conn_.state == CS_SYN_SENT && cc_->retransmit_count() >= 2)) {
				if (send_.close_requested_)
					conn_.state = CS_DESTROY;
				else
					conn_.state = CS_RESET;
				utp_call_on_error(conn_.host->handle(), this, UTP_ETIMEDOUT);
				return;
			}
			cc_->set_retransmit_timeout(new_timeout);
			cc_->set_rto_timeout(conn_.host->current_ms() + new_timeout);

			if (!ignore_loss) {
				duplicate_ack_ = 0;

				int packet_size = get_packet_size();
				cc_->on_rto_timeout(conn_.host->current_ms(), (size_t)packet_size, send_.cur_window_packets);
			}
			for (int i = 0; i < send_.cur_window_packets; ++i) {
				OutgoingPacket *pkt = send_.outbuf.get(send_.seq_nr - i - 1);
				if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
				pkt->need_resend = true;
				cc_->remove_in_flight(pkt->payload);
			}
			if (send_.cur_window_packets > 0) {
				cc_->increment_retransmit_count();
				log(UTP_LOG_NORMAL, "Packet timeout. Resend. seq_nr_:%u. timeout:%u "
					"max_window_:%u cur_window_packets_:%d"
					, send_.seq_nr - send_.cur_window_packets, cc_->retransmit_timeout()
					, (uint)cc_->max_window(), int(send_.cur_window_packets));

				timing_.fast_timeout_ = true;
				timing_.timeout_seq_nr = send_.seq_nr;

				OutgoingPacket *pkt = send_.outbuf.get(send_.seq_nr - send_.cur_window_packets);
				assert(pkt);
				send_packet(pkt);
			}
		}

		if (conn_.state == CS_CONNECTED_FULL && !is_full()) {
			conn_.state = CS_CONNECTED;
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
				(uint)cc_->max_window(), (uint)cc_->cur_window(), (uint)get_packet_size());
			#endif
			utp_call_on_state_change(this->conn_.host->handle(), this, UTP_STATE_WRITABLE);
		}
		if (conn_.state >= CS_CONNECTED && !send_.fin_sent) {
			if ((int)(conn_.host->current_ms() - timing_.last_sent_packet) >= KEEPALIVE_INTERVAL) {
				send_keep_alive();
			}
		}
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
	OutgoingPacket *pkt = send_.outbuf.get(seq);

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

	// 取走所有权：函数返回时自动释放（取代原先的 put(nullptr) + delete）
	std::unique_ptr<OutgoingPacket> owned = send_.outbuf.take(seq);

	if (pkt->transmissions == 1) {
		const uint32 ertt = (uint32)((utp_call_get_microseconds(this->conn_.host->handle(), this) - pkt->time_sent) / 1000);
		cc_->update_rtt(ertt, conn_.host->current_ms());

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "rtt:%u avg:%u var:%u rto:%u",
			ertt, cc_->rtt_ms(), cc_->rtt_var(), cc_->rto_ms());
		#endif

	} else {
		cc_->set_retransmit_timeout(cc_->rto_ms());
		cc_->set_rto_timeout(conn_.host->current_ms() + cc_->rto_ms());
	}
	if (!pkt->need_resend) {
		cc_->remove_in_flight(pkt->payload);
	}
	cc_->set_retransmit_count(0);
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
	if (send_.cur_window_packets == 0) return 0;

	size_t acked_bytes = 0;
	int bits = len * 8;
	uint64 now = utp_call_get_microseconds(this->conn_.host->handle(), this);

	do {
		uint v = base + bits;

		if (((send_.seq_nr - v - 1) & ACK_NR_MASK) >= (uint16)(send_.cur_window_packets - 1))
			continue;

		OutgoingPacket *pkt = send_.outbuf.get(v);
		if (!pkt || pkt->transmissions == 0)
			continue;

		// 上界检查：循环从 bits == len*8 开始（位图外一位，语义为“未确认”），
		// 该位不存在于 mask 中，直接按未置位处理；否则会越界读 mask[len]
		if (bits >= 0 && bits < (int)len * 8 && mask[bits>>3] & (1 << (bits & 7))) {
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
	if (send_.cur_window_packets == 0) return;

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

		if (((send_.seq_nr - v - 1) & ACK_NR_MASK) >= (uint16)(send_.cur_window_packets - 1))
			continue;

		bool bit_set = bits >= 0 && mask[bits>>3] & (1 << (bits & 7));

		if (bit_set) count++;

		OutgoingPacket *pkt = send_.outbuf.get(v);
		if (!pkt || pkt->transmissions == 0) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "skipping %u. pkt:%08x transmissions:%u %s",
				v, pkt, pkt?pkt->transmissions:0, pkt?"(not sent yet?)":"(already acked?)");
			#endif
			continue;
		}

		if (bit_set) {
			assert((v & send_.outbuf.mask()) != ((send_.seq_nr - send_.cur_window_packets) & send_.outbuf.mask()));
			ack_packet(v);
			continue;
		}

		if (((v - timing_.fast_resend_seq_nr) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
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
				v, count, duplicate_ack_, timing_.fast_resend_seq_nr);
			#endif
		}
	} while (--bits >= -1);

	if (((base - 1 - timing_.fast_resend_seq_nr) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
		count >= DUPLICATE_ACKS_BEFORE_RESEND) {
		resends[nr++] = (base - 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "no ack for %u", (base - 1) & ACK_NR_MASK);
		#endif

	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "not resending %u count:%d dup_ack:%u fast_resend_seq_nr_:%u",
			base - 1, count, duplicate_ack_, timing_.fast_resend_seq_nr);
		#endif
	}

	bool back_off = false;
	int i = 0;
	while (nr > 0) {
		uint v = resends[--nr];
		OutgoingPacket *pkt = send_.outbuf.get(v);

		if (!pkt) continue;

		log(UTP_LOG_NORMAL, "Packet %u lost. Resending", v);

		back_off = true;

		#ifdef _DEBUG
		++stats_.rexmit;
		#endif

		send_packet(pkt);
		timing_.fast_resend_seq_nr = (v + 1) & ACK_NR_MASK;

		if (++i >= 4) break;
	}

	if (back_off)
		cc_->maybe_decay_win(conn_.host->current_ms());

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
UtpSocket::UtpSocket(ISocketHost* _host)
	: conn_{.host = _host, .target_delay = _host->default_target_delay()}
	, recv_{.opt_rcvbuf = _host->default_rcvbuf()}
	, send_{.opt_sndbuf = _host->default_sndbuf(), .max_window_user = 255 * PACKET_SIZE}
	, ida_(-1)
	, mtu_(this)
	, cc_(std::make_unique<LedbatController>())
{
	recv_.inbuf.initialize(16);
	send_.outbuf.initialize(16);
	cc_->set_ssthresh(_host->default_sndbuf());
	memset(conn_.extensions, 0, sizeof(conn_.extensions));
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

	utp_call_on_state_change(conn_.host->handle(), this, UTP_STATE_DESTROYING);

	// 从宿主注册表/缓存中注销自己（原先直接操作 ctx->sockets_/last_utp_socket_）
	conn_.host->on_socket_destroyed(this);

	remove_from_ack_list();

	// inbuf/outbuf 持有 unique_ptr，析构时自动释放全部包对象
}

// =============================================================================
// 以下为面向 C API 的 UtpSocket 公开操作实现（原位于 utp_api.cpp，已归位）。
// utp_api.cpp 中对应的 utp_* 函数只做空指针检查后 1 行委托到这里。
// =============================================================================

// 初始化 uTP socket 的内部状态（状态机、连接 ID、计时器、拥塞控制、MTU 等）。
// 这是所有 uTP socket 公共入口（utp_connect、utp_accept、utp_create_socket）的共同底层。
// 参数 addr - 对端地址
// 参数 addrlen - 对端地址长度
// 参数 need_seed_gen - 是否需要重新生成连接种子和连接 ID
//                     （主动 connect 时为 true，服务端接受时为 false）
// 参数 seed - 连接种子；need_seed_gen 为 true 时被本函数覆写
// 参数 id_recv - 接收方向连接 ID；need_seed_gen 为 true 时被本函数覆写
// 参数 id_send - 发送方向连接 ID；need_seed_gen 为 true 时被本函数覆写
void UtpSocket::initialize(	const struct sockaddr *addr,
							socklen_t addrlen,
							bool need_seed_gen,
							uint32 seed,
							uint32 id_recv,
							uint32 id_send)
{
	utp::Address psaddr = utp::Address((const SOCKADDR_STORAGE*)addr, addrlen);

	if (need_seed_gen) {
		do {
			seed = utp_call_get_random(conn_.host->handle(), this);
			seed &= 0xffff;
		} while (conn_.host->has_socket(psaddr, seed));

		id_recv += seed;
		id_send += seed;
	}

	conn_.state						= CS_IDLE;
	conn_.conn_seed					= seed;
	conn_.conn_id_recv				= id_recv;
	conn_.conn_id_send				= id_send;
	conn_.addr					= psaddr;
	conn_.host->refresh_clock(nullptr);
	timing_.last_got_packet			= conn_.host->current_ms();
	timing_.last_sent_packet			= conn_.host->current_ms();
	timing_.last_measured_delay		= conn_.host->current_ms() + 0x70000000;
	cc_->init_timing(conn_.host->current_ms());
	cc_->init_delay_histories(conn_.host->current_ms());

	mtu_.reset((uint32)get_udp_mtu(), conn_.host->current_ms());
	mtu_.set_last_to_ceiling();

	conn_.host->register_socket(this);

	cc_->set_max_window(get_packet_size());

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP socket initialized");
	#endif
}

// 主动发起一次 uTP 连接（发送 SYN）。
// 调用后 socket 状态由 CS_UNINITIALIZED 转为 CS_SYN_SENT，
// 收到 SYN-ACK 后通过 UTP_STATE_CONNECT 事件回调通知用户。
// 参数 to - 对端地址
// 参数 tolen - 对端地址长度
// 返回: 0 表示成功发起；-1 表示 socket 已处于非初始状态
int UtpSocket::connect(const struct sockaddr *to, socklen_t tolen)
{
	assert(conn_.state == CS_UNINITIALIZED);
	if (conn_.state != CS_UNINITIALIZED) {
		conn_.state = CS_DESTROY;
		return -1;
	}

	initialize(to, tolen, true, 0, 0, 1);

	assert(send_.cur_window_packets == 0);
	assert(send_.outbuf.get(send_.seq_nr) == NULL);
	assert(sizeof(PacketFormatV1) == 20);

	conn_.state = CS_SYN_SENT;
	conn_.host->refresh_clock(this);

	log(UTP_LOG_NORMAL, "UTP_Connect conn_seed_:%u packet_size:%u (B) "
			"target_delay_:%u (ms) delay_history:%u "
			"delay_base_history:%u (minutes)",
			conn_.conn_seed, PACKET_SIZE, conn_.target_delay / 1000,
			CUR_DELAY_SIZE, DELAY_BASE_HISTORY);

	cc_->set_retransmit_timeout(3000);
	cc_->set_rto_timeout(conn_.host->current_ms() + cc_->retransmit_timeout());
	recv_.last_rcv_win = get_rcv_window();

	send_.seq_nr = utp_call_get_random(conn_.host->handle(), this);

	const size_t header_size = sizeof(PacketFormatV1);

	auto syn_pkt = std::make_unique<OutgoingPacket>();
	OutgoingPacket *pkt = syn_pkt.get();
	pkt->data.resize(header_size);
	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();

	memset(p1, 0, header_size);
	p1->set_version(1);
	p1->set_type(ST_SYN);
	p1->ext = 0;
	p1->connid = conn_.conn_id_recv;
	p1->windowsize = (uint32)recv_.last_rcv_win;
	p1->seq_nr = send_.seq_nr;
	pkt->transmissions = 0;
	pkt->length = header_size;
	pkt->payload = 0;

	send_.outbuf.ensure_size(send_.seq_nr, send_.cur_window_packets);
	send_.outbuf.put(send_.seq_nr, std::move(syn_pkt));
	send_.seq_nr++;
	send_.cur_window_packets++;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "incrementing cur_window_packets_:%u", send_.cur_window_packets);
	#endif

	send_packet(pkt);
	return 0;
}

// 设置单个 uTP socket 级别的选项。
// 支持的选项：UTP_SNDBUF/UTP_RCVBUF/UTP_TARGET_DELAY。
// 必须在 socket 处于 CS_CONNECTED 之前调用以避免与并发传输冲突。
// 返回: 0 表示成功，-1 表示 opt 未知
int UtpSocket::set_option(int opt, int val)
{
	switch (opt) {

	case UTP_SNDBUF:
		assert(val >= 1);
		send_.opt_sndbuf = val;
		return 0;

	case UTP_RCVBUF:
		assert(val >= 1);
		recv_.opt_rcvbuf = val;
		return 0;

	case UTP_TARGET_DELAY:
		conn_.target_delay = val;
		return 0;
	}

	return -1;
}

// 获取单个 uTP socket 级别选项的当前值。
// 返回: 选项当前值；opt 未知时返回 -1
int UtpSocket::get_option(int opt)
{
	switch (opt) {
		case UTP_SNDBUF:		return send_.opt_sndbuf;
		case UTP_RCVBUF:		return recv_.opt_rcvbuf;
		case UTP_TARGET_DELAY:	return conn_.target_delay;
	}

	return -1;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::writev
// 功能：使用 scatter/gather I/O 向 uTP 连接写入多段不连续数据。
//
// 参数：
//   iovec_input  - iovec 数组，每项包含缓冲区地址和长度
//   num_iovecs   - iovec 数组长度（上限 UTP_IOV_MAX）
//
// 返回值：
//   成功入队的总字节数；socket 未连接或已发送 FIN 时返回 0
// -----------------------------------------------------------------------------
ssize_t UtpSocket::writev(struct utp_iovec *iovec_input, size_t num_iovecs)
{
	static utp_iovec iovec[UTP_IOV_MAX];

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

	if (!fsm().writable()) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "UTP_Write %u bytes = false (not CS_CONNECTED)", (uint)bytes);
		#endif
		return 0;
	}

	if (send_.fin_sent) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "UTP_Write %u bytes = false (fin_sent already)", (uint)bytes);
		#endif
		return 0;
	}

	conn_.host->refresh_clock(this);

	size_t packet_size = get_packet_size();
	size_t num_to_send = min<size_t>(bytes, packet_size);
	while (!is_full(num_to_send)) {
		bytes -= num_to_send;
		sent  += num_to_send;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending packet. seq_nr_:%u ack_nr_:%u wnd:%u/%u/%u rcv_win:%u size:%u cur_window_packets_:%u",
			send_.seq_nr, recv_.ack_nr,
			(uint)(cc_->cur_window() + num_to_send),
			(uint)cc_->max_window(), (uint)send_.max_window_user,
			(uint)recv_.last_rcv_win, num_to_send,
			send_.cur_window_packets);
		#endif
		write_outgoing_packet(num_to_send, ST_DATA, iovec, num_iovecs);
		num_to_send = min<size_t>(bytes, packet_size);

		if (num_to_send == 0) {
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "UTP_Write %u bytes = true", (uint)param);
			#endif
			return sent;
		}
	}

	bool full = is_full();
	if (full) {
		conn_.state = CS_CONNECTED_FULL;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_Write %u bytes = %s", (uint)bytes, full ? "false" : "true");
	#endif

	return sent;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::read_drained
// 功能：通知库应用已消费完数据，释放接收缓冲区。
// -----------------------------------------------------------------------------
void UtpSocket::read_drained()
{
	assert(conn_.state != CS_UNINITIALIZED);
	if (conn_.state == CS_UNINITIALIZED) return;

	const size_t rcvwin = get_rcv_window();

	if (rcvwin > recv_.last_rcv_win) {
		if (recv_.last_rcv_win == 0) {
			send_ack();
		} else {
			conn_.host->refresh_clock(this);
			schedule_ack();
		}
	}
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::get_peername
// 功能：获取对端地址。
//
// 参数：
//   addr_out - 输出缓冲区，用于存储对端地址
//   addrlen  - 输入/输出参数，传入 addr 缓冲区大小，返回实际地址长度
//
// 返回值：
//   0 表示成功；-1 表示 socket 未初始化
// -----------------------------------------------------------------------------
int UtpSocket::get_peername(struct sockaddr *addr_out, socklen_t *addrlen)
{
	assert(conn_.state != CS_UNINITIALIZED);
	if (conn_.state == CS_UNINITIALIZED) return -1;

	socklen_t len;
	const SOCKADDR_STORAGE sa = conn_.addr.get_sockaddr_storage(&len);
	*addrlen = min(len, *addrlen);
	memcpy(addr_out, &sa, *addrlen);
	return 0;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::get_delays
// 功能：获取延迟测量值。
//
// 参数：
//   ours   - 输出参数，本端到对端的延迟（毫秒），可为 NULL
//   theirs - 输出参数，对端到本端的延迟（毫秒），可为 NULL
//   age    - 输出参数，距离上次测量延迟的时间（毫秒），可为 NULL
//
// 返回值：
//   0 表示成功；-1 表示 socket 未初始化
// -----------------------------------------------------------------------------
int UtpSocket::get_delays(uint32 *ours, uint32 *theirs, uint32 *age)
{
	assert(conn_.state != CS_UNINITIALIZED);
	if (conn_.state == CS_UNINITIALIZED) {
		if (ours)   *ours   = 0;
		if (theirs) *theirs = 0;
		if (age)    *age    = 0;
		return -1;
	}

	if (ours)   *ours   = cc_->our_hist().get_value();
	if (theirs) *theirs = cc_->their_hist().get_value();
	if (age)    *age    = (uint32)(conn_.host->current_ms() - timing_.last_measured_delay);
	return 0;
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::close
// 功能：关闭 socket。行为按当前状态多态分派（State 模式）。
// -----------------------------------------------------------------------------
void UtpSocket::close()
{
	assert(conn_.state != CS_UNINITIALIZED
		&& conn_.state != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_Close in state:%s", fsm().name());
	#endif

	// 行为按当前状态多态分派（State 模式）
	fsm().close(*this);

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_Close end in state:%s", fsm().name());
	#endif
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::shutdown
// 功能：关闭 socket 方向（SHUT_RD 关闭读、SHUT_WR 关闭写）。
// -----------------------------------------------------------------------------
void UtpSocket::shutdown(int how)
{
	assert(conn_.state != CS_UNINITIALIZED
		&& conn_.state != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_shutdown(%d) in state:%s", how, fsm().name());
	#endif

	// 行为按当前状态多态分派（State 模式）
	fsm().shutdown(*this, how);
}

// 获取 socket 统计信息（仅 _DEBUG 编译时有效，否则返回 NULL）
utp_socket_stats* UtpSocket::get_stats()
{
	#ifdef _DEBUG
		stats_.mtu_guess = mtu_.raw_mtu();
		return &stats_;
	#else
		return NULL;
	#endif
}
