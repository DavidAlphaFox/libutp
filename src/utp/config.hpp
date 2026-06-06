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

#include <cstddef>
#include <cstdint>

// uTP 协议配置常量
// 定义协议运行所需的各种参数，包括超时、缓冲区、窗口和拥塞控制等配置
namespace utp::config {

// 超时和间隔设置 (单位：毫秒)
constexpr uint32_t TIMEOUT_CHECK_INTERVAL = 500;      // 超时检查间隔
constexpr uint32_t RST_INFO_TIMEOUT = 10000;          // RESET 信息超时
constexpr uint32_t KEEPALIVE_INTERVAL = 29000;        // 保活间隔

// 拥塞控制参数
constexpr uint32_t CCONTROL_TARGET = 100 * 1000;      // 拥塞控制目标延迟 (100毫秒，单位：微秒)
constexpr uint32_t MAX_CWND_INCREASE_BYTES_PER_RTT = 3000;  // 每个 RTT 最多增加的拥塞窗口字节数
constexpr uint32_t CUR_DELAY_SIZE = 3;                // 当前延迟历史记录数量
constexpr uint32_t DELAY_BASE_HISTORY = 13;           // 延迟基线历史记录数量
constexpr uint32_t MAX_WINDOW_DECAY = 100;            // 最大窗口衰减时间 (毫秒)

// 缓冲区大小
constexpr size_t REORDER_BUFFER_SIZE = 32;            // 重排序缓冲区大小
constexpr size_t REORDER_BUFFER_MAX_SIZE = 1024;      // 重排序缓冲区最大值
constexpr size_t OUTGOING_BUFFER_MAX_SIZE = 1024;     // 发送缓冲区最大值

// 数据包大小
constexpr size_t PACKET_SIZE = 1435;                  // 标准数据包大小
constexpr uint32_t PACKET_SIZE_EMPTY_BUCKET = 0;      // 空包分类
constexpr uint32_t PACKET_SIZE_EMPTY = 23;            // 空包大小
constexpr uint32_t PACKET_SIZE_SMALL_BUCKET = 1;      // 小包分类
constexpr uint32_t PACKET_SIZE_SMALL = 373;           // 小包大小
constexpr uint32_t PACKET_SIZE_MID_BUCKET = 2;        // 中包分类
constexpr uint32_t PACKET_SIZE_MID = 723;             // 中包大小
constexpr uint32_t PACKET_SIZE_BIG_BUCKET = 3;        // 大包分类
constexpr uint32_t PACKET_SIZE_BIG = 1400;            // 大包大小
constexpr uint32_t PACKET_SIZE_HUGE_BUCKET = 4;       // 超大包分类

// 窗口参数
constexpr uint32_t MIN_WINDOW_SIZE = 10;                      // 最小窗口大小
constexpr uint32_t DUPLICATE_ACKS_BEFORE_RESEND = 3;          // 重传前需要的重复 ACK 数量
constexpr uint32_t ACK_NR_ALLOWED_WINDOW = DUPLICATE_ACKS_BEFORE_RESEND;  // ACK 序列号允许的窗口
constexpr uint32_t RST_INFO_LIMIT = 1000;                     // RESET 信息限制

// 位掩码
constexpr uint16_t SEQ_NR_MASK = 0xFFFF;             // 序列号掩码 (16 位)
constexpr uint16_t ACK_NR_MASK = 0xFFFF;             // ACK 序列号掩码 (16 位)
constexpr uint32_t TIMESTAMP_MASK = 0xFFFFFFFF;      // 时间戳掩码 (32 位)

}  // namespace utp::config
