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
#include <string.h>
#include <vector>
#include <memory>

#include "utp_socket.hpp"
#include "utp_callbacks.h"

using utp::wrapping_compare_less;
using utp::wire::PacketFormatV1;
using utp::wire::PacketFormatAckV1;
using utp::wire::ST_DATA;
using utp::wire::ST_FIN;
using utp::wire::ST_STATE;
using utp::wire::ST_RESET;
using utp::wire::ST_SYN;
using utp::wire::ST_NUM_STATES;
using std::min;
using std::max;

void utp_initialize_socket(utp_socket *conn,
                           const struct sockaddr *addr,
                           socklen_t addrlen,
                           bool need_seed_gen,
                           uint32 conn_seed_,
                           uint32 conn_id_recv_,
                           uint32 conn_id_send_);

utp_socket *utp_create_socket(utp_context *ctx);

static void utp_register_recv_packet(UtpSocket *conn, size_t len)
{
	#ifdef _DEBUG
	++conn->stats_.nrecv;
	conn->stats_.nbytes_recv += len;
	#endif

	if (len <= PACKET_SIZE_MID) {
		if (len <= PACKET_SIZE_EMPTY) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (len <= PACKET_SIZE_SMALL) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_MID_BUCKET]++;
	} else {
		if (len <= PACKET_SIZE_BIG) {
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_BIG_BUCKET]++;
		} else
			conn->ctx->context_stats_._nraw_recv[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

size_t utp_process_incoming(UtpSocket *conn, const byte *packet, size_t len, bool syn = false)
{
	utp_register_recv_packet(conn, len);
	conn->ctx->current_ms_ = utp_call_get_milliseconds(conn->ctx, conn);

	const PacketFormatV1 *pf1 = (PacketFormatV1*)packet;
	const byte *packet_end = packet + len;

	uint16 pk_seq_nr_ = pf1->seq_nr;
	uint16 pk_ack_nr_ = pf1->ack_nr;
	uint8 pk_flags   = pf1->type();

	if (pk_flags >= ST_NUM_STATES) return 0;

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "Got %s. seq_nr_:%u ack_nr_:%u state:%s timestamp:" I64u " reply_micro_:%u"
		, flagnames[pk_flags], pk_seq_nr_, pk_ack_nr_, statenames[conn->state_]
		, uint64(pf1->tv_usec), (uint32)(pf1->reply_micro));
	#endif

	uint64 time = utp_call_get_microseconds(conn->ctx, conn);
	const uint16 curr_window = max<uint16>(conn->cur_window_packets_ + ACK_NR_ALLOWED_WINDOW, ACK_NR_ALLOWED_WINDOW);

	if ((pk_flags != ST_SYN || conn->state_ != CS_SYN_RECV) &&
		(wrapping_compare_less(conn->seq_nr_ - 1, pk_ack_nr_, ACK_NR_MASK)
		|| wrapping_compare_less(pk_ack_nr_, conn->seq_nr_ - 1 - curr_window, ACK_NR_MASK))) {
#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "Invalid ack_nr_: %u. our seq_nr_: %u last unacked: %u"
	, pk_ack_nr_, conn->seq_nr_, (conn->seq_nr_ - conn->cur_window_packets_) & ACK_NR_MASK);
#endif
		return 0;
	}

	assert(pk_flags != ST_RESET);

	const byte *selack_ptr = NULL;

	const byte *data = (const byte*)pf1 + conn->get_header_size();
	if (conn->get_header_size() > len) {

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Invalid packet size (less than header size)");
		#endif

		return 0;
	}
	uint extension = pf1->ext;
	if (extension != 0) {
		do {
			data += 2;

			if ((int)(packet_end - data) < 0 || (int)(packet_end - data) < data[-1]) {

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "Invalid len of extensions_");
				#endif

				return 0;
			}

			switch(extension) {
			case 1:
				selack_ptr = data;
				break;
			case 2:
				if (data[-1] != 8) {

					#if UTP_DEBUG_LOGGING
					conn->log(UTP_LOG_DEBUG, "Invalid len of extension bits header");
					#endif

					return 0;
				}
				memcpy(conn->extensions_, data, 8);

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "got extension bits:%02x%02x%02x%02x%02x%02x%02x%02x",
					conn->extensions_[0], conn->extensions_[1], conn->extensions_[2], conn->extensions_[3],
					conn->extensions_[4], conn->extensions_[5], conn->extensions_[6], conn->extensions_[7]);
				#endif
			}
			extension = data[-2];
			data += data[-1];
		} while (extension);
	}

	if (conn->state_ == CS_SYN_SENT) {
		conn->ack_nr_ = (pk_seq_nr_ - 1) & SEQ_NR_MASK;
	}
	conn->last_got_packet_ = conn->ctx->current_ms_;
	if (syn) {
		return 0;
	}

	const uint seqnr = (pk_seq_nr_ - conn->ack_nr_ - 1) & SEQ_NR_MASK;
	if (seqnr >= REORDER_BUFFER_MAX_SIZE) {
		if (seqnr >= (SEQ_NR_MASK + 1) - REORDER_BUFFER_MAX_SIZE && pk_flags != ST_STATE) {
			conn->schedule_ack();
		}

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "    Got old Packet/Ack (%u/%u)=%u"
			, pk_seq_nr_, conn->ack_nr_, seqnr);
		#endif
		return 0;
	}

	int acks = (pk_ack_nr_ - (conn->seq_nr_ - 1 - conn->cur_window_packets_)) & ACK_NR_MASK;

	if (acks > conn->cur_window_packets_) acks = 0;

	if (conn->cur_window_packets_ > 0) {
		if (pk_ack_nr_ == ((conn->seq_nr_ - conn->cur_window_packets_ - 1) & ACK_NR_MASK)
			&& conn->cur_window_packets_ > 0
			&& pk_flags == ST_STATE) {
			++conn->duplicate_ack_;
			if (conn->duplicate_ack_ == DUPLICATE_ACKS_BEFORE_RESEND && conn->mtu_.probe_seq()) {
				if (pk_ack_nr_ == ((conn->mtu_.probe_seq() - 1) & ACK_NR_MASK)) {
					conn->mtu_.handle_probe_loss(conn->ctx->current_ms_);
					conn->log(UTP_LOG_MTU, "MTU [DUPACK] floor:%d ceiling:%d current:%d"
						, conn->mtu_.floor(), conn->mtu_.ceiling(), conn->mtu_.last());
				} else {
					conn->mtu_.clear_probe();
				}
			}
		} else {
			conn->duplicate_ack_ = 0;
		}

		// TODO: if duplicate_ack_ == DUPLICATE_ACK_BEFORE_RESEND
		// and fast_resend_seq_nr_ <= ack_nr_ + 1
		//    resend ack_nr_ + 1
		// also call maybe_decay_win()
	}

	size_t acked_bytes = 0;

	int64 min_rtt = INT64_MAX;

	uint64 now = utp_call_get_microseconds(conn->ctx, conn);

	for (int i = 0; i < acks; ++i) {
		int seq = (conn->seq_nr_ - conn->cur_window_packets_ + i) & ACK_NR_MASK;
		OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(seq);
		if (pkt == 0 || pkt->transmissions == 0) continue;
		assert((int)(pkt->payload) >= 0);
		acked_bytes += pkt->payload;
		if (conn->mtu_.handle_probe_ack(seq, conn->ctx->current_ms_)) {
			conn->log(UTP_LOG_MTU, "MTU [ACK] floor:%d ceiling:%d current:%d"
				, conn->mtu_.floor(), conn->mtu_.ceiling(), conn->mtu_.last());
		}
		if (pkt->time_sent < now)
			min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
		else
			min_rtt = min<int64>(min_rtt, 50000);
	}

	if (selack_ptr != NULL) {
		acked_bytes += conn->selective_ack_bytes((pk_ack_nr_ + 2) & ACK_NR_MASK,
												 selack_ptr, selack_ptr[-1], min_rtt);
	}

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%d cur_window_:%u cur_window_packets_:%u relative_seqnr:%u max_window_:%u min_rtt:%u rtt:%u",
		acks, (uint)acked_bytes, conn->seq_nr_, (uint)conn->cc_.cur_window(), conn->cur_window_packets_,
		seqnr, (uint)conn->cc_.max_window(), (uint)(min_rtt / 1000), conn->cc_.rtt_ms());
	#endif

	uint64 p = pf1->tv_usec;

	conn->last_measured_delay_ = conn->ctx->current_ms_;

	const uint32 their_delay = (uint32)(p == 0 ? 0 : time - p);
	conn->reply_micro_ = their_delay;
	uint32 prev_delay_base = conn->cc_.their_hist().delay_base;
	if (their_delay != 0) conn->cc_.their_hist().add_sample(their_delay, conn->ctx->current_ms_);

	if (prev_delay_base != 0 &&
		wrapping_compare_less(conn->cc_.their_hist().delay_base, prev_delay_base, TIMESTAMP_MASK)) {
		if (prev_delay_base - conn->cc_.their_hist().delay_base <= 10000) {
			conn->cc_.our_hist().shift(prev_delay_base - conn->cc_.their_hist().delay_base);
		}
	}
	const uint32 actual_delay = (uint32(pf1->reply_micro) == INT_MAX ? 0 : uint32(pf1->reply_micro));

	if (actual_delay != 0) {
		conn->cc_.our_hist().add_sample(actual_delay, conn->ctx->current_ms_);
		conn->cc_.update_delay_average(actual_delay, conn->ctx->current_ms_);
	}

/*	if (prev_delay_base != 0 &&
		wrapping_compare_less(conn->cc_.our_hist().delay_base, prev_delay_base)) {
		if (prev_delay_base - conn->cc_.our_hist().delay_base <= 10000) {
			conn->cc_.their_hist().Shift(prev_delay_base - conn->cc_.our_hist().delay_base);
		}
	}
*/

	assert(min_rtt >= 0);
	if (int64(conn->cc_.our_hist().get_value()) > min_rtt) {
		conn->cc_.our_hist().shift((uint32)(conn->cc_.our_hist().get_value() - min_rtt));
	}

	if (actual_delay != 0 && acked_bytes >= 1) {
		int32 our_delay = conn->cc_.apply_ccontrol(
			acked_bytes, actual_delay, min_rtt,
			conn->ctx->current_ms_, conn->opt_sndbuf_, conn->target_delay_,
			conn->get_packet_size());
		utp_call_on_delay_sample(conn->ctx, conn, our_delay / 1000);

		// used in parse_log.py
		conn->log(UTP_LOG_NORMAL, "actual_delay:%u our_delay:%d their_delay:%u off_target:%d max_window_:%u "
				"delay_base:%u delay_sum:%d target_delay_:%d acked_bytes:%u cur_window_:%u "
				"scaled_gain:%f rtt:%u rate:%u wnduser:%u rto:%u timeout:%d get_microseconds:" I64u " "
				"cur_window_packets_:%u packet_size:%u their_delay_base:%u their_actual_delay:%u "
				"average_delay_:%d clock_drift_:%d clock_drift_raw_:%d delay_penalty:%d current_delay_sum_:" I64u
				"current_delay_samples_:%d average_delay_base_:%d last_maxed_out_window_:" I64u " opt_sndbuf_:%d "
				"current_ms:" I64u "",
				actual_delay, our_delay / 1000, conn->cc_.their_hist().get_value() / 1000,
				(int)(our_delay / 1000 - (int)(conn->target_delay_ / 1000)), (uint)conn->cc_.max_window(), (uint32)conn->cc_.our_hist().delay_base,
				(int)((our_delay + (int)conn->cc_.their_hist().get_value()) / 1000), (int)(conn->target_delay_ / 1000), (uint)acked_bytes,
				(uint)(conn->cc_.cur_window() - acked_bytes), 0.0f, conn->cc_.rtt_ms(),
				(uint)(conn->cc_.max_window() * 1000 / (conn->cc_.rtt_hist().delay_base ? conn->cc_.rtt_hist().delay_base : 50)),
				(uint)conn->max_window_user_, conn->cc_.rto_ms(), (int)(conn->cc_.rto_timeout() - conn->ctx->current_ms_),
				utp_call_get_microseconds(conn->ctx, conn), conn->cur_window_packets_, (uint)conn->get_packet_size(),
				conn->cc_.their_hist().delay_base, conn->cc_.their_hist().delay_base + conn->cc_.their_hist().get_value(),
				conn->cc_.average_delay(), conn->cc_.clock_drift(), conn->cc_.clock_drift_raw(), 0,
				conn->cc_.current_delay_sum(), conn->cc_.current_delay_samples(), conn->cc_.average_delay_base(),
				conn->cc_.last_maxed_out_window(), (int)conn->opt_sndbuf_, uint64(conn->ctx->current_ms_));
	}

	if (acks <= conn->cur_window_packets_) {
		conn->max_window_user_ = pf1->windowsize;

		if (conn->max_window_user_ == 0)
			conn->cc_.set_zerowindow_time(conn->ctx->current_ms_ + 15000);

		if (pk_flags == ST_DATA && conn->state_ == CS_SYN_RECV) {
			conn->state_ = CS_CONNECTED;
		}
		if (pk_flags == ST_STATE && conn->state_ == CS_SYN_SENT)	{
			conn->state_ = CS_CONNECTED;

			utp_call_on_connect(conn->ctx, conn);

		} else if (conn->fin_sent && conn->cur_window_packets_ == acks) {
			conn->fin_sent_acked_ = true;
			if (conn->close_requested_) {
				conn->state_ = CS_DESTROY;
			}
		}
		if (wrapping_compare_less(conn->fast_resend_seq_nr_
			, (pk_ack_nr_ + 1) & ACK_NR_MASK, ACK_NR_MASK))
			conn->fast_resend_seq_nr_ = (pk_ack_nr_ + 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "fast_resend_seq_nr_:%u", conn->fast_resend_seq_nr_);
		#endif
		for (int i = 0; i < acks; ++i) {
			int ack_status = conn->ack_packet(conn->seq_nr_ - conn->cur_window_packets_);
			if (ack_status == 2) {
				#ifdef _DEBUG
				OutgoingPacket* pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_);
				assert(pkt->transmissions == 0);
				#endif

				break;
			}
			conn->cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", conn->cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (conn->cur_window_packets_ == 0)
			assert(conn->cc_.cur_window() == 0);
		#endif

		while (conn->cur_window_packets_ > 0 && !conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_)) {
			conn->cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", conn->cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (conn->cur_window_packets_ == 0)
			assert(conn->cc_.cur_window() == 0);
		#endif

		assert(conn->cur_window_packets_ == 0 || conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_));
		if (conn->cur_window_packets_ == 1) {
			OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - 1);
			if (pkt->transmissions == 0) {
				conn->send_packet(pkt);
			}
		}

		if (conn->fast_timeout_) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Fast timeout %u,%u,%u?", (uint)conn->cc_.cur_window(), conn->seq_nr_ - conn->timeout_seq_nr_, conn->timeout_seq_nr_);
			#endif

			if (((conn->seq_nr_ - conn->cur_window_packets_) & ACK_NR_MASK) != conn->fast_resend_seq_nr_) {
				conn->fast_timeout_ = false;
			} else {
				OutgoingPacket *pkt = (OutgoingPacket*)conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_);
				if (pkt && pkt->transmissions > 0) {

					#if UTP_DEBUG_LOGGING
					conn->log(UTP_LOG_DEBUG, "Packet %u fast timeout-retry.", conn->seq_nr_ - conn->cur_window_packets_);
					#endif

					#ifdef _DEBUG
					++conn->stats_.fastrexmit;
					#endif

					conn->fast_resend_seq_nr_++;
					conn->send_packet(pkt);
				}
			}
		}
	}
	if (selack_ptr != NULL) {
		conn->selective_ack(pk_ack_nr_ + 2, selack_ptr, selack_ptr[-1]);
	}

	assert(conn->cur_window_packets_ == 0 || conn->outbuf_.get(conn->seq_nr_ - conn->cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	conn->log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%u cur_window_:%u cur_window_packets_:%u ",
		acks, (uint)acked_bytes, conn->seq_nr_, (uint)conn->cc_.cur_window(), conn->cur_window_packets_);
	#endif

	if (conn->state_ == CS_CONNECTED_FULL && !conn->is_full()) {
		conn->state_ = CS_CONNECTED;
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
			(uint)conn->cc_.max_window(), (uint)conn->cc_.cur_window(), (uint)conn->get_packet_size());
		#endif
		utp_call_on_state_change(conn->ctx, conn, UTP_STATE_WRITABLE);
	}
	if (pk_flags == ST_STATE) {
		return 0;
	}
	if (conn->state_ != CS_CONNECTED &&
		conn->state_ != CS_CONNECTED_FULL) {
		return 0;
	}

	if (pk_flags == ST_FIN && !conn->got_fin) {
		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "Got FIN eof_pkt_:%u", pk_seq_nr_);
		#endif
		conn->got_fin = true;
		conn->eof_pkt_ = pk_seq_nr_;
	}

	if (seqnr == 0) {
		size_t count = packet_end - data;
		if (count > 0 && !conn->read_shutdown_) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Got Data len:%u (rb:%u)", (uint)count, (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif

			utp_call_on_read(conn->ctx, conn, data, count);
		}
		conn->ack_nr_++;

		for (;;) {
			if (!conn->got_fin_reached_ && conn->got_fin && conn->eof_pkt_ == conn->ack_nr_) {
				conn->got_fin_reached_ = true;
				conn->cc_.set_rto_timeout(conn->ctx->current_ms_ + min<uint>(conn->cc_.rto_ms() * 3, 60));

				#if UTP_DEBUG_LOGGING
				conn->log(UTP_LOG_DEBUG, "Posting EOF");
				#endif

				utp_call_on_state_change(conn->ctx, conn, UTP_STATE_EOF);
				conn->send_ack();

				conn->reorder_count_ = 0;
			}

			if (conn->reorder_count_ == 0)
				break;

			auto *pkt = (InboundPacket*)conn->inbuf_.get(conn->ack_nr_+1);
			if (pkt == NULL)
				break;
			conn->inbuf_.put(conn->ack_nr_+1, NULL);
			count = pkt->size;
			if (count > 0 && !conn->read_shutdown_) {
				utp_call_on_read(conn->ctx, conn, pkt->data.data(), count);
			}
			delete pkt;
			conn->ack_nr_++;

			assert(conn->reorder_count_ > 0);
			conn->reorder_count_--;
		}
		conn->schedule_ack();
	} else {
		if (conn->got_fin && pk_seq_nr_ > conn->eof_pkt_) {
			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "Got an invalid packet sequence number, past EOF "
				"reorder_count_:%u len:%u (rb:%u)",
				conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif
			return 0;
		}

		if (seqnr > 0x3ff) {

			#if UTP_DEBUG_LOGGING
			conn->log(UTP_LOG_DEBUG, "0x%08x: Got an invalid packet sequence number, too far off "
				"reorder_count_:%u len:%u (rb:%u)",
				conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
			#endif
			return 0;
		}

		conn->inbuf_.ensure_size(pk_seq_nr_ + 1, seqnr + 1);

		if (conn->inbuf_.get(pk_seq_nr_) != NULL) {
			#ifdef _DEBUG
			++conn->stats_.nduprecv;
			#endif

			return 0;
		}

		auto *pkt = new InboundPacket;
		pkt->size = (uint32_t)(packet_end - data);
		pkt->data.assign(data, packet_end);

		assert(conn->inbuf_.get(pk_seq_nr_) == NULL);
		assert((pk_seq_nr_ & conn->inbuf_.mask()) != ((conn->ack_nr_+1) & conn->inbuf_.mask()));
		conn->inbuf_.put(pk_seq_nr_, pkt);
		conn->reorder_count_++;

		#if UTP_DEBUG_LOGGING
		conn->log(UTP_LOG_DEBUG, "0x%08x: Got out of order data reorder_count_:%u len:%u (rb:%u)",
			conn->reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(conn->ctx, conn));
		#endif

		conn->schedule_ack();
	}

	return (size_t)(packet_end - data);
}

inline byte UTP_Version(PacketFormatV1 const* pf)
{
	return (pf->type() < ST_NUM_STATES && pf->ext < 3 ? pf->version() : 0);
}

int utp_process_udp(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(ctx);
	if (!ctx) return 0;

	assert(buffer);
	if (!buffer) return 0;

	assert(to);
	if (!to) return 0;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u too small", addrfmt(addr, addrbuf), (uint)len);
		#endif
		return 0;
	}

	const PacketFormatV1 *pf1 = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf1);
	const uint32 id = uint32(pf1->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u version:%u unsupported version", addrfmt(addr, addrbuf), (uint)len, version);
		#endif

		return 0;
	}

	#if UTP_DEBUG_LOGGING
	ctx->log(UTP_LOG_DEBUG, NULL, "recv %s len:%u id:%u", addrfmt(addr, addrbuf), (uint)len, id);
	ctx->log(UTP_LOG_DEBUG, NULL, "recv id:%u seq_nr:%u ack_nr:%u", id, (uint)pf1->seq_nr, (uint)pf1->ack_nr);
	#endif
	const byte flags = pf1->type();

	if (flags == ST_RESET) {
		UtpSocket* conn = nullptr;
		if (auto it = ctx->sockets_.find(UtpSocketKey(addr, id)); it != ctx->sockets_.end()) {
			conn = it->second;
		} else if (auto it2 = ctx->sockets_.find(UtpSocketKey(addr, id + 1)); it2 != ctx->sockets_.end() && it2->second->conn_id_send_ == id) {
			conn = it2->second;
		} else if (auto it3 = ctx->sockets_.find(UtpSocketKey(addr, id - 1)); it3 != ctx->sockets_.end() && it3->second->conn_id_send_ == id) {
			conn = it3->second;
		}
		if (conn) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv RST for existing connection");
			#endif
			if (conn->close_requested_)
				conn->state_ = CS_DESTROY;
			else
				conn->state_ = CS_RESET;

			utp_call_on_overhead_statistics(conn->ctx, conn, false, len + conn->get_udp_overhead(), close_overhead);
			const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
			utp_call_on_error(conn->ctx, conn, err);
		}
		else {
			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv RST for unknown connection");
			#endif
		}
		return 1;
	}
	else if (flags != ST_SYN) {
		UtpSocket* conn = NULL;
		if (ctx->last_utp_socket_ && ctx->last_utp_socket_->addr == addr && ctx->last_utp_socket_->conn_id_recv_ == id) {
			conn = ctx->last_utp_socket_;
		} else {
			auto it = ctx->sockets_.find(UtpSocketKey(addr, id));
			if (it != ctx->sockets_.end()) {
				conn = it->second;
				ctx->last_utp_socket_ = conn;
			}
		}

		if (conn) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv processing");
			#endif
			const size_t read = utp_process_incoming(conn, buffer, len);
			utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
			return 1;
		}
	}

	const uint32 seq_nr_ = pf1->seq_nr;
	if (flags != ST_SYN) {
		ctx->current_ms_ = utp_call_get_milliseconds(ctx, NULL);
		for (size_t i = 0; i < ctx->rst_info_.size(); i++) {
			if ((ctx->rst_info_[i].connid == id)   &&
				(ctx->rst_info_[i].addr   == addr) &&
				(ctx->rst_info_[i].ack_nr == seq_nr_))
			{
				ctx->rst_info_[i].timestamp = ctx->current_ms_;

				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (stored)");
				#endif

				return 1;
			}
		}

		if (ctx->rst_info_.size() > RST_INFO_LIMIT) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (limit at %u stored)", (uint)ctx->rst_info_.size());
			#endif

			return 1;
		}

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv send RST to non-SYN (%u stored)", (uint)ctx->rst_info_.size());
		#endif

		ctx->rst_info_.emplace_back(); RstInfo &r = ctx->rst_info_.back();
		r.addr = addr;
		r.connid = id;
		r.ack_nr = seq_nr_;
		r.timestamp = ctx->current_ms_;
		UtpSocket::send_rst(ctx, addr, id, seq_nr_, utp_call_get_random(ctx, NULL));
		return 1;
	}

	if (true) {

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Incoming connection from %s", addrfmt(addr, addrbuf));
		#endif

		if (ctx->sockets_.count(UtpSocketKey(addr, id + 1))) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, connection already exists");
			#endif

			return 1;
		}

		if (ctx->sockets_.size() > 3000) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, too many uTP sockets %zu", ctx->sockets_.size());
			#endif

			return 1;
		}
		if (utp_call_on_firewall(ctx, to, tolen)) {

			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, firewall callback returned true");
			#endif

			return 1;
		}
		UtpSocket *conn = utp_create_socket(ctx);
		utp_initialize_socket(conn, to, tolen, false, id, id+1, id);
		conn->ack_nr_ = seq_nr_;
		conn->seq_nr_ = utp_call_get_random(ctx, NULL);
		conn->fast_resend_seq_nr_ = conn->seq_nr_;
		conn->state_ = CS_SYN_RECV;
		const size_t read = utp_process_incoming(conn, buffer, len, true);

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "recv send connect ACK");
		#endif
		conn->send_ack(true);
		utp_call_on_accept(ctx, conn, to, tolen);

		utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
		utp_call_on_overhead_statistics(conn->ctx, conn, true,  conn->get_overhead(),                    ack_overhead);
	}
	else {

		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, UTP_ON_ACCEPT callback not set");
		#endif

	}

	return 1;
}

static UtpSocket* parse_icmp_payload(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(ctx);
	if (!ctx) return NULL;

	assert(buffer);
	if (!buffer) return NULL;

	assert(to);
	if (!to) return NULL;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: runt length %d", addrfmt(addr, addrbuf), len);
		#endif
		return NULL;
	}

	const PacketFormatV1 *pf = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf);
	const uint32 id = uint32(pf->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: not UTP version 1", addrfmt(addr, addrbuf));
		#endif
		return NULL;
	}

	UtpSocket* conn = nullptr;
	if (auto it = ctx->sockets_.find(UtpSocketKey(addr, id)); it != ctx->sockets_.end()) {
		conn = it->second;
	} else if (auto it2 = ctx->sockets_.find(UtpSocketKey(addr, id + 1)); it2 != ctx->sockets_.end() && it2->second->conn_id_send_ == id) {
		conn = it2->second;
	} else if (auto it3 = ctx->sockets_.find(UtpSocketKey(addr, id - 1)); it3 != ctx->sockets_.end() && it3->second->conn_id_send_ == id) {
		conn = it3->second;
	}
	if (conn) {
		return conn;
	}

	#if UTP_DEBUG_LOGGING
	ctx->log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: No matching connection found for id %u", addrfmt(addr, addrbuf), id);
	#endif
	return NULL;
}

int utp_process_icmp_fragmentation(utp_context *ctx, const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu)
{
	UtpSocket* conn = parse_icmp_payload(ctx, buffer, len, to, tolen);
	if (!conn) return 0;

	if (next_hop_mtu >= 576 && next_hop_mtu < 0x2000) {
		conn->mtu_.handle_icmp_fragmentation(next_hop_mtu, conn->ctx->current_ms_);
	} else {
		conn->mtu_.handle_icmp_unknown(conn->ctx->current_ms_);
	}

	conn->log(UTP_LOG_MTU, "MTU [ICMP] floor:%d ceiling:%d current:%d"
		, conn->mtu_.floor(), conn->mtu_.ceiling(), conn->mtu_.last());
	return 1;
}

int utp_process_icmp_error(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	UtpSocket* conn = parse_icmp_payload(ctx, buffer, len, to, tolen);
	if (!conn) return 0;

	const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	switch(conn->state_) {
		case CS_IDLE:
			#if UTP_DEBUG_LOGGING
			ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s in state CS_IDLE, ignoring", addrfmt(addr, addrbuf));
			#endif
			return 1;

		default:
			if (conn->close_requested_) {
				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s after close, setting state to CS_DESTROY and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_DESTROY;
			} else {
				#if UTP_DEBUG_LOGGING
				ctx->log(UTP_LOG_DEBUG, NULL, "ICMP from %s, setting state to CS_RESET and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_RESET;
			}
			break;
	}

	utp_call_on_error(conn->ctx, conn, err);
	return 1;
}
