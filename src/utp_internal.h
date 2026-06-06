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

enum bandwidth_type_t {
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

// RST_Info: 记录已发送过的 RST (复位) 包信息,用于抑制短时间内对同一对端重复发送 RST。
// 当收到无法识别的非 SYN 数据包时,服务端会回复 RST 并把对端 (地址+connid+ack_nr) 缓存下来;
// 之后若再次收到相同数据包,只要仍在缓存窗口内,就不再重复发送 RST,以避免被攻击者放大。
struct PACKED_ATTRIBUTE RST_Info {
	PackedSockAddr addr;
	uint32 connid;
	uint16 ack_nr;
	uint64 timestamp;
};

// It's really important that we don't have duplicate keys in the hash table.
// If we do, we'll eventually crash. if we try to remove the second instance
// of the key, we'll accidentally remove the first instead. then later,
// checkTimeouts will try to access the second one's already freed memory.
void UTP_FreeAll(struct UTPSocketHT *utp_sockets);

// UTPSocketKey: 套接字哈希表的复合键,由"对端地址 + 我方接收连接 ID"组成。
// 这里 recv_id 是对方在 SYN 包中告知我们的 connid (即我方作为接收方使用的 ID),
// 它在建立连接时随机生成,用来在同一 (ip, port) 对端存在多个并发连接时唯一标识某条连接。
// 比较和哈希运算都同时考虑地址与 connid,确保不同连接即使端口相同也能被区分。
struct UTPSocketKey {
	PackedSockAddr addr;
	uint32 recv_id;		 // "conn_seed", "conn_id"

	UTPSocketKey(const PackedSockAddr& _addr, uint32 _recv_id) {
		memset(this, 0, sizeof(*this));
		addr = _addr;
		recv_id = _recv_id;
	}

	bool operator == (const UTPSocketKey &other) const {
		return recv_id == other.recv_id && addr == other.addr;
	}

	uint32 compute_hash() const {
		return recv_id ^ addr.compute_hash();
	}
};

// UTPSocketKeyData: 哈希表中键对应的值,既保存键本身 (便于遍历),
// 也保存指向实际 UTPSocket 对象的指针,实现 O(1) 的连接查找。
struct UTPSocketKeyData {
	UTPSocketKey key;
	UTPSocket *socket;
};

struct UTPSocketKeyHash {
	size_t operator()(const UTPSocketKey& k) const {
		return static_cast<size_t>(k.compute_hash());
	}
};

// UTPSocketHT: 包裹 std::unordered_map 的 uTP 套接字哈希表,负责管理所有活跃连接。
// 之所以使用单独的薄包装类,是为了在析构时统一释放所有 UTPSocket,
// 避免直接遍历 map_ 时遇到迭代器失效等问题。
class UTPSocketHT {
	std::unordered_map<UTPSocketKey, UTPSocketKeyData, UTPSocketKeyHash> map_;
public:
	UTPSocketHT() = default;
	~UTPSocketHT();
	UTPSocketKeyData* Lookup(const UTPSocketKey& key);
	UTPSocketKeyData* Add(const UTPSocketKey& key);
	UTPSocketKeyData* Delete(const UTPSocketKey& key);
	size_t GetCount() { return map_.size(); }
	auto begin() { return map_.begin(); }
	auto end() { return map_.end(); }
};

// struct_utp_context: uTP 协议库全局上下文,所有套接字共享一个 context 实例。
// 整个库采用单线程事件驱动模型,context 持有回调表、活跃套接字表、待发 ACK 列表、
// 已发送 RST 缓存,以及当前时间缓存等公共状态。应用层通过 utp_create_context 间接创建。
struct struct_utp_context {
	void *userdata;
	utp_callback_t* callbacks[UTP_ARRAY_SIZE];

	// current_ms: 当前毫秒时钟的缓存,避免每个操作都向系统查询时间;
	// utp_check_timeouts 会统一刷新,其它路径若需要时间应先更新该值。
	uint64 current_ms; //当前时钟的缓存
	utp_context_stats context_stats;

	// last_utp_socket: 上一被处理过的套接字指针,作为接收数据包时查找的"快路径"缓存。
	// 由于同一对端通常会连续收发包,先比较此缓存可避免每次都做哈希表查找。
	UTPSocket *last_utp_socket;

	// ack_sockets: 收到数据后等待发送 ACK 的套接字列表。utp_issue_deferred_acks 时
	// 统一遍历,实现 ACK 合并 (delayed ACK) 以减少纯 ACK 包数量。
	std::vector<UTPSocket*> ack_sockets; //等待发送ack的sockets
	// rst_info: 已发送 RST 的历史缓存,用于抑制对同一四元组重复回 RST。
	std::vector<RST_Info> rst_info;
	UTPSocketHT *utp_sockets;
	// target_delay: LEDBAT 拥塞控制的目标排队延迟,默认 100ms (CCONTROL_TARGET)。
	size_t target_delay;
	// opt_sndbuf / opt_rcvbuf: 套接字发送/接收缓冲区的默认上限。
	size_t opt_sndbuf;
	size_t opt_rcvbuf;
	// last_check: 上次 utp_check_timeouts 调用的时间戳,用于节流 (TIMEOUT_CHECK_INTERVAL)。
	uint64 last_check;

	struct_utp_context();
	~struct_utp_context();

	void log(int level, utp_socket *socket, char const *fmt, ...);
	void log_unchecked(utp_socket *socket, char const *fmt, ...);
	bool would_log(int level);

	bool log_normal:1;	// log normal events?
	bool log_mtu:1;		// log MTU related events?
	bool log_debug:1;	// log debugging events? (Must also compile with UTP_DEBUG_LOGGING defined)
};

#endif //__UTP_INTERNAL_H__
