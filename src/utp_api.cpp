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
#include "utp_internal.h"

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
	target_delay_ = CCONTROL_TARGET;
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
