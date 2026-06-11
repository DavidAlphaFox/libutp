#pragma once

// UtpUvNode：libutp ↔ libuv 胶水层（server / client 共用）
//
// 线程模型（整个集成的支点）：
//   libutp 是单线程库——UtpContext/UtpSocket 内部无锁，所有 utp_* 调用
//   必须发生在同一线程。本类把这条线程钉死为「跑 uv_run 的 loop 线程」：
//     - uv_udp_t   收 UDP 报文 → utp_process_udp
//     - uv_timer_t 每 500ms    → utp_check_timeouts（与库内节流间隔一致）
//     - uv_check_t 每轮事件循环末尾 → utp_issue_deferred_acks（贴合延迟 ACK 合并语义）
//     - UTP_SENDTO → uv_udp_try_send（同步、零拷贝；EAGAIN 丢包由 uTP 重传兜底）
//   线程池（TBB）永远不接触 utp 句柄，只持有连接 id + 字节拷贝，
//   结果经 uv_async 回流 loop 线程后再查表写回（见 worker_pool.h）。
//
// 读侧回压：
//   Connection::unconsumed 记录「已派发给线程池但尚未处理完」的字节数，
//   经 UTP_GET_READ_BUFFER_SIZE 反馈给 libutp → 接收窗口随计算积压收缩，
//   对端发送自动降速；线程池排空后 consumed() 调 utp_read_drained 重新开窗。

#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <uv.h>

#include "utp.h"
#include "protocol.h"

namespace chat {

class UtpUvNode;

// 单条 uTP 连接。仅允许 loop 线程访问；worker 只能持有 id（uint64，不复用），
// 回流时按 id 查表，连接已死则静默丢弃——因此无需任何跨线程锁。
struct Connection {
	uint64_t id = 0;
	utp_socket* sock = nullptr;  // UTP_STATE_DESTROYING 后置空
	UtpUvNode* node = nullptr;
	bool connected = false;
	bool closing = false;
	size_t unconsumed = 0;       // 线程池在途字节数（→ 接收窗口回压）

	// 出站待写队列：utp 发送窗口满（utp_write 返回 0）时排队，
	// UTP_STATE_WRITABLE 到来时续写
	std::deque<std::vector<uint8_t>> outq;
	size_t out_off = 0;          // outq.front() 中已被 utp_write 接受的偏移

	// 应用层状态（聊天示例：昵称 + 分帧器）
	std::string name;
	FrameParser parser;
};

class UtpUvNode {
public:
	struct Config {
		sockaddr_in bind_addr{};      // UDP 绑定地址（端口 0 = 内核分配）
		bool accept_incoming = false; // server: true；client: false（防火墙回调拒绝）
		bool debug_log = false;       // 打开 libutp 内部日志
	};

	// 应用钩子；全部在 loop 线程、utp 回调栈内同步触发
	struct Hooks {
		std::function<void(Connection&)> on_accept;                          // 新入站连接
		std::function<void(Connection&)> on_connect;                         // 出站连接建立
		std::function<void(Connection&, std::span<const uint8_t>)> on_read;  // 流字节到达
		std::function<void(Connection&)> on_eof;                             // 对端 FIN
		std::function<void(Connection&, int)> on_error;                      // UTP_E*
		std::function<void(Connection&)> on_destroy;                         // socket 已销毁
	};

	UtpUvNode(uv_loop_t* loop, const Config& cfg, Hooks hooks);
	~UtpUvNode();
	UtpUvNode(const UtpUvNode&) = delete;
	UtpUvNode& operator=(const UtpUvNode&) = delete;

	// 发起出站连接（loop 线程）
	Connection* connect(const sockaddr_in& to);

	// 写一帧；窗口满时自动排队，WRITABLE 时续写（loop 线程）
	void write(Connection& c, std::vector<uint8_t> frame);

	// 发起优雅关闭（FIN），DESTROYING 后从表中移除（loop 线程）
	void close(Connection& c);

	// 关停：close 所有连接；全部销毁后关闭 uv 句柄，loop 随之自然退出。
	// FIN 握手需要收发包，因此 uv 句柄必须等连接清零后才关。
	void begin_shutdown();

	// worker 结果回流：递减在途字节并重新通告接收窗口（loop 线程）
	void consumed(uint64_t id, size_t bytes);

	Connection* find(uint64_t id);
	size_t connection_count() const { return conns_.size(); }
	uint16_t local_port() const;
	uv_loop_t* loop() { return loop_; }

	// 遍历活跃连接（广播用）；回调内允许 write，不允许 close/erase
	template <typename F>
	void for_each(F&& f) {
		for (auto& [id, c] : conns_) f(*c);
	}

private:
	// libutp 回调（static → 经 context userdata 取回 this）
	static uint64 s_sendto(utp_callback_arguments* a);
	static uint64 s_on_accept(utp_callback_arguments* a);
	static uint64 s_on_read(utp_callback_arguments* a);
	static uint64 s_on_state(utp_callback_arguments* a);
	static uint64 s_on_error(utp_callback_arguments* a);
	static uint64 s_on_firewall(utp_callback_arguments* a);
	static uint64 s_read_buffer_size(utp_callback_arguments* a);
	static uint64 s_log(utp_callback_arguments* a);

	// libuv 回调
	static void s_alloc(uv_handle_t* h, size_t suggested, uv_buf_t* buf);
	static void s_recv(uv_udp_t* h, ssize_t nread, const uv_buf_t* buf,
	                   const sockaddr* addr, unsigned flags);
	static void s_timer(uv_timer_t* h);
	static void s_check(uv_check_t* h);
	static void s_handle_closed(uv_handle_t* h);

	Connection* adopt(utp_socket* sock);   // 建立 Connection 并登记
	void flush(Connection& c);             // 续写 outq
	void on_destroying(Connection& c);     // DESTROYING 处理
	void maybe_close_handles();            // 关停且连接清零 → 关 uv 句柄
	void assert_loop_thread() const {
		assert(uv_thread_self() == loop_tid_ && "utp 调用必须在 loop 线程");
	}

	uv_loop_t* loop_;
	utp_context* ctx_;
	uv_udp_t udp_{};
	uv_timer_t timer_{};
	uv_check_t check_{};
	Hooks hooks_;
	bool accept_incoming_;
	bool shutdown_ = false;
	bool handles_closing_ = false;
	uint64_t next_id_ = 1;
	std::unordered_map<uint64_t, std::unique_ptr<Connection>> conns_;
	uv_thread_t loop_tid_;
	char rxbuf_[64 * 1024];  // 单线程 loop：所有报文复用一块接收缓冲，零分配
};

}  // namespace chat
