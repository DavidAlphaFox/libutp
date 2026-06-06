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

// 设计说明:
// LedbatController 实现 LEDBAT (Low Extra Delay Background Transport) 拥塞控制算法。
// 它管理拥塞窗口 (max_window_)、RTT 估算、延迟采样和时钟漂移检测。
//
// 设计原则:
//   - 被动数据拥有者: 不持有 UtpSocket 引用, 所有外部依赖通过参数传入
//   - 单向调用: UtpSocket → LedbatController
//   - 纯逻辑: 所有日志记录由调用方 (UtpSocket) 负责
//   - apply_ccontrol() 返回 post-penalty 的 our_delay, 供调用方决定
//     是否调用 utp_call_on_delay_sample() 以及如何记录日志
//
// 字段分组 (与原 UtpSocket 中位置对应):
//   - 拥塞窗口 (max_window_ / cur_window_ / slow_start_ / ssthresh_)
//   - RTT 估算 (rtt_ / rtt_var_ / rto_ / rtt_hist_ / retransmit_timeout_ /
//     retransmit_count_ / rto_timeout_ / zerowindow_time_)
//   - 双向延迟历史 (our_hist_ / their_hist_)
//   - 长时窗延迟统计 (5s 粒度) 与时钟漂移估计

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <climits>

#include "utp_types.h"
#include "utp/delay_history.hpp"

class LedbatController {
public:
	LedbatController() = default;

	// -- LEDBAT 核心算法 --
	// 根据延迟反馈调整拥塞窗口。返回 post-penalty our_delay (微秒),
	// 调用方使用该值调用 utp_call_on_delay_sample() 并构造日志。
	// 参数:
	//   bytes_acked    本次 ACK 覆盖的字节数
	//   actual_delay   对端测得的"我方到对端"延迟 (微秒)
	//   min_rtt        最小 RTT 估计 (微秒), 用于限制 our_delay 上界
	//   current_ms     当前毫秒时间戳
	//   opt_sndbuf     套接字发送缓冲上限
	//   target_delay   LEDBAT 目标排队延迟 (微秒)
	//   packet_size    单包有效载荷大小
	int32 apply_ccontrol(size_t bytes_acked, uint32 actual_delay, int64 min_rtt,
	                     uint64 current_ms, size_t opt_sndbuf, size_t target_delay,
	                     size_t packet_size);

	// -- RTT 估算 --
	// 在 ack_packet 中调用: 更新 rtt_/rtt_var_ (RFC 6298 EWMA),
	// 同步 rto_/retransmit_timeout_/rto_timeout_。
	// ertt 为本次 ACK 测得的 RTT (毫秒)。
	void update_rtt(uint32 ertt, uint64 current_ms);

	// -- 延迟采样 --
	// 处理收到的 actual_delay 样本: 更新长时窗平均延迟 (5s 粒度)
	// 和时钟漂移估计。仅在 actual_delay != 0 时调用。
	void update_delay_average(uint32 actual_delay, uint64 current_ms);

	// -- 窗口衰减 --
	// 每隔 MAX_WINDOW_DECAY 将窗口减半, 同时退出慢启动并更新 ssthresh_。
	void maybe_decay_win(uint64 current_ms);

	// -- 窗口配额管理 --
	void add_in_flight(size_t bytes) { cur_window_ += bytes; }
	void remove_in_flight(size_t bytes) { assert(cur_window_ >= bytes); cur_window_ -= bytes; }
	void mark_window_full(uint64 current_ms) { last_maxed_out_window_ = current_ms; }

	// -- 窗口检查 --
	// 判定是否还能再发 bytes 字节, 限幅 = min(max_window_, opt_sndbuf, max_window_user)。
	bool can_send(size_t bytes, size_t max_window_user, size_t opt_sndbuf) const;

	// -- RTO 超时处理 --
	// RTO 触发: 减小拥塞窗口 (空闲时 2/3 衰减; 否则重置为 packet_size 并进入慢启动)。
	// cur_window_packets 是 UtpSocket 的状态 (在飞包数), 由调用方传入。
	void on_rto_timeout(uint64 current_ms, size_t packet_size, uint16 cur_window_packets);

	// -- 拥塞窗口访问器 --
	size_t max_window() const { return max_window_; }
	size_t cur_window() const { return cur_window_; }
	size_t ssthresh() const  { return ssthresh_; }
	bool slow_start() const  { return slow_start_; }

	// -- RTT 估算访问器 --
	uint rtt_ms() const              { return rtt_; }
	uint rtt_var() const             { return rtt_var_; }
	uint rto_ms() const              { return rto_; }
	uint retransmit_timeout() const  { return retransmit_timeout_; }
	uint16 retransmit_count() const  { return retransmit_count_; }
	uint64 rto_timeout() const       { return rto_timeout_; }
	uint64 zerowindow_time() const   { return zerowindow_time_; }

	// -- 延迟统计访问器 --
	int32 clock_drift() const            { return clock_drift_; }
	int32 clock_drift_raw() const        { return clock_drift_raw_; }
	int32 average_delay() const          { return average_delay_; }
	int64 current_delay_sum() const     { return current_delay_sum_; }
	int current_delay_samples() const   { return current_delay_samples_; }
	uint32 average_delay_base() const   { return average_delay_base_; }
	uint64 last_maxed_out_window() const { return last_maxed_out_window_; }
	int64 last_rwin_decay() const       { return last_rwin_decay_; }

	// -- 设置器 --
	void set_max_window(size_t w)            { max_window_ = w; }
	void set_ssthresh(size_t s)              { ssthresh_ = s; }
	void set_zerowindow_time(uint64 t)       { zerowindow_time_ = t; }
	void set_retransmit_count(uint16 count)  { retransmit_count_ = count; }
	void increment_retransmit_count()        { ++retransmit_count_; }
	void set_retransmit_timeout(uint t)      { retransmit_timeout_ = t; }
	void set_rto_timeout(uint64 t)           { rto_timeout_ = t; }

	// 初始化 RTO 计时器: retransmit_timeout_ = rto_, rto_timeout_ = current_ms + rto_
	// 在 write_outgoing_packet 中首次发送数据包时调用。
	void set_initial_rto(uint64 current_ms) {
		retransmit_timeout_ = rto_;
		rto_timeout_ = current_ms + rto_;
	}

	// 检查 RTO 是否到期
	bool is_rto_expired(uint64 current_ms) const {
		return rto_timeout_ > 0 && (int64)(current_ms - rto_timeout_) >= 0;
	}

	// 检查是否可以执行窗口衰减 (距上次衰减 >= MAX_WINDOW_DECAY ms)
	bool can_decay_win(int64 msec) const {
		return (msec - last_rwin_decay_) >= MAX_WINDOW_DECAY;
	}

	// 初始化 delay history (在 utp_initialize_socket 中调用)
	void init_delay_histories(uint64 current_ms) {
		our_hist_.clear(current_ms);
		their_hist_.clear(current_ms);
		rtt_hist_.clear(current_ms);
	}

	// 初始化时间相关字段 (在 utp_initialize_socket 中调用)
	void init_timing(uint64 current_ms) {
		average_sample_time_ = current_ms + 5000;
		last_rwin_decay_ = (int64)current_ms - MAX_WINDOW_DECAY;
	}

	// -- Delay history 访问 (供 utp_process_incoming 直接操作) --
	utp::DelayHistory& our_hist()                 { return our_hist_; }
	utp::DelayHistory& their_hist()               { return their_hist_; }
	utp::DelayHistory& rtt_hist()                 { return rtt_hist_; }
	const utp::DelayHistory& our_hist() const     { return our_hist_; }
	const utp::DelayHistory& their_hist() const   { return their_hist_; }
	const utp::DelayHistory& rtt_hist() const     { return rtt_hist_; }

	// MIN_WINDOW_SIZE 与 MAX_WINDOW_DECAY 作为公共常量暴露,
	// 供调用方在 LEDBAT 相关决策中使用 (例如 utp_process_incoming 的 clamp)。
	static constexpr size_t MIN_WINDOW_SIZE = 10;
	static constexpr int64 MAX_WINDOW_DECAY = 100;

private:
	// -- 拥塞窗口 --
	size_t max_window_ = 0;
	size_t cur_window_ = 0;
	bool slow_start_ = true;
	size_t ssthresh_ = 0;

	// -- RTT 估算 (单位: 毫秒) 与 RTO 计时器 --
	uint rtt_ = 0;
	uint rtt_var_ = 800;
	uint rto_ = 3000;
	utp::DelayHistory rtt_hist_;
	uint retransmit_timeout_ = 0;
	uint16 retransmit_count_ = 0;
	uint64 rto_timeout_ = 0;
	uint64 zerowindow_time_ = 0;

	// -- 双向延迟历史 (LEDBAT 排队延迟估算) --
	utp::DelayHistory our_hist_;
	utp::DelayHistory their_hist_;

	// -- 长时窗延迟统计 (5s 粒度) 与时钟漂移估计 --
	int32 average_delay_ = 0;
	int64 current_delay_sum_ = 0;
	int current_delay_samples_ = 0;
	uint32 average_delay_base_ = 0;
	uint64 average_sample_time_ = 0;
	int32 clock_drift_ = 0;
	int32 clock_drift_raw_ = 0;

	// -- 辅助 --
	uint64 last_maxed_out_window_ = 0;
	int64 last_rwin_decay_ = 0;
};
