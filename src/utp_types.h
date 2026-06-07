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

#ifndef __UTP_TYPES_H__
#define __UTP_TYPES_H__

// 允许 libutp 的使用者或依赖项在外部覆盖 PACKED_ATTRIBUTE
#ifndef PACKED_ATTRIBUTE
#if defined BROKEN_GCC_STRUCTURE_PACKING && defined __GNUC__
	// 用于那些接受但不支持 #pragma pack 的 GCC 工具链
	// 参考 http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
	#define PACKED_ATTRIBUTE __attribute__((__packed__))
#else
	// 默认情况下该宏定义为空（即不要求紧凑打包）
	#define PACKED_ATTRIBUTE
#endif // defined BROKEN_GCC_STRUCTURE_PACKING && defined __GNUC__
#endif // ndef PACKED_ATTRIBUTE

// 对齐属性宏：GCC 下使用 __attribute__((aligned))，其它编译器留空
#ifdef __GNUC__
	#define ALIGNED_ATTRIBUTE(x)  __attribute__((aligned (x)))
#else
	#define ALIGNED_ATTRIBUTE(x)
#endif

// hash.cpp 需要 socket 相关的定义，因此这部分网络相关代码被放在 utypes.h 中
#ifdef WIN32
	// Windows 平台：包含 WinSock 头文件并定义跨平台兼容宏
	#define _CRT_SECURE_NO_DEPRECATE
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	// 设置“不分片”IP 选项：Windows 名为 IP_DONTFRAGMENT
	#define IP_OPT_DONTFRAG IP_DONTFRAGMENT
	// shutdown() 方向参数在 WinSock 中名为 SD_*
	#define SHUT_RD SD_RECEIVE
	#define SHUT_WR SD_SEND
	#define SHUT_RDWR SD_BOTH
#else
	// POSIX 平台：包含 BSD Socket 头文件
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
	#include <sys/socket.h>

	// 兼容不同系统的“不分片”选项名
	#ifdef IP_DONTFRAG
		#define IP_OPT_DONTFRAG IP_DONTFRAG
	#elif defined IP_DONTFRAGMENT
		#define IP_OPT_DONTFRAG IP_DONTFRAGMENT
	#else
		//#warning "I don't know how to set DF bit on this system"
		// 未知系统上无法设置 DF 位，宏保持未定义
	#endif
#endif

// MSVC 编译器没有定义 ssize_t，通过 SSIZE_T 提供兼容类型
#ifdef _MSC_VER
	#include <BaseTsd.h>
	typedef SSIZE_T ssize_t;
#endif

// POSIX 系统上 SOCKADDR_STORAGE 是标准 socket 头中已定义的类型
#ifdef POSIX
	typedef struct sockaddr_storage SOCKADDR_STORAGE;
#endif

// printf 中用于打印 uint64 类型的格式串：
// Windows 下为 %I64u，其它平台（Linux/macOS）为 %Lu
#ifdef WIN32
	#define I64u "%I64u"
#else
	#define I64u "%Lu"
#endif

// 标准类型别名：简化书写并保持跨平台一致
typedef unsigned char byte;   // 单字节
typedef unsigned char uint8;  // 无符号 8 位整数
typedef signed char int8;     // 有符号 8 位整数
typedef unsigned short uint16;// 无符号 16 位整数
typedef signed short int16;   // 有符号 16 位整数
typedef unsigned int uint;    // 与 sizeof(void*) 相关的无符号整数
typedef unsigned int uint32;  // 无符号 32 位整数
typedef signed int int32;     // 有符号 32 位整数

// 64 位整数类型：MSVC 使用 __int64，其它平台使用 long long
#ifdef _MSC_VER
typedef unsigned __int64 uint64;
typedef signed __int64 int64;
#else
typedef unsigned long long uint64;
typedef long long int64;
#endif

/* 编译期断言：当表达式为假时，数组大小为 -1，编译会失败 */
#ifndef CASSERT
#define CASSERT( exp, name ) typedef int is_not_##name [ (exp ) ? 1 : -1 ];
#endif

// 静态断言：确保 64 位整数确实占用 8 字节
CASSERT(8 == sizeof(uint64), sizeof_uint64_is_8)
CASSERT(8 == sizeof(int64), sizeof_int64_is_8)

// 64 位有符号整数的最大值，标准头中可能没有定义
#ifndef INT64_MAX
#define INT64_MAX 0x7fffffffffffffffLL
#endif

// 始终遵循 ANSI C 的字符串类型别名
typedef const char * cstr; // 只读字符串
typedef char * str;        // 可写字符串

// 在 C 编译环境下（不支持 C++ 内建 bool）使用 uint8 作为 bool
#ifndef __cplusplus
typedef uint8 bool;
#endif

#endif //__UTP_TYPES_H__
