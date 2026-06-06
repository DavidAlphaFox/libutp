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

[[nodiscard]] constexpr inline std::uint16_t host_to_network(std::uint16_t i) { return htons(i); }
[[nodiscard]] constexpr inline std::uint32_t host_to_network(std::uint32_t i) { return htonl(i); }
[[nodiscard]] constexpr inline std::int32_t host_to_network(std::int32_t i) { return htonl(i); }
[[nodiscard]] constexpr inline std::uint16_t network_to_host(std::uint16_t i) { return ntohs(i); }
[[nodiscard]] constexpr inline std::uint32_t network_to_host(std::uint32_t i) { return ntohl(i); }
[[nodiscard]] constexpr inline std::int32_t network_to_host(std::int32_t i) { return ntohl(i); }

}  // namespace aux

template<typename T>
struct PACKED_ATTRIBUTE big_endian
{
	constexpr big_endian() = default;
	constexpr big_endian(T val) : m_integer(aux::host_to_network(val)) {}

	constexpr T operator=(T i) { m_integer = aux::host_to_network(i); return i; }
	[[nodiscard]] constexpr operator T() const { return aux::network_to_host(m_integer); }
private:
	T m_integer;
};

using int32_big = big_endian<std::int32_t>;
using uint32_big = big_endian<std::uint32_t>;
using uint16_big = big_endian<std::uint16_t>;

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(0)
#else
	#pragma pack(pop)
#endif

template<typename T>
constexpr inline void zeromem(T* a, std::size_t count = 1) { std::memset(a, 0, count * sizeof(T)); }

}  // namespace utp
