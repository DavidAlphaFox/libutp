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
#include "utp_utils.h"

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
	, current_ms_(0)
	, last_utp_socket_(NULL)
	, log_normal_(false)
	, log_mtu_(false)
	, log_debug_(false)
{
	memset(&context_stats_, 0, sizeof(context_stats_));
	memset(callbacks_, 0, sizeof(callbacks_));
	target_delay_ = CCONTROL_TARGET;
	utp_sockets_ = new UtpSocketTable;

	// 设置默认回调函数
	callbacks_[UTP_GET_UDP_MTU]      = &utp_default_get_udp_mtu;
	callbacks_[UTP_GET_UDP_OVERHEAD] = &utp_default_get_udp_overhead;
	callbacks_[UTP_GET_MILLISECONDS] = &utp_default_get_milliseconds;
	callbacks_[UTP_GET_MICROSECONDS] = &utp_default_get_microseconds;
	callbacks_[UTP_GET_RANDOM]       = &utp_default_get_random;

	// 1 MB 接收缓冲区（即最大带宽延迟积）
	// 对于 RTT 为 200ms 的对端，无法以超过 5 MB/s 的速度接收
	// 对于 RTT 为 10ms 的对端，无法以超过 100 MB/s 的速度接收
	// 这假设是足够的，因为带宽通常与 RTT 成正比
	// 在设置下载速率限制时，所有 socket 的接收缓冲区应设置得更低，比如 60 kiB 左右
	opt_rcvbuf_ = opt_sndbuf_ = 1024 * 1024;
	last_check_ = 0;
}

// utp_context 析构函数，清理资源
UtpContext::~UtpContext() {
	delete this->utp_sockets_;
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
// 参数: ctx - 上下文指针
//       callback_name - 回调类型（如 UTP_ON_FIREWALL）
//       proc - 回调函数指针
void utp_set_callback(utp_context *ctx, int callback_name, utp_callback_t *proc) {
	assert(ctx);
	if (ctx) ctx->callbacks_[callback_name] = proc;
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
