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

#pragma once

#include <cstddef>
#include <memory>

#include "utp_types.h"

// UtpSocket 前向声明（完整定义在 utp_internal.cpp）
struct UtpSocket;

// UtpCallbacks: uTP 协议回调虚基类。
// 用户继承此类，重写所需方法。仅 sendto() 为纯虚函数（必须实现），
// 其余均有默认实现。回调对象由 UtpContext 持有生命周期。
//
// 所有回调在库的单线程事件循环中调用，无需同步。
class UtpCallbacks {
public:
	virtual ~UtpCallbacks() = default;

	UtpCallbacks(const UtpCallbacks&) = delete;
	UtpCallbacks& operator=(const UtpCallbacks&) = delete;

	// ── ACTION（必须实现）──────────────────────────────────

	// 发送 UDP 数据包。数据离开本库的唯一出口。
	// socket 在 RST 包和 SYN-ACK 重传时为 nullptr。
	// 默认实现触发断言失败：用户必须重写此方法。
	virtual void sendto(UtpSocket* socket,
	                    const uint8_t* data, size_t len,
	                    const sockaddr* address, socklen_t address_len,
	                    uint32_t flags);

	// ── QUERY（协议逻辑依赖返回值，均有默认实现）──────────

	// 主时钟（毫秒）。驱动所有超时计算。
	virtual uint64 get_milliseconds(UtpSocket* socket);

	// 高精度时钟（微秒）。用于包时间戳和 RTT 测量。
	virtual uint64 get_microseconds(UtpSocket* socket);

	// 路径 MTU（字节）。
	virtual uint16 get_udp_mtu(UtpSocket* socket,
	                           const sockaddr* address, socklen_t address_len);

	// IP + UDP 头部开销（字节）。
	virtual uint16 get_udp_overhead(UtpSocket* socket,
	                                const sockaddr* address, socklen_t address_len);

	// 随机数。用于连接种子、序列号初始化。
	virtual uint32 get_random(UtpSocket* socket);

	// 用户读取缓冲区中尚未消费的字节数。
	// 用于计算接收窗口通告。默认 0（假设所有数据已消费）。
	virtual size_t get_read_buffer_size(UtpSocket* socket);

	// ── EVENT（fire-and-forget，默认 no-op）───────────────

	// 防火墙过滤。返回 0=接受连接，非 0=拒绝。
	virtual int on_firewall(const sockaddr* address, socklen_t address_len);

	// 新入站连接已被接受。
	virtual void on_accept(UtpSocket* socket,
	                       const sockaddr* address, socklen_t address_len);

	// 出站连接已建立。
	virtual void on_connect(UtpSocket* socket);

	// socket 发生错误。
	virtual void on_error(UtpSocket* socket, int error_code);

	// 有序数据已交付。
	virtual void on_read(UtpSocket* socket, const uint8_t* data, size_t len);

	// socket 状态变迁。
	virtual void on_state_change(UtpSocket* socket, int state);

	// 延迟采样（毫秒）。
	virtual void on_delay_sample(UtpSocket* socket, int sample_ms);

	// 协议开销统计。
	virtual void on_overhead_statistics(UtpSocket* socket,
	                                    bool send, size_t len, int type);

	// 调试日志。
	virtual void log(UtpSocket* socket, const char* message);

	UtpCallbacks() = default;
};
