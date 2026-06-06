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

// Wire protocol structures. Byte layout MUST match original PacketFormatV1 for protocol compatibility.

#include <cstdint>
#include "endian.hpp"

namespace utp::wire {

// 4-bit values, stored in high nibble of ver_type
enum PacketType : std::uint8_t {
	ST_DATA       = 0,
	ST_FIN        = 1,
	ST_STATE      = 2,
	ST_RESET      = 3,
	ST_SYN        = 4,
	ST_NUM_STATES = 5,
};

// Protocol version
constexpr std::uint8_t PROTOCOL_VERSION = 1;

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(1)
#else
	#pragma pack(push, 1)
#endif

struct PacketFormatV1 {
	std::uint8_t ver_type;

	[[nodiscard]] std::uint8_t version() const { return ver_type & 0xf; }
	[[nodiscard]] std::uint8_t type()    const { return ver_type >> 4; }
	void set_version(std::uint8_t v) { ver_type = (ver_type & 0xf0) | (v & 0xf); }
	void set_type(std::uint8_t t)    { ver_type = (ver_type & 0x0f) | (t << 4); }

	std::uint8_t ext;
	uint16_big connid;
	uint32_big tv_usec;
	uint32_big reply_micro;
	uint32_big windowsize;
	uint16_big seq_nr;
	uint16_big ack_nr;
};

static_assert(sizeof(PacketFormatV1) == 20, "PacketFormatV1 must be exactly 20 bytes");

struct PacketFormatAckV1 {
	PacketFormatV1 pf;
	std::uint8_t ext_next;
	std::uint8_t ext_len;
	std::uint8_t acks[4];
};

static_assert(sizeof(PacketFormatAckV1) == 26, "PacketFormatAckV1 must be exactly 26 bytes");

#if (defined(__SVR4) && defined(__sun))
	#pragma pack(0)
#else
	#pragma pack(pop)
#endif

// Connection state machine
enum ConnState : std::uint8_t {
	CS_UNINITIALIZED  = 0,
	CS_IDLE,
	CS_SYN_SENT,
	CS_SYN_RECV,
	CS_CONNECTED,
	CS_CONNECTED_FULL,
	CS_RESET,
	CS_DESTROY,
};

// Packet size buckets for statistics
enum PacketSizeBucket : std::uint32_t {
	PACKET_SIZE_EMPTY_BUCKET = 0,
	PACKET_SIZE_SMALL_BUCKET = 1,
	PACKET_SIZE_MID_BUCKET   = 2,
	PACKET_SIZE_BIG_BUCKET   = 3,
	PACKET_SIZE_HUGE_BUCKET  = 4,
};

inline constexpr std::uint32_t packet_size_from_bucket(PacketSizeBucket b) {
	switch (b) {
		case PACKET_SIZE_EMPTY_BUCKET: return 23;
		case PACKET_SIZE_SMALL_BUCKET: return 373;
		case PACKET_SIZE_MID_BUCKET:   return 723;
		case PACKET_SIZE_BIG_BUCKET:   return 1400;
		default: return 0;
	}
}

}  // namespace utp::wire
