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

// 日志角色接口（Role Interface / 依赖倒置）：
// 把"可写日志的对象"抽象出来，让子组件（如 MtuDiscovery）只依赖这个窄接口，
// 而不必依赖庞大的 UtpSocket。UtpSocket 实现该接口（负责按 level 过滤、加地址前缀
// 并转发给上层 log 回调）。这样子组件可在单元测试中接入一个假的 ILogger。

#include <cstdarg>

namespace utp {

struct ILogger {
	virtual ~ILogger() = default;

	// 平台/上层实现：按 level 过滤并输出。va_list 版本便于上层转发。
	virtual void vlog(int level, const char* fmt, va_list ap) = 0;

	// 便捷变参包装（非虚），调用点用法与 printf 一致。
	void log(int level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
		__attribute__((format(printf, 3, 4)))
#endif
	{
		va_list ap;
		va_start(ap, fmt);
		vlog(level, fmt, ap);
		va_end(ap);
	}
};

}  // namespace utp
