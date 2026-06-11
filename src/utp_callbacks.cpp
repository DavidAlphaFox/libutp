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

// 回调函数转发层实现
// 每个函数将调用转发到 ctx->callbacks_ 虚基类对应的方法
// 这是内部实现层，将 C 风格调用桥接到 C++ 虚函数接口

#include "utp_callbacks.h"
#include "utp_internal.h"

// 转发到 on_firewall 回调
int utp_call_on_firewall(utp_context *ctx, const struct sockaddr *address, socklen_t address_len) {
	UtpContext *c = ctx;
	return c->callbacks()->on_firewall(address, address_len);
}

// 转发到 on_accept 回调
void utp_call_on_accept(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	UtpContext *c = ctx;
	c->callbacks()->on_accept(socket, address, address_len);
}

// 转发到 on_connect 回调
void utp_call_on_connect(utp_context *ctx, utp_socket *socket) {
	UtpContext *c = ctx;
	c->callbacks()->on_connect(socket);
}

// 转发到 on_error 回调
void utp_call_on_error(utp_context *ctx, utp_socket *socket, int error_code) {
	UtpContext *c = ctx;
	c->callbacks()->on_error(socket, error_code);
}

// 转发到 on_read 回调
void utp_call_on_read(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len) {
	UtpContext *c = ctx;
	c->callbacks()->on_read(socket, buf, len);
}

// 转发到 on_overhead_statistics 回调（将 int send 转为 bool）
void utp_call_on_overhead_statistics(utp_context *ctx, utp_socket *socket, int send, size_t len, int type) {
	UtpContext *c = ctx;
	c->callbacks()->on_overhead_statistics(socket, send != 0, len, type);
}

// 转发到 on_delay_sample 回调
void utp_call_on_delay_sample(utp_context *ctx, utp_socket *socket, int sample_ms) {
	UtpContext *c = ctx;
	c->callbacks()->on_delay_sample(socket, sample_ms);
}

// 转发到 on_state_change 回调
void utp_call_on_state_change(utp_context *ctx, utp_socket *socket, int state) {
	UtpContext *c = ctx;
	c->callbacks()->on_state_change(socket, state);
}

// 转发到 get_udp_mtu 查询
uint16 utp_call_get_udp_mtu(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	UtpContext *c = ctx;
	return c->callbacks()->get_udp_mtu(socket, address, address_len);
}

// 转发到 get_udp_overhead 查询
uint16 utp_call_get_udp_overhead(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	UtpContext *c = ctx;
	return c->callbacks()->get_udp_overhead(socket, address, address_len);
}

// 转发到 get_milliseconds 时钟查询
uint64 utp_call_get_milliseconds(utp_context *ctx, utp_socket *socket) {
	UtpContext *c = ctx;
	return c->callbacks()->get_milliseconds(socket);
}

// 转发到 get_microseconds 时钟查询
uint64 utp_call_get_microseconds(utp_context *ctx, utp_socket *socket) {
	UtpContext *c = ctx;
	return c->callbacks()->get_microseconds(socket);
}

// 转发到 get_random 随机数查询
uint32 utp_call_get_random(utp_context *ctx, utp_socket *socket) {
	UtpContext *c = ctx;
	return c->callbacks()->get_random(socket);
}

// 转发到 get_read_buffer_size 查询
size_t utp_call_get_read_buffer_size(utp_context *ctx, utp_socket *socket) {
	UtpContext *c = ctx;
	return c->callbacks()->get_read_buffer_size(socket);
}

// 转发到 log 日志回调（将 byte* 转为 char*）
void utp_call_log(utp_context *ctx, utp_socket *socket, const byte *buf) {
	UtpContext *c = ctx;
	c->callbacks()->log(socket, reinterpret_cast<const char*>(buf));
}

// 转发到 sendto 发送回调
void utp_call_sendto(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len, const struct sockaddr *address, socklen_t address_len, uint32 flags) {
	UtpContext *c = ctx;
	c->callbacks()->sendto(socket, buf, len, address, address_len, flags);
}
