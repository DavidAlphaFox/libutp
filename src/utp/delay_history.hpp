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

// 延迟历史记录模块
// 用于测量和跟踪网络延迟，实现 LEDBAT 拥塞控制算法
// 通过维护延迟基线和当前延迟历史来计算排队延迟

#include <cstdint>
#include <cstddef>
#include <climits>
#include <algorithm>

namespace utp {

static constexpr size_t kCurDelaySize = 3;      // 当前延迟历史记录数量
static constexpr size_t kDelayBaseHistory = 13; // 延迟基线历史记录数量 (覆盖约 2 分钟)
static constexpr uint32_t kTimestampMask = 0xFFFFFFFF;  // 时间戳掩码 (32 位)

// 比较两个时间戳是否 lhs < rhs，考虑回绕情况
// 如果 lhs 接近 UINT_MAX 且 rhs 接近 0，则认为 lhs 已回绕，lhs < rhs
static inline bool wrapping_compare_less(std::uint32_t lhs, std::uint32_t rhs, std::uint32_t mask)
{
	const std::uint32_t dist_down = (lhs - rhs) & mask;  // 向下走需要的距离
	const std::uint32_t dist_up = (rhs - lhs) & mask;    // 向上走需要的距离

	// 如果向上走的距离更短，则 lhs < rhs
	// 如果向下走的距离更短，则 rhs < lhs
	return dist_up < dist_down;
}

// 延迟历史记录类
// 用于跟踪网络延迟并计算排队延迟
class DelayHistory {
	static constexpr uint32_t kUintMax = UINT_MAX;

public:
	std::uint32_t delay_base;  // 延迟基线 (历史最小延迟)

	// 当前延迟历史 (归一化后的延迟样本，以 delay_base 为基准)
	// 值始终 > 0，测量排队延迟 (单位：微秒)
	std::uint32_t cur_delay_hist[kCurDelaySize];
	size_t cur_delay_idx;  // 当前延迟历史索引

	// 延迟基线历史
	// 延迟基线是一个相对值，没有绝对意义
	// 只有相对于实际观测值才有意义
	std::uint32_t delay_base_hist[kDelayBaseHistory];
	size_t delay_base_idx;  // 延迟基线历史索引
	std::uint64_t delay_base_time;  // 最后更新 delay_base_idx 的时间

	bool delay_base_initialized;  // 延迟基线是否已初始化

	DelayHistory() : delay_base(0)
		, cur_delay_idx(0)
		, delay_base_idx(0)
		, delay_base_time(0)
		, delay_base_initialized(false)
	{
		for (size_t i = 0; i < kCurDelaySize; i++) {
			cur_delay_hist[i] = 0;
		}
		for (size_t i = 0; i < kDelayBaseHistory; i++) {
			delay_base_hist[i] = 0;
		}
	}

	// 清空延迟历史
	void clear(std::uint64_t current_ms)
	{
		delay_base_initialized = false;
		delay_base = 0;
		cur_delay_idx = 0;
		delay_base_idx = 0;
		delay_base_time = current_ms;
		for (size_t i = 0; i < kCurDelaySize; i++) {
			cur_delay_hist[i] = 0;
		}
		for (size_t i = 0; i < kDelayBaseHistory; i++) {
			delay_base_hist[i] = 0;
		}
	}

	// 调整延迟基线以处理时钟漂移
	// 将所有基线延迟增加偏移量
	// 通过观察对端的 delay_base 变化来考虑时钟偏差
	void shift(const std::uint32_t offset)
	{
		for (size_t i = 0; i < kDelayBaseHistory; i++) {
			delay_base_hist[i] += offset;
		}
		delay_base += offset;
	}

	// 添加延迟样本
	// 样本处理考虑时钟漂移和序列号回绕
	// 每分钟更新一次延迟基线，使用过去 2 分钟的最小值
	void add_sample(const std::uint32_t sample, std::uint64_t current_ms)
	{
		// 双方时钟的速率不完全相同，存在时钟漂移
		// 这会导致延迟样本包含系统误差（低估或高估）
		// 因此每 2 分钟更新一次 delay_base 来调整

		// 这意味着值会不断漂移并最终回绕
		// 可以在两个方向上跨越回绕边界：向上（超过最大值）或向下（低于 0）

		// 如果 delay_base 接近最大值，且样本在对端回绕
		// 例如：delay_base = 0xffffff00, sample = 0x00000400
		// sample - delay_base = 0x500 (正确差值)

		// 如果 delay_base 接近 0，且收到更小的样本
		// 例如：delay_base = 0x00000400, sample = 0xffffff00
		// sample - delay_base = 0xfffffb00
		// 这应解释为负数，实际记录的延迟应为 0

		// 重要：所有假设回绕的算术运算必须使用无符号整数
		// 有符号整数不一定像无符号整数那样回绕

		// 移除时钟偏移和传播延迟
		// delay_base 是样本和当前延迟基线的最小值
		// 最小值运算考虑回绕，需要正确选择真正的最小值

		// 特殊情况：delay_base 很小，样本很大（因为回绕过 0）
		// 此时样本应被视为更小的值

		if (!delay_base_initialized) {
			// delay_base 为 0 表示尚未初始化
			// 使用此样本初始化所有历史记录
			for (size_t i = 0; i < kDelayBaseHistory; i++) {
				// 如果没有值，设置为当前样本
				delay_base_hist[i] = sample;
				continue;
			}
			delay_base = sample;
			delay_base_initialized = true;
		}

		if (wrapping_compare_less(sample, delay_base_hist[delay_base_idx], kTimestampMask)) {
			// 样本小于当前的 delay_base_hist 条目
			// 更新它
			delay_base_hist[delay_base_idx] = sample;
		}

		// 样本是否小于 delay_base？如果是则更新 delay_base
		if (wrapping_compare_less(sample, delay_base, kTimestampMask)) {
			// 样本小于当前的 delay_base
			// 更新它
			delay_base = sample;
		}

		// 此运算可能回绕，这是预期的
		const std::uint32_t delay = sample - delay_base;
		// 健全性检查。如果触发，说明有问题
		// 这意味着测量的样本超过 32 秒！
		//assert(delay < 0x2000000);

		cur_delay_hist[cur_delay_idx] = delay;
		cur_delay_idx = (cur_delay_idx + 1) % kCurDelaySize;

		// 每分钟一次
		if (current_ms - delay_base_time > 60 * 1000) {
			delay_base_time = current_ms;
			delay_base_idx = (delay_base_idx + 1) % kDelayBaseHistory;
			// 通过将新的延迟基线历史位置初始化为当前样本来清理
			// 然后更新它
			delay_base_hist[delay_base_idx] = sample;
			delay_base = delay_base_hist[0];
			// 将过去 2 分钟内的最小延迟分配给 delay_base
			for (size_t i = 0; i < kDelayBaseHistory; i++) {
				if (wrapping_compare_less(delay_base_hist[i], delay_base, kTimestampMask))
					delay_base = delay_base_hist[i];
			}
		}
	}

	// 获取当前最小延迟值
	std::uint32_t get_value() const
	{
		std::uint32_t value = kUintMax;
		for (size_t i = 0; i < kCurDelaySize; i++) {
			value = std::min(cur_delay_hist[i], value);
		}
		// 如果还没有样本，value 可能是 UINT_MAX
		return value;
	}
};

}  // namespace utp