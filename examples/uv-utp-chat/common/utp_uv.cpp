#include "utp_uv.h"

#include <cstdio>
#include <cstring>

namespace chat {

namespace {
UtpUvNode* node_of(utp_callback_arguments* a) {
	return (UtpUvNode*)utp_context_get_userdata(a->context);
}
Connection* conn_of(utp_callback_arguments* a) {
	return a->socket ? (Connection*)utp_get_userdata(a->socket) : nullptr;
}
}  // namespace

// ── 构造 / 析构 ──────────────────────────────────────────────

UtpUvNode::UtpUvNode(uv_loop_t* loop, const Config& cfg, Hooks hooks)
	: loop_(loop), hooks_(std::move(hooks)), accept_incoming_(cfg.accept_incoming),
	  loop_tid_(uv_thread_self()) {
	ctx_ = utp_init(2);
	utp_context_set_userdata(ctx_, this);

	utp_set_callback(ctx_, UTP_SENDTO, &UtpUvNode::s_sendto);
	utp_set_callback(ctx_, UTP_ON_ACCEPT, &UtpUvNode::s_on_accept);
	utp_set_callback(ctx_, UTP_ON_READ, &UtpUvNode::s_on_read);
	utp_set_callback(ctx_, UTP_ON_STATE_CHANGE, &UtpUvNode::s_on_state);
	utp_set_callback(ctx_, UTP_ON_ERROR, &UtpUvNode::s_on_error);
	utp_set_callback(ctx_, UTP_ON_FIREWALL, &UtpUvNode::s_on_firewall);
	utp_set_callback(ctx_, UTP_GET_READ_BUFFER_SIZE, &UtpUvNode::s_read_buffer_size);
	if (cfg.debug_log) {
		utp_set_callback(ctx_, UTP_LOG, &UtpUvNode::s_log);
		utp_context_set_option(ctx_, UTP_LOG_NORMAL, 1);
		utp_context_set_option(ctx_, UTP_LOG_DEBUG, 1);
		utp_context_set_option(ctx_, UTP_LOG_MTU, 1);
	}
	// 时间/随机数回调不设置：UtpCallbacks 基类默认实现（单调时钟）即为所需

	uv_udp_init(loop_, &udp_);
	udp_.data = this;
	if (int rc = uv_udp_bind(&udp_, (const sockaddr*)&cfg.bind_addr, 0); rc != 0) {
		fprintf(stderr, "uv_udp_bind: %s\n", uv_strerror(rc));
		abort();
	}
	uv_udp_recv_start(&udp_, &UtpUvNode::s_alloc, &UtpUvNode::s_recv);

	uv_timer_init(loop_, &timer_);
	timer_.data = this;
	uv_timer_start(&timer_, &UtpUvNode::s_timer, 500, 500);

	uv_check_init(loop_, &check_);
	check_.data = this;
	uv_check_start(&check_, &UtpUvNode::s_check);
}

UtpUvNode::~UtpUvNode() {
	// 约定：调用方先 begin_shutdown() 并把 loop 跑到句柄全部关闭再析构。
	// utp_destroy 必须最后调——析构期间仍会发 DESTROYING 回调（此时表已空）。
	utp_destroy(ctx_);
}

// ── 公开操作（loop 线程）─────────────────────────────────────

Connection* UtpUvNode::connect(const sockaddr_in& to) {
	assert_loop_thread();
	utp_socket* s = utp_create_socket(ctx_);
	Connection* c = adopt(s);
	utp_connect(s, (const sockaddr*)&to, sizeof(to));
	return c;
}

void UtpUvNode::write(Connection& c, std::vector<uint8_t> frame) {
	assert_loop_thread();
	if (!c.sock || c.closing) return;
	c.outq.push_back(std::move(frame));
	flush(c);
}

void UtpUvNode::close(Connection& c) {
	assert_loop_thread();
	if (!c.sock || c.closing) return;
	c.closing = true;
	utp_close(c.sock);  // FIN；DESTROYING 回调后才真正移除
}

void UtpUvNode::begin_shutdown() {
	assert_loop_thread();
	if (shutdown_) return;
	shutdown_ = true;
	// close 不会同步移除元素（移除发生在 DESTROYING），直接遍历安全
	for (auto& [id, c] : conns_) close(*c);
	maybe_close_handles();
}

void UtpUvNode::consumed(uint64_t id, size_t bytes) {
	assert_loop_thread();
	Connection* c = find(id);
	if (!c) return;  // 连接已死：结果作废
	assert(c->unconsumed >= bytes);
	c->unconsumed -= bytes;
	// 窗口可能因积压收缩过，通告新窗口（内部只在窗口实际变大时发 ACK）
	if (c->sock && !c->closing) utp_read_drained(c->sock);
}

Connection* UtpUvNode::find(uint64_t id) {
	auto it = conns_.find(id);
	return it == conns_.end() ? nullptr : it->second.get();
}

uint16_t UtpUvNode::local_port() const {
	sockaddr_storage ss{};
	int len = sizeof(ss);
	uv_udp_getsockname(&udp_, (sockaddr*)&ss, &len);
	return ntohs(((sockaddr_in*)&ss)->sin_port);
}

// ── 内部 ────────────────────────────────────────────────────

Connection* UtpUvNode::adopt(utp_socket* sock) {
	auto c = std::make_unique<Connection>();
	c->id = next_id_++;
	c->sock = sock;
	c->node = this;
	utp_set_userdata(sock, c.get());
	Connection* raw = c.get();
	conns_.emplace(raw->id, std::move(c));
	return raw;
}

void UtpUvNode::flush(Connection& c) {
	if (!c.sock || !c.connected) return;  // 未建立时排队，CONNECT 后续写
	while (!c.outq.empty()) {
		auto& front = c.outq.front();
		const ssize_t n = utp_write(c.sock, front.data() + c.out_off,
		                            front.size() - c.out_off);
		if (n <= 0) break;  // 发送窗口满，等 WRITABLE
		c.out_off += (size_t)n;
		if (c.out_off == front.size()) {
			c.outq.pop_front();
			c.out_off = 0;
		}
	}
}

void UtpUvNode::on_destroying(Connection& c) {
	c.sock = nullptr;
	if (hooks_.on_destroy) hooks_.on_destroy(c);
	conns_.erase(c.id);  // c 失效
	maybe_close_handles();
}

void UtpUvNode::maybe_close_handles() {
	if (!shutdown_ || handles_closing_ || !conns_.empty()) return;
	handles_closing_ = true;
	uv_udp_recv_stop(&udp_);
	uv_timer_stop(&timer_);
	uv_check_stop(&check_);
	uv_close((uv_handle_t*)&udp_, &UtpUvNode::s_handle_closed);
	uv_close((uv_handle_t*)&timer_, &UtpUvNode::s_handle_closed);
	uv_close((uv_handle_t*)&check_, &UtpUvNode::s_handle_closed);
}

void UtpUvNode::s_handle_closed(uv_handle_t*) {}

// ── libutp 回调 ─────────────────────────────────────────────

uint64 UtpUvNode::s_sendto(utp_callback_arguments* a) {
	UtpUvNode* n = node_of(a);
	// UTP_SENDTO 的缓冲区仅在回调期间有效 → 用 try_send（同步交内核，无需保活）。
	// 内核缓冲满（UV_EAGAIN）直接丢弃：uTP 按丢包设计，重传会兜底。
	uv_buf_t buf = uv_buf_init((char*)a->buf, (unsigned)a->len);
	uv_udp_try_send(&n->udp_, &buf, 1, a->address);
	return 0;
}

uint64 UtpUvNode::s_on_accept(utp_callback_arguments* a) {
	UtpUvNode* n = node_of(a);
	Connection* c = n->adopt(a->socket);
	c->connected = true;  // 入站连接到达即可读写
	if (n->hooks_.on_accept) n->hooks_.on_accept(*c);
	return 0;
}

uint64 UtpUvNode::s_on_read(utp_callback_arguments* a) {
	UtpUvNode* n = node_of(a);
	Connection* c = conn_of(a);
	if (c && n->hooks_.on_read)
		n->hooks_.on_read(*c, std::span<const uint8_t>(a->buf, a->len));
	// 字节已交给应用（拷入分帧器/派发线程池）；线程池积压经
	// UTP_GET_READ_BUFFER_SIZE 单独反馈，这里即时确认本批字节
	if (a->socket) utp_read_drained(a->socket);
	return 0;
}

uint64 UtpUvNode::s_on_state(utp_callback_arguments* a) {
	UtpUvNode* n = node_of(a);
	Connection* c = conn_of(a);
	if (!c) return 0;
	switch (a->state) {
	case UTP_STATE_CONNECT:
		c->connected = true;
		if (n->hooks_.on_connect) n->hooks_.on_connect(*c);
		n->flush(*c);
		break;
	case UTP_STATE_WRITABLE:
		n->flush(*c);
		break;
	case UTP_STATE_EOF:
		if (n->hooks_.on_eof) n->hooks_.on_eof(*c);
		break;
	case UTP_STATE_DESTROYING:
		n->on_destroying(*c);
		break;
	}
	return 0;
}

uint64 UtpUvNode::s_on_error(utp_callback_arguments* a) {
	UtpUvNode* n = node_of(a);
	Connection* c = conn_of(a);
	if (c) {
		if (n->hooks_.on_error) n->hooks_.on_error(*c, a->error_code);
		// 出错的 socket 不会再可用，主动进入关闭流程
		n->close(*c);
	}
	return 0;
}

uint64 UtpUvNode::s_on_firewall(utp_callback_arguments* a) {
	return node_of(a)->accept_incoming_ ? 0 : 1;  // 非 0 = 拒绝入站
}

uint64 UtpUvNode::s_read_buffer_size(utp_callback_arguments* a) {
	// 接收窗口回压：通告值 = opt_rcvbuf - 本返回值。
	// 线程池积压越多，窗口越小，对端发送越慢。
	Connection* c = conn_of(a);
	return c ? c->unconsumed : 0;
}

uint64 UtpUvNode::s_log(utp_callback_arguments* a) {
	fprintf(stderr, "[utp] %s\n", (const char*)a->buf);
	return 0;
}

// ── libuv 回调 ──────────────────────────────────────────────

void UtpUvNode::s_alloc(uv_handle_t* h, size_t, uv_buf_t* buf) {
	UtpUvNode* n = (UtpUvNode*)h->data;
	*buf = uv_buf_init(n->rxbuf_, sizeof(n->rxbuf_));
}

void UtpUvNode::s_recv(uv_udp_t* h, ssize_t nread, const uv_buf_t* buf,
                       const sockaddr* addr, unsigned) {
	UtpUvNode* n = (UtpUvNode*)h->data;
	if (nread <= 0 || !addr) return;  // 0/NULL = 本批结束；<0 = 瞬态错误，忽略
	const socklen_t alen = addr->sa_family == AF_INET ? sizeof(sockaddr_in)
	                                                  : sizeof(sockaddr_in6);
	utp_process_udp(n->ctx_, (const byte*)buf->base, (size_t)nread, addr, alen);
}

void UtpUvNode::s_timer(uv_timer_t* h) {
	utp_check_timeouts(((UtpUvNode*)h->data)->ctx_);
}

void UtpUvNode::s_check(uv_check_t* h) {
	// 每轮事件循环末尾合并发出延迟 ACK（一个 ACK 确认整批数据包）
	utp_issue_deferred_acks(((UtpUvNode*)h->data)->ctx_);
}

}  // namespace chat
