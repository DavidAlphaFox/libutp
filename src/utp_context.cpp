// vim:set ts=4 sw=4 ai:

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

// UtpContext（uTP 全局上下文）成员实现
// 包括：生命周期、socket 注册表与所有权管理、延迟 ACK 列表、
// 定时器驱动、选项读写、日志输出、ISocketHost 宿主服务实现。
// （报文解析路由 process_udp / ICMP 处理在 utp_process.cpp；
//   C API 薄包装在 utp_api.cpp。）

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <cassert>

#include "utp_internal.h"
#include "utp/config.hpp"
#include "utp_socket.hpp"
#include "utp_callbacks.h"

using namespace utp::config;

// -----------------------------------------------------------------------------
// 函数：UtpContext::UtpContext
// 功能：构造函数，初始化 uTP 上下文。
// -----------------------------------------------------------------------------
UtpContext::UtpContext()
	: userdata_(NULL)
	, callbacks_(std::make_unique<UtpCallbacks>())
	, current_ms_(0)
	, last_utp_socket_(NULL)
	, log_normal_(false)
	, log_mtu_(false)
	, log_debug_(false)
{
	memset(&context_stats_, 0, sizeof(context_stats_));
	target_delay_ = utp::config::CCONTROL_TARGET;
	opt_rcvbuf_ = opt_sndbuf_ = 1024 * 1024;
	last_check_ = 0;
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::~UtpContext
// 功能：析构函数，清理上下文及其管理的所有 socket 资源。
// -----------------------------------------------------------------------------
UtpContext::~UtpContext() {
	// 先把所有权整体移出哈希表再统一析构：
	// ~UtpSocket 会回调 on_socket_destroyed / remove_from_ack_list，
	// 不能在遍历容器的同时触发这些回调。
	// socks 是局部变量，在本函数体结束（成员析构之前）销毁，
	// 因此回调期间 ack_sockets_/callbacks_ 等成员仍然有效。
	std::vector<std::unique_ptr<UtpSocket>> socks;
	socks.reserve(sockets_.size());
	for (auto& [key, socket] : sockets_) {
		socks.push_back(std::move(socket));
	}
	sockets_.clear();
}

// 创建一个未初始化的 uTP socket。
// 创建后 socket 处于 CS_UNINITIALIZED 状态，必须通过 utp_connect 发起主动连接，
// 或由协议栈在接收到 SYN 时内部调用 UtpSocket::initialize 转为 CS_SYN_RECV。
// 返回: 新创建的 socket 指针
UtpSocket* UtpContext::create_socket()
{
	// 构造后 send_.seq_nr 与 timing_.fast_resend_seq_nr 均为默认值 1（NSDMI 已同步）；
	// connect()/accept_syn() 会在随机化 seq_nr 时重新对齐二者。
	UtpSocket *conn = new UtpSocket(this);
	return conn;
}

// 设置 uTP 上下文级别的选项。
// 支持的选项：UTP_LOG_NORMAL/UTP_LOG_MTU/UTP_LOG_DEBUG/UTP_TARGET_DELAY/UTP_SNDBUF/UTP_RCVBUF。
// 参数 opt - 选项名（UTP_* 枚举值）
// 参数 val - 选项值（非 0/0 用于布尔选项，正整数用于缓冲区/延迟）
// 返回: 0 表示成功，-1 表示 opt 未知
int UtpContext::set_option(int opt, int val)
{
	switch (opt) {
    	case UTP_LOG_NORMAL:
			log_normal_ = val ? true : false;
			return 0;

    	case UTP_LOG_MTU:
			log_mtu_ = val ? true : false;
			return 0;

    	case UTP_LOG_DEBUG:
			log_debug_ = val ? true : false;
			return 0;

    	case UTP_TARGET_DELAY:
			target_delay_ = val;
			return 0;

		case UTP_SNDBUF:
			assert(val >= 1);
			opt_sndbuf_ = val;
			return 0;

		case UTP_RCVBUF:
			assert(val >= 1);
			opt_rcvbuf_ = val;
			return 0;
	}
	return -1;
}

// 获取 uTP 上下文级别选项的当前值。选项集与 set_option 相同。
// 返回: 选项当前值；opt 未知时返回 -1
int UtpContext::get_option(int opt)
{
	switch (opt) {
    	case UTP_LOG_NORMAL:	return log_normal_ ? 1 : 0;
    	case UTP_LOG_MTU:		return log_mtu_    ? 1 : 0;
    	case UTP_LOG_DEBUG:		return log_debug_  ? 1 : 0;
    	case UTP_TARGET_DELAY:	return target_delay_;
		case UTP_SNDBUF:		return opt_sndbuf_;
		case UTP_RCVBUF:		return opt_rcvbuf_;
	}
	return -1;
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::issue_deferred_acks
// 功能：触发延迟 ACK 发送。
//
// 说明：send_ack() 会把 socket 从 ack_sockets_ 中移除（swap-pop），
// 因此用 i-- 抵消下标前进，始终处理当前位置上换入的新元素。
// -----------------------------------------------------------------------------
void UtpContext::issue_deferred_acks()
{
	for (size_t i = 0; i < ack_sockets_.size(); i++) {
		UtpSocket *conn = ack_sockets_[i];
		conn->send_ack();
		i--;
	}
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::check_timeouts
// 功能：周期性驱动库的定时器：RST 缓存过期清理、各 socket 超时检查、
//       销毁进入 CS_DESTROY 状态的 socket。
// -----------------------------------------------------------------------------
void UtpContext::check_timeouts()
{
	current_ms_ = utp_call_get_milliseconds(this, NULL);

	if (current_ms_ - last_check_ < TIMEOUT_CHECK_INTERVAL)
		return;

	last_check_ = current_ms_;

	for (size_t i = 0; i < rst_info_.size(); i++) {
		if ((int)(current_ms_ - rst_info_[i].timestamp) >= RST_INFO_TIMEOUT) {
			rst_info_[i] = std::move(rst_info_.back());
			rst_info_.pop_back();
			i--;
		}
	}
	if (rst_info_.size() != rst_info_.capacity()) {
		rst_info_.shrink_to_fit();
	}
	std::vector<UtpSocket*> sockets;
	sockets.reserve(sockets_.size());
	for (auto& [key, socket] : sockets_) {
		sockets.push_back(socket.get());
	}
	for (auto* conn : sockets) {
		conn->check_timeouts();

		if (conn->state() == CS_DESTROY) {
			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Destroying");
			#endif
			destroy_socket(conn);
		}
	}
}

// =============================================================================
// ISocketHost 接口实现：这些方法需要 UtpSocket 完整类型（访问 ack_index 等），
// 故定义在 .cpp（此处 UtpSocket 已完整）。UtpSocket 通过 ISocketHost* 调用它们，
// 不再 friend 访问 UtpContext 私有成员。
// =============================================================================

// 用 get_milliseconds 回调刷新缓存时钟并返回新值
uint64 UtpContext::refresh_clock(UtpSocket* who)
{
	current_ms_ = utp_call_get_milliseconds(this, who);
	return current_ms_;
}

// 延迟 ACK 登记：去重后加入列表尾，把下标回写到 socket
void UtpContext::schedule_ack(UtpSocket* s)
{
	if (s->ack_index() == -1) {
		ack_sockets_.push_back(s);
		s->set_ack_index((int)ack_sockets_.size() - 1);
	}
}

// 延迟 ACK 注销：swap-pop O(1) 移除，并修正被移动元素的下标
void UtpContext::remove_ack(UtpSocket* s)
{
	const int idx = s->ack_index();
	if (idx < 0) return;

	UtpSocket *last = ack_sockets_.back();
	assert(last->ack_index() < (int)ack_sockets_.size());
	assert(ack_sockets_[last->ack_index()] == last);

	last->set_ack_index(idx);
	ack_sockets_[idx] = last;
	s->set_ack_index(-1);
	ack_sockets_.pop_back();
}

// 发送记账：按尺寸桶累加（由 send_to_addr_impl 调用）
void UtpContext::register_sent_packet(size_t length)
{
	context_stats_._nraw_send[utp::wire::packet_size_bucket(length)]++;
}

// 接收记账：按尺寸桶累加
void UtpContext::record_raw_recv(size_t len)
{
	context_stats_._nraw_recv[utp::wire::packet_size_bucket(len)]++;
}

// 把 socket 按 (对端地址, 接收连接ID) 登记进注册表（所有权移交给哈希表）
void UtpContext::register_socket(UtpSocket* s)
{
	UtpSocketKey key(s->peer_addr(), s->recv_conn_id());
	assert(sockets_.count(key) == 0);
	sockets_[key] = std::unique_ptr<UtpSocket>(s);
}

// socket 析构通知：仅失效查找缓存。
// 注册表的移除/释放由 destroy_socket 或 ~UtpContext 负责，此处不得操作容器
// （析构正发生在容器元素释放过程中，重入修改是未定义行为）。
void UtpContext::on_socket_destroyed(UtpSocket* s)
{
	if (last_utp_socket_ == s) {
		last_utp_socket_ = NULL;
	}
}

// 从注册表移除并销毁 socket：先把所有权移出哈希表，再让其析构
void UtpContext::destroy_socket(UtpSocket* s)
{
	auto it = sockets_.find(UtpSocketKey(s->peer_addr(), s->recv_conn_id()));
	assert(it != sockets_.end() && it->second.get() == s);
	if (it == sockets_.end()) return;

	std::unique_ptr<UtpSocket> dying = std::move(it->second);
	sockets_.erase(it);
	// dying 离开作用域时析构 ~UtpSocket（其间回调 on_socket_destroyed 仅清缓存）
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::send_to_addr_impl
// 功能：通过 utp 回调接口将一段字节流作为 UDP 数据报发送到指定对端地址。
//
// 上下文：
//   这是 uTP 所有出站数据包的最终发送出口。utp_call_sendto 是一个由调用方
//   注册的回调函数（参见 utp_callbacks.h），实际执行 sendto() 系统调用。
//
// 参数：
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
void UtpContext::send_to_addr_impl(const byte *p, size_t len, const utp::Address &addr, int flags)
{
	socklen_t tolen;
	// 取出平台原生 sockaddr 表示
	SOCKADDR_STORAGE to = addr.get_sockaddr_storage(&tolen);
	// 计入发送统计
	register_sent_packet(len);
	// 通过注册的回调执行真正的 sendto
	utp_call_sendto(this, NULL, p, len, (const struct sockaddr *)&to, tolen, flags);
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::log
// 功能：日志记录。根据日志级别决定是否输出日志。
//
// 参数：
//   level  - 日志级别（UTP_LOG_NORMAL/UTP_LOG_MTU/UTP_LOG_DEBUG）
//   socket - 关联的 socket 指针（可能为 nullptr）
//   fmt    - printf 格式字符串
// -----------------------------------------------------------------------------
void UtpContext::log(int level, utp_socket *socket, char const *fmt, ...)
{
	if (!would_log(level)) {
		return;
	}

	va_list va;
	va_start(va, fmt);
	vlog_unchecked(socket, fmt, va);
	va_end(va);
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::log_unchecked
// 功能：无条件日志记录。不检查日志级别，直接格式化并输出日志。
// -----------------------------------------------------------------------------
void UtpContext::log_unchecked(utp_socket *socket, char const *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vlog_unchecked(socket, fmt, va);
	va_end(va);
}

// va_list 版本：log/log_unchecked 共同的格式化出口。
// （原实现把 va_list 当作可变参数二次转发，格式参数会被解释成垃圾值——已修复。）
void UtpContext::vlog_unchecked(utp_socket *socket, char const *fmt, va_list va)
{
	char buf[4096];

	vsnprintf(buf, 4096, fmt, va);
	buf[4095] = '\0';

	utp_call_log(this, socket, (const byte *)buf);
}

// 注：UtpContext::would_log 为 utp_internal.h 中的内联定义
// （供 UtpSocket::vlog 等多个翻译单元内联调用，避免跨 TU 未定义引用）。
