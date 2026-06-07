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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <vector>
#include <memory>

#include "utp_socket.hpp"
#include "utp_callbacks.h"

using utp::wrapping_compare_less;
using utp::wire::PacketFormatV1;
using utp::wire::PacketFormatAckV1;
using utp::zeromem;
using std::min;
using std::max;
using std::clamp;

char addrbuf[65];

enum {
	ST_DATA = 0,
	ST_FIN = 1,
	ST_STATE = 2,
	ST_RESET = 3,
	ST_SYN = 4,
	ST_NUM_STATES,
};

static const cstr flagnames[] = {
	"ST_DATA","ST_FIN","ST_STATE","ST_RESET","ST_SYN"
};

static const cstr statenames[] = {
	"UNINITIALIZED", "IDLE","SYN_SENT", "SYN_RECV", "CONNECTED","CONNECTED_FULL","DESTROY_DELAY","RESET","DESTROY"
};

void remove_socket_from_ack_list(UtpSocket *conn)
{
	if (conn->ida >= 0)
	{
		UtpSocket *last = conn->ctx->ack_sockets_.back();

		assert(last->ida < (int)(conn->ctx->ack_sockets_.size()));
		assert(conn->ctx->ack_sockets_[last->ida] == last);
		last->ida = conn->ida;
		conn->ctx->ack_sockets_[conn->ida] = last;
		conn->ida = -1;

		conn->ctx->ack_sockets_.pop_back();
	}
}

static void utp_register_sent_packet(utp_context *ctx, size_t length)
{
	if (length <= PACKET_SIZE_MID) {
		if (length <= PACKET_SIZE_EMPTY) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (length <= PACKET_SIZE_SMALL) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_MID_BUCKET]++;
	} else {
		if (length <= PACKET_SIZE_BIG) {
			ctx->context_stats_._nraw_send[PACKET_SIZE_BIG_BUCKET]++;
		} else
			ctx->context_stats_._nraw_send[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

void send_to_addr(utp_context *ctx, const byte *p, size_t len, const utp::Address &addr, int flags = 0)
{
	socklen_t tolen;
	SOCKADDR_STORAGE to = addr.get_sockaddr_storage(&tolen);
	utp_register_sent_packet(ctx, len);
	utp_call_sendto(ctx, NULL, p, len, (const struct sockaddr *)&to, tolen, flags);
}

void UtpSocket::schedule_ack()
{
	if (ida == -1){
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack");
		#endif
		ctx->ack_sockets_.push_back(this); ida = ctx->ack_sockets_.size() - 1;
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "schedule_ack: already in list");
		#endif
	}
}

void UtpSocket::send_data(byte* b, size_t length, BandwidthType type, uint32 flags)
{
	uint64 time = utp_call_get_microseconds(ctx, this);

	PacketFormatV1* b1 = (PacketFormatV1*)b;
	b1->tv_usec = (uint32)time;
	b1->reply_micro = reply_micro_;

	last_sent_packet_ = ctx->current_ms_;

	#ifdef _DEBUG
	stats_.nbytes_xmit += length;
	++stats_.nxmit;
	#endif

	if (true) {
		size_t n;
		if (type == payload_bandwidth) {
			type = header_overhead;
			n = get_overhead();
		} else {
			n = length + get_udp_overhead();
		}
		utp_call_on_overhead_statistics(ctx, this, true, n, type);
	}
#if UTP_DEBUG_LOGGING
	int flags2 = b1->type();
	uint16 seq_nr_ = b1->seq_nr;
	uint16 ack_nr_ = b1->ack_nr;
	log(UTP_LOG_DEBUG, "send %s len:%u id:%u timestamp:" I64u " reply_micro_:%u flags:%s seq_nr_:%u ack_nr_:%u",
		addrfmt(addr, addrbuf), (uint)length, conn_id_send_, time, reply_micro_, flagnames[flags2],
		seq_nr_, ack_nr_);
#endif
	send_to_addr(ctx, b, length, addr, flags);
	remove_socket_from_ack_list(this);
}

void UtpSocket::send_ack(bool synack)
{
	PacketFormatAckV1 pfa;
	zeromem(&pfa);

	size_t len;
	last_rcv_win_ = get_rcv_window();
	pfa.pf.set_version(1);
	pfa.pf.set_type(ST_STATE);
	pfa.pf.ext = 0;
	pfa.pf.connid = conn_id_send_;
	pfa.pf.ack_nr = ack_nr_;
	pfa.pf.seq_nr = seq_nr_;
	pfa.pf.windowsize = (uint32)last_rcv_win_;
	len = sizeof(PacketFormatV1);

	if (reorder_count_ != 0 && !got_fin_reached_) {
		assert(!synack);
		pfa.pf.ext = 1;
		pfa.ext_next = 0;
		pfa.ext_len = 4;
		uint m = 0;

		assert(inbuf_.get(ack_nr_ + 1) == NULL);
		size_t window = min<size_t>(14+16, inbuf_.size());
		for (size_t i = 0; i < window; i++) {
			if (inbuf_.get(ack_nr_ + i + 2) != NULL) {
				m |= 1 << i;

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "EACK packet [%u]", ack_nr_ + i + 2);
				#endif
			}
		}
		pfa.acks[0] = (byte)m;
		pfa.acks[1] = (byte)(m >> 8);
		pfa.acks[2] = (byte)(m >> 16);
		pfa.acks[3] = (byte)(m >> 24);
		len += 4 + 2;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending EACK %u [%u] bits:[%032b]", ack_nr_, conn_id_send_, m);
		#endif
	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Sending ACK %u [%u]", ack_nr_, conn_id_send_);
		#endif
	}

	send_data((byte*)&pfa, len, ack_overhead);
	remove_socket_from_ack_list(this);
}

void UtpSocket::send_keep_alive()
{
	ack_nr_--;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Sending KeepAlive ACK %u [%u]", ack_nr_, conn_id_send_);
	#endif
	send_ack();
	ack_nr_++;
}

void UtpSocket::send_rst(utp_context *ctx,
	const utp::Address &addr, uint32 conn_id_send_, uint16 ack_nr_, uint16 seq_nr_)
{
	PacketFormatV1 pf1;
	zeromem(&pf1);

	size_t len;
	pf1.set_version(1);
	pf1.set_type(ST_RESET);
	pf1.ext = 0;
	pf1.connid = conn_id_send_;
	pf1.ack_nr = ack_nr_;
	pf1.seq_nr = seq_nr_;
	pf1.windowsize = 0;
	len = sizeof(PacketFormatV1);

	send_to_addr(ctx, (const byte*)&pf1, len, addr);
}

void UtpSocket::send_packet(OutgoingPacket *pkt)
{
	time_t cur_time = utp_call_get_milliseconds(this->ctx, this);

	if (pkt->transmissions == 0 || pkt->need_resend) {
		cc_.add_in_flight(pkt->payload);
	}

	pkt->need_resend = false;

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
	p1->ack_nr = ack_nr_;
	pkt->time_sent = utp_call_get_microseconds(this->ctx, this);

	bool use_as_mtu_probe = false;

 	if (mtu_.should_rediscover((uint64)cur_time)) {
		mtu_.reset((uint32)get_udp_mtu(), (uint64)cur_time);
	}

 	if (mtu_.floor() < mtu_.ceiling()
		&& pkt->length > mtu_.floor()
		&& pkt->length <= mtu_.ceiling()
		&& !mtu_.probe_seq()
		&& seq_nr_ != 1
		&& pkt->transmissions == 0) {

 		mtu_.set_probe((seq_nr_ - 1) & ACK_NR_MASK, (uint32)pkt->length);
		assert(pkt->length >= mtu_.floor());
		assert(pkt->length <= mtu_.ceiling());
 		use_as_mtu_probe = true;
		log(UTP_LOG_MTU, "MTU [PROBE] floor:%d ceiling:%d current:%d"
			, mtu_.floor(), mtu_.ceiling(), mtu_.probe_size());
  	}

	pkt->transmissions++;
	send_data((byte*)pkt->data.data(), pkt->length,
		(state_ == CS_SYN_SENT) ? connect_overhead
		: (pkt->transmissions == 1) ? payload_bandwidth
		: retransmit_overhead, use_as_mtu_probe ? UTP_UDP_DONTFRAG : 0);
}

bool UtpSocket::is_full(int bytes)
{
	size_t packet_size = get_packet_size();
	if (bytes < 0) bytes = packet_size;
	else if (bytes > (int)packet_size) bytes = (int)packet_size;
	size_t max_send = min(min(cc_.max_window(), opt_sndbuf_), max_window_user_);

	if (cur_window_packets_ >= OUTGOING_BUFFER_MAX_SIZE - 1) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "is_full:false cur_window_packets_:%d MAX:%d", cur_window_packets_, OUTGOING_BUFFER_MAX_SIZE - 1);
		#endif

		cc_.mark_window_full(ctx->current_ms_);
		return true;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "is_full:%s. cur_window_:%u pkt:%u max:%u cur_window_packets_:%u max_window_:%u"
		, (cc_.cur_window() + bytes > max_send) ? "true" : "false"
		, (uint)cc_.cur_window(), bytes, max_send, cur_window_packets_
		, (uint)cc_.max_window());
	#endif

	if (cc_.cur_window() + bytes > max_send) {
		cc_.mark_window_full(ctx->current_ms_);
		return true;
	}
	return false;
}

bool UtpSocket::flush_packets()
{
	size_t packet_size = get_packet_size();
	for (uint16 i = seq_nr_ - cur_window_packets_; i != seq_nr_; ++i) {
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(i);
		if (pkt == 0 || (pkt->transmissions > 0 && pkt->need_resend == false)) continue;
		if (is_full()) return true;

		if (i != ((seq_nr_ - 1) & ACK_NR_MASK) ||
			cur_window_packets_ == 1 ||
			pkt->payload >= packet_size) {
			send_packet(pkt);
		}
	}
	return false;
}

void UtpSocket::write_outgoing_packet(size_t payload, uint flags, struct utp_iovec *iovec, size_t num_iovecs)
{
	if (cur_window_packets_ == 0) {
		cc_.set_initial_rto(ctx->current_ms_);
		assert(cc_.cur_window() == 0);
	}

	size_t packet_size = get_packet_size();
	do {
		assert(cur_window_packets_ < OUTGOING_BUFFER_MAX_SIZE);
		assert(flags == ST_DATA || flags == ST_FIN);

		size_t added = 0;

		OutgoingPacket *pkt = NULL;

		if (cur_window_packets_ > 0) {
			pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - 1);
		}

		const size_t header_size = get_header_size();
		bool append = true;

		if (payload && pkt && !pkt->transmissions && pkt->payload < packet_size) {
			added = min(payload + pkt->payload, max<size_t>(packet_size, pkt->payload)) - pkt->payload;
			pkt->data.resize(header_size + pkt->payload + added);
			outbuf_.put(seq_nr_ - 1, pkt);
			append = false;
			assert(!pkt->need_resend);
		} else {
			added = payload;
			pkt = new OutgoingPacket();
			pkt->data.resize(header_size + added);
			pkt->payload = 0;
			pkt->transmissions = 0;
			pkt->need_resend = false;
		}

		if (added) {
			assert(flags == ST_DATA);

			unsigned char *p = pkt->data.data() + header_size + pkt->payload;
			size_t needed = added;

			/*
			while (needed) {
				*p = *(char*)iovec[0].iov_base;
				p++;
				iovec[0].iov_base = (char *)iovec[0].iov_base + 1;
				needed--;
			}
			*/

			for (size_t i = 0; i < num_iovecs && needed; i++) {
				if (iovec[i].iov_len == 0)
					continue;

				size_t num = min<size_t>(needed, iovec[i].iov_len);
				memcpy(p, iovec[i].iov_base, num);

				p += num;

				iovec[i].iov_len -= num;
				iovec[i].iov_base = (byte*)iovec[i].iov_base + num;
				needed -= num;
			}

			assert(needed == 0);
		}
		pkt->payload += added;
		pkt->length = header_size + pkt->payload;

		last_rcv_win_ = get_rcv_window();

	PacketFormatV1* p1 = (PacketFormatV1*)pkt->data.data();
		p1->set_version(1);
		p1->set_type(flags);
		p1->ext = 0;
		p1->connid = conn_id_send_;
		p1->windowsize = (uint32)last_rcv_win_;
		p1->ack_nr = ack_nr_;

		if (append) {
			outbuf_.ensure_size(seq_nr_, cur_window_packets_);
			outbuf_.put(seq_nr_, pkt);
			p1->seq_nr = seq_nr_;
			seq_nr_++;
			cur_window_packets_++;
		}

		payload -= added;

	} while (payload);

	flush_packets();
}

#ifdef _DEBUG
void UtpSocket::check_invariant()
{
	if (reorder_count_ > 0) {
		assert(inbuf_.get(ack_nr_ + 1) == NULL);
	}

	size_t outstanding_bytes = 0;
	for (int i = 0; i < cur_window_packets_; ++i) {
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - i - 1);
		if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
		outstanding_bytes += pkt->payload;
	}
	assert(outstanding_bytes == cc_.cur_window());
}
#endif

void UtpSocket::check_timeouts()
{
	#ifdef _DEBUG
	check_invariant();
	#endif

	assert(cur_window_packets_ == 0 || outbuf_.get(seq_nr_ - cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "CheckTimeouts timeout:%d max_window_:%u cur_window_:%u "
			 "state:%s cur_window_packets_:%u",
			 (int)(cc_.rto_timeout() - ctx->current_ms_), (uint)cc_.max_window(), (uint)cc_.cur_window(),
			 statenames[state_], cur_window_packets_);
	#endif
	if (state_ != CS_DESTROY) flush_packets();

	switch (state_) {
	case CS_SYN_SENT:
	case CS_SYN_RECV:
	case CS_CONNECTED_FULL:
	case CS_CONNECTED: {

		if ((int)(ctx->current_ms_ - cc_.zerowindow_time()) >= 0 && max_window_user_ == 0) {
			max_window_user_ = PACKET_SIZE;
		}

		if (cc_.is_rto_expired(ctx->current_ms_)) {

			bool ignore_loss = mtu_.handle_probe_timeout(
				(seq_nr_ - 1) & ACK_NR_MASK, cur_window_packets_, ctx->current_ms_);
			if (ignore_loss) {
				log(UTP_LOG_MTU, "MTU [PROBE-TIMEOUT] floor:%d ceiling:%d current:%d"
					, mtu_.floor(), mtu_.ceiling(), mtu_.last());
			}
			log(UTP_LOG_MTU, "MTU [TIMEOUT]");

			/*
			OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);

			// If there were a lot of retransmissions, force recomputation of round trip time
			if (pkt->transmissions >= 4)
				rtt = 0;
			*/
			const uint new_timeout = ignore_loss ? cc_.retransmit_timeout() : cc_.retransmit_timeout() * 2;

			if (state_ == CS_SYN_RECV) {
				state_ = CS_DESTROY;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			if (cc_.retransmit_count() >= 4 || (state_ == CS_SYN_SENT && cc_.retransmit_count() >= 2)) {
				if (close_requested_)
					state_ = CS_DESTROY;
				else
					state_ = CS_RESET;
				utp_call_on_error(ctx, this, UTP_ETIMEDOUT);
				return;
			}
			cc_.set_retransmit_timeout(new_timeout);
			cc_.set_rto_timeout(ctx->current_ms_ + new_timeout);

			if (!ignore_loss) {
				duplicate_ack_ = 0;

				int packet_size = get_packet_size();
				cc_.on_rto_timeout(ctx->current_ms_, (size_t)packet_size, cur_window_packets_);
			}
			for (int i = 0; i < cur_window_packets_; ++i) {
				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - i - 1);
				if (pkt == 0 || pkt->transmissions == 0 || pkt->need_resend) continue;
				pkt->need_resend = true;
				cc_.remove_in_flight(pkt->payload);
			}
			if (cur_window_packets_ > 0) {
				cc_.increment_retransmit_count();
				log(UTP_LOG_NORMAL, "Packet timeout. Resend. seq_nr_:%u. timeout:%u "
					"max_window_:%u cur_window_packets_:%d"
					, seq_nr_ - cur_window_packets_, cc_.retransmit_timeout()
					, (uint)cc_.max_window(), int(cur_window_packets_));

				fast_timeout_ = true;
				timeout_seq_nr_ = seq_nr_;

				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);
				assert(pkt);
				send_packet(pkt);
			}
		}

		if (state_ == CS_CONNECTED_FULL && !is_full()) {
			state_ = CS_CONNECTED;
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
				(uint)cc_.max_window(), (uint)cc_.cur_window(), (uint)get_packet_size());
			#endif
			utp_call_on_state_change(this->ctx, this, UTP_STATE_WRITABLE);
		}
		if (state_ >= CS_CONNECTED && !fin_sent) {
			if ((int)(ctx->current_ms_ - last_sent_packet_) >= KEEPALIVE_INTERVAL) {
				send_keep_alive();
			}
		}
		break;
	}

	case CS_UNINITIALIZED:
	case CS_IDLE:
	case CS_RESET:
	case CS_DESTROY:
		break;
	}
}

int UtpSocket::ack_packet(uint16 seq)
{
	OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq);

	if (pkt == NULL) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "got ack for:%u (already acked, or never sent)", seq);
		#endif

		return 1;
	}

	if (pkt->transmissions == 0) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "got ack for:%u (never sent, pkt_size:%u need_resend:%u)",
			seq, (uint)pkt->payload, pkt->need_resend);
		#endif

		return 2;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "got ack for:%u (pkt_size:%u need_resend:%u)",
		seq, (uint)pkt->payload, pkt->need_resend);
	#endif

	outbuf_.put(seq, nullptr);

	if (pkt->transmissions == 1) {
		const uint32 ertt = (uint32)((utp_call_get_microseconds(this->ctx, this) - pkt->time_sent) / 1000);
		cc_.update_rtt(ertt, ctx->current_ms_);

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "rtt:%u avg:%u var:%u rto:%u",
			ertt, cc_.rtt_ms(), cc_.rtt_var(), cc_.rto_ms());
		#endif

	} else {
		cc_.set_retransmit_timeout(cc_.rto_ms());
		cc_.set_rto_timeout(ctx->current_ms_ + cc_.rto_ms());
	}
	if (!pkt->need_resend) {
		cc_.remove_in_flight(pkt->payload);
	}
	delete pkt;
	cc_.set_retransmit_count(0);
	return 0;
}

size_t UtpSocket::selective_ack_bytes(uint base, const byte* mask, byte len, int64& min_rtt)
{
	if (cur_window_packets_ == 0) return 0;

	size_t acked_bytes = 0;
	int bits = len * 8;
	uint64 now = utp_call_get_microseconds(this->ctx, this);

	do {
		uint v = base + bits;

		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0)
			continue;

		if (bits >= 0 && mask[bits>>3] & (1 << (bits & 7))) {
			assert((int)(pkt->payload) >= 0);
			acked_bytes += pkt->payload;
			if (pkt->time_sent < now)
				min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
			else
				min_rtt = min<int64>(min_rtt, 50000);
			continue;
		}
	} while (--bits >= -1);
	return acked_bytes;
}

enum { MAX_EACK = 128 };

void UtpSocket::selective_ack(uint base, const byte *mask, byte len)
{
	if (cur_window_packets_ == 0) return;

	int bits = len * 8 - 1;

	int count = 0;

	int resends[MAX_EACK];
	int nr = 0;

#if UTP_DEBUG_LOGGING
	char bitmask[1024] = {0};
	int counter = bits;
	for (int i = 0; i <= bits; ++i) {
		bool bit_set = counter >= 0 && mask[counter>>3] & (1 << (counter & 7));
		bitmask[i] = bit_set ? '1' : '0';
		--counter;
	}

	log(UTP_LOG_DEBUG, "Got EACK [%s] base:%u", bitmask, base);
#endif

	do {
		uint v = base + bits;

		if (((seq_nr_ - v - 1) & ACK_NR_MASK) >= (uint16)(cur_window_packets_ - 1))
			continue;

		bool bit_set = bits >= 0 && mask[bits>>3] & (1 << (bits & 7));

		if (bit_set) count++;

		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);
		if (!pkt || pkt->transmissions == 0) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "skipping %u. pkt:%08x transmissions:%u %s",
				v, pkt, pkt?pkt->transmissions:0, pkt?"(not sent yet?)":"(already acked?)");
			#endif
			continue;
		}

		if (bit_set) {
			assert((v & outbuf_.mask()) != ((seq_nr_ - cur_window_packets_) & outbuf_.mask()));
			ack_packet(v);
			continue;
		}

		if (((v - fast_resend_seq_nr_) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
			count >= DUPLICATE_ACKS_BEFORE_RESEND) {
			if (nr >= MAX_EACK - 2) {
				memmove(resends, &resends[MAX_EACK/2], MAX_EACK/2 * sizeof(resends[0]));
				nr -= MAX_EACK / 2;
			}
			resends[nr++] = v;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "no ack for %u", v);
			#endif

		} else {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "not resending %u count:%d dup_ack:%u fast_resend_seq_nr_:%u",
				v, count, duplicate_ack_, fast_resend_seq_nr_);
			#endif
		}
	} while (--bits >= -1);

	if (((base - 1 - fast_resend_seq_nr_) & ACK_NR_MASK) <= OUTGOING_BUFFER_MAX_SIZE &&
		count >= DUPLICATE_ACKS_BEFORE_RESEND) {
		resends[nr++] = (base - 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "no ack for %u", (base - 1) & ACK_NR_MASK);
		#endif

	} else {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "not resending %u count:%d dup_ack:%u fast_resend_seq_nr_:%u",
			base - 1, count, duplicate_ack_, fast_resend_seq_nr_);
		#endif
	}

	bool back_off = false;
	int i = 0;
	while (nr > 0) {
		uint v = resends[--nr];
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(v);

		if (!pkt) continue;

		log(UTP_LOG_NORMAL, "Packet %u lost. Resending", v);

		back_off = true;

		#ifdef _DEBUG
		++stats_.rexmit;
		#endif

		send_packet(pkt);
		fast_resend_seq_nr_ = (v + 1) & ACK_NR_MASK;

		if (++i >= 4) break;
	}

	if (back_off)
		cc_.maybe_decay_win(ctx->current_ms_);

	duplicate_ack_ = count;
}

size_t UtpSocket::get_packet_size() const
{
	return mtu_.effective_mtu(get_header_size());
}

UtpSocket::UtpSocket(utp_context* _ctx)
	: addr()
	, ctx(_ctx)
	, ida(-1)
	, reorder_count_(0)
	, duplicate_ack_(0)
	, cur_window_packets_(0)
	, opt_sndbuf_(_ctx->opt_sndbuf_)
	, opt_rcvbuf_(_ctx->opt_rcvbuf_)
	, target_delay_(_ctx->target_delay_)
	, got_fin(false)
	, got_fin_reached_(false)
	, fin_sent(false)
	, fin_sent_acked_(false)
	, read_shutdown_(false)
	, close_requested_(false)
	, fast_timeout_(false)
	, max_window_user_(255 * PACKET_SIZE)
	, state_(CS_UNINITIALIZED)
	, eof_pkt_(0)
	, ack_nr_(0)
	, seq_nr_(1)
	, timeout_seq_nr_(0)
	, fast_resend_seq_nr_(1)
	, reply_micro_(0)
	, last_got_packet_(0)
	, last_sent_packet_(0)
	, last_measured_delay_(0)
	, userdata_(NULL)
	, conn_seed_(0)
	, conn_id_recv_(0)
	, conn_id_send_(0)
	, last_rcv_win_(0)
	, extensions_()
	, mtu_(this)
	, cc_()
{
	inbuf_.initialize(16);
	outbuf_.initialize(16);
	cc_.set_ssthresh(_ctx->opt_sndbuf_);
	memset(extensions_, 0, sizeof(extensions_));
	#ifdef _DEBUG
	memset(&stats_, 0, sizeof(utp_socket_stats));
	#endif
}

UtpSocket::~UtpSocket()
{
	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Killing socket");
	#endif

	utp_call_on_state_change(ctx, this, UTP_STATE_DESTROYING);

	if (ctx->last_utp_socket_ == this) {
		ctx->last_utp_socket_ = NULL;
	}

	auto erased = ctx->sockets_.erase(UtpSocketKey(addr, conn_id_recv_));
	assert(erased == 1);

	remove_socket_from_ack_list(this);

	for (size_t i = 0; i < inbuf_.buf_size(); i++) {
		delete (InboundPacket*)inbuf_.element(i);
	}
 	for (size_t i = 0; i < outbuf_.buf_size(); i++) {
		delete (OutgoingPacket*)outbuf_.element(i);
	}
}

void MtuDiscovery::reset(uint32 udp_mtu, uint64 current_ms)
{
	mtu_ceiling_ = udp_mtu;
	mtu_floor_ = 576;
	owner_->log(UTP_LOG_MTU, "MTU [RESET] floor:%d ceiling:%d current:%d"
		, mtu_floor_, mtu_ceiling_, mtu_last_);
	assert(mtu_floor_ <= mtu_ceiling_);
	mtu_discover_time_ = current_ms + 30 * 60 * 1000;
}

void MtuDiscovery::search_update(uint64 current_ms)
{
	assert(mtu_floor_ <= mtu_ceiling_);

	mtu_last_ = (mtu_floor_ + mtu_ceiling_) / 2;

	mtu_probe_seq_ = mtu_probe_size_ = 0;

	if (mtu_ceiling_ - mtu_floor_ <= 16) {
		mtu_last_ = mtu_floor_;
		owner_->log(UTP_LOG_MTU, "MTU [DONE] floor:%d ceiling:%d current:%d"
			, mtu_floor_, mtu_ceiling_, mtu_last_);
		mtu_ceiling_ = mtu_floor_;
		assert(mtu_floor_ <= mtu_ceiling_);
		mtu_discover_time_ = current_ms + 30 * 60 * 1000;
	}
}

bool MtuDiscovery::handle_probe_ack(uint32 seq, uint64 current_ms)
{
	if (is_probe(seq)) {
		mtu_floor_ = mtu_probe_size_;
		search_update(current_ms);
		return true;
	}
	return false;
}

bool MtuDiscovery::handle_probe_timeout(uint32 outstanding_seq, uint32 cur_window_packets, uint64 current_ms)
{
	bool ignore_loss = false;
	if (cur_window_packets == 1 && is_probe(outstanding_seq)) {
		mtu_ceiling_ = mtu_probe_size_ - 1;
		search_update(current_ms);
		ignore_loss = true;
	}
	clear_probe();
	return ignore_loss;
}

void MtuDiscovery::handle_probe_loss(uint64 current_ms)
{
	mtu_ceiling_ = mtu_probe_size_ - 1;
	search_update(current_ms);
	clear_probe();
}

void MtuDiscovery::handle_icmp_fragmentation(uint16 next_hop_mtu, uint64 current_ms)
{
	mtu_ceiling_ = std::min<uint32>(next_hop_mtu, mtu_ceiling_);
	search_update(current_ms);
	mtu_last_ = mtu_ceiling_;
}

void MtuDiscovery::handle_icmp_unknown(uint64 current_ms)
{
	mtu_ceiling_ = (mtu_floor_ + mtu_ceiling_) / 2;
	search_update(current_ms);
}
