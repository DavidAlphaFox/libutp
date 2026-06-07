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

// =====================================================================================
// 模块说明 (Module Description)
// -------------------------------------------------------------------------------------
// 本文件是 libutp（uTP / Micro Transport Protocol）协议库的核心数据包处理模块，
// 负责将下层 UDP 套接字接收到的原始字节流解析为 uTP 协议数据，并完成：
//
//   1) 入口路由：utp_process_udp()
//      应用层在 UDP 套接字上读到数据后调用此函数。它根据数据包首部的 type 字段
//      （ST_DATA / ST_FIN / ST_STATE / ST_RESET / ST_SYN）把数据分发到不同处理路径，
//      并管理 SYN 握手、RST 抑制、对未知连接回送 RST 等“控制平面”行为。
//
//   2) 入站处理：utp_process_incoming()
//      单个已存在连接收到对端发来的数据包后被调用。它负责：
//        - 解析 uTP 头部（版本号、类型、连接 ID、序列号、ACK 号、窗口、时间戳）；
//        - 处理选择性 ACK（Selective ACK）扩展，从 ACK 中恢复被跳过的包；
//        - 测量单向延迟并将样本送入 LEDBAT 拥塞控制算法；
//        - 推进发送窗口（cur_window_packets_）、处理 FIN、处理重复 ACK / 快速重传；
//        - 通过 inbuf_ 重排序缓冲区（reorder buffer）处理乱序到达的 ST_DATA 包，
//          一旦序列号连续就交付给应用层 on_read 回调。
//
//   3) ICMP 处理：parse_icmp_payload() / utp_process_icmp_fragmentation() / utp_process_icmp_error()
//      当下层 UDP 套接字收到 ICMP “目的不可达” / “需要分片” 报文时，
//      内核会把触发该报文的原始 uTP 头部作为 payload 一并返回。这组函数负责
//      从 ICMP payload 中识别出受影响的 uTP 连接并通知 MTU 发现模块或终止连接。
//
// 总体数据流（简化）：
//
//   udp_read() --> utp_process_udp()  --(SYN)-->  新建连接并回 SYN-ACK
//                                |
//                                +--(RST)-->   处理复位
//                                |
//                                +--(其他)--> utp_process_incoming()
//                                                  |
//                                                  +--> on_read() / schedule_ack()
//                                                  +--> LEDBAT 拥塞控制更新
//
// 关键不变量：
//   - 连接以 (peer_addr, recv_id) 为键哈希存储于 UtpContext::sockets_；
//   - 发送窗口大小由 cur_window_packets_ 跟踪，拥塞窗口由 LedbatController 跟踪；
//   - 所有延迟样本来自对端的 tv_usec / reply_micro 字段（绝对时间戳 - 回显时间戳）。
// =====================================================================================

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

// =====================================================================================
// 函数：utp_register_recv_packet
// -------------------------------------------------------------------------------------
// 作用：
//   记录一次 uTP 数据包接收事件，并按数据包大小（bucket）累加上下文级统计计数器。
//   该函数只做“记账”，不修改任何协议状态，调用开销极小，因此被 utp_process_incoming
//   在解析头部之前无条件调用一次。
//
// 参数：
//   conn - 接收数据包的连接（用于 _DEBUG 模式下的 per-socket 统计）
//   len  - 接收到的原始 UDP 负载长度（包含 uTP 头 + 扩展 + 数据）
//
// 副作用：
//   - 在 _DEBUG 模式下累加 conn->stats_.nrecv / nbytes_recv
//   - 累加 ctx->context_stats_._nraw_recv[bucket] 计数器
//
// 桶（bucket）划分（见 config.hpp）：
//   PACKET_SIZE_EMPTY   (23  B)  - 空包 / 仅控制头
//   PACKET_SIZE_SMALL   (373 B)  - 小包
//   PACKET_SIZE_MID     (723 B)  - 中包
//   PACKET_SIZE_BIG     (1400 B)- 大包
//   PACKET_SIZE_HUGE    (>1400) - 超大包
// =====================================================================================
void UtpSocket::register_recv_packet(size_t len)
{
	#ifdef _DEBUG
	++stats_.nrecv;                  // DEBUG 模式下递增接收包计数
	stats_.nbytes_recv += len;        // DEBUG 模式下累加接收字节数
	#endif

	if (len <= PACKET_SIZE_MID) {
		if (len <= PACKET_SIZE_EMPTY) {
			ctx->context_stats_._nraw_recv[PACKET_SIZE_EMPTY_BUCKET]++;
		} else if (len <= PACKET_SIZE_SMALL) {
			ctx->context_stats_._nraw_recv[PACKET_SIZE_SMALL_BUCKET]++;
		} else
			ctx->context_stats_._nraw_recv[PACKET_SIZE_MID_BUCKET]++;
	} else {
		if (len <= PACKET_SIZE_BIG) {
			ctx->context_stats_._nraw_recv[PACKET_SIZE_BIG_BUCKET]++;
		} else
			ctx->context_stats_._nraw_recv[PACKET_SIZE_HUGE_BUCKET]++;
	}
}

size_t UtpSocket::process_incoming(const byte *packet, size_t len, bool syn)
{
	register_recv_packet(len);
	ctx->current_ms_ = utp_call_get_milliseconds(ctx, this);

	const PacketFormatV1 *pf1 = (PacketFormatV1*)packet;
	const byte *packet_end = packet + len;

	uint16 pk_seq_nr_ = pf1->seq_nr;
	uint16 pk_ack_nr_ = pf1->ack_nr;
	uint8 pk_flags   = pf1->type();

	if (pk_flags >= ST_NUM_STATES) return 0;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Got %s. seq_nr_:%u ack_nr_:%u state:%s timestamp:" I64u " reply_micro_:%u"
		, flagnames[pk_flags], pk_seq_nr_, pk_ack_nr_, statenames[state_]
		, uint64(pf1->tv_usec), (uint32)(pf1->reply_micro));
	#endif

	uint64 time = utp_call_get_microseconds(ctx, this);
	const uint16 curr_window = max<uint16>(cur_window_packets_ + ACK_NR_ALLOWED_WINDOW, ACK_NR_ALLOWED_WINDOW);

	if ((pk_flags != ST_SYN || state_ != CS_SYN_RECV) &&
		(wrapping_compare_less(seq_nr_ - 1, pk_ack_nr_, ACK_NR_MASK)
		|| wrapping_compare_less(pk_ack_nr_, seq_nr_ - 1 - curr_window, ACK_NR_MASK))) {
#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Invalid ack_nr_: %u. our seq_nr_: %u last unacked: %u"
	, pk_ack_nr_, seq_nr_, (seq_nr_ - cur_window_packets_) & ACK_NR_MASK);
#endif
		return 0;
	}

	assert(pk_flags != ST_RESET);

	const byte *selack_ptr = NULL;

	const byte *data = (const byte*)pf1 + get_header_size();
	if (get_header_size() > len) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Invalid packet size (less than header size)");
		#endif

		return 0;
	}
	uint extension = pf1->ext;
	if (extension != 0) {
		do {
			data += 2;

			if ((int)(packet_end - data) < 0 || (int)(packet_end - data) < data[-1]) {

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "Invalid len of extensions_");
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
					log(UTP_LOG_DEBUG, "Invalid len of extension bits header");
					#endif

					return 0;
				}
				memcpy(extensions_, data, 8);

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "got extension bits:%02x%02x%02x%02x%02x%02x%02x%02x",
					extensions_[0], extensions_[1], extensions_[2], extensions_[3],
					extensions_[4], extensions_[5], extensions_[6], extensions_[7]);
				#endif
			}
			extension = data[-2];
			data += data[-1];
		} while (extension);
	}

	if (state_ == CS_SYN_SENT) {
		ack_nr_ = (pk_seq_nr_ - 1) & SEQ_NR_MASK;
	}
	last_got_packet_ = ctx->current_ms_;
	if (syn) {
		return 0;
	}

	const uint seqnr = (pk_seq_nr_ - ack_nr_ - 1) & SEQ_NR_MASK;
	if (seqnr >= REORDER_BUFFER_MAX_SIZE) {
		if (seqnr >= (SEQ_NR_MASK + 1) - REORDER_BUFFER_MAX_SIZE && pk_flags != ST_STATE) {
			schedule_ack();
		}

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "    Got old Packet/Ack (%u/%u)=%u"
			, pk_seq_nr_, ack_nr_, seqnr);
		#endif
		return 0;
	}

	int acks = (pk_ack_nr_ - (seq_nr_ - 1 - cur_window_packets_)) & ACK_NR_MASK;

	if (acks > cur_window_packets_) acks = 0;

	if (cur_window_packets_ > 0) {
		if (pk_ack_nr_ == ((seq_nr_ - cur_window_packets_ - 1) & ACK_NR_MASK)
			&& cur_window_packets_ > 0
			&& pk_flags == ST_STATE) {
			++duplicate_ack_;
			if (duplicate_ack_ == DUPLICATE_ACKS_BEFORE_RESEND && mtu_.probe_seq()) {
				if (pk_ack_nr_ == ((mtu_.probe_seq() - 1) & ACK_NR_MASK)) {
					mtu_.handle_probe_loss(ctx->current_ms_);
					log(UTP_LOG_MTU, "MTU [DUPACK] floor:%d ceiling:%d current:%d"
						, mtu_.floor(), mtu_.ceiling(), mtu_.last());
				} else {
					mtu_.clear_probe();
				}
			}
		} else {
			duplicate_ack_ = 0;
		}

		// TODO: if duplicate_ack_ == DUPLICATE_ACK_BEFORE_RESEND
		// and fast_resend_seq_nr_ <= ack_nr_ + 1
		//    resend ack_nr_ + 1
		// also call maybe_decay_win()
	}

	size_t acked_bytes = 0;

	int64 min_rtt = INT64_MAX;

	uint64 now = utp_call_get_microseconds(ctx, this);

	for (int i = 0; i < acks; ++i) {
		int seq = (seq_nr_ - cur_window_packets_ + i) & ACK_NR_MASK;
		OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq);
		if (pkt == 0 || pkt->transmissions == 0) continue;
		assert((int)(pkt->payload) >= 0);
		acked_bytes += pkt->payload;
		if (mtu_.handle_probe_ack(seq, ctx->current_ms_)) {
			log(UTP_LOG_MTU, "MTU [ACK] floor:%d ceiling:%d current:%d"
				, mtu_.floor(), mtu_.ceiling(), mtu_.last());
		}
		if (pkt->time_sent < now)
			min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
		else
			min_rtt = min<int64>(min_rtt, 50000);
	}

	if (selack_ptr != NULL) {
		acked_bytes += selective_ack_bytes((pk_ack_nr_ + 2) & ACK_NR_MASK,
												 selack_ptr, selack_ptr[-1], min_rtt);
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%d cur_window_:%u cur_window_packets_:%u relative_seqnr:%u max_window_:%u min_rtt:%u rtt:%u",
		acks, (uint)acked_bytes, seq_nr_, (uint)cc_.cur_window(), cur_window_packets_,
		seqnr, (uint)cc_.max_window(), (uint)(min_rtt / 1000), cc_.rtt_ms());
	#endif

	uint64 p = pf1->tv_usec;

	last_measured_delay_ = ctx->current_ms_;

	const uint32 their_delay = (uint32)(p == 0 ? 0 : time - p);
	reply_micro_ = their_delay;
	uint32 prev_delay_base = cc_.their_hist().delay_base;
	if (their_delay != 0) cc_.their_hist().add_sample(their_delay, ctx->current_ms_);

	if (prev_delay_base != 0 &&
		wrapping_compare_less(cc_.their_hist().delay_base, prev_delay_base, TIMESTAMP_MASK)) {
		if (prev_delay_base - cc_.their_hist().delay_base <= 10000) {
			cc_.our_hist().shift(prev_delay_base - cc_.their_hist().delay_base);
		}
	}
	const uint32 actual_delay = (uint32(pf1->reply_micro) == INT_MAX ? 0 : uint32(pf1->reply_micro));

	if (actual_delay != 0) {
		cc_.our_hist().add_sample(actual_delay, ctx->current_ms_);
		cc_.update_delay_average(actual_delay, ctx->current_ms_);
	}

/*	if (prev_delay_base != 0 &&
		wrapping_compare_less(cc_.our_hist().delay_base, prev_delay_base)) {
		if (prev_delay_base - cc_.our_hist().delay_base <= 10000) {
			cc_.their_hist().Shift(prev_delay_base - cc_.our_hist().delay_base);
		}
	}
*/

	assert(min_rtt >= 0);
	if (int64(cc_.our_hist().get_value()) > min_rtt) {
		cc_.our_hist().shift((uint32)(cc_.our_hist().get_value() - min_rtt));
	}

	if (actual_delay != 0 && acked_bytes >= 1) {
		int32 our_delay = cc_.apply_ccontrol(
			acked_bytes, actual_delay, min_rtt,
			ctx->current_ms_, opt_sndbuf_, target_delay_,
			get_packet_size());
		utp_call_on_delay_sample(ctx, this, our_delay / 1000);

		// used in parse_log.py
		log(UTP_LOG_NORMAL, "actual_delay:%u our_delay:%d their_delay:%u off_target:%d max_window_:%u "
				"delay_base:%u delay_sum:%d target_delay_:%d acked_bytes:%u cur_window_:%u "
				"scaled_gain:%f rtt:%u rate:%u wnduser:%u rto:%u timeout:%d get_microseconds:" I64u " "
				"cur_window_packets_:%u packet_size:%u their_delay_base:%u their_actual_delay:%u "
				"average_delay_:%d clock_drift_:%d clock_drift_raw_:%d delay_penalty:%d current_delay_sum_:" I64u
				"current_delay_samples_:%d average_delay_base_:%d last_maxed_out_window_:" I64u " opt_sndbuf_:%d "
				"current_ms:" I64u "",
				actual_delay, our_delay / 1000, cc_.their_hist().get_value() / 1000,
				(int)(our_delay / 1000 - (int)(target_delay_ / 1000)), (uint)cc_.max_window(), (uint32)cc_.our_hist().delay_base,
				(int)((our_delay + (int)cc_.their_hist().get_value()) / 1000), (int)(target_delay_ / 1000), (uint)acked_bytes,
				(uint)(cc_.cur_window() - acked_bytes), 0.0f, cc_.rtt_ms(),
				(uint)(cc_.max_window() * 1000 / (cc_.rtt_hist().delay_base ? cc_.rtt_hist().delay_base : 50)),
				(uint)max_window_user_, cc_.rto_ms(), (int)(cc_.rto_timeout() - ctx->current_ms_),
				utp_call_get_microseconds(ctx, this), cur_window_packets_, (uint)get_packet_size(),
				cc_.their_hist().delay_base, cc_.their_hist().delay_base + cc_.their_hist().get_value(),
				cc_.average_delay(), cc_.clock_drift(), cc_.clock_drift_raw(), 0,
				cc_.current_delay_sum(), cc_.current_delay_samples(), cc_.average_delay_base(),
				cc_.last_maxed_out_window(), (int)opt_sndbuf_, uint64(ctx->current_ms_));
	}

	if (acks <= cur_window_packets_) {
		max_window_user_ = pf1->windowsize;

		if (max_window_user_ == 0)
			cc_.set_zerowindow_time(ctx->current_ms_ + 15000);

		if (pk_flags == ST_DATA && state_ == CS_SYN_RECV) {
			state_ = CS_CONNECTED;
		}
		if (pk_flags == ST_STATE && state_ == CS_SYN_SENT)	{
			state_ = CS_CONNECTED;

			utp_call_on_connect(ctx, this);

		} else if (send_.fin_sent && cur_window_packets_ == acks) {
			send_.fin_sent_acked_ = true;
			if (send_.close_requested_) {
				state_ = CS_DESTROY;
			}
		}
		if (wrapping_compare_less(fast_resend_seq_nr_
			, (pk_ack_nr_ + 1) & ACK_NR_MASK, ACK_NR_MASK))
			fast_resend_seq_nr_ = (pk_ack_nr_ + 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "fast_resend_seq_nr_:%u", fast_resend_seq_nr_);
		#endif
		for (int i = 0; i < acks; ++i) {
			int ack_status = ack_packet(seq_nr_ - cur_window_packets_);
			if (ack_status == 2) {
				#ifdef _DEBUG
				OutgoingPacket* pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);
				assert(pkt->transmissions == 0);
				#endif

				break;
			}
			cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (cur_window_packets_ == 0)
			assert(cc_.cur_window() == 0);
		#endif

		while (cur_window_packets_ > 0 && !outbuf_.get(seq_nr_ - cur_window_packets_)) {
			cur_window_packets_--;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", cur_window_packets_);
			#endif

		}

		#ifdef _DEBUG
		if (cur_window_packets_ == 0)
			assert(cc_.cur_window() == 0);
		#endif

		assert(cur_window_packets_ == 0 || outbuf_.get(seq_nr_ - cur_window_packets_));
		if (cur_window_packets_ == 1) {
			OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - 1);
			if (pkt->transmissions == 0) {
				send_packet(pkt);
			}
		}

		if (timing_.fast_timeout_) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Fast timeout %u,%u,%u?", (uint)cc_.cur_window(), seq_nr_ - timeout_seq_nr_, timeout_seq_nr_);
			#endif

			if (((seq_nr_ - cur_window_packets_) & ACK_NR_MASK) != fast_resend_seq_nr_) {
				timing_.fast_timeout_ = false;
			} else {
				OutgoingPacket *pkt = (OutgoingPacket*)outbuf_.get(seq_nr_ - cur_window_packets_);
				if (pkt && pkt->transmissions > 0) {

					#if UTP_DEBUG_LOGGING
					log(UTP_LOG_DEBUG, "Packet %u fast timeout-retry.", seq_nr_ - cur_window_packets_);
					#endif

					#ifdef _DEBUG
					++stats_.fastrexmit;
					#endif

					fast_resend_seq_nr_++;
					send_packet(pkt);
				}
			}
		}
	}
	if (selack_ptr != NULL) {
		selective_ack(pk_ack_nr_ + 2, selack_ptr, selack_ptr[-1]);
	}

	assert(cur_window_packets_ == 0 || outbuf_.get(seq_nr_ - cur_window_packets_));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%u cur_window_:%u cur_window_packets_:%u ",
		acks, (uint)acked_bytes, seq_nr_, (uint)cc_.cur_window(), cur_window_packets_);
	#endif

	if (state_ == CS_CONNECTED_FULL && !is_full()) {
		state_ = CS_CONNECTED;
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
			(uint)cc_.max_window(), (uint)cc_.cur_window(), (uint)get_packet_size());
		#endif
		utp_call_on_state_change(ctx, this, UTP_STATE_WRITABLE);
	}
	if (pk_flags == ST_STATE) {
		return 0;
	}
	if (state_ != CS_CONNECTED &&
		state_ != CS_CONNECTED_FULL) {
		return 0;
	}

	if (pk_flags == ST_FIN && !recv_.got_fin) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Got FIN eof_pkt_:%u", pk_seq_nr_);
		#endif
		recv_.got_fin = true;
		eof_pkt_ = pk_seq_nr_;
	}

	if (seqnr == 0) {
		size_t count = packet_end - data;
		if (count > 0 && !recv_.read_shutdown) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Got Data len:%u (rb:%u)", (uint)count, (uint)utp_call_get_read_buffer_size(ctx, this));
			#endif

			utp_call_on_read(ctx, this, data, count);
		}
		ack_nr_++;

		for (;;) {
			if (!recv_.got_fin_reached_ && recv_.got_fin && eof_pkt_ == ack_nr_) {
				recv_.got_fin_reached_ = true;
				cc_.set_rto_timeout(ctx->current_ms_ + min<uint>(cc_.rto_ms() * 3, 60));

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "Posting EOF");
				#endif

				utp_call_on_state_change(ctx, this, UTP_STATE_EOF);
				send_ack();

				reorder_count_ = 0;
			}

			if (reorder_count_ == 0)
				break;

			auto *pkt = (InboundPacket*)inbuf_.get(ack_nr_+1);
			if (pkt == NULL)
				break;
			inbuf_.put(ack_nr_+1, NULL);
			count = pkt->size;
			if (count > 0 && !recv_.read_shutdown) {
				utp_call_on_read(ctx, this, pkt->data.data(), count);
			}
			delete pkt;
			ack_nr_++;

			assert(reorder_count_ > 0);
			reorder_count_--;
		}
		schedule_ack();
	} else {
		if (recv_.got_fin && pk_seq_nr_ > eof_pkt_) {
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Got an invalid packet sequence number, past EOF "
				"reorder_count_:%u len:%u (rb:%u)",
				reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(ctx, this));
			#endif
			return 0;
		}

		if (seqnr > 0x3ff) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "0x%08x: Got an invalid packet sequence number, too far off "
				"reorder_count_:%u len:%u (rb:%u)",
				reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(ctx, this));
			#endif
			return 0;
		}

		inbuf_.ensure_size(pk_seq_nr_ + 1, seqnr + 1);

		if (inbuf_.get(pk_seq_nr_) != NULL) {
			#ifdef _DEBUG
			++stats_.nduprecv;
			#endif

			return 0;
		}

		auto *pkt = new InboundPacket;
		pkt->size = (uint32_t)(packet_end - data);
		pkt->data.assign(data, packet_end);

		assert(inbuf_.get(pk_seq_nr_) == NULL);
		assert((pk_seq_nr_ & inbuf_.mask()) != ((ack_nr_+1) & inbuf_.mask()));
		inbuf_.put(pk_seq_nr_, pkt);
		reorder_count_++;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "0x%08x: Got out of order data reorder_count_:%u len:%u (rb:%u)",
			reorder_count_, (uint)(packet_end - data), (uint)utp_call_get_read_buffer_size(ctx, this));
		#endif

		schedule_ack();
	}

	return (size_t)(packet_end - data);
}

inline byte UTP_Version(PacketFormatV1 const* pf)
{
	return (pf->type() < ST_NUM_STATES && pf->ext < 3 ? pf->version() : 0);
}

int UtpContext::process_udp(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(buffer);
	if (!buffer) return 0;

	assert(to);
	if (!to) return 0;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "recv %s len:%u too small", addrfmt(addr, addrbuf), (uint)len);
		#endif
		return 0;
	}

	const PacketFormatV1 *pf1 = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf1);
	const uint32 id = uint32(pf1->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "recv %s len:%u version:%u unsupported version", addrfmt(addr, addrbuf), (uint)len, version);
		#endif

		return 0;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, NULL, "recv %s len:%u id:%u", addrfmt(addr, addrbuf), (uint)len, id);
	log(UTP_LOG_DEBUG, NULL, "recv id:%u seq_nr:%u ack_nr:%u", id, (uint)pf1->seq_nr, (uint)pf1->ack_nr);
	#endif
	const byte flags = pf1->type();

	if (flags == ST_RESET) {
		UtpSocket* conn = nullptr;
		if (auto it = sockets_.find(UtpSocketKey(addr, id)); it != sockets_.end()) {
			conn = it->second;
		} else if (auto it2 = sockets_.find(UtpSocketKey(addr, id + 1)); it2 != sockets_.end() && it2->second->conn_id_send_ == id) {
			conn = it2->second;
		} else if (auto it3 = sockets_.find(UtpSocketKey(addr, id - 1)); it3 != sockets_.end() && it3->second->conn_id_send_ == id) {
			conn = it3->second;
		}
		if (conn) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "recv RST for existing connection");
			#endif
			if (conn->send_.close_requested_)
				conn->state_ = CS_DESTROY;
			else
				conn->state_ = CS_RESET;

			utp_call_on_overhead_statistics(conn->ctx, conn, false, len + conn->get_udp_overhead(), close_overhead);
			const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
			utp_call_on_error(conn->ctx, conn, err);
		}
		else {
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "recv RST for unknown connection");
			#endif
		}
		return 1;
	}
	else if (flags != ST_SYN) {
		UtpSocket* conn = NULL;
		if (last_utp_socket_ && last_utp_socket_->addr == addr && last_utp_socket_->conn_id_recv_ == id) {
			conn = last_utp_socket_;
		} else {
			auto it = sockets_.find(UtpSocketKey(addr, id));
			if (it != sockets_.end()) {
				conn = it->second;
				last_utp_socket_ = conn;
			}
		}

		if (conn) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "recv processing");
			#endif
			const size_t read = conn->process_incoming(buffer, len);
			utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
			return 1;
		}
	}

	const uint32 seq_nr_ = pf1->seq_nr;
	if (flags != ST_SYN) {
		current_ms_ = utp_call_get_milliseconds(this, NULL);
		for (size_t i = 0; i < rst_info_.size(); i++) {
			if ((rst_info_[i].connid == id)   &&
				(rst_info_[i].addr   == addr) &&
				(rst_info_[i].ack_nr == seq_nr_))
			{
				rst_info_[i].timestamp = current_ms_;

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (stored)");
				#endif

				return 1;
			}
		}

		if (rst_info_.size() > RST_INFO_LIMIT) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "recv not sending RST to non-SYN (limit at %u stored)", (uint)rst_info_.size());
			#endif

			return 1;
		}

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "recv send RST to non-SYN (%u stored)", (uint)rst_info_.size());
		#endif

		rst_info_.emplace_back(); RstInfo &r = rst_info_.back();
		r.addr = addr;
		r.connid = id;
		r.ack_nr = seq_nr_;
		r.timestamp = current_ms_;
		UtpSocket::send_rst(this, addr, id, seq_nr_, utp_call_get_random(this, NULL));
		return 1;
	}

	if (true) {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "Incoming connection from %s", addrfmt(addr, addrbuf));
		#endif

		if (sockets_.count(UtpSocketKey(addr, id + 1))) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, connection already exists");
			#endif

			return 1;
		}

		if (sockets_.size() > 3000) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, too many uTP sockets %zu", sockets_.size());
			#endif

			return 1;
		}
		if (utp_call_on_firewall(this, to, tolen)) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, firewall callback returned true");
			#endif

			return 1;
		}
		UtpSocket *conn = utp_create_socket(this);
		utp_initialize_socket(conn, to, tolen, false, id, id+1, id);
		conn->ack_nr_ = seq_nr_;
		conn->seq_nr_ = utp_call_get_random(this, NULL);
		conn->fast_resend_seq_nr_ = conn->seq_nr_;
		conn->state_ = CS_SYN_RECV;
		const size_t read = conn->process_incoming(buffer, len, true);

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "recv send connect ACK");
		#endif
		conn->send_ack(true);
		utp_call_on_accept(this, conn, to, tolen);

		utp_call_on_overhead_statistics(conn->ctx, conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
		utp_call_on_overhead_statistics(conn->ctx, conn, true,  conn->get_overhead(),                    ack_overhead);
	}
	else {

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "rejected incoming connection, UTP_ON_ACCEPT callback not set");
		#endif

	}

	return 1;
}

UtpSocket* UtpContext::parse_icmp_payload(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	assert(buffer);
	if (!buffer) return NULL;

	assert(to);
	if (!to) return NULL;

	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	if (len < sizeof(PacketFormatV1)) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: runt length %d", addrfmt(addr, addrbuf), len);
		#endif
		return NULL;
	}

	const PacketFormatV1 *pf = (PacketFormatV1*)buffer;
	const byte version = UTP_Version(pf);
	const uint32 id = uint32(pf->connid);

	if (version != 1) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: not UTP version 1", addrfmt(addr, addrbuf));
		#endif
		return NULL;
	}

	UtpSocket* conn = nullptr;
	if (auto it = sockets_.find(UtpSocketKey(addr, id)); it != sockets_.end()) {
		conn = it->second;
	} else if (auto it2 = sockets_.find(UtpSocketKey(addr, id + 1)); it2 != sockets_.end() && it2->second->conn_id_send_ == id) {
		conn = it2->second;
	} else if (auto it3 = sockets_.find(UtpSocketKey(addr, id - 1)); it3 != sockets_.end() && it3->second->conn_id_send_ == id) {
		conn = it3->second;
	}
	if (conn) {
		return conn;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: No matching connection found for id %u", addrfmt(addr, addrbuf), id);
	#endif
	return NULL;
}

int UtpContext::process_icmp_fragmentation(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu)
{
	UtpSocket* conn = parse_icmp_payload(buffer, len, to, tolen);
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

int UtpContext::process_icmp_error(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	UtpSocket* conn = parse_icmp_payload(buffer, len, to, tolen);
	if (!conn) return 0;

	const int err = (conn->state_ == CS_SYN_SENT) ? UTP_ECONNREFUSED : UTP_ECONNRESET;
	const utp::Address addr((const SOCKADDR_STORAGE*)to, tolen);

	switch(conn->state_) {
		case CS_IDLE:
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "ICMP from %s in state CS_IDLE, ignoring", addrfmt(addr, addrbuf));
			#endif
			return 1;

		default:
			if (conn->send_.close_requested_) {
				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, NULL, "ICMP from %s after close, setting state to CS_DESTROY and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_DESTROY;
			} else {
				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, NULL, "ICMP from %s, setting state to CS_RESET and causing error %d", addrfmt(addr, addrbuf), err);
				#endif
				conn->state_ = CS_RESET;
			}
			break;
	}

	utp_call_on_error(conn->ctx, conn, err);
	return 1;
}
