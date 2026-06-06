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

#include <cstdint>
#include <cstddef>
#include <climits>
#include <algorithm>

namespace utp {

static constexpr size_t kCurDelaySize = 3;
static constexpr size_t kDelayBaseHistory = 13;
static constexpr uint32_t kTimestampMask = 0xFFFFFFFF;

// compare if lhs is less than rhs, taking wrapping
// into account. if lhs is close to UINT_MAX and rhs
// is close to 0, lhs is assumed to have wrapped and
// considered smaller
static inline bool wrapping_compare_less(std::uint32_t lhs, std::uint32_t rhs, std::uint32_t mask)
{
	const std::uint32_t dist_down = (lhs - rhs) & mask;
	const std::uint32_t dist_up = (rhs - lhs) & mask;

	// if the distance walking up is shorter, lhs
	// is less than rhs. If the distance walking down
	// is shorter, then rhs is less than lhs
	return dist_up < dist_down;
}

class DelayHistory {
	static constexpr uint32_t kUintMax = UINT_MAX;

public:
	std::uint32_t delay_base;

	// this is the history of delay samples,
	// normalized by using the delay_base. These
	// values are always greater than 0 and measures
	// the queuing delay in microseconds
	std::uint32_t cur_delay_hist[kCurDelaySize];
	size_t cur_delay_idx;

	// this is the history of delay_base. It's
	// a number that doesn't have an absolute meaning
	// only relative. It doesn't make sense to initialize
	// it to anything other than values relative to
	// what's been seen in the real world.
	std::uint32_t delay_base_hist[kDelayBaseHistory];
	size_t delay_base_idx;
	// the time when we last stepped the delay_base_idx
	std::uint64_t delay_base_time;

	bool delay_base_initialized;

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

	void shift(const std::uint32_t offset)
	{
		// the offset should never be "negative"
		// assert(offset < 0x10000000);

		// increase all of our base delays by this amount
		// this is used to take clock skew into account
		// by observing the other side's changes in its base_delay
		for (size_t i = 0; i < kDelayBaseHistory; i++) {
			delay_base_hist[i] += offset;
		}
		delay_base += offset;
	}

	void add_sample(const std::uint32_t sample, std::uint64_t current_ms)
	{
		// The two clocks (in the two peers) are assumed not to
		// progress at the exact same rate. They are assumed to be
		// drifting, which causes the delay samples to contain
		// a systematic error, either they are under-
		// estimated or over-estimated. This is why we update the
		// delay_base every two minutes, to adjust for this.

		// This means the values will keep drifting and eventually wrap.
		// We can cross the wrapping boundry in two directions, either
		// going up, crossing the highest value, or going down, crossing 0.

		// if the delay_base is close to the max value and sample actually
		// wrapped on the other end we would see something like this:
		// delay_base = 0xffffff00, sample = 0x00000400
		// sample - delay_base = 0x500 which is the correct difference

		// if the delay_base is instead close to 0, and we got an even lower
		// sample (that will eventually update the delay_base), we may see
		// something like this:
		// delay_base = 0x00000400, sample = 0xffffff00
		// sample - delay_base = 0xfffffb00
		// this needs to be interpreted as a negative number and the actual
		// recorded delay should be 0.

		// It is important that all arithmetic that assume wrapping
		// is done with unsigned intergers. Signed integers are not guaranteed
		// to wrap the way unsigned integers do. At least GCC takes advantage
		// of this relaxed rule and won't necessarily wrap signed ints.

		// remove the clock offset and propagation delay.
		// delay base is min of the sample and the current
		// delay base. This min-operation is subject to wrapping
		// and care needs to be taken to correctly choose the
		// true minimum.

		// specifically the problem case is when delay_base is very small
		// and sample is very large (because it wrapped past zero), sample
		// needs to be considered the smaller

		if (!delay_base_initialized) {
			// delay_base being 0 suggests that we haven't initialized
			// it or its history with any real measurements yet. Initialize
			// everything with this sample.
			for (size_t i = 0; i < kDelayBaseHistory; i++) {
				// if we don't have a value, set it to the current sample
				delay_base_hist[i] = sample;
				continue;
			}
			delay_base = sample;
			delay_base_initialized = true;
		}

		if (wrapping_compare_less(sample, delay_base_hist[delay_base_idx], kTimestampMask)) {
			// sample is smaller than the current delay_base_hist entry
			// update it
			delay_base_hist[delay_base_idx] = sample;
		}

		// is sample lower than delay_base? If so, update delay_base
		if (wrapping_compare_less(sample, delay_base, kTimestampMask)) {
			// sample is smaller than the current delay_base
			// update it
			delay_base = sample;
		}

		// this operation may wrap, and is supposed to
		const std::uint32_t delay = sample - delay_base;
		// sanity check. If this is triggered, something fishy is going on
		// it means the measured sample was greater than 32 seconds!
		//assert(delay < 0x2000000);

		cur_delay_hist[cur_delay_idx] = delay;
		cur_delay_idx = (cur_delay_idx + 1) % kCurDelaySize;

		// once every minute
		if (current_ms - delay_base_time > 60 * 1000) {
			delay_base_time = current_ms;
			delay_base_idx = (delay_base_idx + 1) % kDelayBaseHistory;
			// clear up the new delay base history spot by initializing
			// it to the current sample, then update it
			delay_base_hist[delay_base_idx] = sample;
			delay_base = delay_base_hist[0];
			// Assign the lowest delay in the last 2 minutes to delay_base
			for (size_t i = 0; i < kDelayBaseHistory; i++) {
				if (wrapping_compare_less(delay_base_hist[i], delay_base, kTimestampMask))
					delay_base = delay_base_hist[i];
			}
		}
	}

	std::uint32_t get_value() const
	{
		std::uint32_t value = kUintMax;
		for (size_t i = 0; i < kCurDelaySize; i++) {
			value = std::min(cur_delay_hist[i], value);
		}
		// value could be UINT_MAX if we have no samples yet...
		return value;
	}
};

} // namespace utp