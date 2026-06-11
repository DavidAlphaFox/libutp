// vim:set ts=4 sw=4 ai:

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

// libutp 公共 C API —— 纯薄委托层
// 每个 utp_* 函数只做空指针检查后 1-3 行委托给 UtpContext / UtpSocket 成员方法。
// 协议实现见 utp_socket.cpp / utp_process.cpp / utp_context.cpp。
// 本文件还包含 CFunctionCallbackAdapter：把 C 函数指针回调适配到
// UtpCallbacks 虚接口（C 边界专属，故与 C API 同文件）。

#include <string.h>
#include <cassert>
#include <memory>

#include "utp_internal.h"
#include "utp_socket.hpp"
#include "utp_callbacks.h"

// CFunctionCallbackAdapter: 将旧的 C 函数指针回调适配到 UtpCallbacks 虚接口。
// 由 utp_set_callback() 内部使用，无需用户直接操作。
class CFunctionCallbackAdapter : public UtpCallbacks {
	// 所属 context（非拥有）：C API 约定每个回调的 args->context 必须有效，
	// 而 UtpCallbacks 虚接口签名不携带 context，故由适配器回填。
	utp_context* ctx_;

	// 函数指针数组，按 UTP_* 回调 ID 索引。
	// 值为 nullptr 表示该回调未通过 utp_set_callback() 注册，应回退到 UtpCallbacks 虚基类默认实现。
	utp_callback_t* handlers_[UTP_ARRAY_SIZE] = {};

	// 构造一个 utp_callback_arguments 结构体并预填 context、callback_type 和 socket 字段。
	// 各 override 方法再按需填入其余的 args 字段。
	// 参数 type - 回调类型，对应 UTP_ON_FIREWALL/UTP_GET_MILLISECONDS 等枚举值
	// 参数 sock - 关联的 utp_socket 指针，若回调不涉及具体 socket 可传 nullptr
	// 返回: 已填充 context、callback_type 和 socket 的参数结构体
	utp_callback_arguments make_args(int type, utp_socket* sock) const {
		utp_callback_arguments args{};
		args.context = ctx_;
		args.callback_type = type;
		args.socket = sock;
		return args;
	}

public:
	explicit CFunctionCallbackAdapter(utp_context* ctx) : ctx_(ctx) {}

	// 注册（或覆盖）指定 ID 的 C 风格回调函数。
	// 由 utp_set_callback() 内部调用，外部用户通常无需直接调用。
	// 参数 id - 回调 ID（UTP_SENDTO、UTP_GET_MILLISECONDS 等）
	// 参数 fn - 用户提供的 C 风格回调函数指针，传 nullptr 表示清除
	void set_handler(int id, utp_callback_t* fn) { handlers_[id] = fn; }

	// ── ACTION ──
	// ACTION 类型回调：将数据包通过底层 UDP socket 发送出去
	// 参数 data - 待发送的数据（含 uTP 协议头）
	// 参数 len - 数据长度
	// 参数 address - 目标对端地址
	// 参数 address_len - 目标地址长度
	// 参数 flags - 平台特定的发送标志位
	void sendto(UtpSocket*, const uint8_t* data, size_t len,
	            const sockaddr* address, socklen_t address_len, uint32_t flags) override {
		if (!handlers_[UTP_SENDTO]) return;
		auto args = make_args(UTP_SENDTO, nullptr);
		args.buf = data; args.len = len;
		args.address = address; args.address_len = address_len; args.flags = flags;
		handlers_[UTP_SENDTO](&args);
	}

	// ── QUERY ──
	// QUERY 类型回调：返回当前单调递增的毫秒时间戳。
	// 如果用户未提供 UTP_GET_MILLISECONDS 回调，则回退到基类默认实现（平台相关的单调时钟）。
	uint64 get_milliseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MILLISECONDS]) return UtpCallbacks::get_milliseconds(s);
		auto args = make_args(UTP_GET_MILLISECONDS, s);
		return handlers_[UTP_GET_MILLISECONDS](&args);
	}
	// QUERY 类型回调：返回当前单调递增的微秒时间戳。
	// 精度高于 get_milliseconds，用于 LEDBAT 拥塞控制等需要更细粒度采样的场景。
	uint64 get_microseconds(UtpSocket* s) override {
		if (!handlers_[UTP_GET_MICROSECONDS]) return UtpCallbacks::get_microseconds(s);
		auto args = make_args(UTP_GET_MICROSECONDS, s);
		return handlers_[UTP_GET_MICROSECONDS](&args);
	}
	// QUERY 类型回调：返回到达指定对端地址的 UDP 路径 MTU。
	// 用于 uTP 动态 MTU 探测时的上限参考值。
	// 参数 a - 对端地址
	// 参数 l - 对端地址长度
	// 返回: 路径 MTU（字节）
	uint16 get_udp_mtu(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_MTU]) return UtpCallbacks::get_udp_mtu(s, a, l);
		auto args = make_args(UTP_GET_UDP_MTU, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_MTU](&args);
	}
	// QUERY 类型回调：返回 UDP 层（IP+UDP 头）的固定开销字节数。
	// 用于从路径 MTU 推导出 uTP 实际可用载荷大小。
	uint16 get_udp_overhead(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_GET_UDP_OVERHEAD]) return UtpCallbacks::get_udp_overhead(s, a, l);
		auto args = make_args(UTP_GET_UDP_OVERHEAD, s);
		args.address = a; args.address_len = l;
		return (uint16)handlers_[UTP_GET_UDP_OVERHEAD](&args);
	}
	// QUERY 类型回调：返回 32 位随机数。用于生成连接种子和初始序列号。
	uint32 get_random(UtpSocket* s) override {
		if (!handlers_[UTP_GET_RANDOM]) return UtpCallbacks::get_random(s);
		auto args = make_args(UTP_GET_RANDOM, s);
		return (uint32)handlers_[UTP_GET_RANDOM](&args);
	}
	// QUERY 类型回调：返回用户已读取但尚未提交的数据量。
	// 用于在拥塞控制中正确估算接收窗口大小。
	size_t get_read_buffer_size(UtpSocket* s) override {
		if (!handlers_[UTP_GET_READ_BUFFER_SIZE]) return UtpCallbacks::get_read_buffer_size(s);
		auto args = make_args(UTP_GET_READ_BUFFER_SIZE, s);
		return (size_t)handlers_[UTP_GET_READ_BUFFER_SIZE](&args);
	}

	// ── EVENT ──
	// EVENT 类型回调：判断指定对端地址是否在防火墙后。
	// 返回: 非 0 表示无法到达（应丢弃该地址的入站数据包）。
	int on_firewall(const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_FIREWALL]) return UtpCallbacks::on_firewall(a, l);
		auto args = make_args(UTP_ON_FIREWALL, nullptr);
		args.address = a; args.address_len = l;
		return (int)handlers_[UTP_ON_FIREWALL](&args);
	}
	// EVENT 类型回调：通知用户有新的入站连接已被接受。
	// 参数 s - 服务端新创建的 socket
	// 参数 a - 对端地址
	// 参数 l - 对端地址长度
	void on_accept(UtpSocket* s, const sockaddr* a, socklen_t l) override {
		if (!handlers_[UTP_ON_ACCEPT]) return;
		auto args = make_args(UTP_ON_ACCEPT, s);
		args.address = a; args.address_len = l;
		handlers_[UTP_ON_ACCEPT](&args);
	}
	// EVENT 类型回调：通知用户主动连接（SYN）已建立。
	// C API 历史语义：未注册 UTP_ON_CONNECT 时，回退为
	// UTP_ON_STATE_CHANGE(UTP_STATE_CONNECT)，经典应用（如 ucat）依赖此行为。
	// 参数 s - 已连接的 socket
	void on_connect(UtpSocket* s) override {
		if (!handlers_[UTP_ON_CONNECT]) {
			on_state_change(s, UTP_STATE_CONNECT);
			return;
		}
		auto args = make_args(UTP_ON_CONNECT, s);
		handlers_[UTP_ON_CONNECT](&args);
	}
	// EVENT 类型回调：通知用户 socket 发生错误。
	// 参数 s - 出错的 socket
	// 参数 error_code - 错误码（UTP_ECONNREFUSED/UTP_ECONNRESET/UTP_ETIMEDOUT）
	void on_error(UtpSocket* s, int error_code) override {
		if (!handlers_[UTP_ON_ERROR]) return;
		auto args = make_args(UTP_ON_ERROR, s);
		args.error_code = error_code;
		handlers_[UTP_ON_ERROR](&args);
	}
	// EVENT 类型回调：通知用户有已就绪的应用层数据可读取。
	// 参数 s - 有数据可读的 socket
	// 参数 data - 数据缓冲区
	// 参数 len - 数据长度
	void on_read(UtpSocket* s, const uint8_t* data, size_t len) override {
		if (!handlers_[UTP_ON_READ]) return;
		auto args = make_args(UTP_ON_READ, s);
		args.buf = data; args.len = len;
		handlers_[UTP_ON_READ](&args);
	}
	// EVENT 类型回调：通知用户 socket 连接状态发生变化。
	// 参数 s - 状态发生变化的 socket
	// 参数 state - 新状态（UTP_STATE_CONNECT/UTP_STATE_WRITABLE/UTP_STATE_EOF/UTP_STATE_DESTROYING）
	void on_state_change(UtpSocket* s, int state) override {
		if (!handlers_[UTP_ON_STATE_CHANGE]) return;
		auto args = make_args(UTP_ON_STATE_CHANGE, s);
		args.state = state;
		handlers_[UTP_ON_STATE_CHANGE](&args);
	}
	// EVENT 类型回调：通知用户记录一次测量的端到端延迟样本（毫秒）。
	// 参数 s - 对应的 socket
	// 参数 sample_ms - 单次延迟采样值
	void on_delay_sample(UtpSocket* s, int sample_ms) override {
		if (!handlers_[UTP_ON_DELAY_SAMPLE]) return;
		auto args = make_args(UTP_ON_DELAY_SAMPLE, s);
		args.sample_ms = sample_ms;
		handlers_[UTP_ON_DELAY_SAMPLE](&args);
	}
	// EVENT 类型回调：通知用户底层带宽开销统计信息。
	// 参数 s - 对应的 socket
	// 参数 send - true 表示发送方向，false 表示接收方向
	// 参数 len - 该次事件涉及的字节数
	// 参数 type - 带宽类型（BandwidthType 枚举）
	void on_overhead_statistics(UtpSocket* s, bool send, size_t len, int type) override {
		if (!handlers_[UTP_ON_OVERHEAD_STATISTICS]) return;
		auto args = make_args(UTP_ON_OVERHEAD_STATISTICS, s);
		args.send = send ? 1 : 0; args.len = len; args.type = type;
		handlers_[UTP_ON_OVERHEAD_STATISTICS](&args);
	}
	// EVENT 类型回调：将内部日志消息转发给用户。
	// 参数 s - 关联的 socket（可能为 nullptr）
	// 参数 message - UTF-8/ASCII 格式的日志消息
	void log(UtpSocket* s, const char* message) override {
		if (!handlers_[UTP_LOG]) return;
		auto args = make_args(UTP_LOG, s);
		args.buf = reinterpret_cast<const uint8_t*>(message);
		handlers_[UTP_LOG](&args);
	}
};

// 设置（或覆盖）单个 C 风格回调。
// 首次调用时，将默认 UtpCallbacks 替换为 C 函数指针适配器。
// （定义于此而非 utp_context.cpp：CFunctionCallbackAdapter 是 C 边界专属适配层。）
void UtpContext::set_callback(int callback_name, utp_callback_t *proc)
{
	auto* adapter = dynamic_cast<CFunctionCallbackAdapter*>(callbacks_.get());
	if (!adapter) {
		auto new_adapter = std::make_unique<CFunctionCallbackAdapter>(this);
		adapter = new_adapter.get();
		callbacks_ = std::move(new_adapter);
	}
	adapter->set_handler(callback_name, proc);
}

// =============================================================================
// C API：以下全部为薄包装（空指针检查 + 1 行委托）
// =============================================================================

extern "C" {

// 回调类型名称字符串数组，用于调试和日志输出
const char * utp_callback_names[] = {
	"UTP_ON_FIREWALL",
	"UTP_ON_ACCEPT",
	"UTP_ON_CONNECT",
	"UTP_ON_ERROR",
	"UTP_ON_READ",
	"UTP_ON_OVERHEAD_STATISTICS",
	"UTP_ON_STATE_CHANGE",
	"UTP_GET_READ_BUFFER_SIZE",
	"UTP_ON_DELAY_SAMPLE",
	"UTP_GET_UDP_MTU",
	"UTP_GET_UDP_OVERHEAD",
	"UTP_GET_MILLISECONDS",
	"UTP_GET_MICROSECONDS",
	"UTP_GET_RANDOM",
	"UTP_LOG",
	"UTP_SENDTO",
};

// 错误代码名称字符串数组
const char * utp_error_code_names[] = {
	"UTP_ECONNREFUSED",
	"UTP_ECONNRESET",
	"UTP_ETIMEDOUT",
};

// 状态名称字符串数组
const char *utp_state_names[] = {
	NULL,
	"UTP_STATE_CONNECT",
	"UTP_STATE_WRITABLE",
	"UTP_STATE_EOF",
	"UTP_STATE_DESTROYING",
};

// ── 上下文生命周期 ───────────────────────────────────────────

// 初始化 uTP 上下文
// 参数: version - 版本号，必须是 2
// 返回: 新创建的上下文指针，版本不匹配时返回 NULL
utp_context* utp_init(int version)
{
	assert(version == 2);
	if (version != 2)
		return NULL;
	return new UtpContext;
}

// 销毁 uTP 上下文及其所有资源
void utp_destroy(utp_context *ctx) {
	assert(ctx);
	if (ctx) delete ctx;
}

// 设置回调函数
void utp_set_callback(utp_context *ctx, int callback_name, utp_callback_t *proc) {
	assert(ctx);
	if (!ctx) return;
	ctx->set_callback(callback_name, proc);
}

// ── 上下文属性 ──────────────────────────────────────────────

// 设置用户自定义数据，返回之前的用户数据指针
void* utp_context_set_userdata(utp_context *ctx, void *userdata) {
	assert(ctx);
	if (!ctx) return NULL;
	ctx->set_userdata(userdata);
	return ctx->userdata();
}

// 获取用户自定义数据
void* utp_context_get_userdata(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return NULL;
	return ctx->userdata();
}

// 获取上下文统计信息
utp_context_stats* utp_get_context_stats(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return NULL;
	return ctx->stats();
}

// 设置 uTP 上下文级别的选项（UTP_LOG_*/UTP_TARGET_DELAY/UTP_SNDBUF/UTP_RCVBUF）
// 返回: 0 表示成功，-1 表示 ctx 为 NULL 或 opt 未知
int utp_context_set_option(utp_context *ctx, int opt, int val)
{
	assert(ctx);
	if (!ctx) return -1;
	return ctx->set_option(opt, val);
}

// 获取 uTP 上下文级别选项的当前值
// 返回: 选项当前值；ctx 为 NULL 或 opt 未知时返回 -1
int utp_context_get_option(utp_context *ctx, int opt)
{
	assert(ctx);
	if (!ctx) return -1;
	return ctx->get_option(opt);
}

// ── socket 生命周期与连接 ───────────────────────────────────

// 创建一个未初始化的 uTP socket
utp_socket* utp_create_socket(utp_context *ctx)
{
	assert(ctx);
	if (!ctx) return NULL;
	return ctx->create_socket();
}

// 主动发起一次 uTP 连接（发送 SYN）
int utp_connect(utp_socket *s, const struct sockaddr *to, socklen_t tolen) {
	assert(s);
	if (!s) return -1;
	return s->connect(to, tolen);
}

// 关闭 socket
void utp_close(utp_socket *s) {
	assert(s);
	if (!s) return;
	s->close();
}

// 关闭 socket 方向（SHUT_RD 关闭读、SHUT_WR 关闭写）
void utp_shutdown(utp_socket *s, int how) {
	assert(s);
	if (!s) return;
	s->shutdown(how);
}

// ── 数据读写 ───────────────────────────────────────────────

// 写入数据到 uTP socket
// 返回: 实际写入的字节数
ssize_t utp_write(utp_socket *socket, void *buf, size_t len) {
	struct utp_iovec iovec = { buf, len };
	return utp_writev(socket, &iovec, 1);
}

// 使用 scatter/gather I/O 向 uTP 连接写入多段不连续数据
// 返回: 成功入队的总字节数；socket 未连接或已发送 FIN 时返回 0；参数错误返回 -1
ssize_t utp_writev(utp_socket *s, struct utp_iovec *iovec_input, size_t num_iovecs)
{
	assert(s);
	if (!s) return -1;

	assert(iovec_input);
	if (!iovec_input) return -1;

	assert(num_iovecs);
	if (!num_iovecs) return -1;

	return s->writev(iovec_input, num_iovecs);
}

// 通知库应用已消费完数据，释放接收缓冲区
void utp_read_drained(utp_socket *s) {
	assert(s);
	if (!s) return;
	s->read_drained();
}

// ── 定时驱动 ───────────────────────────────────────────────

// 触发延迟 ACK 发送
void utp_issue_deferred_acks(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return;
	ctx->issue_deferred_acks();
}

// 周期性驱动库的定时器
void utp_check_timeouts(utp_context *ctx) {
	assert(ctx);
	if (!ctx) return;
	ctx->check_timeouts();
}

// ── socket 属性与查询 ──────────────────────────────────────

// 设置单个 uTP socket 级别的选项（UTP_SNDBUF/UTP_RCVBUF/UTP_TARGET_DELAY）
int utp_setsockopt(utp_socket *s, int opt, int val)
{
	assert(s);
	if (!s) return -1;
	return s->set_option(opt, val);
}

// 获取单个 uTP socket 级别选项的当前值
int utp_getsockopt(utp_socket *s, int opt)
{
	assert(s);
	if (!s) return -1;
	return s->get_option(opt);
}

// 获取对端地址
// 返回: 0 表示成功；-1 表示参数错误或 socket 未初始化
int utp_getpeername(utp_socket *conn, struct sockaddr *addr, socklen_t *addrlen)
{
	assert(addr);
	if (!addr) return -1;

	assert(addrlen);
	if (!addrlen) return -1;

	assert(conn);
	if (!conn) return -1;

	return conn->get_peername(addr, addrlen);
}

// 获取延迟测量值（本端/对端单向延迟与测量时刻）
int utp_get_delays(utp_socket *s, uint32 *ours, uint32 *theirs, uint32 *age)
{
	assert(s);
	if (!s) return -1;
	return s->get_delays(ours, theirs, age);
}

// 获取 socket 所属 context
utp_context* utp_get_context(utp_socket *socket) {
	assert(socket);
	if (!socket) return NULL;
	return socket->context();
}

// 设置 socket 用户数据，返回设置后的用户数据指针
void* utp_set_userdata(utp_socket *socket, void *userdata) {
	assert(socket);
	if (!socket) return NULL;
	socket->set_userdata(userdata);
	return socket->userdata();
}

// 获取 socket 用户数据
void* utp_get_userdata(utp_socket *socket) {
	assert(socket);
	if (!socket) return NULL;
	return socket->userdata();
}

// 获取 socket 统计信息（仅 _DEBUG 编译时有效，否则返回 NULL）
utp_socket_stats* utp_get_stats(utp_socket *s)
{
	assert(s);
	if (!s) return NULL;
	return s->get_stats();
}

// ── 入站数据投递 ───────────────────────────────────────────

// 处理收到的 UDP 数据报
int utp_process_udp(utp_context *ctx, const byte *buf, size_t len, const struct sockaddr *to, socklen_t tolen) {
	assert(ctx);
	if (!ctx) return 0;
	return ctx->process_udp(buf, len, to, tolen);
}

// 处理 ICMP「需要分片」报文
int utp_process_icmp_fragmentation(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu) {
	assert(ctx);
	if (!ctx) return 0;
	return ctx->process_icmp_fragmentation(buffer, len, to, tolen, next_hop_mtu);
}

// 处理 ICMP 错误报文（目的不可达等）
int utp_process_icmp_error(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen) {
	assert(ctx);
	if (!ctx) return 0;
	return ctx->process_icmp_error(buffer, len, to, tolen);
}

} // extern "C"
