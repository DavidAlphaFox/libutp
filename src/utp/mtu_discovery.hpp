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

// 设计说明:
// MtuDiscovery 管理 Path MTU Discovery 的二分搜索过程。
// 它通过在常规数据包中嵌入探测包来探测路径最大传输单元,
// 使用二分搜索逐步逼近真实 MTU 值。
//
// 调用方向: UtpSocket → MtuDiscovery (单向)
// MtuDiscovery 不持有 UtpSocket 的完整引用, 仅保存指针用于日志记录。
//
// 实现细节: 需要 owner_->log() 的方法 (reset/search_update/handle_*)
// 在 utp_internal.cpp 中定义 (位于 UtpSocket 完整定义之后),
// 因为本头文件被包含在 UtpSocket 定义之前, 此时 UtpSocket 是不完整类型。
// 不调用 owner_->log() 的简单方法 (访问器、set_probe、clear_probe 等) 仍
// 保持 inline, 充分利用 header-only 的优势。

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "utp.h"

// UtpSocket 的完整定义位于 utp_internal.cpp,
// MtuDiscovery 仅持有非拥有指针并通过该指针调用 UtpSocket::log()。
class UtpSocket;

class MtuDiscovery {
public:
	explicit MtuDiscovery(UtpSocket* owner)
		: owner_(owner) {}

	// 重置 MTU 搜索范围, 通常在初始化或定时重置时调用。
	void reset(uint32 udp_mtu, uint64 current_ms);

	// 推进二分搜索 (在 floor/ceiling 变更后调用)。
	void search_update(uint64 current_ms);

	// 返回有效载荷 MTU (扣除报文头)
	size_t effective_mtu(size_t header_size) const {
		return (mtu_last_ ? mtu_last_ : mtu_ceiling_) - header_size;
	}

	// 返回原始 MTU 值 (用于统计)
	uint32 raw_mtu() const {
		return mtu_last_ ? mtu_last_ : mtu_ceiling_;
	}

	// 设置在途探测包的序列号和大小
	void set_probe(uint32 seq, uint32 size) {
		mtu_probe_seq_ = seq;
		mtu_probe_size_ = size;
	}

	// 检查指定序列号是否为探测包 (且存在在途探测)
	bool is_probe(uint32 seq) const {
		return mtu_probe_seq_ != 0 && seq == mtu_probe_seq_;
	}

	// 探测包被确认, 提升 floor 并推进二分搜索。
	// 返回 true 表示处理了一个探测 ACK。
	bool handle_probe_ack(uint32 seq, uint64 current_ms);

	// 探测包超时 (RTO 触发): 若在飞包仅一个且为探测, 则收紧 ceiling。
	bool handle_probe_timeout(uint32 outstanding_seq, uint32 cur_window_packets, uint64 current_ms);

	// 处理探测包被视为"丢失"的通用路径 (DUPACK 等场景)。
	void handle_probe_loss(uint64 current_ms);

	// ICMP need-frag: 用 next_hop_mtu 收紧 ceiling 并直接采纳为当前 MTU。
	void handle_icmp_fragmentation(uint16 next_hop_mtu, uint64 current_ms);

	// ICMP 无有效 next_hop_mtu: 走标准二分, 取中点作为新 ceiling。
	void handle_icmp_unknown(uint64 current_ms);

	// 检查是否到了重置搜索的截止时间
	bool should_rediscover(uint64 current_ms) const {
		return mtu_discover_time_ < current_ms;
	}

	// 清除在途探测状态
	void clear_probe() {
		mtu_probe_seq_ = mtu_probe_size_ = 0;
	}

	// 直接将 mtu_last_ 同步到 mtu_ceiling_
	void set_last_to_ceiling() {
		mtu_last_ = mtu_ceiling_;
	}

	// -- 访问器 --
	uint32 floor() const       { return mtu_floor_; }
	uint32 ceiling() const     { return mtu_ceiling_; }
	uint32 last() const        { return mtu_last_; }
	uint32 probe_seq() const   { return mtu_probe_seq_; }
	uint32 probe_size() const  { return mtu_probe_size_; }

private:
	UtpSocket* owner_;
	uint64 mtu_discover_time_ = 0;  // 下一次重置搜索的时间
	uint32 mtu_ceiling_ = 0;        // 二分搜索上界
	uint32 mtu_floor_ = 0;          // 二分搜索下界
	uint32 mtu_last_ = 0;           // 当前生效的 MTU
	uint32 mtu_probe_seq_ = 0;      // 在途探测包的序列号 (0 表示无探测)
	uint32 mtu_probe_size_ = 0;     // 在途探测包的大小
};
