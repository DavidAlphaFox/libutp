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

// 平台相关类型定义和宏
// 替代内部 C++ 代码使用的 utp_types.h，公共 C API 仍使用 utp_types.h
// 提供跨平台的类型定义和编译器特性支持

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

// 字节类型定义：用于协议数据缓冲区
using byte = std::uint8_t;

// PACKED_ATTRIBUTE：结构体紧凑打包属性 (无填充)
// 用于确保网络协议结构体在内存中紧凑排列，与网络字节流一致
#if !defined(PACKED_ATTRIBUTE)
	#if defined(__GNUC__) || defined(__clang__)
		#define PACKED_ATTRIBUTE __attribute__((__packed__))
	#else
		#define PACKED_ATTRIBUTE
	#endif
#endif

// ALIGNED_ATTRIBUTE：内存对齐属性
// 用于指定结构体或变量的对齐要求
#if defined(__GNUC__) || defined(__clang__)
	#define ALIGNED_ATTRIBUTE(x) __attribute__((aligned(x)))
#else
	#define ALIGNED_ATTRIBUTE(x)
#endif

// 在非 MSVC 平台上提供 ssize_t 类型定义
#if !defined(_MSC_VER)
	using ssize_t = ::ssize_t;
#endif

// Windows 平台特定的套接字宏定义
#ifdef _WIN32
	#define IP_OPT_DONTFRAG IP_DONTFRAGMENT   // IP 禁止分片选项
	#define SHUT_RD   SD_RECEIVE              // 关闭接收
	#define SHUT_WR   SD_SEND                 // 关闭发送
	#define SHUT_RDWR SD_BOTH                 // 关闭读写
#else
	// 非 Windows 平台的 IP 禁止分片选项
	#ifdef IP_DONTFRAG
		#define IP_OPT_DONTFRAG IP_DONTFRAG
	#elif defined(IP_DONTFRAGMENT)
		#define IP_OPT_DONTFRAG IP_DONTFRAGMENT
	#endif
#endif

}  // namespace utp
