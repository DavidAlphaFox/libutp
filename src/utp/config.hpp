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

namespace utp::config {

// Timeout and interval settings
constexpr uint32_t TIMEOUT_CHECK_INTERVAL = 500;
constexpr uint32_t RST_INFO_TIMEOUT = 10000;
constexpr uint32_t KEEPALIVE_INTERVAL = 29000;

// Congestion control
constexpr uint32_t CCONTROL_TARGET = 100 * 1000;  // microseconds
constexpr uint32_t MAX_CWND_INCREASE_BYTES_PER_RTT = 3000;
constexpr uint32_t CUR_DELAY_SIZE = 3;
constexpr uint32_t DELAY_BASE_HISTORY = 13;
constexpr uint32_t MAX_WINDOW_DECAY = 100;  // ms

// Buffer sizes
constexpr size_t REORDER_BUFFER_SIZE = 32;
constexpr size_t REORDER_BUFFER_MAX_SIZE = 1024;
constexpr size_t OUTGOING_BUFFER_MAX_SIZE = 1024;

// Packet sizes
constexpr size_t PACKET_SIZE = 1435;
constexpr uint32_t PACKET_SIZE_EMPTY_BUCKET = 0;
constexpr uint32_t PACKET_SIZE_EMPTY = 23;
constexpr uint32_t PACKET_SIZE_SMALL_BUCKET = 1;
constexpr uint32_t PACKET_SIZE_SMALL = 373;
constexpr uint32_t PACKET_SIZE_MID_BUCKET = 2;
constexpr uint32_t PACKET_SIZE_MID = 723;
constexpr uint32_t PACKET_SIZE_BIG_BUCKET = 3;
constexpr uint32_t PACKET_SIZE_BIG = 1400;
constexpr uint32_t PACKET_SIZE_HUGE_BUCKET = 4;

// Window settings
constexpr uint32_t MIN_WINDOW_SIZE = 10;
constexpr uint32_t DUPLICATE_ACKS_BEFORE_RESEND = 3;
constexpr uint32_t ACK_NR_ALLOWED_WINDOW = DUPLICATE_ACKS_BEFORE_RESEND;
constexpr uint32_t RST_INFO_LIMIT = 1000;

// Bit masks
constexpr uint16_t SEQ_NR_MASK = 0xFFFF;
constexpr uint16_t ACK_NR_MASK = 0xFFFF;
constexpr uint32_t TIMESTAMP_MASK = 0xFFFFFFFF;

}  // namespace utp::config
