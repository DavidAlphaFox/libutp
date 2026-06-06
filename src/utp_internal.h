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

#ifndef __UTP_INTERNAL_H__
#define __UTP_INTERNAL_H__

#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>

#include "utp.h"
#include "utp_callbacks.h"
#include "utp/endian.hpp"
#include "utp_packedsockaddr.h"

/* These originally lived in utp_config.h */
#define CCONTROL_TARGET (100 * 1000) // us

enum BandwidthType {
	payload_bandwidth, connect_overhead,
	close_overhead, ack_overhead,
	header_overhead, retransmit_overhead
};

#ifdef WIN32
	#ifdef _MSC_VER
		#include "libutp_inet_ntop.h"
	#endif

	// newer versions of MSVC define these in errno.h
	#ifndef ECONNRESET
		#define ECONNRESET WSAECONNRESET
		#define EMSGSIZE WSAEMSGSIZE
		#define ECONNREFUSED WSAECONNREFUSED
		#define ETIMEDOUT WSAETIMEDOUT
	#endif
#endif

// RstInfo: 记录已发送过的 RST (复位) 包信息,用于抑制短时间内对同一对端重复发送 RST。
// 当收到无法识别的非 SYN 数据包时,服务端会回复 RST 并把对端 (地址+connid+ack_nr) 缓存下来;
// 之后若再次收到相同数据包,只要仍在缓存窗口内,就不再重复发送 RST,以避免被攻击者放大。
struct PACKED_ATTRIBUTE RstInfo {
	PackedSockAddr addr;
	uint32 connid;
	uint16 ack_nr;
	uint64 timestamp;
};

// UtpSocketKey: 套接字哈希表的复合键,由"对端地址 + 我方接收连接 ID"组成。
// 这里 recv_id 是对方在 SYN 包中告知我们的 connid (即我方作为接收方使用的 ID),
// 它在建立连接时随机生成,用来在同一 (ip, port) 对端存在多个并发连接时唯一标识某条连接。
// 比较和哈希运算都同时考虑地址与 connid,确保不同连接即使端口相同也能被区分。
struct UtpSocketKey {
	PackedSockAddr addr;
	uint32 recv_id;		 // "conn_seed", "conn_id"

	UtpSocketKey(const PackedSockAddr& _addr, uint32 _recv_id) {
		memset(this, 0, sizeof(*this));
		addr = _addr;
		recv_id = _recv_id;
	}

	bool operator == (const UtpSocketKey &other) const {
		return recv_id == other.recv_id && addr == other.addr;
	}

	uint32 compute_hash() const {
		return recv_id ^ addr.compute_hash();
	}
};

struct UtpSocketKeyHash {
	size_t operator()(const UtpSocketKey& k) const {
		return static_cast<size_t>(k.compute_hash());
	}
};

// SocketMap: 所有活跃 uTP 连接的哈希表。
// 键为 UtpSocketKey (对端地址+接收连接ID),值为 UtpSocket 裸指针。
// UtpContext 拥有所有 socket 的生命周期,负责析构时统一释放。
using SocketMap = std::unordered_map<UtpSocketKey, UtpSocket*, UtpSocketKeyHash>;

// UtpContext: uTP 协议库全局上下文,所有套接字共享一个 context 实例。
// 整个库采用单线程事件驱动模型,context 持有回调表、活跃套接字表、待发 ACK 列表、
// 已发送 RST 缓存,以及当前时间缓存等公共状态。应用层通过 utp_create_context 间接创建。
class UtpContext {
public:
	void *userdata_;
	utp_callback_t* callbacks_[UTP_ARRAY_SIZE];

	// current_ms_: 当前毫秒时钟的缓存,避免每个操作都向系统查询时间;
	// utp_check_timeouts 会统一刷新,其它路径若需要时间应先更新该值。
	uint64 current_ms_; //当前时钟的缓存
	utp_context_stats context_stats_;

	// last_utp_socket_: 上一被处理过的套接字指针,作为接收数据包时查找的"快路径"缓存。
	// 由于同一对端通常会连续收发包,先比较此缓存可避免每次都做哈希表查找。
	UtpSocket *last_utp_socket_;

	// ack_sockets_: 收到数据后等待发送 ACK 的套接字列表。utp_issue_deferred_acks 时
	// 统一遍历,实现 ACK 合并 (delayed ACK) 以减少纯 ACK 包数量。
	std::vector<UtpSocket*> ack_sockets_; //等待发送ack的sockets
	// rst_info_: 已发送 RST 的历史缓存,用于抑制对同一四元组重复回 RST。
	std::vector<RstInfo> rst_info_;
	SocketMap sockets_;
	// target_delay_: LEDBAT 拥塞控制的目标排队延迟,默认 100ms (CCONTROL_TARGET)。
	size_t target_delay_;
	// opt_sndbuf_ / opt_rcvbuf_: 套接字发送/接收缓冲区的默认上限。
	size_t opt_sndbuf_;
	size_t opt_rcvbuf_;
	// last_check_: 上次 utp_check_timeouts 调用的时间戳,用于节流 (TIMEOUT_CHECK_INTERVAL)。
	uint64 last_check_;

	UtpContext();
	~UtpContext();

	void log(int level, utp_socket *socket, char const *fmt, ...);
	void log_unchecked(utp_socket *socket, char const *fmt, ...);
	bool would_log(int level);

	bool log_normal_:1;	// log normal events?
	bool log_mtu_:1;		// log MTU related events?
	bool log_debug_:1;	// log debugging events? (Must also compile with UTP_DEBUG_LOGGING defined)
};

#endif //__UTP_INTERNAL_H__
