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

// 回调函数调用实现
// 使用 C 函数指针机制（非 std::function），为用户提供事件通知接口
// 每个回调函数在特定事件发生时被触发

#include "utp_callbacks.h"

// 触发时机：收到新的入站连接时，在接受连接之前调用
// 用途：防火墙检查，允许用户拒绝某些连接
int utp_call_on_firewall(utp_context *ctx, const struct sockaddr *address, socklen_t address_len)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_FIREWALL]) return 0;
	args.callback_type = UTP_ON_FIREWALL;
	args.context = ctx;
	args.socket = NULL;
	args.address = address;
	args.address_len = address_len;
	return (int)ctx->callbacks_[UTP_ON_FIREWALL](&args);
}

// 触发时机：新的入站连接被接受后调用
// 用途：通知用户新连接已建立
void utp_call_on_accept(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_ACCEPT]) return;
	args.callback_type = UTP_ON_ACCEPT;
	args.context = ctx;
	args.socket = socket;
	args.address = address;
	args.address_len = address_len;
	ctx->callbacks_[UTP_ON_ACCEPT](&args);
}

// 触发时机：出站连接成功建立时调用
// 用途：通知用户连接已完成
void utp_call_on_connect(utp_context *ctx, utp_socket *socket)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_CONNECT]) return;
	args.callback_type = UTP_ON_CONNECT;
	args.context = ctx;
	args.socket = socket;
	ctx->callbacks_[UTP_ON_CONNECT](&args);
}

// 触发时机：发生错误时调用
// 用途：通知用户 socket 错误（如连接拒绝、超时等）
void utp_call_on_error(utp_context *ctx, utp_socket *socket, int error_code)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_ERROR]) return;
	args.callback_type = UTP_ON_ERROR;
	args.context = ctx;
	args.socket = socket;
	args.error_code = error_code;
	ctx->callbacks_[UTP_ON_ERROR](&args);
}

// 触发时机：接收到数据时调用
// 用途：通知用户有数据可读
void utp_call_on_read(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_READ]) return;
	args.callback_type = UTP_ON_READ;
	args.context = ctx;
	args.socket = socket;
	args.buf = buf;
	args.len = len;
	ctx->callbacks_[UTP_ON_READ](&args);
}

// 触发时机：协议开销统计信息更新时调用
// 用途：报告 UDP 开销（如头部、重传等）
void utp_call_on_overhead_statistics(utp_context *ctx, utp_socket *socket, int send, size_t len, int type)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_OVERHEAD_STATISTICS]) return;
	args.callback_type = UTP_ON_OVERHEAD_STATISTICS;
	args.context = ctx;
	args.socket = socket;
	args.send = send;
	args.len = len;
	args.type = type;
	ctx->callbacks_[UTP_ON_OVERHEAD_STATISTICS](&args);
}

// 触发时机：延迟采样完成时调用
// 用途：提供延迟样本用于拥塞控制
void utp_call_on_delay_sample(utp_context *ctx, utp_socket *socket, int sample_ms)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_DELAY_SAMPLE]) return;
	args.callback_type = UTP_ON_DELAY_SAMPLE;
	args.context = ctx;
	args.socket = socket;
	args.sample_ms = sample_ms;
	ctx->callbacks_[UTP_ON_DELAY_SAMPLE](&args);
}

// 触发时机：socket 状态改变时调用
// 用途：通知用户状态变化（如可写、EOF、销毁中）
void utp_call_on_state_change(utp_context *ctx, utp_socket *socket, int state)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_ON_STATE_CHANGE]) return;
	args.callback_type = UTP_ON_STATE_CHANGE;
	args.context = ctx;
	args.socket = socket;
	args.state = state;
	ctx->callbacks_[UTP_ON_STATE_CHANGE](&args);
}

// 触发时机：需要获取 UDP MTU 时调用
// 用途：查询路径 MTU，用于分片控制
uint16 utp_call_get_udp_mtu(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_UDP_MTU]) return 0;
	args.callback_type = UTP_GET_UDP_MTU;
	args.context = ctx;
	args.socket = socket;
	args.address = address;
	args.address_len = address_len;
	return (uint16)ctx->callbacks_[UTP_GET_UDP_MTU](&args);
}

// 触发时机：需要获取 UDP 协议开销时调用
// 用途：计算带宽占用时需要考虑的头部开销
uint16 utp_call_get_udp_overhead(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_UDP_OVERHEAD]) return 0;
	args.callback_type = UTP_GET_UDP_OVERHEAD;
	args.context = ctx;
	args.socket = socket;
	args.address = address;
	args.address_len = address_len;
	return (uint16)ctx->callbacks_[UTP_GET_UDP_OVERHEAD](&args);
}

// 触发时机：需要获取当前时间（毫秒）时调用
// 用途：超时计算、RTT 测量等时间相关操作
uint64 utp_call_get_milliseconds(utp_context *ctx, utp_socket *socket)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_MILLISECONDS]) return 0;
	args.callback_type = UTP_GET_MILLISECONDS;
	args.context = ctx;
	args.socket = socket;
	return ctx->callbacks_[UTP_GET_MILLISECONDS](&args);
}

// 触发时机：需要获取当前时间（微秒）时调用
// 用途：高精度时间戳、延迟测量
uint64 utp_call_get_microseconds(utp_context *ctx, utp_socket *socket)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_MICROSECONDS]) return 0;
	args.callback_type = UTP_GET_MICROSECONDS;
	args.context = ctx;
	args.socket = socket;
	return ctx->callbacks_[UTP_GET_MICROSECONDS](&args);
}

// 触发时机：需要随机数时调用
// 用途：生成随机连接 ID 等
uint32 utp_call_get_random(utp_context *ctx, utp_socket *socket)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_RANDOM]) return 0;
	args.callback_type = UTP_GET_RANDOM;
	args.context = ctx;
	args.socket = socket;
	return (uint32)ctx->callbacks_[UTP_GET_RANDOM](&args);
}

// 触发时机：需要读取缓冲区大小时调用
// 用途：获取可用读取缓冲区大小
size_t utp_call_get_read_buffer_size(utp_context *ctx, utp_socket *socket)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_GET_READ_BUFFER_SIZE]) return 0;
	args.callback_type = UTP_GET_READ_BUFFER_SIZE;
	args.context = ctx;
	args.socket = socket;
	return (size_t)ctx->callbacks_[UTP_GET_READ_BUFFER_SIZE](&args);
}

// 触发时机：需要输出日志时调用
// 用途：调试日志输出
void utp_call_log(utp_context *ctx, utp_socket *socket, const byte *buf)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_LOG]) return;
	args.callback_type = UTP_LOG;
	args.context = ctx;
	args.socket = socket;
	args.buf = buf;
	ctx->callbacks_[UTP_LOG](&args);
}

// 触发时机：需要发送 UDP 数据包时调用
// 用途：实际执行 UDP sendto 操作
void utp_call_sendto(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len, const struct sockaddr *address, socklen_t address_len, uint32 flags)
{
	utp_callback_arguments args;
	if (!ctx->callbacks_[UTP_SENDTO]) return;
	args.callback_type = UTP_SENDTO;
	args.context = ctx;
	args.socket = socket;
	args.buf = buf;
	args.len = len;
	args.address = address;
	args.address_len = address_len;
	args.flags = flags;
	ctx->callbacks_[UTP_SENDTO](&args);
}

