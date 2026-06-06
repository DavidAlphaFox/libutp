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

// 打包的 socket 地址结构
// 统一存储 IPv4 和 IPv6 地址，使用 V4MAPPED 格式（RFC 4291）
// IPv4 地址存储为 ::ffff:a.b.c.d 格式，位于 _in6d[3]
// 端口以主机字节序存储

#ifndef __UTP_PACKEDSOCKADDR_H__
#define __UTP_PACKEDSOCKADDR_H__

#include "utp_types.h"

struct PACKED_ATTRIBUTE PackedSockAddr {
	// 值总是以网络字节序存储
	union {
		byte _in6[16];		// IPv6 地址
		uint16 _in6w[8];	// IPv6，字访问（方便）
		uint32 _in6d[4];	// 双字访问
		in6_addr _in6addr;	// 方便访问
	} _in;

	// 主机字节序
	uint16 _port;

	#define _sin4 _in._in6d[3]	// IPv4 存储在映射后的位置

	#define _sin6 _in._in6
	#define _sin6w _in._in6w
	#define _sin6d _in._in6d

	byte get_family() const;
	bool operator==(const PackedSockAddr& rhs) const;
	bool operator!=(const PackedSockAddr& rhs) const;
	void set(const SOCKADDR_STORAGE* sa, socklen_t len);

	PackedSockAddr(const SOCKADDR_STORAGE* sa, socklen_t len);
	PackedSockAddr(void);

	SOCKADDR_STORAGE get_sockaddr_storage(socklen_t *len) const;
	cstr fmt(str s, size_t len) const;

	uint32 compute_hash() const;
} ALIGNED_ATTRIBUTE(4);

#endif //__UTP_PACKEDSOCKADDR_H__
