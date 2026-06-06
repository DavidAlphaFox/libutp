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
#include <cstring>

#if defined(_WIN32)
	#include <winsock2.h>
#else
	#include <arpa/inet.h>
#endif

namespace utp {

// 大端字节序处理模块
// 提供 big_endian<T> 模板用于网络协议中的字节序转换
// 网络传输使用大端字节序，需要在不同主机字节序间转换

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(1)
#else
	#pragma pack(push, 1)
#endif

#if !defined(PACKED_ATTRIBUTE)
	#if defined(__GNUC__) || defined(__clang__)
		#define PACKED_ATTRIBUTE __attribute__((__packed__))
	#else
		#define PACKED_ATTRIBUTE
	#endif
#endif

namespace aux {

// 主机字节序到网络字节序转换
[[nodiscard]] constexpr inline std::uint16_t host_to_network(std::uint16_t i) { return htons(i); }
[[nodiscard]] constexpr inline std::uint32_t host_to_network(std::uint32_t i) { return htonl(i); }
[[nodiscard]] constexpr inline std::int32_t host_to_network(std::int32_t i) { return htonl(i); }

// 网络字节序到主机字节序转换
[[nodiscard]] constexpr inline std::uint16_t network_to_host(std::uint16_t i) { return ntohs(i); }
[[nodiscard]] constexpr inline std::uint32_t network_to_host(std::uint32_t i) { return ntohl(i); }
[[nodiscard]] constexpr inline std::int32_t network_to_host(std::int32_t i) { return ntohl(i); }

}  // namespace aux

// 大端字节序类型包装器
// 自动处理主机字节序与网络字节序的转换
// 构造时自动转换为网络字节序，读取时自动转换为主机字节序
template<typename T>
struct PACKED_ATTRIBUTE big_endian
{
	constexpr big_endian() = default;
	constexpr big_endian(T val) : m_integer(aux::host_to_network(val)) {}  // 转换为网络字节序

	constexpr T operator=(T i) { m_integer = aux::host_to_network(i); return i; }  // 赋值时转换
	[[nodiscard]] constexpr operator T() const { return aux::network_to_host(m_integer); }  // 读取时转换
private:
	T m_integer;  // 以网络字节序存储
};

// 大端字节序类型别名
using int32_big = big_endian<std::int32_t>;
using uint32_big = big_endian<std::uint32_t>;
using uint16_big = big_endian<std::uint16_t>;

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(0)
#else
	#pragma pack(pop)
#endif

// 内存清零工具函数
// 将指定数量的 T 类型的内存清零
template<typename T>
constexpr inline void zeromem(T* a, std::size_t count = 1) { std::memset(a, 0, count * sizeof(T)); }

}  // namespace utp
