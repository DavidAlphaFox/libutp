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

// libutp 公共 C API 实现
// 提供外部 C 语言接口，包括上下文初始化、销毁、回调设置和统计数据获取等功能

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <algorithm>
#include <cassert>

#include "utp_internal.h"
#include "utp/config.hpp"
#include "utp_socket.hpp"
#include "utp_callbacks.h"

using namespace utp::config;
using utp::wire::PacketFormatV1;
using utp::wire::ST_DATA;
using utp::wire::ST_FIN;
using utp::wire::ST_STATE;
using utp::wire::ST_SYN;
using std::min;

// CFunctionCallbackAdapter: 将旧的 C 函数指针回调适配到 UtpCallbacks 虚接口。
// 由 utp_set_callback() 内部使用，无需用户直接操作。
class CFunctionCallbackAdapter : public UtpCallbacks {
	utp_callback_t* handlers_[UTP_ARRAY_SIZE] = {};

	static utp_callback_arguments make_args(int type, utp_socket* sock) {
		utp_callback_arguments args{};
		args.callback_type = type;
		args.socket = sock;
		return args;
	}

public:
	void set_handler(int id, utp_callback_t* fn) { handlers_[id] = fn; }

	// ── ACTION ──
	void sendto(UtpSocket*, const uint8_t* data, size_t len,
	            const sockaddr* address, socklen_t address_len, uint32_t flags) override {
		if (!handlers_[UTP_SENDTO]) return;
		auto args = make_args(UTP_SENDTO, nullptr);
		args.buf = data; args.len = len;
		args.address = address; args.address_len = address_len; args.flags = flags;
		handlers_[UTP_SENDTO](&args);
	}

	// ── QUERY ──
	uint64 get_milliseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MILLISECONDS]) return UtpCallbacks::get_milliseconds(s);
		auto args = make_args(UTP_GET_MILLISECONDS, s);
		return handlers_[UTP_GET_MILLISECONDS](&args);
	}
	uint64 get_microseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MICROSECONDS]) return UtpCallbacks::get_microseconds(s);
		auto args = make_args(UTP_GET_MICROSECONDS, s);
		return handlers_[UTP_GET_MICROSECONDS](&args);
	}
	uint16 get_udp_mtu(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_MTU]) return UtpCallbacks::get_udp_mtu(s, a, l);
		auto args = make_args(UTP_GET_UDP_MTU, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_MTU](&args);
	}
	uint16 get_udp_overhead(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_OVERHEAD]) return UtpCallbacks::get_udp_overhead(s, a, l);
		auto args = make_args(UTP_GET_UDP_OVERHEAD, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_OVERHEAD](&args);
	}
	uint32 get_random(UtpSocket* s) override {
		if (!handlers_[UTP_GET_RANDOM]) return UtpCallbacks::get_random(s);
		auto args = make_args(UTP_GET_RANDOM, s);
		return (uint32)handlers_[UTP_GET_RANDOM](&args);
	}
	size_t get_read_buffer_size(UtpSocket* s) override {
		if (!handlers_[UTP_GET_READ_BUFFER_SIZE]) return UtpCallbacks::get_read_buffer_size(s);
		auto args = make_args(UTP_GET_READ_BUFFER_SIZE, s);
		return (size_t)handlers_[UTP_GET_READ_BUFFER_SIZE](&args);
	}

	// ── EVENT ──
	int on_firewall(const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_FIREWALL]) return UtpCallbacks::on_firewall(a, l);
		auto args = make_args(UTP_ON_FIREWALL, nullptr);
		args.address = a; args.address_len = l;
		return (int)handlers_[UTP_ON_FIREWALL](&args);
	}
	void on_accept(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_ACCEPT]) return;
		auto args = make_args(UTP_ON_ACCEPT, s);
		args.address = a; args.address_len = l;
		handlers_[UTP_ON_ACCEPT](&args);
	}
	void on_connect(UtpSocket* s) override {
		if (!handlers_[UTP_ON_CONNECT]) return;
		auto args = make_args(UTP_ON_CONNECT, s);
		handlers_[UTP_ON_CONNECT](&args);
	}
	void on_error(UtpSocket* s, int error_code) override {
		if (!handlers_[UTP_ON_ERROR]) return;
		auto args = make_args(UTP_ON_ERROR, s);
		args.error_code = error_code;
		handlers_[UTP_ON_ERROR](&args);
	}
	void on_read(UtpSocket* s, const uint8_t* data, size_t len) override {
		if (!handlers_[UTP_ON_READ]) return;
		auto args = make_args(UTP_ON_READ, s);
		args.buf = data; args.len = len;
		handlers_[UTP_ON_READ](&args);
	}
	void on_state_change(UtpSocket* s, int state) override {
		if (!handlers_[UTP_ON_STATE_CHANGE]) return;
		auto args = make_args(UTP_ON_STATE_CHANGE, s);
		args.state = state;
		handlers_[UTP_ON_STATE_CHANGE](&args);
	}
	void on_delay_sample(UtpSocket* s, int sample_ms) override {
		if (!handlers_[UTP_ON_DELAY_SAMPLE]) return;
		auto args = make_args(UTP_ON_DELAY_SAMPLE, s);
		args.sample_ms = sample_ms;
		handlers_[UTP_ON_DELAY_SAMPLE](&args);
	}
	void on_overhead_statistics(UtpSocket* s, bool send, size_t len, int type) override {
		if (!handlers_[UTP_ON_OVERHEAD_STATISTICS]) return;
		auto args = make_args(UTP_ON_OVERHEAD_STATISTICS, s);
		args.send = send ? 1 : 0; args.len = len; args.type = type;
		handlers_[UTP_ON_OVERHEAD_STATISTICS](&args);
	}
	void log(UtpSocket* s, const char* message) override {
		if (!handlers_[UTP_LOG]) return;
		auto args = make_args(UTP_LOG, s);
		args.buf = reinterpret_cast<const uint8_t*>(message);
		handlers_[UTP_LOG](&args);
	}
};

extern "C" {

// 回调类型名称字符串数组，用于调试和日志输出
const char * utp_callback_names[] = {
	"UTP_ON_FIREWALL",
	"UTP_ON_ACCEPT",
	"UTP_ON_CONNECT",
	"UTP_ON_ERROR",
	"UTP_ON_READ",
	"UTP_ON_OVERHEAD_STATISTICS",
	"UTP_ON_STATE_CHANGE",
	"UTP_GET_READ_BUFFER_SIZE",
	"UTP_ON_DELAY_SAMPLE",
	"UTP_GET_UDP_MTU",
	"UTP_GET_UDP_OVERHEAD",
	"UTP_GET_MILLISECONDS",
	"UTP_GET_MICROSECONDS",
	"UTP_GET_RANDOM",
	"UTP_LOG",
	"UTP_SENDTO",
};

// 错误代码名称字符串数组
const char * utp_error_code_names[] = {
	"UTP_ECONNREFUSED",
	"UTP_ECONNRESET",
	"UTP_ETIMEDOUT",
};

// 状态名称字符串数组
const char *utp_state_names[] = {
	NULL,
	"UTP_STATE_CONNECT",
	"UTP_STATE_WRITABLE",
	"UTP_STATE_EOF",
	"UTP_STATE_DESTROYING",
};

// utp_context 构造函数，初始化上下文
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

// utp_context 析构函数，清理资源
UtpContext::~UtpContext() {
	std::vector<UtpSocket*> socks;
	socks.reserve(sockets_.size());
	for (auto& [key, socket] : sockets_) {
		socks.push_back(socket);
	}
	for (auto* s : socks) {
		delete s;
	}
}

// 初始化 uTP 上下文
// 参数: version - 版本号，必须是 2
// 返回: 新创建的上下文指针，版本不匹配时返回 NULL
utp_context* utp_init (int version)
{
	assert(version == 2);
	if (version != 2)
		return NULL;
	utp_context *ctx = new utp_context;
	return ctx;
}

// 销毁 uTP 上下文及其所有资源
// 参数: ctx - 上下文指针
void utp_destroy(utp_context *ctx) {
	assert(ctx);
	if (ctx) delete ctx;
}

// 设置回调函数
void utp_set_callback(utp_context *ctx, int callback_name, utp_callback_t *proc) {
	assert(ctx);
	if (!ctx) return;
	// 首次调用时，将默认 UtpCallbacks 替换为 C 函数指针适配器
	auto* adapter = dynamic_cast<CFunctionCallbackAdapter*>(ctx->callbacks_.get());
	if (!adapter) {
		auto new_adapter = std::make_unique<CFunctionCallbackAdapter>();
		adapter = new_adapter.get();
		ctx->callbacks_ = std::move(new_adapter);
	}
	adapter->set_handler(callback_name, proc);
}

// 设置用户自定义数据
// 参数: ctx - 上下文指针
//       userdata - 用户数据指针
// 返回: 之前的用户数据指针
void* utp_context_set_userdata(utp_context *ctx, void *userdata) {
	assert(ctx);
	if (ctx) ctx->userdata_ = userdata;
	return ctx ? ctx->userdata_ : NULL;
}

// 获取用户自定义数据
// 参数: ctx - 上下文指针
// 返回: 用户数据指针
void* utp_context_get_userdata(utp_context *ctx) {
	assert(ctx);
	return ctx ? ctx->userdata_ : NULL;
}

// 获取上下文统计信息
// 参数: ctx - 上下文指针
// 返回: 统计信息结构体指针
utp_context_stats* utp_get_context_stats(utp_context *ctx) {
	assert(ctx);
	return ctx ? &ctx->context_stats_ : NULL;
}

// 写入数据到 uTP socket
// 参数: socket - socket 指针
//       buf - 数据缓冲区
//       len - 数据长度
// 返回: 实际写入的字节数
ssize_t utp_write(utp_socket *socket, void *buf, size_t len) {
	struct utp_iovec iovec = { buf, len };
	return utp_writev(socket, &iovec, 1);
}

}

// =============================================================================

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
	conn->cc_.init_timing(conn->ctx->current_ms_);
	conn->cc_.init_delay_histories(conn->ctx->current_ms_);

	conn->mtu_.reset((uint32)conn->get_udp_mtu(), conn->ctx->current_ms_);
	conn->mtu_.set_last_to_ceiling();

	conn->ctx->sockets_[UtpSocketKey(conn->addr, conn->conn_id_recv_)] = conn;

	conn->cc_.set_max_window(conn->get_packet_size());

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP socket initialized");
	#endif
}

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

	conn->log(UTP_LOG_NORMAL, "UTP_Connect conn_seed_:%u packet_size:%u (B) "
			"target_delay_:%u (ms) delay_history:%u "
			"delay_base_history:%u (minutes)",
			conn->conn_seed_, PACKET_SIZE, conn->target_delay_ / 1000,
			CUR_DELAY_SIZE, DELAY_BASE_HISTORY);

	conn->cc_.set_retransmit_timeout(3000);
	conn->cc_.set_rto_timeout(conn->ctx->current_ms_ + conn->cc_.retransmit_timeout());
	conn->last_rcv_win_ = conn->get_rcv_window();

	conn->seq_nr_ = utp_call_get_random(conn->ctx, conn);

	const size_t header_size = sizeof(PacketFormatV1);

	OutgoingPacket *pkt = new OutgoingPacket();
	pkt->data.resize(header_size);
	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();

	memset(p1, 0, header_size);
	p1->set_version(1);
	p1->set_type(ST_SYN);
	p1->ext = 0;
	p1->connid = conn->conn_id_recv_;
	p1->windowsize = (uint32)conn->last_rcv_win_;
	p1->seq_nr = conn->seq_nr_;
	pkt->transmissions = 0;
	pkt->length = header_size;
	pkt->payload = 0;

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

	size_t packet_size = conn->get_packet_size();
	size_t num_to_send = min<size_t>(bytes, packet_size);
	while (!conn->is_full(num_to_send)) {
		bytes -= num_to_send;
		sent  += num_to_send;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Sending packet. seq_nr_:%u ack_nr_:%u wnd:%u/%u/%u rcv_win:%u size:%u cur_window_packets_:%u",
			conn->seq_nr_, conn->ack_nr_,
			(uint)(conn->cc_.cur_window() + num_to_send),
			(uint)conn->cc_.max_window(), (uint)conn->max_window_user_,
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
		conn->state_ = CS_CONNECTED_FULL;
	}

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "UTP_Write %u bytes = %s", (uint)bytes, full ? "false" : "true");
	#endif

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
		if (conn->last_rcv_win_ == 0) {
			conn->send_ack();
		} else {
			conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);
			conn->schedule_ack();
		}
	}
}

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

void utp_check_timeouts(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return;
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

	if (ours)   *ours   = conn->cc_.our_hist().get_value();
	if (theirs) *theirs = conn->cc_.their_hist().get_value();
	if (age)    *age    = (uint32)(conn->ctx->current_ms_ - conn->last_measured_delay_);
	return 0;
}

void utp_close(UtpSocket *conn)
{
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
		conn->read_shutdown_ = true;
		conn->close_requested_ = true;
		if (!conn->fin_sent) {
			conn->fin_sent = true;
			conn->write_outgoing_packet(0, ST_FIN, NULL, 0);
		} else if (conn->fin_sent_acked_) {
			conn->state_ = CS_DESTROY;
		}
		break;

	case CS_SYN_SENT:
		conn->cc_.set_rto_timeout(utp_call_get_milliseconds(conn->ctx, conn) + min<uint>(conn->cc_.rto_ms() * 2, 60));
	case CS_SYN_RECV:
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
			conn->cc_.set_rto_timeout(utp_call_get_milliseconds(conn->ctx, conn) + min<uint>(conn->cc_.rto_ms() * 2, 60));
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
		socket->stats_.mtu_guess = socket->mtu_.raw_mtu();
		return &socket->stats_;
	#else
		return NULL;
	#endif
}
