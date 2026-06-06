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

#include "utp/ledbat.hpp"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdlib>

// 慢启动窗口每 RTT 增量: 字节数。 与原 utp_internal.cpp 中保持一致;
// 该常量仅 apply_ccontrol 内部使用, 故仅在 .cpp 中定义。
#define MAX_CWND_INCREASE_BYTES_PER_RTT 3000

using std::min;
using std::max;
using std::clamp;

// =============================================================================
// apply_ccontrol: LEDBAT 拥塞控制核心, 每收到一个 ACK 都会调用一次。
// 算法思路:
//   - 用 our_delay = min(对端测得的本端到对端延迟, 当前 RTT) 作为"我方方向上
//     引入的排队延迟"估计。
//   - off_target = target_delay - our_delay 表示距离目标延迟还有多少余量;
//     正值 (我们还有余量) -> 加窗, 负值 (我们引入的排队过多) -> 减窗。
//   - scaled_gain = MAX_CWND_INCREASE_BYTES_PER_RTT * window_factor * delay_factor,
//     把"每个 RTT 至多增加 X 字节"按 ACK 所占窗口比例和延迟比例缩放。
//   - 慢启动阶段: 窗口每次按 packet_size 比例增长, 超过 ssthresh_ 或排队
//     延迟达到 target 的 90% 时, 切到 LEDBAT 线性 (减/增) 模式。
//   - 防"作弊": 时钟漂移 < -200000 时追加 penalty, 抑制其获得过多带宽。
//   - 若 1 秒以上未触达 CWIN 上限, 不再继续加窗, 防止窗口无限膨胀。
//
// 本方法只做状态变更, 不调用 log() 也不调用 utp_call_on_delay_sample();
// 调用方 (UtpSocket) 负责在拿到返回的 post-penalty our_delay 之后执行
// 上述两个副作用。
// =============================================================================
int32 LedbatController::apply_ccontrol(size_t bytes_acked, uint32 actual_delay, int64 min_rtt,
                                       uint64 current_ms, size_t opt_sndbuf, size_t target_delay,
                                       size_t packet_size)
{
	(void)actual_delay;

	// the delay can never be greater than the rtt. The min_rtt
	// variable is the RTT in microseconds
	assert(min_rtt >= 0);
	int32 our_delay = (int32)min<uint32>(our_hist_.get_value(), (uint32)min_rtt);
	assert(our_delay != INT_MAX);
	assert(our_delay >= 0);

	int target = (int)target_delay;
	if (target <= 0) target = 100000;

	// compensate for very large clock drift that affects the congestion
	// controller into giving certain endpoints an unfair share of bandwidth.
	// clock_drift_ unit is microseconds per 5 seconds. empirically, a
	// reasonable cut-off appears to be about 200000. The main purpose is
	// to compensate for people trying to "cheat" uTP by making their clock
	// run slower. if clock_drift_ < -200000 start applying a penalty delay
	// proportional to how far beyond -200000 the clock drift is.
	int32 penalty = 0;
	if (clock_drift_ < -200000) {
		penalty = (-clock_drift_ - 200000) / 7;
		our_delay += penalty;
	}

	double off_target = (double)(target - our_delay);

	// this is the same as:
	//
	//    (min(off_target, target) / target) * (bytes_acked / max_window_) * MAX_CWND_INCREASE_BYTES_PER_RTT
	//
	// so, it's scaling the max increase by the fraction of the window this ack
	// represents, and the fraction of the target delay the current delay represents.
	// The min() around off_target protects against crazy values of our_delay, which
	// may happen when the timestamps wraps, or by just having a malicious peer
	// sending garbage. This caps the increase of the window size to
	// MAX_CWND_INCREASE_BYTES_PER_RTT per rtt.
	// as for large negative numbers, this direction is already capped at the min
	// packet size further down. The min around the bytes_acked protects against
	// the case where the window size was recently shrunk and the number of acked
	// bytes exceeds that. This is considered no more than one full window, in
	// order to keep the gain within sane boundries.

	assert(bytes_acked > 0);
	double window_factor = (double)min(bytes_acked, max_window_) / (double)max(max_window_, bytes_acked);

	double delay_factor = off_target / (double)target;
	double scaled_gain = MAX_CWND_INCREASE_BYTES_PER_RTT * window_factor * delay_factor;

	// since MAX_CWND_INCREASE_BYTES_PER_RTT is a cap on how much the window size
	// (max_window_) may increase per RTT, we may not increase the window size more
	// than that proportional to the number of bytes that were acked, so that once
	// one window has been acked (one rtt) the increase limit is not exceeded.
	// the +1. is to allow for floating point imprecision.
	assert(scaled_gain <= 1. + MAX_CWND_INCREASE_BYTES_PER_RTT * (double)min(bytes_acked, max_window_) / (double)max(max_window_, bytes_acked));

	if (scaled_gain > 0 && (int64)(current_ms - last_maxed_out_window_) > 1000) {
		// if it was more than 1 second since we tried to send a packet and stopped
		// because we hit the max window, we're most likely rate limited (which
		// prevents us from ever hitting the window size). If this is the case, we
		// cannot let the max_window_ grow indefinitely.
		scaled_gain = 0;
	}

	size_t ledbat_cwnd = (max_window_ + (size_t)scaled_gain < MIN_WINDOW_SIZE)
		? MIN_WINDOW_SIZE
		: (size_t)(max_window_ + scaled_gain);

	// 慢启动 vs 拥塞避免 分支:
	//   - 慢启动: 每个 ACK 把窗口按 packet_size 比例往上推 (近似指数增长),
	//     直到 ss_cwnd > ssthresh_ 或排队延迟达到 target 的 90%。
	//   - 拥塞避免: 改用 LEDBAT 公式 (线性/亚线性) 计算 ledbat_cwnd。
	// 退出慢启动的同时会把当前 max_window_ 记为新的 ssthresh_, 与 TCP 类似。
	if (slow_start_) {
		size_t ss_cwnd = max_window_ + (size_t)(window_factor * (double)packet_size);
		if (ss_cwnd > ssthresh_) {
			slow_start_ = false;
		} else if (our_delay > (int)(target * 0.9)) {
			// even if we're a little under the target delay, we conservatively
			// discontinue the slow start phase
			slow_start_ = false;
			ssthresh_ = max_window_;
		} else {
			max_window_ = max(ss_cwnd, ledbat_cwnd);
		}
	} else {
		max_window_ = ledbat_cwnd;
	}

	// make sure that the congestion window is below max
	// make sure that we don't shrink our window too small
	max_window_ = clamp<size_t>(max_window_, MIN_WINDOW_SIZE, opt_sndbuf);

	return our_delay;
}

// =============================================================================
// update_rtt: RTT EWMA 更新 + RTO 同步。 在 ack_packet 中当 transmissions == 1
// (即未发生重传) 时调用。 算法风格遵循 RFC 6298:
//   - 首次样本: rtt_ = ertt, rtt_var_ = ertt / 2
//   - 后续样本: rtt_var_ = rtt_var_ + (|delta| - rtt_var_) / 4
//              rtt_     = rtt_ - rtt_/8 + ertt/8
//   - rto_ = max(rtt_ + rtt_var_ * 4, 1000ms) (RFC 6298 下界 1s)
// 更新 rto_ 后同步 retransmit_timeout_ 与 rto_timeout_。
// =============================================================================
void LedbatController::update_rtt(uint32 ertt, uint64 current_ms)
{
	if (rtt_ == 0) {
		// First round trip time sample
		rtt_ = ertt;
		rtt_var_ = ertt / 2;
		// sanity check. rtt should never be more than 6 seconds
//		assert(rtt_ < 6000);
	} else {
		// Compute new round trip times
		const int delta = (int)rtt_ - (int)ertt;
		rtt_var_ = rtt_var_ + (int)(abs(delta) - (int)rtt_var_) / 4;
		rtt_ = rtt_ - rtt_/8 + ertt/8;
		// sanity check. rtt should never be more than 6 seconds
//		assert(rtt_ < 6000);
		rtt_hist_.add_sample(ertt, current_ms);
	}
	rto_ = max<uint>(rtt_ + rtt_var_ * 4, 1000);
	retransmit_timeout_ = rto_;
	rto_timeout_ = current_ms + rto_;
}

// =============================================================================
// update_delay_average: 长时窗延迟平均 (5s 粒度) 与时钟漂移估计。
// 与原 utp_process_incoming 中代码语义一致:
//   - 每次收到非零 actual_delay 样本, 累加 (sample - average_delay_base_)
//     到 current_delay_sum_ (带符号距离)。
//   - 5s 窗口到期后, 计算窗口平均作为新 average_delay_, 复位 sum/samples。
//   - 时钟漂移 = (新平均 - 旧平均), 滚动平均 7/8 旧 + 1/8 新。
//   - 时钟漂移的"基线归一化"逻辑 (min/max 调整) 防止 wraparound 失真。
// =============================================================================
void LedbatController::update_delay_average(uint32 actual_delay, uint64 current_ms)
{
	if (average_delay_base_ == 0) average_delay_base_ = actual_delay;
	int64 average_delay_sample = 0;
	// distance walking from lhs to rhs, downwards
	const uint32 dist_down = average_delay_base_ - actual_delay;
	// distance walking from lhs to rhs, upwards
	const uint32 dist_up = actual_delay - average_delay_base_;

	if (dist_down > dist_up) {
		// average_delay_base_ < actual_delay, we should end up
		// with a positive sample
		average_delay_sample = (int64)dist_up;
	} else {
		// average_delay_base_ >= actual_delay, we should end up
		// with a negative sample
		average_delay_sample = -(int64)dist_down;
	}
	current_delay_sum_ += average_delay_sample;
	++current_delay_samples_;

	if ((int64)current_ms > (int64)average_sample_time_) {

		int32 prev_average_delay = average_delay_;

		assert(current_delay_sum_ / current_delay_samples_ < INT_MAX);
		assert(current_delay_sum_ / current_delay_samples_ > -INT_MAX);
		// write the new average
		average_delay_ = (int32)(current_delay_sum_ / current_delay_samples_);
		// each slot represents 5 seconds
		average_sample_time_ += 5000;

		current_delay_sum_ = 0;
		current_delay_samples_ = 0;

		// normalize the average samples. since we're only interested in the slope
		// of the curve formed by the average delay samples, we can cancel out the
		// actual offset to make sure we won't have problems with wrapping.
		int min_sample = (int)min(prev_average_delay, average_delay_);
		int max_sample = (int)max(prev_average_delay, average_delay_);

		// normalize around zero. Try to keep the min <= 0 and max >= 0
		int adjust = 0;
		if (min_sample > 0) {
			// adjust all samples (and the baseline) down by min_sample
			adjust = -min_sample;
		} else if (max_sample < 0) {
			// adjust all samples (and the baseline) up by -max_sample
			adjust = -max_sample;
		}
		if (adjust) {
			average_delay_base_ -= adjust;
			average_delay_ += adjust;
			prev_average_delay += adjust;
		}

		// update the clock drift estimate. the unit is microseconds per 5 seconds.
		// what we're doing is just calculating the average of the difference between
		// each slot. Since each slot is 5 seconds and the timestamps unit are
		// microseconds, we'll end up with the average slope across our history. If
		// there is a consistent trend, it will show up in this value.
		int32 drift = average_delay_ - prev_average_delay;

		// clock_drift_ is a rolling average
		clock_drift_ = (int32)(((int64)clock_drift_ * 7 + (int64)drift) / 8);
		clock_drift_raw_ = drift;
	}
}

// =============================================================================
// maybe_decay_win: TCP 风格的窗口衰减。 每隔 MAX_WINDOW_DECAY (100ms) 将窗口
// 减半, 限幅到 MIN_WINDOW_SIZE; 同时退出慢启动, 并把新窗口记为 ssthresh_。
// 当 selective_ack 触发 SACK 重传 (back_off) 时调用。
// =============================================================================
void LedbatController::maybe_decay_win(uint64 current_ms)
{
	if (can_decay_win((int64)current_ms)) {
		// TCP uses 0.5
		max_window_ = (size_t)(max_window_ * 0.5);
		last_rwin_decay_ = (int64)current_ms;
		if (max_window_ < MIN_WINDOW_SIZE)
			max_window_ = MIN_WINDOW_SIZE;
		slow_start_ = false;
		ssthresh_ = max_window_;
	}
}

// =============================================================================
// can_send: 判定是否还能再发 bytes 字节, 限幅 = min(max_window_, opt_sndbuf, max_window_user)。
// =============================================================================
bool LedbatController::can_send(size_t bytes, size_t max_window_user, size_t opt_sndbuf) const
{
	size_t max_send = min(min(max_window_, opt_sndbuf), max_window_user);
	return cur_window_ + bytes <= max_send;
}

// =============================================================================
// on_rto_timeout: RTO 触发后的窗口收缩策略。
//   - 空闲态 (cur_window_packets == 0): 窗口衰减 2/3, 下限 packet_size。
//     体现"连接空闲时不要激进降窗"的设计意图。
//   - 实际有在飞包: 窗口重置为 packet_size, 重新进入慢启动 (与 TCP 类似)。
// cur_window_packets 是 UtpSocket 的状态 (LEDBAT 不知道是否有在飞包),
// 因此由调用方传入。
// =============================================================================
void LedbatController::on_rto_timeout(uint64 /*current_ms*/, size_t packet_size, uint16 cur_window_packets)
{
	if (cur_window_packets == 0 && (int64)max_window_ > (int64)packet_size) {
		// we don't have any packets in-flight, even though we could. This implies
		// that the connection is just idling. No need to be aggressive about
		// resetting the congestion window. Just let it decay by a 3:rd.
		// Don't set it any lower than the packet size though.
		max_window_ = max(max_window_ * 2 / 3, packet_size);
	} else {
		// our delay was so high that our congestion window was shrunk below one
		// packet, preventing us from sending anything for one time-out period.
		// Now, reset the congestion window to fit one packet, to start over again.
		max_window_ = packet_size;
		slow_start_ = true;
	}
}
