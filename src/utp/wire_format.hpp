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

// uTP 网络协议线格式定义
// 定义网络传输的数据包结构，字节布局必须与原始 PacketFormatV1 匹配以保持协议兼容性

#include <cstddef>
#include <cstdint>
#include "endian.hpp"

namespace utp::wire {

// 数据包类型枚举 (4 位，存储在 ver_type 的高半字节)
enum PacketType : std::uint8_t {
	ST_DATA       = 0,  // 数据包：传输实际数据
	ST_FIN        = 1,  // 结束包：正常关闭连接
	ST_STATE      = 2,  // 状态包：仅用于窗口大小更新
	ST_RESET      = 3,  // 重置包：强制关闭连接
	ST_SYN        = 4,  // 同步包：建立连接
	ST_NUM_STATES = 5,  // 数据包类型总数
};

// 协议版本
constexpr std::uint8_t PROTOCOL_VERSION = 1;

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(1)
#else
	#pragma pack(push, 1)
#endif

// uTP 数据包头部结构 (20 字节)
// 包含版本、类型、连接 ID、时间戳和窗口信息等关键字段
struct PacketFormatV1 {
	std::uint8_t ver_type;         // 版本(低4位) + 类型(高4位)

	[[nodiscard]] std::uint8_t version() const { return ver_type & 0xf; }   // 获取协议版本
	[[nodiscard]] std::uint8_t type()    const { return ver_type >> 4; }     // 获取数据包类型
	void set_version(std::uint8_t v) { ver_type = (ver_type & 0xf0) | (v & 0xf); }  // 设置协议版本
	void set_type(std::uint8_t t)    { ver_type = (ver_type & 0x0f) | (t << 4); }    // 设置数据包类型

	std::uint8_t ext;                // 扩展字段
	uint16_big connid;               // 连接 ID
	uint32_big tv_usec;              // 发送时间戳 (微秒)
	uint32_big reply_micro;          // 对端的延迟时间戳 (微秒)
	uint32_big windowsize;           // 接收窗口大小
	uint16_big seq_nr;               // 数据包序列号
	uint16_big ack_nr;               // 确认序列号
};

static_assert(sizeof(PacketFormatV1) == 20, "PacketFormatV1 must be exactly 20 bytes");

// ACK 数据包结构 (26 字节)
// 包含选择性确认信息
struct PacketFormatAckV1 {
	PacketFormatV1 pf;          // 基础数据包头部
	std::uint8_t ext_next;      // 下一个扩展类型
	std::uint8_t ext_len;       // 扩展长度
	std::uint8_t acks[4];       // 选择性确认位图
};

static_assert(sizeof(PacketFormatAckV1) == 26, "PacketFormatAckV1 must be exactly 26 bytes");

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(0)
#else
	#pragma pack(pop)
#endif

// 连接状态机
enum ConnState : std::uint8_t {
	CS_UNINITIALIZED  = 0,  // 未初始化
	CS_IDLE,                  // 空闲：连接已关闭
	CS_SYN_SENT,              // 已发送 SYN：等待 SYN-ACK
	CS_SYN_RECV,              // 已收到 SYN：等待连接建立
	CS_CONNECTED,             // 已连接：可以传输数据
	CS_CONNECTED_FULL,        // 连接已满：接收窗口已满
	CS_RESET,                 // 重置：连接被强制关闭
	CS_DESTROY,               // 销毁：连接正在清理资源
};

// 数据包大小分类 (用于统计)
enum PacketSizeBucket : std::uint32_t {
	PACKET_SIZE_EMPTY_BUCKET = 0,  // 空包 (23 字节)
	PACKET_SIZE_SMALL_BUCKET = 1,  // 小包 (373 字节)
	PACKET_SIZE_MID_BUCKET   = 2,  // 中包 (723 字节)
	PACKET_SIZE_BIG_BUCKET   = 3,  // 大包 (1400 字节)
	PACKET_SIZE_HUGE_BUCKET  = 4,  // 超大包 (1435 字节)
};

// 根据分类获取数据包大小
inline constexpr std::uint32_t packet_size_from_bucket(PacketSizeBucket b) {
	switch (b) {
		case PACKET_SIZE_EMPTY_BUCKET: return 23;
		case PACKET_SIZE_SMALL_BUCKET: return 373;
		case PACKET_SIZE_MID_BUCKET:   return 723;
		case PACKET_SIZE_BIG_BUCKET:   return 1400;
		default: return 0;
	}
}

// 根据数据包大小获取分类（packet_size_from_bucket 的正向映射，
// 边界取自同一处，收发统计共用，避免两份阶梯漂移）
inline constexpr PacketSizeBucket packet_size_bucket(std::size_t len) {
	if (len <= packet_size_from_bucket(PACKET_SIZE_EMPTY_BUCKET)) return PACKET_SIZE_EMPTY_BUCKET;
	if (len <= packet_size_from_bucket(PACKET_SIZE_SMALL_BUCKET)) return PACKET_SIZE_SMALL_BUCKET;
	if (len <= packet_size_from_bucket(PACKET_SIZE_MID_BUCKET))   return PACKET_SIZE_MID_BUCKET;
	if (len <= packet_size_from_bucket(PACKET_SIZE_BIG_BUCKET))   return PACKET_SIZE_BIG_BUCKET;
	return PACKET_SIZE_HUGE_BUCKET;
}

}  // namespace utp::wire
