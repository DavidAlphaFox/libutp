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

// UtpCallbacks 默认实现
// 基于 std::chrono::steady_clock 的单调时钟，保守 MTU 估算，std::rand 随机数

#include "utp_callbacks.hpp"

#include <cassert>
#include <cstdlib>
#include <chrono>

#include <netinet/in.h>
#include <sys/socket.h>

#include "utp_types.h"

namespace {

using Clock = std::chrono::steady_clock;

// steady_clock 保证单调递增，不受系统时间调整影响
// 替代了原来 WIN32/mach_absolute_time/POSIX clock_gettime 的 180 行平台特定代码
const Clock::time_point g_start = Clock::now();

constexpr uint16 kIpv4Mtu = 1500 - 20 - 8 - 24 - 8 - 2 - 36;  // Ethernet - IP - UDP - GRE - PPPoE - MPPE - fudge
constexpr uint16 kTeredoMtu = 1280 - 40 - 8;                    // Teredo - IPv6 - UDP
constexpr uint16 kIpv4Overhead = 20 + 8;                         // IP + UDP
constexpr uint16 kTeredoOverhead = kIpv4Overhead + 40 + 8;       // IPv4 + IPv6 + UDP

} // anonymous namespace

// ── ACTION 默认实现 ─────────────────────────────────────

void UtpCallbacks::sendto(UtpSocket*, const uint8_t*, size_t,
                          const sockaddr*, socklen_t, uint32_t) {
	assert(false && "UtpCallbacks::sendto must be overridden — no default UDP transport");
}

// ── QUERY 默认实现 ──────────────────────────────────────

uint64 UtpCallbacks::get_milliseconds(UtpSocket*) {
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start);
	return static_cast<uint64>(elapsed.count());
}

uint64 UtpCallbacks::get_microseconds(UtpSocket*) {
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_start);
	return static_cast<uint64>(elapsed.count());
}

uint16 UtpCallbacks::get_udp_mtu(UtpSocket*, const sockaddr* address, socklen_t) {
	return (address->sa_family == AF_INET6) ? kTeredoMtu : kIpv4Mtu;
}

uint16 UtpCallbacks::get_udp_overhead(UtpSocket*, const sockaddr* address, socklen_t) {
	return (address->sa_family == AF_INET6) ? kTeredoOverhead : kIpv4Overhead;
}

uint32 UtpCallbacks::get_random(UtpSocket*) {
	return static_cast<uint32>(std::rand());
}

size_t UtpCallbacks::get_read_buffer_size(UtpSocket*) {
	return 0;
}

// ── EVENT 默认实现（no-op）──────────────────────────────

int  UtpCallbacks::on_firewall(const sockaddr*, socklen_t) { return 0; }
void UtpCallbacks::on_accept(UtpSocket*, const sockaddr*, socklen_t) {}
void UtpCallbacks::on_connect(UtpSocket*) {}
void UtpCallbacks::on_error(UtpSocket*, int) {}
void UtpCallbacks::on_read(UtpSocket*, const uint8_t*, size_t) {}
void UtpCallbacks::on_state_change(UtpSocket*, int) {}
void UtpCallbacks::on_delay_sample(UtpSocket*, int) {}
void UtpCallbacks::on_overhead_statistics(UtpSocket*, bool, size_t, int) {}
void UtpCallbacks::log(UtpSocket*, const char*) {}
