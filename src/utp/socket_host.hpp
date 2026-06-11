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

// ISocketHost：依赖倒置接口（Dependency Inversion）。
//
// UtpSocket 运行时需要从"宿主环境"获取若干服务：缓存时钟、把字节发到对端、
// 延迟 ACK 调度、统计记账、读取上下文默认配置、销毁时注销自己，以及回调分派。
// 过去这些都通过具体类型 UtpContext 的私有成员（靠 friend）直接访问，造成
// UtpSocket 与 UtpContext 双向强耦合。
//
// 把这组服务抽象为 ISocketHost 后：
//   - UtpSocket 只依赖此窄接口，不再 #include / friend 具体的 UtpContext；
//   - UtpContext 实现该接口（生产环境宿主）；
//   - 单元测试可注入假宿主，单独测试 socket 状态机。

#include <cstddef>

#include "utp.h"            // utp_context 等公开类型（typedef struct UtpContext utp_context）
#include "utp_types.h"
#include "utp/address.hpp"

class UtpSocket;
class UtpCallbacks;

struct ISocketHost {
	virtual ~ISocketHost() = default;

	// ── 时钟（缓存的当前毫秒值，全上下文共享）──
	virtual uint64 current_ms() const = 0;
	// 用 get_milliseconds 回调刷新缓存时钟并返回新值
	virtual uint64 refresh_clock(UtpSocket* who) = 0;

	// ── 出站：把一段字节作为 UDP 报文发往 addr ──
	virtual void send_to(const byte* p, size_t len, const utp::Address& addr, int flags) = 0;

	// ── 延迟 ACK 列表（宿主持有，socket 仅登记/注销自己）──
	virtual void schedule_ack(UtpSocket* s) = 0;
	virtual void remove_ack(UtpSocket* s) = 0;

	// ── 统计：按尺寸桶累加一次接收记账 ──
	virtual void record_raw_recv(size_t len) = 0;

	// ── 上下文默认配置（socket 构造时读取）──
	virtual size_t default_target_delay() const = 0;
	virtual size_t default_sndbuf() const = 0;
	virtual size_t default_rcvbuf() const = 0;

	// ── 连接注册表 ──
	virtual bool has_socket(const utp::Address& addr, uint32 recv_id) const = 0;
	virtual void register_socket(UtpSocket* s) = 0;
	// ── 生命周期：socket 析构时通知宿主清理注册表/缓存 ──
	virtual void on_socket_destroyed(UtpSocket* s) = 0;

	// ── 回调分派 ──
	virtual UtpCallbacks* callbacks() = 0;
	// 公开 C 句柄（utp_call_* 转发与 utp_get_context 需要）
	virtual utp_context* handle() = 0;

	// ── 日志策略（按 level 过滤）──
	virtual bool would_log(int level) = 0;
};
