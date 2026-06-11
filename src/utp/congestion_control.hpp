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

// ICongestionController：拥塞控制策略接口（Strategy 模式）。
//
// UtpSocket 通过此接口驱动拥塞控制，不直接依赖具体算法实现。默认实现是
// LedbatController（LEDBAT 低延迟背景传输）；将来可替换为其它算法（如经典
// TCP Reno、CUBIC），或在单元测试中注入一个确定性的假实现，而不改动 UtpSocket。
//
// 接口仅包含 UtpSocket 实际调用的方法（其余 LEDBAT 内部细节仍是实现私有）。

#include <cstddef>

#include "utp_types.h"
#include "utp/delay_history.hpp"

struct ICongestionController {
	virtual ~ICongestionController() = default;

	// -- 核心算法：按延迟反馈调整窗口，返回 post-penalty our_delay(微秒) --
	virtual int32 apply_ccontrol(size_t bytes_acked, uint32 actual_delay, int64 min_rtt,
	                             uint64 current_ms, size_t opt_sndbuf, size_t target_delay,
	                             size_t packet_size) = 0;

	// -- RTT / 延迟采样 --
	virtual void update_rtt(uint32 ertt, uint64 current_ms) = 0;
	virtual void update_delay_average(uint32 actual_delay, uint64 current_ms) = 0;

	// -- 窗口衰减 / 配额 --
	virtual void maybe_decay_win(uint64 current_ms) = 0;
	virtual void add_in_flight(size_t bytes) = 0;
	virtual void remove_in_flight(size_t bytes) = 0;
	virtual void mark_window_full(uint64 current_ms) = 0;

	// -- RTO 超时处理 --
	virtual void on_rto_timeout(uint64 current_ms, size_t packet_size, uint16 cur_window_packets) = 0;

	// -- 窗口 / RTT / 延迟 访问器（多用于决策与日志）--
	virtual size_t max_window() const = 0;
	virtual size_t cur_window() const = 0;
	virtual uint rtt_ms() const = 0;
	virtual uint rtt_var() const = 0;
	virtual uint rto_ms() const = 0;
	virtual uint retransmit_timeout() const = 0;
	virtual uint16 retransmit_count() const = 0;
	virtual uint64 rto_timeout() const = 0;
	virtual uint64 zerowindow_time() const = 0;
	virtual int32 clock_drift() const = 0;
	virtual int32 clock_drift_raw() const = 0;
	virtual int32 average_delay() const = 0;
	virtual int64 current_delay_sum() const = 0;
	virtual int current_delay_samples() const = 0;
	virtual uint32 average_delay_base() const = 0;
	virtual uint64 last_maxed_out_window() const = 0;

	// -- 设置器 --
	virtual void set_max_window(size_t w) = 0;
	virtual void set_ssthresh(size_t s) = 0;
	virtual void set_zerowindow_time(uint64 t) = 0;
	virtual void set_retransmit_count(uint16 count) = 0;
	virtual void increment_retransmit_count() = 0;
	virtual void set_retransmit_timeout(uint t) = 0;
	virtual void set_rto_timeout(uint64 t) = 0;
	virtual void set_initial_rto(uint64 current_ms) = 0;

	// -- RTO / 初始化 --
	virtual bool is_rto_expired(uint64 current_ms) const = 0;
	virtual void init_delay_histories(uint64 current_ms) = 0;
	virtual void init_timing(uint64 current_ms) = 0;

	// -- Delay history 访问（供入站处理直接读写）--
	virtual utp::DelayHistory& our_hist() = 0;
	virtual utp::DelayHistory& their_hist() = 0;
	virtual utp::DelayHistory& rtt_hist() = 0;
};
