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

// Address: 统一存储 IPv4 和 IPv6 地址 (V4MAPPED 格式, RFC 4291)。
// IPv4 地址存放在 in6_._in6d[3] (即 ::ffff:a.b.c.d 形式),端口以主机字节序保存。

#include <cstddef>
#include <cstdint>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if !defined(PACKED_ATTRIBUTE)
	#if defined(__GNUC__) || defined(__clang__)
		#define PACKED_ATTRIBUTE __attribute__((__packed__))
	#else
		#define PACKED_ATTRIBUTE
	#endif
#endif

#if !defined(ALIGNED_ATTRIBUTE)
	#if defined(__GNUC__) || defined(__clang__)
		#define ALIGNED_ATTRIBUTE(x) __attribute__((aligned(x)))
	#else
		#define ALIGNED_ATTRIBUTE(x)
	#endif
#endif

namespace utp {

class PACKED_ATTRIBUTE Address {
public:
	Address();
	explicit Address(const sockaddr_storage* sa, socklen_t len);

	bool operator==(const Address& rhs) const;
	bool operator!=(const Address& rhs) const;

	sockaddr_storage get_sockaddr_storage(socklen_t* len = nullptr) const;
	const char* fmt(char* s, std::size_t len) const;

	std::uint32_t compute_hash() const;

private:
	void set(const sockaddr_storage* sa, socklen_t len);
	std::uint8_t get_family() const;

	// 值总是以网络字节序存储
	union {
		std::uint8_t  _in6[16];
		std::uint16_t _in6w[8];
		std::uint32_t _in6d[4];
		in6_addr      _in6addr;
	} in6_;

	// 主机字节序
	std::uint16_t port_;
} ALIGNED_ATTRIBUTE(4);

}  // namespace utp
