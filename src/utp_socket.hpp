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

#include "utp_internal.h"
#include "utp/config.hpp"
#include "utp/ledbat.hpp"
#include "utp/mtu_discovery.hpp"
#include "utp/sequence_buffer.hpp"
#include "utp/delay_history.hpp"
#include "utp/wire_format.hpp"
#include "utp_callbacks.h"

using namespace utp::config;

enum BandwidthType {
	payload_bandwidth, connect_overhead,
	close_overhead, ack_overhead,
	header_overhead, retransmit_overhead
};

enum CONN_STATE {
	CS_UNINITIALIZED = 0,
	CS_IDLE,
	CS_SYN_SENT,
	CS_SYN_RECV,
	CS_CONNECTED,
	CS_CONNECTED_FULL,
	CS_RESET,
	CS_DESTROY
};

struct OutgoingPacket {
	size_t length = 0;
	size_t payload = 0;
	uint64 time_sent = 0; // microseconds
	uint transmissions:31 = 0;
	bool need_resend:1 = false;
	std::vector<uint8_t> data;
};

struct InboundPacket {
	uint32_t size = 0;
	std::vector<uint8_t> data;
};

class UtpSocket {
public:
	UtpSocket(utp_context* _ctx);
	~UtpSocket();

	utp::Address addr;
	utp_context *ctx;

	int ida; //for ack socket list

	uint16 reorder_count_;
	byte duplicate_ack_;
	uint16 cur_window_packets_;

	size_t opt_sndbuf_;
	size_t opt_rcvbuf_;

	size_t target_delay_;

	bool got_fin:1;
	bool got_fin_reached_:1;

	bool fin_sent:1;
	bool fin_sent_acked_:1;

	bool read_shutdown_:1;
	bool close_requested_:1;

	bool fast_timeout_:1;

	size_t max_window_user_;
	CONN_STATE state_;

	uint16 eof_pkt_;

	uint16 ack_nr_;
	uint16 seq_nr_;

	uint16 timeout_seq_nr_;

	uint16 fast_resend_seq_nr_;

	uint32 reply_micro_;

	uint64 last_got_packet_;
	uint64 last_sent_packet_;
	uint64 last_measured_delay_;

	void *userdata_;

	uint32 conn_seed_;
	uint32 conn_id_recv_;
	uint32 conn_id_send_;
	size_t last_rcv_win_;

	byte extensions_[8];

	MtuDiscovery mtu_;
	LedbatController cc_;

	utp::RawSequenceBuffer inbuf_, outbuf_;

	#ifdef _DEBUG
	utp_socket_stats stats_;
	#endif

	void log(int level, char const *fmt, ...)
	{
		va_list va;
		char buf[4096], buf2[4096];

		if (!ctx->would_log(level)) {
			return;
		}

		va_start(va, fmt);
		vsnprintf(buf, 4096, fmt, va);
		va_end(va);
		buf[4095] = '\0';

		snprintf(buf2, 4096, "%p %s %06u %s", this, addrfmt(addr, addrbuf), conn_id_recv_, buf);
		buf2[4095] = '\0';

		ctx->log_unchecked(this, buf2);
	}

	void schedule_ack();

	size_t get_rcv_window()
	{
		const size_t numbuf = utp_call_get_read_buffer_size(this->ctx, this);
		assert((int)numbuf >= 0);
		return opt_rcvbuf_ > numbuf ? opt_rcvbuf_ - numbuf : 0;
	}

	size_t get_header_size() const
	{
		return sizeof(utp::wire::PacketFormatV1);
	}

	size_t get_udp_mtu()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_mtu(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	size_t get_udp_overhead()
	{
		socklen_t len;
		SOCKADDR_STORAGE sa = addr.get_sockaddr_storage(&len);
		return utp_call_get_udp_overhead(this->ctx, this, (const struct sockaddr *)&sa, len);
	}

	size_t get_overhead()
	{
		return get_udp_overhead() + get_header_size();
	}

	void send_data(byte* b, size_t length, BandwidthType type, uint32 flags = 0);

	void send_ack(bool synack = false);

	void send_keep_alive();

	static void send_rst(utp_context *ctx,
						 const utp::Address &addr, uint32 conn_id_send_,
						 uint16 ack_nr_, uint16 seq_nr_);

	void send_packet(OutgoingPacket *pkt);

	bool is_full(int bytes = -1);
	bool flush_packets();
	void write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs);

	#ifdef _DEBUG
	void check_invariant();
	#endif

	void check_timeouts();
	int ack_packet(uint16 seq);
	size_t selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt);
	void selective_ack(uint base, const byte *mask, byte len);
	size_t get_packet_size() const;
};
