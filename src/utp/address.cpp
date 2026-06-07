// vim:set ts=4 sw=4 ai:

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

#include "utp/address.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

#include "libutp_inet_ntop.h"

namespace utp {

// -----------------------------------------------------------------------------
// Address::get_family
// 功能：判断当前地址的地址族。
//
// 返回值：
//   AF_INET  - 如果地址是 IPv4 映射地址（::ffff:a.b.c.d 格式）
//   AF_INET6 - 如果地址是原生 IPv6 地址
//
// 说明：
//   内部存储统一使用 IPv6 格式（128 位），IPv4 地址通过 V4MAPPED 方式
//   （RFC 4291）映射到 IPv6 地址空间中。SH4 平台使用手动比较替代
//   IN6_IS_ADDR_V4MAPPED 宏。
// -----------------------------------------------------------------------------
std::uint8_t utp::Address::get_family() const
{
	#if defined(__sh__)
		return ((in6_._in6d[0] == 0) && (in6_._in6d[1] == 0) && (in6_._in6d[2] == htonl(0xffff)) != 0) ?
			AF_INET : AF_INET6;
	#else
		return (IN6_IS_ADDR_V4MAPPED(&in6_._in6addr) != 0) ? AF_INET : AF_INET6;
	#endif // defined(__sh__)
}

// -----------------------------------------------------------------------------
// Address::operator==
// 功能：地址相等比较。
//
// 参数：
//   rhs - 右侧比较的 Address 对象
//
// 返回值：
//   true  - 两个地址相等
//   false - 两个地址不相等
//
// 说明：
//   先比较 port_（端口号），若相同再比较 in6_ 的 16 字节地址内容。
//   若两个对象引用相同（同一实例），直接返回 true。
// -----------------------------------------------------------------------------
bool utp::Address::operator==(const Address& rhs) const
{
	if (&rhs == this)
		return true;
	if (port_ != rhs.port_)
		return false;
	return std::memcmp(in6_._in6, rhs.in6_._in6, sizeof(in6_._in6)) == 0;
}

// -----------------------------------------------------------------------------
// Address::operator!=
// 功能：地址不等比较。
//
// 参数：
//   rhs - 右侧比较的 Address 对象
//
// 返回值：
//   true  - 两个地址不相等
//   false - 两个地址相等
//
// 说明：
//   直接取反 operator== 的结果。
// -----------------------------------------------------------------------------
bool utp::Address::operator!=(const Address& rhs) const
{
	return !(*this == rhs);
}

// -----------------------------------------------------------------------------
// Address::compute_hash
// 功能：计算地址的哈希值。
//
// 返回值：
//   32 位无符号整数哈希值
//
// 说明：
//   使用旋转哈希算法：按 4 字节块异或并左循环移位 13 位，
//   剩余字节逐个异或并左循环移位 8 位，最后异或 port_。
// -----------------------------------------------------------------------------
std::uint32_t utp::Address::compute_hash() const {
	std::uint32_t h = 0;
	const std::uint8_t *p = reinterpret_cast<const std::uint8_t *>(&in6_);
	std::size_t n = sizeof(in6_);
	while (n >= 4) {
		h ^= *reinterpret_cast<const std::uint32_t *>(p);
		p += sizeof(std::uint32_t);
		h = (h << 13) | (h >> 19);
		n -= 4;
	}
	while (n != 0) {
		h ^= *p++;
		h = (h << 8) | (h >> 24);
		n--;
	}
	return h ^ port_;
}

// -----------------------------------------------------------------------------
// Address::set
// 功能：从 sockaddr_storage 设置内部地址。
//
// 参数：
//   sa  - 指向 sockaddr_storage 的指针
//   len - sockaddr 结构体的长度
//
// 说明：
//   若 sa->ss_family 为 AF_INET，将 IPv4 地址映射为 V4MAPPED 格式
//   （前 10 字节为 0，后 2 字节为 0xffff，最后 4 字节为 IPv4 地址）；
//   若为 AF_INET6，直接复制 sin6_addr 和端口。
// -----------------------------------------------------------------------------
void utp::Address::set(const sockaddr_storage* sa, socklen_t len)
{
	if (sa->ss_family == AF_INET) {
		assert(len >= sizeof(sockaddr_in));
		const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(sa);
		in6_._in6w[0] = 0;
		in6_._in6w[1] = 0;
		in6_._in6w[2] = 0;
		in6_._in6w[3] = 0;
		in6_._in6w[4] = 0;
		in6_._in6w[5] = 0xffff;
		in6_._in6d[3] = sin->sin_addr.s_addr;
		port_ = ntohs(sin->sin_port);
	} else {
		assert(len >= sizeof(sockaddr_in6));
		const sockaddr_in6 *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
		in6_._in6addr = sin6->sin6_addr;
		port_ = ntohs(sin6->sin6_port);
	}
}

// -----------------------------------------------------------------------------
// Address::Address(const sockaddr_storage*, socklen_t)
// 功能：从 sockaddr 构造 Address 对象。
//
// 参数：
//   sa  - 指向 sockaddr_storage 的指针
//   len - sockaddr 结构体的长度
//
// 说明：
//   调用 set() 方法完成初始化。
// -----------------------------------------------------------------------------
utp::Address::Address(const sockaddr_storage* sa, socklen_t len)
{
	set(sa, len);
}

// -----------------------------------------------------------------------------
// Address::Address()
// 功能：默认构造函数。
//
// 说明：
//   构造一个空的 IPv4 地址（通过构造全零的 sockaddr_storage 并设置
//   ss_family 为 AF_INET，再调用 set() 完成初始化）。
// -----------------------------------------------------------------------------
utp::Address::Address()
{
	sockaddr_storage sa;
	socklen_t len = sizeof(sockaddr_storage);
	std::memset(&sa, 0, len);
	sa.ss_family = AF_INET;
	set(&sa, len);
}

// -----------------------------------------------------------------------------
// Address::get_sockaddr_storage
// 功能：将内部地址转换回平台原生 sockaddr 结构。
//
// 参数：
//   len - 输出参数，返回生成的 sockaddr 结构体长度（可为 nullptr）
//
// 返回值：
//   填充好的 sockaddr_storage 结构体
//
// 说明：
//   若内部地址为 V4MAPPED 格式，反映射为 sockaddr_in；
//   若为原生 IPv6，映射为 sockaddr_in6。
// -----------------------------------------------------------------------------
sockaddr_storage utp::Address::get_sockaddr_storage(socklen_t *len) const
{
	sockaddr_storage sa;
	const std::uint8_t family = get_family();
	if (family == AF_INET) {
		sockaddr_in *sin = reinterpret_cast<sockaddr_in *>(&sa);
		if (len) *len = sizeof(sockaddr_in);
		std::memset(sin, 0, sizeof(sockaddr_in));
		sin->sin_family = static_cast<decltype(sin->sin_family)>(family);
		sin->sin_port = htons(port_);
		sin->sin_addr.s_addr = in6_._in6d[3];
	} else {
		sockaddr_in6 *sin6 = reinterpret_cast<sockaddr_in6 *>(&sa);
		std::memset(sin6, 0, sizeof(sockaddr_in6));
		if (len) *len = sizeof(sockaddr_in6);
		sin6->sin6_family = static_cast<decltype(sin6->sin6_family)>(family);
		sin6->sin6_addr = in6_._in6addr;
		sin6->sin6_port = htons(port_);
	}
	return sa;
}

// -----------------------------------------------------------------------------
// Address::fmt
// 功能：将地址格式化为可读的字符串。
//
// 参数：
//   s   - 输出缓冲区指针
//   len - 缓冲区长度
//
// 返回值：
//   指向输出缓冲区 s 的指针
//
// 说明：
//   IPv4 格式："a.b.c.d:port"
//   IPv6 格式："[::1]:port"
//   使用 INET_NTOP 进行地址到字符串的转换。
// -----------------------------------------------------------------------------
// #define addrfmt(x, s) x.fmt(s, sizeof(s))
const char *utp::Address::fmt(char *s, std::size_t len) const
{
	std::memset(s, 0, len);
	const std::uint8_t family = get_family();
	char *i;
	if (family == AF_INET) {
		INET_NTOP(family, reinterpret_cast<const std::uint32_t *>(&in6_._in6d[3]), s, len);
		i = s;
		while (*++i) {}
	} else {
		i = s;
		*i++ = '[';
		INET_NTOP(family, reinterpret_cast<const in6_addr *>(&in6_._in6addr), i, len-1);
		while (*++i) {}
		*i++ = ']';
	}
	std::snprintf(i, len - (i-s), ":%u", port_);
	return s;
}

}  // namespace utp
