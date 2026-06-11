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

	conn_.host->record_raw_recv(len);
}

size_t UtpSocket::process_incoming(const byte *packet, size_t len, bool syn)
{
	register_recv_packet(len);
	conn_.host->refresh_clock(this);

	ParsedPacket pp;
	if (!parse_packet(packet, len, pp))
		return 0;

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "Got %s. seq_nr_:%u ack_nr_:%u state:%s timestamp:" I64u " reply_micro_:%u"
		, flagnames[pp.type], pp.seq_nr, pp.ack_nr, statenames[conn_.state]
		, uint64(pp.timestamp), pp.reply_micro);
	#endif

	uint64 time = utp_call_get_microseconds(conn_.host->handle(), this);
	const uint16 curr_window = max<uint16>(send_.cur_window_packets + ACK_NR_ALLOWED_WINDOW, ACK_NR_ALLOWED_WINDOW);

	if ((pp.type != ST_SYN || conn_.state != CS_SYN_RECV) &&
		(wrapping_compare_less(send_.seq_nr - 1, pp.ack_nr, ACK_NR_MASK)
		|| wrapping_compare_less(pp.ack_nr, send_.seq_nr - 1 - curr_window, ACK_NR_MASK))) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Invalid ack_nr_: %u. our seq_nr_: %u last unacked: %u"
			, pp.ack_nr, send_.seq_nr, (send_.seq_nr - send_.cur_window_packets) & ACK_NR_MASK);
		#endif
		return 0;
	}

	assert(pp.type != ST_RESET);

	if (conn_.state == CS_SYN_SENT) {
		recv_.ack_nr = (pp.seq_nr - 1) & SEQ_NR_MASK;
	}
	timing_.last_got_packet = conn_.host->current_ms();
	if (syn) return 0;

	const uint seqnr = (pp.seq_nr - recv_.ack_nr - 1) & SEQ_NR_MASK;
	if (seqnr >= REORDER_BUFFER_MAX_SIZE) {
		if (seqnr >= (SEQ_NR_MASK + 1) - REORDER_BUFFER_MAX_SIZE && pp.type != ST_STATE) {
			schedule_ack();
		}
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "    Got old Packet/Ack (%u/%u)=%u", pp.seq_nr, recv_.ack_nr, seqnr);
		#endif
		return 0;
	}

	int acks = (pp.ack_nr - (send_.seq_nr - 1 - send_.cur_window_packets)) & ACK_NR_MASK;
	if (acks > send_.cur_window_packets) acks = 0;

	if (send_.cur_window_packets > 0) {
		if (pp.ack_nr == ((send_.seq_nr - send_.cur_window_packets - 1) & ACK_NR_MASK)
			&& send_.cur_window_packets > 0 && pp.type == ST_STATE) {
			++duplicate_ack_;
			if (duplicate_ack_ == DUPLICATE_ACKS_BEFORE_RESEND && mtu_.probe_seq()) {
				if (pp.ack_nr == ((mtu_.probe_seq() - 1) & ACK_NR_MASK)) {
					mtu_.handle_probe_loss(conn_.host->current_ms());
					log(UTP_LOG_MTU, "MTU [DUPACK] floor:%d ceiling:%d current:%d"
						, mtu_.floor(), mtu_.ceiling(), mtu_.last());
				} else {
					mtu_.clear_probe();
				}
			}
		} else {
			duplicate_ack_ = 0;
		}
	}

	[[maybe_unused]] size_t acked_bytes = process_acks(pp, acks, time);
	advance_send_window(pp, acks);

	if (pp.selack != NULL) {
		selective_ack(pp.ack_nr + 2, pp.selack, pp.selack[-1]);
	}

	assert(send_.cur_window_packets == 0 || send_.outbuf.get(send_.seq_nr - send_.cur_window_packets));

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%u cur_window_:%u cur_window_packets_:%u ",
		acks, (uint)acked_bytes, send_.seq_nr, (uint)cc_->cur_window(), send_.cur_window_packets);
	#endif

	if (conn_.state == CS_CONNECTED_FULL && !is_full()) {
		conn_.state = CS_CONNECTED;
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Socket writable. max_window_:%u cur_window_:%u packet_size:%u",
			(uint)cc_->max_window(), (uint)cc_->cur_window(), (uint)get_packet_size());
		#endif
		utp_call_on_state_change(conn_.host->handle(), this, UTP_STATE_WRITABLE);
	}
	if (pp.type == ST_STATE) return 0;
	if (conn_.state != CS_CONNECTED && conn_.state != CS_CONNECTED_FULL) return 0;

	return deliver_data(pp, seqnr);
}

bool UtpSocket::parse_packet(const byte* packet, size_t len, ParsedPacket& pp)
{
	const PacketFormatV1 *pf1 = (PacketFormatV1*)packet;
	const byte *packet_end = packet + len;

	pp.seq_nr = pf1->seq_nr;
	pp.ack_nr = pf1->ack_nr;
	pp.type   = pf1->type();

	if (pp.type >= ST_NUM_STATES) return false;

	pp.timestamp = pf1->tv_usec;
	pp.reply_micro = pf1->reply_micro;
	pp.windowsize = pf1->windowsize;
	pp.selack = NULL;

	pp.payload = (const byte*)pf1 + get_header_size();
	if (get_header_size() > len) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Invalid packet size (less than header size)");
		#endif
		return false;
	}
	pp.end = packet_end;

	uint extension = pf1->ext;
	if (extension != 0) {
		do {
			pp.payload += 2;

			if ((int)(packet_end - pp.payload) < 0 || (int)(packet_end - pp.payload) < pp.payload[-1]) {
				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "Invalid len of extensions_");
				#endif
				return false;
			}

			switch(extension) {
			case 1:
				pp.selack = pp.payload;
				break;
			case 2:
				if (pp.payload[-1] != 8) {
					#if UTP_DEBUG_LOGGING
					log(UTP_LOG_DEBUG, "Invalid len of extension bits header");
					#endif
					return false;
				}
				memcpy(conn_.extensions, pp.payload, 8);

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "got extension bits:%02x%02x%02x%02x%02x%02x%02x%02x",
					conn_.extensions[0], conn_.extensions[1], conn_.extensions[2], conn_.extensions[3],
					conn_.extensions[4], conn_.extensions[5], conn_.extensions[6], conn_.extensions[7]);
				#endif
			}
			extension = pp.payload[-2];
			pp.payload += pp.payload[-1];
		} while (extension);
	}

	return true;
}

size_t UtpSocket::process_acks(const ParsedPacket& pp, int acks, uint64 time)
{
	size_t acked_bytes = 0;

	int64 min_rtt = INT64_MAX;

	uint64 now = utp_call_get_microseconds(conn_.host->handle(), this);

	for (int i = 0; i < acks; ++i) {
		int seq = (send_.seq_nr - send_.cur_window_packets + i) & ACK_NR_MASK;
		OutgoingPacket *pkt = (OutgoingPacket*)send_.outbuf.get(seq);
		if (pkt == 0 || pkt->transmissions == 0) continue;
		assert((int)(pkt->payload) >= 0);
		acked_bytes += pkt->payload;
		if (mtu_.handle_probe_ack(seq, conn_.host->current_ms())) {
			log(UTP_LOG_MTU, "MTU [ACK] floor:%d ceiling:%d current:%d"
				, mtu_.floor(), mtu_.ceiling(), mtu_.last());
		}
		if (pkt->time_sent < now)
			min_rtt = min<int64>(min_rtt, now - pkt->time_sent);
		else
			min_rtt = min<int64>(min_rtt, 50000);
	}

	if (pp.selack != NULL) {
		acked_bytes += selective_ack_bytes((pp.ack_nr + 2) & ACK_NR_MASK,
												 pp.selack, pp.selack[-1], min_rtt);
	}

	#if UTP_DEBUG_LOGGING
	const uint seqnr_dbg = (pp.seq_nr - recv_.ack_nr - 1) & SEQ_NR_MASK;
	log(UTP_LOG_DEBUG, "acks:%d acked_bytes:%u seq_nr_:%d cur_window_:%u cur_window_packets_:%u relative_seqnr:%u max_window_:%u min_rtt:%u rtt:%u",
		acks, (uint)acked_bytes, send_.seq_nr, (uint)cc_->cur_window(), send_.cur_window_packets,
		seqnr_dbg, (uint)cc_->max_window(), (uint)(min_rtt / 1000), cc_->rtt_ms());
	#endif

	uint64 p = pp.timestamp;

	timing_.last_measured_delay = conn_.host->current_ms();

	const uint32 their_delay = (uint32)(p == 0 ? 0 : time - p);
	timing_.reply_micro = their_delay;
	uint32 prev_delay_base = cc_->their_hist().delay_base;
	if (their_delay != 0) cc_->their_hist().add_sample(their_delay, conn_.host->current_ms());

	if (prev_delay_base != 0 &&
		wrapping_compare_less(cc_->their_hist().delay_base, prev_delay_base, TIMESTAMP_MASK)) {
		if (prev_delay_base - cc_->their_hist().delay_base <= 10000) {
			cc_->our_hist().shift(prev_delay_base - cc_->their_hist().delay_base);
		}
	}
	const uint32 actual_delay = (uint32(pp.reply_micro) == INT_MAX ? 0 : uint32(pp.reply_micro));

	if (actual_delay != 0) {
		cc_->our_hist().add_sample(actual_delay, conn_.host->current_ms());
		cc_->update_delay_average(actual_delay, conn_.host->current_ms());
	}

/*	if (prev_delay_base != 0 &&
		wrapping_compare_less(cc_->our_hist().delay_base, prev_delay_base)) {
		if (prev_delay_base - cc_->our_hist().delay_base <= 10000) {
			cc_->their_hist().Shift(prev_delay_base - cc_->our_hist().delay_base);
		}
	}
*/

	assert(min_rtt >= 0);
	if (int64(cc_->our_hist().get_value()) > min_rtt) {
		cc_->our_hist().shift((uint32)(cc_->our_hist().get_value() - min_rtt));
	}

	if (actual_delay != 0 && acked_bytes >= 1) {
		int32 our_delay = cc_->apply_ccontrol(
			acked_bytes, actual_delay, min_rtt,
			conn_.host->current_ms(), send_.opt_sndbuf, conn_.target_delay,
			get_packet_size());
		utp_call_on_delay_sample(conn_.host->handle(), this, our_delay / 1000);

		// used in parse_log.py
		log(UTP_LOG_NORMAL, "actual_delay:%u our_delay:%d their_delay:%u off_target:%d max_window_:%u "
				"delay_base:%u delay_sum:%d target_delay_:%d acked_bytes:%u cur_window_:%u "
				"scaled_gain:%f rtt:%u rate:%u wnduser:%u rto:%u timeout:%d get_microseconds:" I64u " "
				"cur_window_packets_:%u packet_size:%u their_delay_base:%u their_actual_delay:%u "
				"average_delay_:%d clock_drift_:%d clock_drift_raw_:%d delay_penalty:%d current_delay_sum_:" I64u
				"current_delay_samples_:%d average_delay_base_:%d last_maxed_out_window_:" I64u " opt_sndbuf_:%d "
				"current_ms:" I64u "",
				actual_delay, our_delay / 1000, cc_->their_hist().get_value() / 1000,
				(int)(our_delay / 1000 - (int)(conn_.target_delay / 1000)), (uint)cc_->max_window(), (uint32)cc_->our_hist().delay_base,
				(int)((our_delay + (int)cc_->their_hist().get_value()) / 1000), (int)(conn_.target_delay / 1000), (uint)acked_bytes,
				(uint)(cc_->cur_window() - acked_bytes), 0.0f, cc_->rtt_ms(),
				(uint)(cc_->max_window() * 1000 / (cc_->rtt_hist().delay_base ? cc_->rtt_hist().delay_base : 50)),
				(uint)send_.max_window_user, cc_->rto_ms(), (int)(cc_->rto_timeout() - conn_.host->current_ms()),
				utp_call_get_microseconds(conn_.host->handle(), this), send_.cur_window_packets, (uint)get_packet_size(),
				cc_->their_hist().delay_base, cc_->their_hist().delay_base + cc_->their_hist().get_value(),
				cc_->average_delay(), cc_->clock_drift(), cc_->clock_drift_raw(), 0,
				cc_->current_delay_sum(), cc_->current_delay_samples(), cc_->average_delay_base(),
				cc_->last_maxed_out_window(), (int)send_.opt_sndbuf, uint64(conn_.host->current_ms()));
	}

	return acked_bytes;
}

void UtpSocket::advance_send_window(const ParsedPacket& pp, int acks)
{
	if (acks <= send_.cur_window_packets) {
		send_.max_window_user = pp.windowsize;

		if (send_.max_window_user == 0)
			cc_->set_zerowindow_time(conn_.host->current_ms() + 15000);

		if (pp.type == ST_DATA && conn_.state == CS_SYN_RECV) {
			conn_.state = CS_CONNECTED;
		}
		if (pp.type == ST_STATE && conn_.state == CS_SYN_SENT)	{
			conn_.state = CS_CONNECTED;

			utp_call_on_connect(conn_.host->handle(), this);

		} else if (send_.fin_sent && send_.cur_window_packets == acks) {
			send_.fin_sent_acked_ = true;
			if (send_.close_requested_) {
				conn_.state = CS_DESTROY;
			}
		}
		if (wrapping_compare_less(timing_.fast_resend_seq_nr
			, (pp.ack_nr + 1) & ACK_NR_MASK, ACK_NR_MASK))
			timing_.fast_resend_seq_nr = (pp.ack_nr + 1) & ACK_NR_MASK;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "fast_resend_seq_nr_:%u", timing_.fast_resend_seq_nr);
		#endif
		for (int i = 0; i < acks; ++i) {
			int ack_status = ack_packet(send_.seq_nr - send_.cur_window_packets);
			if (ack_status == 2) {
				#ifdef _DEBUG
				OutgoingPacket* pkt = (OutgoingPacket*)send_.outbuf.get(send_.seq_nr - send_.cur_window_packets);
				assert(pkt->transmissions == 0);
				#endif

				break;
			}
			send_.cur_window_packets--;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", send_.cur_window_packets);
			#endif

		}

		#ifdef _DEBUG
		if (send_.cur_window_packets == 0)
			assert(cc_->cur_window() == 0);
		#endif

		while (send_.cur_window_packets > 0 && !send_.outbuf.get(send_.seq_nr - send_.cur_window_packets)) {
			send_.cur_window_packets--;

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "decementing cur_window_packets_:%u", send_.cur_window_packets);
			#endif

		}

		#ifdef _DEBUG
		if (send_.cur_window_packets == 0)
			assert(cc_->cur_window() == 0);
		#endif

		assert(send_.cur_window_packets == 0 || send_.outbuf.get(send_.seq_nr - send_.cur_window_packets));
		if (send_.cur_window_packets == 1) {
			OutgoingPacket *pkt = (OutgoingPacket*)send_.outbuf.get(send_.seq_nr - 1);
			if (pkt->transmissions == 0) {
				send_packet(pkt);
			}
		}

		if (timing_.fast_timeout_) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Fast timeout %u,%u,%u?", (uint)cc_->cur_window(), send_.seq_nr - timing_.timeout_seq_nr, timing_.timeout_seq_nr);
			#endif

			if (((send_.seq_nr - send_.cur_window_packets) & ACK_NR_MASK) != timing_.fast_resend_seq_nr) {
				timing_.fast_timeout_ = false;
			} else {
				OutgoingPacket *pkt = (OutgoingPacket*)send_.outbuf.get(send_.seq_nr - send_.cur_window_packets);
				if (pkt && pkt->transmissions > 0) {

					#if UTP_DEBUG_LOGGING
					log(UTP_LOG_DEBUG, "Packet %u fast timeout-retry.", send_.seq_nr - send_.cur_window_packets);
					#endif

					#ifdef _DEBUG
					++stats_.fastrexmit;
					#endif

					timing_.fast_resend_seq_nr++;
					send_packet(pkt);
				}
			}
		}
	}
}

size_t UtpSocket::deliver_data(const ParsedPacket& pp, uint seqnr)
{
	if (pp.type == ST_FIN && !recv_.got_fin) {
		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "Got FIN eof_pkt_:%u", pp.seq_nr);
		#endif
		recv_.got_fin = true;
		recv_.eof_pkt = pp.seq_nr;
	}

	if (seqnr == 0) {
		size_t count = pp.end - pp.payload;
		if (count > 0 && !recv_.read_shutdown) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Got Data len:%u (rb:%u)", (uint)count, (uint)utp_call_get_read_buffer_size(conn_.host->handle(), this));
			#endif

			utp_call_on_read(conn_.host->handle(), this, pp.payload, count);
		}
		recv_.ack_nr++;

		for (;;) {
			if (!recv_.got_fin_reached_ && recv_.got_fin && recv_.eof_pkt == recv_.ack_nr) {
				recv_.got_fin_reached_ = true;
				cc_->set_rto_timeout(conn_.host->current_ms() + min<uint>(cc_->rto_ms() * 3, 60));

				#if UTP_DEBUG_LOGGING
				log(UTP_LOG_DEBUG, "Posting EOF");
				#endif

				utp_call_on_state_change(conn_.host->handle(), this, UTP_STATE_EOF);
				send_ack();

				recv_.reorder_count = 0;
			}

			if (recv_.reorder_count == 0)
				break;

			auto *pkt = (InboundPacket*)recv_.inbuf.get(recv_.ack_nr+1);
			if (pkt == NULL)
				break;
			recv_.inbuf.put(recv_.ack_nr+1, NULL);
			count = pkt->size;
			if (count > 0 && !recv_.read_shutdown) {
				utp_call_on_read(conn_.host->handle(), this, pkt->data.data(), count);
			}
			delete pkt;
			recv_.ack_nr++;

			assert(recv_.reorder_count > 0);
			recv_.reorder_count--;
		}
		schedule_ack();
	} else {
		// 序列号 mod 65536 回绕，不能直接比较绝对值；
		// 比较两者相对 ack_nr 的回绕距离，超过 FIN 的距离才算越过 EOF
		if (recv_.got_fin && seqnr > ((recv_.eof_pkt - recv_.ack_nr - 1) & SEQ_NR_MASK)) {
			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "Got an invalid packet sequence number, past EOF "
				"reorder_count_:%u len:%u (rb:%u)",
				recv_.reorder_count, (uint)(pp.end - pp.payload), (uint)utp_call_get_read_buffer_size(conn_.host->handle(), this));
			#endif
			return 0;
		}

		if (seqnr > 0x3ff) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, "0x%08x: Got an invalid packet sequence number, too far off "
				"reorder_count_:%u len:%u (rb:%u)",
				recv_.reorder_count, (uint)(pp.end - pp.payload), (uint)utp_call_get_read_buffer_size(conn_.host->handle(), this));
			#endif
			return 0;
		}

		recv_.inbuf.ensure_size(pp.seq_nr + 1, seqnr + 1);

		if (recv_.inbuf.get(pp.seq_nr) != NULL) {
			#ifdef _DEBUG
			++stats_.nduprecv;
			#endif

			return 0;
		}

		auto *pkt = new InboundPacket;
		pkt->size = (uint32_t)(pp.end - pp.payload);
		pkt->data.assign(pp.payload, pp.end);

		assert(recv_.inbuf.get(pp.seq_nr) == NULL);
		assert((pp.seq_nr & recv_.inbuf.mask()) != ((recv_.ack_nr+1) & recv_.inbuf.mask()));
		recv_.inbuf.put(pp.seq_nr, pkt);
		recv_.reorder_count++;

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, "0x%08x: Got out of order data reorder_count_:%u len:%u (rb:%u)",
			recv_.reorder_count, (uint)(pp.end - pp.payload), (uint)utp_call_get_read_buffer_size(conn_.host->handle(), this));
		#endif

		schedule_ack();
	}

	return (size_t)(pp.end - pp.payload);
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
		UtpSocket* conn = find_socket_for_id(addr, id);
		if (conn) {

			#if UTP_DEBUG_LOGGING
			log(UTP_LOG_DEBUG, NULL, "recv RST for existing connection");
			#endif
			conn->on_reset(len);
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
		if (last_utp_socket_ && last_utp_socket_->peer_addr() == addr && last_utp_socket_->recv_conn_id() == id) {
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
			utp_call_on_overhead_statistics(conn->context(), conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
			return 1;
		}
	}

	const uint32 pkt_seq_nr = pf1->seq_nr;
	if (flags != ST_SYN) {
		current_ms_ = utp_call_get_milliseconds(this, NULL);
		for (size_t i = 0; i < rst_info_.size(); i++) {
			if ((rst_info_[i].connid == id)   &&
				(rst_info_[i].addr   == addr) &&
				(rst_info_[i].ack_nr == pkt_seq_nr))
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
		r.ack_nr = pkt_seq_nr;
		r.timestamp = current_ms_;
		UtpSocket::send_rst(this, addr, id, pkt_seq_nr, utp_call_get_random(this, NULL));
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
		UtpSocket *conn = create_socket();
		conn->initialize(to, tolen, false, id, id+1, id);
		conn->accept_syn(pkt_seq_nr);
		const size_t read = conn->process_incoming(buffer, len, true);

		#if UTP_DEBUG_LOGGING
		log(UTP_LOG_DEBUG, NULL, "recv send connect ACK");
		#endif
		conn->send_ack(true);
		utp_call_on_accept(this, conn, to, tolen);

		utp_call_on_overhead_statistics(conn->context(), conn, false, (len - read) + conn->get_udp_overhead(), header_overhead);
		utp_call_on_overhead_statistics(conn->context(), conn, true,  conn->get_overhead(),                    ack_overhead);
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

	UtpSocket* conn = find_socket_for_id(addr, id);
	if (conn) {
		return conn;
	}

	#if UTP_DEBUG_LOGGING
	log(UTP_LOG_DEBUG, NULL, "Ignoring ICMP from %s: No matching connection found for id %u", addrfmt(addr, addrbuf), id);
	#endif
	return NULL;
}

UtpSocket* UtpContext::find_socket_for_id(const utp::Address &addr, uint32 id)
{
	if (auto it = sockets_.find(UtpSocketKey(addr, id)); it != sockets_.end()) {
		return it->second;
	}
	if (auto it = sockets_.find(UtpSocketKey(addr, id + 1)); it != sockets_.end() && it->second->conn_id_send() == id) {
		return it->second;
	}
	if (auto it = sockets_.find(UtpSocketKey(addr, id - 1)); it != sockets_.end() && it->second->conn_id_send() == id) {
		return it->second;
	}
	return nullptr;
}

int UtpContext::process_icmp_fragmentation(const byte* buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu)
{
	UtpSocket* conn = parse_icmp_payload(buffer, len, to, tolen);
	if (!conn) return 0;

	conn->on_icmp_fragmentation(next_hop_mtu);
	return 1;
}

int UtpContext::process_icmp_error(const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen)
{
	UtpSocket* conn = parse_icmp_payload(buffer, len, to, tolen);
	if (!conn) return 0;

	conn->on_icmp_error();
	return 1;
}
