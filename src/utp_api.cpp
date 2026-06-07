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
	// 函数指针数组，按 UTP_* 回调 ID 索引。
	// 值为 nullptr 表示该回调未通过 utp_set_callback() 注册，应回退到 UtpCallbacks 虚基类默认实现。
	utp_callback_t* handlers_[UTP_ARRAY_SIZE] = {};

	// 构造一个 utp_callback_arguments 结构体并预填 callback_type 和 socket 字段。
	// 各 override 方法再按需填入其余的 args 字段。
	// 参数 type - 回调类型，对应 UTP_ON_FIREWALL/UTP_GET_MILLISECONDS 等枚举值
	// 参数 sock - 关联的 utp_socket 指针，若回调不涉及具体 socket 可传 nullptr
	// 返回: 已填充 callback_type 和 socket 的参数结构体
	static utp_callback_arguments make_args(int type, utp_socket* sock) {
		utp_callback_arguments args{};
		args.callback_type = type;
		args.socket = sock;
		return args;
	}

public:
	// 注册（或覆盖）指定 ID 的 C 风格回调函数。
	// 由 utp_set_callback() 内部调用，外部用户通常无需直接调用。
	// 参数 id - 回调 ID（UTP_SENDTO、UTP_GET_MILLISECONDS 等）
	// 参数 fn - 用户提供的 C 风格回调函数指针，传 nullptr 表示清除
	void set_handler(int id, utp_callback_t* fn) { handlers_[id] = fn; }

	// ── ACTION ──
	// ACTION 类型回调：将数据包通过底层 UDP socket 发送出去
	// 参数 data - 待发送的数据（含 uTP 协议头）
	// 参数 len - 数据长度
	// 参数 address - 目标对端地址
	// 参数 address_len - 目标地址长度
	// 参数 flags - 平台特定的发送标志位
	void sendto(UtpSocket*, const uint8_t* data, size_t len,
	            const sockaddr* address, socklen_t address_len, uint32_t flags) override {
		if (!handlers_[UTP_SENDTO]) return;
		auto args = make_args(UTP_SENDTO, nullptr);
		args.buf = data; args.len = len;
		args.address = address; args.address_len = address_len; args.flags = flags;
		handlers_[UTP_SENDTO](&args);
	}

	// ── QUERY ──
	// QUERY 类型回调：返回当前单调递增的毫秒时间戳。
	// 如果用户未提供 UTP_GET_MILLISECONDS 回调，则回退到基类默认实现（平台相关的单调时钟）。
	uint64 get_milliseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MILLISECONDS]) return UtpCallbacks::get_milliseconds(s);
		auto args = make_args(UTP_GET_MILLISECONDS, s);
		return handlers_[UTP_GET_MILLISECONDS](&args);
	}
	// QUERY 类型回调：返回当前单调递增的微秒时间戳。
	// 精度高于 get_milliseconds，用于 LEDBAT 拥塞控制等需要更细粒度采样的场景。
	uint64 get_microseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MICROSECONDS]) return UtpCallbacks::get_microseconds(s);
		auto args = make_args(UTP_GET_MICROSECONDS, s);
		return handlers_[UTP_GET_MICROSECONDS](&args);
	}
	// QUERY 类型回调：返回到达指定对端地址的 UDP 路径 MTU。
	// 用于 uTP 动态 MTU 探测时的上限参考值。
	// 参数 a - 对端地址
	// 参数 l - 对端地址长度
	// 返回: 路径 MTU（字节）
	uint16 get_udp_mtu(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_MTU]) return UtpCallbacks::get_udp_mtu(s, a, l);
		auto args = make_args(UTP_GET_UDP_MTU, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_MTU](&args);
	}
	// QUERY 类型回调：返回 UDP 层（IP+UDP 头）的固定开销字节数。
	// 用于从路径 MTU 推导出 uTP 实际可用载荷大小。
	uint16 get_udp_overhead(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_OVERHEAD]) return UtpCallbacks::get_udp_overhead(s, a, l);
		auto args = make_args(UTP_GET_UDP_OVERHEAD, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_OVERHEAD](&args);
	}
	// QUERY 类型回调：返回 32 位随机数。用于生成连接种子和初始序列号。
	uint32 get_random(UtpSocket* s) override {
		if (!handlers_[UTP_GET_RANDOM]) return UtpCallbacks::get_random(s);
		auto args = make_args(UTP_GET_RANDOM, s);
		return (uint32)handlers_[UTP_GET_RANDOM](&args);
	}
	// QUERY 类型回调：返回用户已读取但尚未提交的数据量。
	// 用于在拥塞控制中正确估算接收窗口大小。
	size_t get_read_buffer_size(UtpSocket* s) override {
		if (!handlers_[UTP_GET_READ_BUFFER_SIZE]) return UtpCallbacks::get_read_buffer_size(s);
		auto args = make_args(UTP_GET_READ_BUFFER_SIZE, s);
		return (size_t)handlers_[UTP_GET_READ_BUFFER_SIZE](&args);
	}

	// ── EVENT ──
	// EVENT 类型回调：判断指定对端地址是否在防火墙后。
	// 返回: 非 0 表示无法到达（应丢弃该地址的入站数据包）。
	int on_firewall(const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_FIREWALL]) return UtpCallbacks::on_firewall(a, l);
		auto args = make_args(UTP_ON_FIREWALL, nullptr);
		args.address = a; args.address_len = l;
		return (int)handlers_[UTP_ON_FIREWALL](&args);
	}
	// EVENT 类型回调：通知用户有新的入站连接已被接受。
	// 参数 s - 服务端新创建的 socket
	// 参数 a - 对端地址
	// 参数 l - 对端地址长度
	void on_accept(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_ACCEPT]) return;
		auto args = make_args(UTP_ON_ACCEPT, s);
		args.address = a; args.address_len = l;
		handlers_[UTP_ON_ACCEPT](&args);
	}
	// EVENT 类型回调：通知用户主动连接（SYN）已建立。
	// 参数 s - 已连接的 socket
	void on_connect(UtpSocket* s) override {
		if (!handlers_[UTP_ON_CONNECT]) return;
		auto args = make_args(UTP_ON_CONNECT, s);
		handlers_[UTP_ON_CONNECT](&args);
	}
	// EVENT 类型回调：通知用户 socket 发生错误。
	// 参数 s - 出错的 socket
	// 参数 error_code - 错误码（UTP_ECONNREFUSED/UTP_ECONNRESET/UTP_ETIMEDOUT）
	void on_error(UtpSocket* s, int error_code) override {
		if (!handlers_[UTP_ON_ERROR]) return;
		auto args = make_args(UTP_ON_ERROR, s);
		args.error_code = error_code;
		handlers_[UTP_ON_ERROR](&args);
	}
	// EVENT 类型回调：通知用户有已就绪的应用层数据可读取。
	// 参数 s - 有数据可读的 socket
	// 参数 data - 数据缓冲区
	// 参数 len - 数据长度
	void on_read(UtpSocket* s, const uint8_t* data, size_t len) override {
		if (!handlers_[UTP_ON_READ]) return;
		auto args = make_args(UTP_ON_READ, s);
		args.buf = data; args.len = len;
		handlers_[UTP_ON_READ](&args);
	}
	// EVENT 类型回调：通知用户 socket 连接状态发生变化。
	// 参数 s - 状态发生变化的 socket
	// 参数 state - 新状态（UTP_STATE_CONNECT/UTP_STATE_WRITABLE/UTP_STATE_EOF/UTP_STATE_DESTROYING）
	void on_state_change(UtpSocket* s, int state) override {
		if (!handlers_[UTP_ON_STATE_CHANGE]) return;
		auto args = make_args(UTP_ON_STATE_CHANGE, s);
		args.state = state;
		handlers_[UTP_ON_STATE_CHANGE](&args);
	}
	// EVENT 类型回调：通知用户记录一次测量的端到端延迟样本（毫秒）。
	// 参数 s - 对应的 socket
	// 参数 sample_ms - 单次延迟采样值
	void on_delay_sample(UtpSocket* s, int sample_ms) override {
		if (!handlers_[UTP_ON_DELAY_SAMPLE]) return;
		auto args = make_args(UTP_ON_DELAY_SAMPLE, s);
		args.sample_ms = sample_ms;
		handlers_[UTP_ON_DELAY_SAMPLE](&args);
	}
	// EVENT 类型回调：通知用户底层带宽开销统计信息。
	// 参数 s - 对应的 socket
	// 参数 send - true 表示发送方向，false 表示接收方向
	// 参数 len - 该次事件涉及的字节数
	// 参数 type - 带宽类型（BandwidthType 枚举）
	void on_overhead_statistics(UtpSocket* s, bool send, size_t len, int type) override {
		if (!handlers_[UTP_ON_OVERHEAD_STATISTICS]) return;
		auto args = make_args(UTP_ON_OVERHEAD_STATISTICS, s);
		args.send = send ? 1 : 0; args.len = len; args.type = type;
		handlers_[UTP_ON_OVERHEAD_STATISTICS](&args);
	}
	// EVENT 类型回调：将内部日志消息转发给用户。
	// 参数 s - 关联的 socket（可能为 nullptr）
	// 参数 message - UTF-8/ASCII 格式的日志消息
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

// -----------------------------------------------------------------------------
// 函数：UtpContext::UtpContext
// 功能：构造函数，初始化 uTP 上下文。
//
// 参数：无
//
// 返回值：无
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
//
// 参数：无
//
// 返回值：无
// -----------------------------------------------------------------------------
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
	utp_context *ctx = new UtpContext;
	return ctx;
}

// 销毁 uTP 上下文及其所有资源
// 参数: ctx - 上下文指针
void utp_destroy(utp_context *ctx) {
	assert(ctx);
	if (ctx) delete static_cast<UtpContext*>(ctx);
}

// 设置回调函数
void utp_set_callback(utp_context *ctx, int callback_name, utp_callback_t *proc) {
	assert(ctx);
	if (!ctx) return;
	UtpContext *c = static_cast<UtpContext*>(ctx);
	// 首次调用时，将默认 UtpCallbacks 替换为 C 函数指针适配器
	auto* adapter = dynamic_cast<CFunctionCallbackAdapter*>(c->callbacks_.get());
	if (!adapter) {
		auto new_adapter = std::make_unique<CFunctionCallbackAdapter>();
		adapter = new_adapter.get();
		c->callbacks_ = std::move(new_adapter);
	}
	adapter->set_handler(callback_name, proc);
}

// 设置用户自定义数据
// 参数: ctx - 上下文指针
//       userdata - 用户数据指针
// 返回: 之前的用户数据指针
void* utp_context_set_userdata(utp_context *ctx, void *userdata) {
	assert(ctx);
	if (!ctx) return NULL;
	UtpContext *c = static_cast<UtpContext*>(ctx);
	c->userdata_ = userdata;
	return c->userdata_;
}

// 获取用户自定义数据
// 参数: ctx - 上下文指针
// 返回: 用户数据指针
void* utp_context_get_userdata(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return NULL;
	UtpContext *c = static_cast<UtpContext*>(ctx);
	return c->userdata_;
}

// 获取上下文统计信息
// 参数: ctx - 上下文指针
// 返回: 统计信息结构体指针
utp_context_stats* utp_get_context_stats(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return NULL;
	UtpContext *c = static_cast<UtpContext*>(ctx);
	return &c->context_stats_;
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

// 初始化一个 uTP socket 的内部状态（状态机、连接 ID、计时器、拥塞控制、MTU 等）。
// 这是所有 uTP socket 公共入口（utp_connect、utp_accept、utp_create_socket）的共同底层。
// 参数 conn - 待初始化的 socket 指针
// 参数 addr - 对端地址
// 参数 addrlen - 对端地址长度
// 参数 need_seed_gen - 是否需要重新生成 conn_seed_ 和 conn_id_*
//                     （主动 connect 时为 true，服务端接受时为 false）
// 参数 conn_seed_ - 连接种子；need_seed_gen 为 true 时被本函数覆写
// 参数 conn_id_recv_ - 接收方向连接 ID；need_seed_gen 为 true 时被本函数覆写
// 参数 conn_id_send_ - 发送方向连接 ID；need_seed_gen 为 true 时被本函数覆写
void utp_initialize_socket(	utp_socket *conn,
							const struct sockaddr *addr,
							socklen_t addrlen,
							bool need_seed_gen,
							uint32 conn_seed_,
							uint32 conn_id_recv_,
							uint32 conn_id_send_)
{
	UtpSocket *c = static_cast<UtpSocket*>(conn);
	utp::Address psaddr = utp::Address((const SOCKADDR_STORAGE*)addr, addrlen);

	if (need_seed_gen) {
		do {
			conn_seed_ = utp_call_get_random(c->ctx, c);
			conn_seed_ &= 0xffff;
		} while (c->ctx->sockets_.count(UtpSocketKey(psaddr, conn_seed_)));

		conn_id_recv_ += conn_seed_;
		conn_id_send_ += conn_seed_;
	}

	c->state_					= CS_IDLE;
	c->conn_seed_				= conn_seed_;
	c->conn_id_recv_			= conn_id_recv_;
	c->conn_id_send_			= conn_id_send_;
	c->addr						= psaddr;
	c->ctx->current_ms_			= utp_call_get_milliseconds(c->ctx, NULL);
	c->last_got_packet_			= c->ctx->current_ms_;
	c->last_sent_packet_			= c->ctx->current_ms_;
	c->last_measured_delay_		= c->ctx->current_ms_ + 0x70000000;
	c->cc_.init_timing(c->ctx->current_ms_);
	c->cc_.init_delay_histories(c->ctx->current_ms_);

	c->mtu_.reset((uint32)c->get_udp_mtu(), c->ctx->current_ms_);
	c->mtu_.set_last_to_ceiling();

	c->ctx->sockets_[UtpSocketKey(c->addr, c->conn_id_recv_)] = c;

	c->cc_.set_max_window(c->get_packet_size());

	#if UTP_DEBUG_LOGGING
	c->log(UTP_LOG_DEBUG, "UTP socket initialized");
	#endif
}

// 创建一个未初始化的 uTP socket。
// 创建后 socket 处于 CS_UNINITIALIZED 状态，必须通过 utp_connect 发起主动连接，
// 或由协议栈在接收到 SYN 时内部调用 utp_initialize_socket 转为 CS_SYN_RECV。
// 参数 ctx - 所属的 uTP 上下文
// 返回: 新创建的 socket 指针；ctx 为 NULL 时返回 NULL
utp_socket*	utp_create_socket(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return NULL;

	UtpContext *c = static_cast<UtpContext*>(ctx);
	UtpSocket *conn = new UtpSocket(c);
	conn->fast_resend_seq_nr_ = conn->seq_nr_;

	return conn;
}

// 设置 uTP 上下文级别的选项。
// 支持的选项：UTP_LOG_NORMAL/UTP_LOG_MTU/UTP_LOG_DEBUG/UTP_TARGET_DELAY/UTP_SNDBUF/UTP_RCVBUF。
// 参数 ctx - 上下文指针
// 参数 opt - 选项名（UTP_* 枚举值）
// 参数 val - 选项值（非 0/0 用于布尔选项，正整数用于缓冲区/延迟）
// 返回: 0 表示成功，-1 表示 ctx 为 NULL 或 opt 未知
int utp_context_set_option(utp_context *ctx, int opt, int val)
{
	assert(ctx);
	if (!ctx) return -1;
	UtpContext *c = static_cast<UtpContext*>(ctx);

	switch (opt) {
    	case UTP_LOG_NORMAL:
			c->log_normal_ = val ? true : false;
			return 0;

    	case UTP_LOG_MTU:
			c->log_mtu_ = val ? true : false;
			return 0;

    	case UTP_LOG_DEBUG:
			c->log_debug_ = val ? true : false;
			return 0;

    	case UTP_TARGET_DELAY:
			c->target_delay_ = val;
			return 0;

		case UTP_SNDBUF:
			assert(val >= 1);
			c->opt_sndbuf_ = val;
			return 0;

		case UTP_RCVBUF:
			assert(val >= 1);
			c->opt_rcvbuf_ = val;
			return 0;
	}
	return -1;
}

// 获取 uTP 上下文级别选项的当前值。
// 选项集与 utp_context_set_option 相同。
// 参数 ctx - 上下文指针
// 参数 opt - 选项名（UTP_* 枚举值）
// 返回: 选项当前值；ctx 为 NULL 或 opt 未知时返回 -1
int utp_context_get_option(utp_context *ctx, int opt)
{
	assert(ctx);
	if (!ctx) return -1;
	UtpContext *c = static_cast<UtpContext*>(ctx);

	switch (opt) {
    	case UTP_LOG_NORMAL:	return c->log_normal_ ? 1 : 0;
    	case UTP_LOG_MTU:		return c->log_mtu_    ? 1 : 0;
    	case UTP_LOG_DEBUG:		return c->log_debug_  ? 1 : 0;
    	case UTP_TARGET_DELAY:	return c->target_delay_;
		case UTP_SNDBUF:		return c->opt_sndbuf_;
		case UTP_RCVBUF:		return c->opt_rcvbuf_;
	}
	return -1;
}


// 设置单个 uTP socket 级别的选项。
// 支持的选项：UTP_SNDBUF/UTP_RCVBUF/UTP_TARGET_DELAY。
// 必须在 socket 处于 CS_CONNECTED 之前调用以避免与并发传输冲突。
// 参数 conn - socket 指针
// 参数 opt - 选项名
// 参数 val - 选项值
// 返回: 0 表示成功，-1 表示 conn 为 NULL 或 opt 未知
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

// 获取单个 uTP socket 级别选项的当前值。
// 参数 conn - socket 指针
// 参数 opt - 选项名（UTP_SNDBUF/UTP_RCVBUF/UTP_TARGET_DELAY）
// 返回: 选项当前值；conn 为 NULL 或 opt 未知时返回 -1
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

// 主动发起一次 uTP 连接（发送 SYN）。
// 调用后 socket 状态由 CS_UNINITIALIZED 转为 CS_SYN_SENT，
// 收到 SYN-ACK 后通过 UTP_STATE_CONNECT 事件回调通知用户。
// 参数 conn - 由 utp_create_socket() 创建的未连接 socket
// 参数 to - 对端地址
// 参数 tolen - 对端地址长度
// 返回: 0 表示成功发起；-1 表示 conn 为 NULL 或 socket 已处于非初始状态
int UtpSocket::connect(const struct sockaddr *to, socklen_t tolen)
{
	assert(state_ == CS_UNINITIALIZED);
	if (state_ != CS_UNINITIALIZED) {
		state_ = CS_DESTROY;
		return -1;
	}

	utp_initialize_socket(this, to, tolen, true, 0, 0, 1);

	assert(cur_window_packets_ == 0);
	assert(outbuf_.get(seq_nr_) == NULL);
	assert(sizeof(PacketFormatV1) == 20);

	state_ = CS_SYN_SENT;
	ctx->current_ms_ = utp_call_get_milliseconds(ctx, this);

	log(UTP_LOG_NORMAL, "UTP_Connect conn_seed_:%u packet_size:%u (B) "
			"target_delay_:%u (ms) delay_history:%u "
			"delay_base_history:%u (minutes)",
			conn_seed_, PACKET_SIZE, target_delay_ / 1000,
			CUR_DELAY_SIZE, DELAY_BASE_HISTORY);

	cc_.set_retransmit_timeout(3000);
	cc_.set_rto_timeout(ctx->current_ms_ + cc_.retransmit_timeout());
	last_rcv_win_ = get_rcv_window();

	seq_nr_ = utp_call_get_random(ctx, this);

	const size_t header_size = sizeof(PacketFormatV1);

	OutgoingPacket *pkt = new OutgoingPacket();
	pkt->data.resize(header_size);
	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();

	memset(p1, 0, header_size);
	p1->set_version(1);
	p1->set_type(ST_SYN);
	p1->ext = 0;
	p1->connid = conn_id_recv_;
	p1->windowsize = (uint32)last_rcv_win_;
	p1->seq_nr = seq_nr_;
	pkt->transmissions = 0;
	pkt->length = header_size;
	pkt->payload = 0;

	outbuf_.ensure_size(seq_nr_, cur_window_packets_);
	outbuf_.put(seq_nr_, pkt);
	seq_nr_++;
	cur_window_packets_++;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "incrementing cur_window_packets_:%u", cur_window_packets_);
	#endif

	send_packet(pkt);
	return 0;
}

int utp_connect(utp_socket *s, const struct sockaddr *to, socklen_t tolen) {
	return static_cast<UtpSocket*>(s)->connect(to, tolen);
}

// -----------------------------------------------------------------------------
// 函数：utp_writev
// 功能：使用 scatter/gather I/O 向 uTP 连接写入多段不连续数据。
//
// 参数：
//   conn         - 目标 socket 指针
//   iovec_input  - iovec 数组，每项包含缓冲区地址和长度
//   num_iovecs   - iovec 数组长度（上限 UTP_IOV_MAX）
//
// 返回值：
//   成功入队的总字节数；socket 未连接或已发送 FIN 时返回 0；参数错误返回 -1
// -----------------------------------------------------------------------------
ssize_t utp_writev(utp_socket *s, struct utp_iovec *iovec_input, size_t num_iovecs)
{
	static utp_iovec iovec[UTP_IOV_MAX];

	assert(s);
	if (!s) return -1;
	UtpSocket *conn = static_cast<UtpSocket*>(s);

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

	if (conn->send_.fin_sent) {
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

// -----------------------------------------------------------------------------
// 函数：UtpSocket::read_drained
// 功能：通知库应用已消费完数据，释放接收缓冲区。
//
// 参数：
//   conn - socket 指针
//
// 返回值：无
// -----------------------------------------------------------------------------
void UtpSocket::read_drained()
{
	assert(state_ != CS_UNINITIALIZED);
	if (state_ == CS_UNINITIALIZED) return;

	const size_t rcvwin = get_rcv_window();

	if (rcvwin > last_rcv_win_) {
		if (last_rcv_win_ == 0) {
			send_ack();
		} else {
			ctx->current_ms_ = utp_call_get_milliseconds(ctx, this);
			schedule_ack();
		}
	}
}

void utp_read_drained(utp_socket *s) {
	static_cast<UtpSocket*>(s)->read_drained();
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::issue_deferred_acks
// 功能：触发延迟 ACK 发送。
//
// 参数：
//   ctx - uTP 上下文指针
//
// 返回值：无
// -----------------------------------------------------------------------------
void UtpContext::issue_deferred_acks()
{
	for (size_t i = 0; i < ack_sockets_.size(); i++) {
		UtpSocket *conn = ack_sockets_[i];
		conn->send_ack();
		i--;
	}
}

void utp_issue_deferred_acks(utp_context *ctx) {
	static_cast<UtpContext*>(ctx)->issue_deferred_acks();
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::check_timeouts
// 功能：周期性驱动库的定时器。
//
// 参数：
//   ctx - uTP 上下文指针
//
// 返回值：无
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

void utp_check_timeouts(utp_context *ctx) {
	static_cast<UtpContext*>(ctx)->check_timeouts();
}

// -----------------------------------------------------------------------------
// 函数：utp_getpeername
// 功能：获取对端地址。
//
// 参数：
//   conn    - socket 指针
//   addr    - 输出缓冲区，用于存储对端地址
//   addrlen - 输入/输出参数，传入 addr 缓冲区大小，返回实际地址长度
//
// 返回值：
//   0 表示成功；-1 表示参数错误或 socket 未初始化
// -----------------------------------------------------------------------------
int utp_getpeername(utp_socket *conn, struct sockaddr *addr, socklen_t *addrlen)
{
	assert(addr);
	if (!addr) return -1;

	assert(addrlen);
	if (!addrlen) return -1;

	assert(conn);
	if (!conn) return -1;
	UtpSocket *c = static_cast<UtpSocket*>(conn);

	assert(c->state_ != CS_UNINITIALIZED);
	if (c->state_ == CS_UNINITIALIZED) return -1;

	socklen_t len;
	const SOCKADDR_STORAGE sa = c->addr.get_sockaddr_storage(&len);
	*addrlen = min(len, *addrlen);
	memcpy(addr, &sa, *addrlen);
	return 0;
}

// -----------------------------------------------------------------------------
// 函数：utp_get_delays
// 功能：获取延迟测量值。
//
// 参数：
//   conn   - socket 指针
//   ours   - 输出参数，本端到对端的延迟（毫秒），可为 NULL
//   theirs - 输出参数，对端到本端的延迟（毫秒），可为 NULL
//   age    - 输出参数，距离上次测量延迟的时间（毫秒），可为 NULL
//
// 返回值：
//   0 表示成功；-1 表示 socket 未初始化
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// 函数：UtpSocket::close
// 功能：关闭 socket。
//
// 参数：
//   conn - socket 指针
//
// 返回值：无
// -----------------------------------------------------------------------------
void UtpSocket::close()
{
	assert(state_ != CS_UNINITIALIZED
		&& state_ != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_Close in state:%s", statenames[state_]);
	#endif

	switch(state_) {
	case CS_CONNECTED:
	case CS_CONNECTED_FULL:
		recv_.read_shutdown = true;
		send_.close_requested_ = true;
		if (!send_.fin_sent) {
			send_.fin_sent = true;
			write_outgoing_packet(0, ST_FIN, NULL, 0);
		} else if (send_.fin_sent_acked_) {
			state_ = CS_DESTROY;
		}
		break;

	case CS_SYN_SENT:
		cc_.set_rto_timeout(utp_call_get_milliseconds(ctx, this) + min<uint>(cc_.rto_ms() * 2, 60));
	case CS_SYN_RECV:
	default:
		state_ = CS_DESTROY;
		break;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_Close end in state:%s", statenames[state_]);
	#endif
}

void utp_close(utp_socket *s) {
	static_cast<UtpSocket*>(s)->close();
}

// -----------------------------------------------------------------------------
// 函数：UtpSocket::shutdown
// 功能：关闭 socket 方向。
//
// 参数：
//   conn - socket 指针
//   how  - 关闭方向（SHUT_RD 关闭读、SHUT_WR 关闭写）
//
// 返回值：无
// -----------------------------------------------------------------------------
void UtpSocket::shutdown(int how)
{
	assert(state_ != CS_UNINITIALIZED
		&& state_ != CS_DESTROY);

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "UTP_shutdown(%d) in state:%s", how, statenames[state_]);
	#endif

	if (how != SHUT_WR) {
		recv_.read_shutdown = true;
	}
	if (how != SHUT_RD) {
		switch(state_) {
		case CS_CONNECTED:
		case CS_CONNECTED_FULL:
			if (!send_.fin_sent) {
				send_.fin_sent = true;
				write_outgoing_packet(0, ST_FIN, NULL, 0);
			}
			break;
		case CS_SYN_SENT:
			cc_.set_rto_timeout(utp_call_get_milliseconds(ctx, this) + min<uint>(cc_.rto_ms() * 2, 60));
		default:
			break;
		}
	}
}

void utp_shutdown(utp_socket *s, int how) {
	static_cast<UtpSocket*>(s)->shutdown(how);
}

// -----------------------------------------------------------------------------
// 函数：utp_get_context
// 功能：获取 socket 所属 context。
//
// 参数：
//   socket - socket 指针
//
// 返回值：
//   所属上下文指针；socket 为 NULL 时返回 NULL
// -----------------------------------------------------------------------------
utp_context* utp_get_context(utp_socket *socket) {
	assert(socket);
	if (!socket) return NULL;
	UtpSocket *s = static_cast<UtpSocket*>(socket);
	return s->ctx;
}

// -----------------------------------------------------------------------------
// 函数：utp_set_userdata
// 功能：设置用户数据。
//
// 参数：
//   socket     - socket 指针
//   userdata_  - 用户数据指针
//
// 返回值：
//   设置后的用户数据指针；socket 为 NULL 时返回 NULL
// -----------------------------------------------------------------------------
void* utp_set_userdata(utp_socket *socket, void *userdata_) {
	assert(socket);
	if (!socket) return NULL;
	UtpSocket *s = static_cast<UtpSocket*>(socket);
	s->userdata_ = userdata_;
	return s->userdata_;
}

// -----------------------------------------------------------------------------
// 函数：utp_get_userdata
// 功能：获取用户数据。
//
// 参数：
//   socket - socket 指针
//
// 返回值：
//   用户数据指针；socket 为 NULL 时返回 NULL
// -----------------------------------------------------------------------------
void* utp_get_userdata(utp_socket *socket) {
	assert(socket);
	if (!socket) return NULL;
	UtpSocket *s = static_cast<UtpSocket*>(socket);
	return s->userdata_;
}

// -----------------------------------------------------------------------------
// 函数：UtpContext::log
// 功能：日志记录。根据日志级别决定是否输出日志。
//
// 参数：
//   level  - 日志级别（UTP_LOG_NORMAL/UTP_LOG_MTU/UTP_LOG_DEBUG）
//   socket - 关联的 socket 指针（可能为 nullptr）
//   fmt    - printf 格式字符串
//   ...    - 可变参数
//
// 返回值：无
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// 函数：UtpContext::log_unchecked
// 功能：无条件日志记录。不检查日志级别，直接格式化并输出日志。
//
// 参数：
//   socket - 关联的 socket 指针（可能为 nullptr）
//   fmt    - printf 格式字符串
//   ...    - 可变参数
//
// 返回值：无
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// 函数：UtpContext::would_log
// 功能：检查日志级别。判断指定级别的日志是否应被输出。
//
// 参数：
//   level - 日志级别（UTP_LOG_NORMAL/UTP_LOG_MTU/UTP_LOG_DEBUG）
//
// 返回值：
//   true 表示该级别日志已启用；false 表示未启用
// -----------------------------------------------------------------------------
inline bool UtpContext::would_log(int level)
{
	if (level == UTP_LOG_NORMAL) return log_normal_;
	if (level == UTP_LOG_MTU) return log_mtu_;
	if (level == UTP_LOG_DEBUG) return log_debug_;
	return true;
}

// -----------------------------------------------------------------------------
// 函数：utp_get_stats
// 功能：获取 socket 统计信息。
//
// 参数：
//   socket - socket 指针
//
// 返回值：
//   统计信息结构体指针；非 _DEBUG 编译时返回 NULL
// -----------------------------------------------------------------------------
utp_socket_stats* utp_get_stats(utp_socket *s)
{
	#ifdef _DEBUG
		assert(s);
		if (!s) return NULL;
		UtpSocket *socket = static_cast<UtpSocket*>(s);
		socket->stats_.mtu_guess = socket->mtu_.raw_mtu();
		return &socket->stats_;
	#else
		return NULL;
	#endif
}

int utp_process_udp(utp_context *ctx, const byte *buf, size_t len, const struct sockaddr *to, socklen_t tolen) {
	return static_cast<UtpContext*>(ctx)->process_udp(buf, len, to, tolen);
}

int utp_process_icmp_fragmentation(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu) {
	return static_cast<UtpContext*>(ctx)->process_icmp_fragmentation(buffer, len, to, tolen, next_hop_mtu);
}

int utp_process_icmp_error(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen) {
	return static_cast<UtpContext*>(ctx)->process_icmp_error(buffer, len, to, tolen);
}
