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

// Replaces utp_types.h for internal C++ code. Public C API still uses utp_types.h.

#include <cstdint>

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

#include <sys/types.h>

namespace utp {

using byte = std::uint8_t;
#if !defined(PACKED_ATTRIBUTE)
	#if defined(__GNUC__) || defined(__clang__)
		#define PACKED_ATTRIBUTE __attribute__((__packed__))
	#else
		#define PACKED_ATTRIBUTE
	#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
	#define ALIGNED_ATTRIBUTE(x) __attribute__((aligned(x)))
#else
	#define ALIGNED_ATTRIBUTE(x)
#endif

#if !defined(_MSC_VER)
	using ssize_t = ::ssize_t;
#endif

#ifdef _WIN32
	#define IP_OPT_DONTFRAG IP_DONTFRAGMENT
	#define SHUT_RD   SD_RECEIVE
	#define SHUT_WR   SD_SEND
	#define SHUT_RDWR SD_BOTH
#else
	#ifdef IP_DONTFRAG
		#define IP_OPT_DONTFRAG IP_DONTFRAG
	#elif defined(IP_DONTFRAGMENT)
		#define IP_OPT_DONTFRAG IP_DONTFRAGMENT
	#endif
#endif

}  // namespace utp
