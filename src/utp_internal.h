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
#include "utp/socket_host.hpp"

// 地址格式化辅助：把地址写进按值传入的临时缓冲并返回其内部指针。
// 返回指针仅在当前完整表达式内有效，适合直接作为日志参数；
// 取代原先的全局可变缓冲 addrbuf（多 context / 多线程下不安全）。
struct AddrFmtBuf { char s[65]; };
inline const char* addrfmt(const utp::Address& addr, AddrFmtBuf&& buf = {}) {
	return addr.fmt(buf.s, sizeof(buf.s));
}

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

	UtpSocketKey(const utp::Address& _addr, uint32 _recv_id)
		: addr(_addr), recv_id(_recv_id) {}

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
// 键为 UtpSocketKey (对端地址+接收连接ID)。
// 哈希表以 unique_ptr 持有 socket 所有权：注册即移交，销毁经 destroy_socket
// 或 ~UtpContext 统一释放，不再依赖手动 delete。
using SocketMap = std::unordered_map<UtpSocketKey, std::unique_ptr<UtpSocket>, UtpSocketKeyHash>;

// UtpContext: uTP 协议库全局上下文,所有套接字共享一个 context 实例。
// 整个库采用单线程事件驱动模型,context 持有回调表、活跃套接字表、待发 ACK 列表、
// 已发送 RST 缓存,以及当前时间缓存等公共状态。应用层通过 utp_create_context 间接创建。
class UtpContext : public ISocketHost {
public:
	UtpContext();
	~UtpContext() override;

	void log(int level, utp_socket *socket, char const *fmt, ...);
	void log_unchecked(utp_socket *socket, char const *fmt, ...);
	void vlog_unchecked(utp_socket *socket, char const *fmt, va_list va);
	// ISocketHost：按 level 过滤日志（内联，供多 TU 调用）
	bool would_log(int level) override {
		if (level == UTP_LOG_NORMAL) return log_normal_;
		if (level == UTP_LOG_MTU) return log_mtu_;
		if (level == UTP_LOG_DEBUG) return log_debug_;
		return true;
	}

	void register_sent_packet(size_t length);
	void send_to_addr_impl(const byte *p, size_t len, const utp::Address &addr, int flags = 0);
	int process_udp(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen);
	int process_icmp_fragmentation(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu);
	int process_icmp_error(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen);
	void issue_deferred_acks();
	void check_timeouts();

	UtpSocket* parse_icmp_payload(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen);

	UtpSocket* create_socket();
	void set_callback(int callback_name, utp_callback_t *proc);
	int set_option(int opt, int val);
	int get_option(int opt);

	utp_context_stats* stats() { return &context_stats_; }
	void* userdata() { return userdata_; }
	void set_userdata(void *userdata) { userdata_ = userdata; }

	// ── ISocketHost 实现：UtpSocket 通过此窄接口获取宿主服务，
	//    不再 friend 访问 UtpContext 私有成员（依赖倒置）──
	uint64 current_ms() const override { return current_ms_; }
	uint64 refresh_clock(UtpSocket* who) override;          // .cpp（需 get_milliseconds 回调）
	void send_to(const byte* p, size_t len, const utp::Address& addr, int flags) override {
		send_to_addr_impl(p, len, addr, flags);
	}
	void schedule_ack(UtpSocket* s) override;               // .cpp（需 UtpSocket 完整类型）
	void remove_ack(UtpSocket* s) override;                 // .cpp
	void record_raw_recv(size_t len) override;              // .cpp（需 wire::packet_size_bucket）
	size_t default_target_delay() const override { return target_delay_; }
	size_t default_sndbuf() const override { return opt_sndbuf_; }
	size_t default_rcvbuf() const override { return opt_rcvbuf_; }
	bool has_socket(const utp::Address& addr, uint32 recv_id) const override {
		return sockets_.count(UtpSocketKey(addr, recv_id)) != 0;
	}
	void register_socket(UtpSocket* s) override;            // .cpp
	void on_socket_destroyed(UtpSocket* s) override;        // .cpp
	UtpCallbacks* callbacks() override { return callbacks_.get(); }
	utp_context* handle() override { return this; }

private:
	// 按连接 ID 查找 socket：先按 id 精确匹配（接收方向 ID），
	// 再尝试 id±1 且发送方向 ID 等于 id 的连接（对端视角的 ID 偏移）。
	// RST 处理与 ICMP 错误归属共用此规则。
	UtpSocket* find_socket_for_id(const utp::Address &addr, uint32 id);

	// 从注册表移除并销毁 socket（唯一的逐个销毁入口）。
	// 先把所有权移出哈希表再析构，保证 ~UtpSocket 的回调期间容器不被重入修改。
	void destroy_socket(UtpSocket* s);

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
