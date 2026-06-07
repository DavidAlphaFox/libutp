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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "libutp_inet_ntop.h"


//######################################################################
/**
 * @brief libutp::inet_ntop 的 Windows 实现
 *
 * 借助 Winsock 提供的 WSAAddressToStringA 函数将数值型 IP 地址转换为字符串。
 * 由于 Windows XP 之前的平台没有 inet_ntop，本函数为这些平台提供兼容实现。
 *
 * @param af     地址族：AF_INET（IPv4）或 AF_INET6（IPv6）
 * @param src    指向数值型 IP 地址（in_addr 或 in6_addr）的指针
 * @param dest   输出字符串缓冲区
 * @param length 输出缓冲区的字节长度
 * @return 成功返回 dest，参数非法或调用失败时返回 NULL
 */
const char *libutp::inet_ntop(int af, const void *src, char *dest, size_t length)
{
	// 仅支持 IPv4 与 IPv6 两种地址族，其它情况直接返回 NULL
	if (af != AF_INET && af != AF_INET6)
	{
		return NULL;
	}

	// 使用通用 SOCKADDR_STORAGE 存储任意类型的 sockaddr
	SOCKADDR_STORAGE address;
	DWORD address_length;

	if (af == AF_INET)
	{
		// IPv4：填充 sockaddr_in 各个字段
		address_length = sizeof(sockaddr_in);
		sockaddr_in* ipv4_address = (sockaddr_in*)(&address);
		ipv4_address->sin_family = AF_INET;
		ipv4_address->sin_port = 0;
		memcpy(&ipv4_address->sin_addr, src, sizeof(in_addr));
	}
	else // AF_INET6
	{
		// IPv6：填充 sockaddr_in6 各个字段
		address_length = sizeof(sockaddr_in6);
		sockaddr_in6* ipv6_address = (sockaddr_in6*)(&address);
		ipv6_address->sin6_family = AF_INET6;
		ipv6_address->sin6_port = 0;
		ipv6_address->sin6_flowinfo = 0;
		// hmmm —— 保留原作者的疑问性注释（作用域 ID 未设置）
		ipv6_address->sin6_scope_id = 0;
		memcpy(&ipv6_address->sin6_addr, src, sizeof(in6_addr));
	}

	// 调用 Winsock 将 sockaddr 转换为字符串
	DWORD string_length = (DWORD)(length);
	int result;
	result = WSAAddressToStringA((sockaddr*)(&address),
								 address_length, 0, dest,
								 &string_length);

	// 该函数常见的失败原因之一是系统未启用 IPv6 协议栈

	return result == SOCKET_ERROR ? NULL : dest;
}

//######################################################################
/**
 * @brief libutp::inet_pton 的 Windows 实现
 *
 * 借助 Winsock 提供的 WSAStringToAddressA 函数将字符串形式的 IP 地址
 * 转换为数值形式。WinXP 之前没有 inet_pton，本函数为这些平台提供兼容实现。
 *
 * @param af   地址族：AF_INET（IPv4）或 AF_INET6（IPv6）
 * @param src  输入字符串（如 "192.168.0.1"）
 * @param dest 输出数值型 IP 地址的缓冲区（in_addr 或 in6_addr）
 * @return 成功返回 1，调用失败返回 -1
 */
int libutp::inet_pton(int af, const char* src, void* dest)
{
	// 仅支持 IPv4 与 IPv6 两种地址族
	if (af != AF_INET && af != AF_INET6)
	{
		return -1;
	}

	// 先用 Winsock 把字符串解析为通用 SOCKADDR_STORAGE
	SOCKADDR_STORAGE address;
	int address_length = sizeof(SOCKADDR_STORAGE);
	int result = WSAStringToAddressA((char*)(src), af, 0,
									 (sockaddr*)(&address),
									 &address_length);

	if (af == AF_INET)
	{
		if (result != SOCKET_ERROR)
		{
			// 解析成功：将 IPv4 地址部分拷贝至输出缓冲区
			sockaddr_in* ipv4_address =(sockaddr_in*)(&address);
			memcpy(dest, &ipv4_address->sin_addr, sizeof(in_addr));
		}
		else if (strcmp(src, "255.255.255.255") == 0)
		{
			// Winsock 不接受 "255.255.255.255"（受限广播地址）作为合法输入，
			// 这里做特殊处理：直接置为 INADDR_NONE
			((in_addr*)(dest))->s_addr = INADDR_NONE;
		}
	}
	else // AF_INET6
	{
		if (result != SOCKET_ERROR)
		{
			// 解析成功：将 IPv6 地址部分拷贝至输出缓冲区
			sockaddr_in6* ipv6_address = (sockaddr_in6*)(&address);
			memcpy(dest, &ipv6_address->sin6_addr, sizeof(in6_addr));
		}
	}

	return result == SOCKET_ERROR ? -1 : 1;
}
