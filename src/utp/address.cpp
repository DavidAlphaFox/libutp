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

std::uint8_t utp::Address::get_family() const
{
	#if defined(__sh__)
		return ((in6_._in6d[0] == 0) && (in6_._in6d[1] == 0) && (in6_._in6d[2] == htonl(0xffff)) != 0) ?
			AF_INET : AF_INET6;
	#else
		return (IN6_IS_ADDR_V4MAPPED(&in6_._in6addr) != 0) ? AF_INET : AF_INET6;
	#endif // defined(__sh__)
}

bool utp::Address::operator==(const Address& rhs) const
{
	if (&rhs == this)
		return true;
	if (port_ != rhs.port_)
		return false;
	return std::memcmp(in6_._in6, rhs.in6_._in6, sizeof(in6_._in6)) == 0;
}

bool utp::Address::operator!=(const Address& rhs) const
{
	return !(*this == rhs);
}

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

utp::Address::Address(const sockaddr_storage* sa, socklen_t len)
{
	set(sa, len);
}

utp::Address::Address()
{
	sockaddr_storage sa;
	socklen_t len = sizeof(sockaddr_storage);
	std::memset(&sa, 0, len);
	sa.ss_family = AF_INET;
	set(&sa, len);
}

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
