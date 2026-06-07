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
#include <memory>

#include "utp.h"
#include "utp_callbacks.hpp"
#include "utp/address.hpp"
#include "utp/endian.hpp"

extern char addrbuf[65];
#define addrfmt(x, s) x.fmt(s, sizeof(s))

class UtpSocket;

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
	utp::Address addr;
	uint32 connid;
	uint16 ack_nr;
	uint64 timestamp;
};

// UtpSocketKey: 套接字哈希表的复合键,由"对端地址 + 我方接收连接 ID"组成。
// 这里 recv_id 是对方在 SYN 包中告知我们的 connid (即我方作为接收方使用的 ID),
// 它在建立连接时随机生成,用来在同一 (ip, port) 对端存在多个并发连接时唯一标识某条连接。
// 比较和哈希运算都同时考虑地址与 connid,确保不同连接即使端口相同也能被区分。
struct UtpSocketKey {
	utp::Address addr;
	uint32 recv_id;		 // "conn_seed", "conn_id"

	UtpSocketKey(const utp::Address& _addr, uint32 _recv_id) {
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
	UtpContext();
	~UtpContext();

	void log(int level, utp_socket *socket, char const *fmt, ...);
	void log_unchecked(utp_socket *socket, char const *fmt, ...);
	bool would_log(int level);

	void register_sent_packet(size_t length);
	void send_to_addr_impl(const byte *p, size_t len, const utp::Address &addr, int flags = 0);
	int process_udp(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen);
	int process_icmp_fragmentation(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu);
	int process_icmp_error(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen);
	void issue_deferred_acks();
	void check_timeouts();

	UtpSocket* parse_icmp_payload(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen);

	UtpCallbacks* callbacks() { return callbacks_.get(); }

	friend class UtpSocket;
	friend void utp_set_callback(utp_context*, int, utp_callback_t*);
	friend void* utp_context_set_userdata(utp_context*, void*);
	friend void* utp_context_get_userdata(utp_context*);
	friend utp_context_stats* utp_get_context_stats(utp_context*);
	friend int utp_context_set_option(utp_context*, int, int);
	friend int utp_context_get_option(utp_context*, int);
	friend utp_socket* utp_create_socket(utp_context*);
	friend void utp_initialize_socket(utp_socket*, const struct sockaddr*, socklen_t, bool, uint32, uint32, uint32);
	friend ssize_t utp_writev(utp_socket*, struct utp_iovec*, size_t);
	friend int utp_get_delays(UtpSocket*, uint32*, uint32*, uint32*);

private:
	void *userdata_;
	std::unique_ptr<UtpCallbacks> callbacks_;

	uint64 current_ms_;
	utp_context_stats context_stats_;

	UtpSocket *last_utp_socket_;

	std::vector<UtpSocket*> ack_sockets_;
	std::vector<RstInfo> rst_info_;
	SocketMap sockets_;
	size_t target_delay_;
	size_t opt_sndbuf_;
	size_t opt_rcvbuf_;
	uint64 last_check_;

	bool log_normal_:1;
	bool log_mtu_:1;
	bool log_debug_:1;
};

#endif //__UTP_INTERNAL_H__
