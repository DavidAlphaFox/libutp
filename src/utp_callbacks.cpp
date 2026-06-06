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

#include "utp_callbacks.h"

int utp_call_on_firewall(utp_context *ctx, const struct sockaddr *address, socklen_t address_len) {
	return ctx->callbacks_->on_firewall(address, address_len);
}

void utp_call_on_accept(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	ctx->callbacks_->on_accept(socket, address, address_len);
}

void utp_call_on_connect(utp_context *ctx, utp_socket *socket) {
	ctx->callbacks_->on_connect(socket);
}

void utp_call_on_error(utp_context *ctx, utp_socket *socket, int error_code) {
	ctx->callbacks_->on_error(socket, error_code);
}

void utp_call_on_read(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len) {
	ctx->callbacks_->on_read(socket, buf, len);
}

void utp_call_on_overhead_statistics(utp_context *ctx, utp_socket *socket, int send, size_t len, int type) {
	ctx->callbacks_->on_overhead_statistics(socket, send != 0, len, type);
}

void utp_call_on_delay_sample(utp_context *ctx, utp_socket *socket, int sample_ms) {
	ctx->callbacks_->on_delay_sample(socket, sample_ms);
}

void utp_call_on_state_change(utp_context *ctx, utp_socket *socket, int state) {
	ctx->callbacks_->on_state_change(socket, state);
}

uint16 utp_call_get_udp_mtu(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	return ctx->callbacks_->get_udp_mtu(socket, address, address_len);
}

uint16 utp_call_get_udp_overhead(utp_context *ctx, utp_socket *socket, const struct sockaddr *address, socklen_t address_len) {
	return ctx->callbacks_->get_udp_overhead(socket, address, address_len);
}

uint64 utp_call_get_milliseconds(utp_context *ctx, utp_socket *socket) {
	return ctx->callbacks_->get_milliseconds(socket);
}

uint64 utp_call_get_microseconds(utp_context *ctx, utp_socket *socket) {
	return ctx->callbacks_->get_microseconds(socket);
}

uint32 utp_call_get_random(utp_context *ctx, utp_socket *socket) {
	return ctx->callbacks_->get_random(socket);
}

size_t utp_call_get_read_buffer_size(utp_context *ctx, utp_socket *socket) {
	return ctx->callbacks_->get_read_buffer_size(socket);
}

void utp_call_log(utp_context *ctx, utp_socket *socket, const byte *buf) {
	ctx->callbacks_->log(socket, reinterpret_cast<const char*>(buf));
}

void utp_call_sendto(utp_context *ctx, utp_socket *socket, const byte *buf, size_t len, const struct sockaddr *address, socklen_t address_len, uint32 flags) {
	ctx->callbacks_->sendto(socket, buf, len, address, address_len, flags);
}
