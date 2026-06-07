#ifndef LIBUTP_INET_NTOP_H
#define LIBUTP_INET_NTOP_H

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

// 关于链接系统 inet_pton 与 inet_ntop 符号的说明：
// 1) 这些符号在 POSIX 系统上通常已经定义
// 2) 它们在 Vista 之前的 Windows 版本上并未定义
// 定义位置：
// ut_utils/src/sockaddr.cpp
// libutp/win32_inet_ntop.obj
//
// 当我们停止对 Windows XP 的支持后，可以直接 #include <ws2tcpip.h> 并使用系统函数
// 目前在 Windows 上无论什么构建配置，我们都使用自己的实现
// 原因：我们希望调试版本的运行行为尽可能接近发行版本
// 与其在调试时链接系统版本、再在发布版本中链接自己的版本，
// 不如在调试构建时就捕获问题

#if defined(_WIN32_WINNT)
#if _WIN32_WINNT >= 0x600 // Win32, post-XP
#include <ws2tcpip.h> // 引入 inet_ntop、inet_pton
#define INET_NTOP inet_ntop
#define INET_PTON inet_pton
#else
// Win32 预 XP 平台：使用我们自己的实现
#define INET_NTOP libutp::inet_ntop
#define INET_PTON libutp::inet_pton
#endif
#else // 非 WIN32 平台
#include <arpa/inet.h> // 引入 inet_ntop、inet_pton
#define INET_NTOP inet_ntop
#define INET_PTON inet_pton
#endif

//######################################################################
//######################################################################
// libutp 命名空间：包含跨平台网络地址转换辅助函数
namespace libutp {


//######################################################################
/**
 * @brief 将数值型 IP 地址转换为可读字符串（inet_ntop 的跨平台实现）
 *
 * @param af     地址族：AF_INET（IPv4）或 AF_INET6（IPv6）
 * @param src    指向数值型 IP 地址（in_addr 或 in6_addr）的指针
 * @param dest   输出字符串缓冲区
 * @param length 输出缓冲区的字节长度
 * @return 成功返回 dest，失败返回 NULL
 */
const char *inet_ntop(int af, const void *src, char *dest, size_t length);

//######################################################################
/**
 * @brief 将可读字符串形式的 IP 地址转换为数值形式（inet_pton 的跨平台实现）
 *
 * @param af   地址族：AF_INET（IPv4）或 AF_INET6（IPv6）
 * @param src  输入字符串（如 "192.168.0.1"）
 * @param dest 输出数值型 IP 地址的缓冲区（in_addr 或 in6_addr）
 * @return 成功返回 1，输入非法返回 0，地址族不支持返回 -1
 */
int inet_pton(int af, const char* src, void* dest);


} // 命名空间 libutp 结束

#endif // LIBUTP_INET_NTOP_H