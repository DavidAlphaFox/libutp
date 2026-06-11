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

// IConnectionState：连接状态机的 State 模式接口。
//
// uTP 连接的生命周期行为（关闭、半关闭、是否可写、超时是否生效）随状态而变。
// 过去这些散落为 close()/shutdown()/writev()/check_timeouts() 内部的 switch。
// 这里把"每个状态在这些事件下怎么做"封装成一个状态对象，按 CONN_STATE 取单例分派。
//
// 关键设计（保持低耦合）：
//   - 状态对象是无数据的单例（stateless），不持有 socket 引用；
//   - 状态对象只调用 UtpSocket 的一组公开原语（mark_read_shutdown/send_fin/...），
//     不访问其私有成员，因此无需 friend，不会重新引入 Phase 3 已消除的耦合；
//   - enum conn_.state 仍是状态的唯一真相来源（线格式/日志直接用它），
//     状态对象按需经 UtpSocket::state_descriptor() 查表获得。
//
// 注意：每包共享的窗口/ACK 处理流水线不随状态切换而多态化（那部分是跨状态共享的
// 算法，不属于"每状态各异"的行为），仍留在 UtpSocket，符合"在合适粒度上用模式"。

class UtpSocket;

struct IConnectionState {
	virtual ~IConnectionState() = default;

	// 状态名（调试日志用）
	virtual const char* name() const = 0;

	// 该状态下 utp_writev 是否允许写入应用数据（仅 CS_CONNECTED 为真）
	virtual bool writable() const { return false; }

	// 该状态下 check_timeouts 是否需要运行 RTO/保活等定时机制
	virtual bool timeout_active() const { return false; }

	// 用户请求关闭连接（utp_close）
	virtual void close(UtpSocket& s) const = 0;

	// 用户半关闭连接（utp_shutdown）
	virtual void shutdown(UtpSocket& s, int how) const = 0;
};
