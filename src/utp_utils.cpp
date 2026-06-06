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

// 工具函数实现
// 提供时间获取（基于 std::chrono 单调时钟）、MTU 查询、UDP 开销计算和随机数生成
// 时间函数使用 std::chrono::steady_clock，保证跨平台单调递增

#include <cstdlib>
#include <chrono>

#include "utp.h"
#include "utp_types.h"
#include "utp_utils.h"

namespace {

// steady_clock 保证单调递增，不受系统时间调整影响
// 替代了原来 WIN32/mach_absolute_time/POSIX clock_gettime 的 180 行平台特定代码
using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

const Clock::time_point g_start = Clock::now();

uint64 get_microseconds() {
	const auto elapsed = std::chrono::duration_cast<Microseconds>(Clock::now() - g_start);
	return static_cast<uint64>(elapsed.count());
}

uint64 get_milliseconds() {
	const auto elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - g_start);
	return static_cast<uint64>(elapsed.count());
}

} // anonymous namespace

// MTU 和协议头部常量
#define ETHERNET_MTU 1500
#define IPV4_HEADER_SIZE 20
#define IPV6_HEADER_SIZE 40
#define UDP_HEADER_SIZE 8
#define GRE_HEADER_SIZE 24
#define PPPOE_HEADER_SIZE 8
#define MPPE_HEADER_SIZE 2
// 在实际网络中观察到分片载荷为 1416 的数据包
// 有报告称路由器 MTU 可小至 1392
#define FUDGE_HEADER_SIZE 36
#define TEREDO_MTU 1280

#define UDP_IPV4_OVERHEAD (IPV4_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_IPV6_OVERHEAD (IPV6_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_TEREDO_OVERHEAD (UDP_IPV4_OVERHEAD + UDP_IPV6_OVERHEAD)

#define UDP_IPV4_MTU (ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_IPV6_MTU (ETHERNET_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_TEREDO_MTU (TEREDO_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE)

// 获取默认 UDP MTU
// 由于不知道本地接口地址，保守假设所有 IPv6 连接都是 Teredo
uint64 utp_default_get_udp_mtu(utp_callback_arguments *args) {
	return (args->address->sa_family == AF_INET6) ? UDP_TEREDO_MTU : UDP_IPV4_MTU;
}

// 获取默认 UDP 协议开销
// 由于不知道本地接口地址，保守假设所有 IPv6 连接都是 Teredo
uint64 utp_default_get_udp_overhead(utp_callback_arguments *args) {
	return (args->address->sa_family == AF_INET6) ? UDP_TEREDO_OVERHEAD : UDP_IPV4_OVERHEAD;
}

// 获取默认随机数
// 简单使用 rand()，生产环境建议使用更安全的随机源
uint64 utp_default_get_random(utp_callback_arguments *args) {
	return std::rand();
}

// 获取当前时间（毫秒）
// 基于 std::chrono::steady_clock，保证单调递增
uint64 utp_default_get_milliseconds(utp_callback_arguments *args) {
	return get_milliseconds();
}

// 获取当前时间（微秒）
// 基于 std::chrono::steady_clock，保证单调递增，用于高精度时间戳和延迟测量
uint64 utp_default_get_microseconds(utp_callback_arguments *args) {
	return get_microseconds();
}
