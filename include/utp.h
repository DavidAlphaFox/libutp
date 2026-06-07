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

/*
 * utp.h - libutp 公共 C API 头文件
 *
 * 本头文件定义了 uTP (Micro Transport Protocol) 协议的公共 C 语言接口。
 * uTP 是 BitTorrent 在 BEP-29 中定义的传输层协议，基于 UDP 实现，
 * 通过 LEDBAT 拥塞控制算法提供可靠、有序的字节流传输，同时尽量降低延迟。
 *
 * uTP 的 Socket 接口采用事件驱动 + 回调机制的设计（与传统 Berkeley Socket 不同）：
 *   - 接收方向是“被动”回调：网络数据到达时由 on_read 回调通知用户
 *   - 发送方向是“主动”调用：用户通过 utp_write/utp_writev 声明要发送的数据，
 *     然后由 on_write 等回调为每个数据包分配缓冲区
 *   - 计时、UDP 收发、随机数等底层操作均通过回调注入，不直接依赖系统调用
 *
 * 注意：libutp 接口非线程安全，设计用于单线程异步上下文。
 *
 * 用法概览：
 *   1. 调用 utp_init() 创建 utp_context
 *   2. 调用 utp_set_callback() 注册所需回调（UTP_GET_MILLISECONDS、UTP_SENDTO 等必须注册）
 *   3. 调用 utp_create_socket() 创建 utp_socket
 *   4. 调用 utp_connect() 发起连接，或等待 UTP_ON_ACCEPT 回调处理入站连接
 *   5. 通过 utp_process_udp() 将接收到的 UDP 数据包送入库内处理
 *   6. 周期性调用 utp_check_timeouts() 触发超时重传
 *   7. 使用 utp_write() / utp_read_drained() 进行数据收发
 *   8. 关闭时调用 utp_close() / utp_shutdown()，最后 utp_destroy()
 */

#ifndef __UTP_H__
#define __UTP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include "utp_types.h"

// uTP Socket 句柄（不透明结构体，具体定义位于库内部）
typedef struct UtpSocket					utp_socket;
// uTP Context 句柄（不透明结构体，保存一组 Socket 的全局状态）
typedef struct UtpContext					utp_context;

/*
 * 通用枚举常量
 *
 * UTP_UDP_DONTFRAG - 在发送 UDP 数据包时请求“不分片”标志
 * 提示底层协议栈在 IP 层设置 DF 位，用于 PMTU 探测。
 * 注：历史上曾以 UDP_IP_DONTFRAG 宏的形式出现，现改为枚举形式。
 */
enum {
	UTP_UDP_DONTFRAG = 2,	// Used to be a #define as UDP_IP_DONTFRAG
};

/*
 * UTP 状态机取值
 *
 * 这些状态通过 UTP_ON_STATE_CHANGE 回调通知到应用层，
 * 用于描述 uTP Socket 在协议交互过程中所处的阶段。
 */
enum {
	// socket 已接收到 SYN-ACK（仅对“出站”连接生效，表示三次握手完成）
	// 该状态隐含“socket 可写”的语义
	// socket has reveived syn-ack (notification only for outgoing connection completion)
	// this implies writability
	UTP_STATE_CONNECT = 1,

	// socket 发送窗口已腾出空间，应用可以通过 utp_write() 写入更多数据
	// socket is able to send more data
	UTP_STATE_WRITABLE = 2,

	// 对端已发送 FIN，表示连接正常关闭（半关闭或全关闭）
	// connection closed
	UTP_STATE_EOF = 3,

	// socket 正在被销毁，库会尽力将剩余数据发出
	// 在收到该状态后，应用不应再访问此 socket
	// socket is being destroyed, meaning all data has been sent if possible.
	// it is not valid to refer to the socket after this state change occurs
	UTP_STATE_DESTROYING = 4,
};

// UTP_STATE_* 取值的可读字符串名称数组（按枚举顺序排列，用于日志等场景）
extern const char *utp_state_names[];

/*
 * 错误码枚举
 *
 * 这些错误码会通过 UTP_ON_ERROR 回调传递给应用层，
 * 语义上与 POSIX errno 中的对应值类似，但取值为正整数。
 */
// Errors codes that can be passed to UTP_ON_ERROR callback
enum {
	// 连接被对端拒绝（无监听或主动 RST）
	UTP_ECONNREFUSED = 0,
	// 连接被对端重置
	UTP_ECONNRESET,
	// 连接超时（多次重传未收到 ACK）
	UTP_ETIMEDOUT,
};

// UTP 错误码的可读字符串名称数组（按枚举顺序排列）
extern const char *utp_error_code_names[];

/*
 * 回调类型 / 选项枚举
 *
 * 整个枚举分为两段：
 *   1) 前 16 个值为“回调名称”，用于 utp_set_callback() 注册
 *   2) 之后的值为“Context/Socket 选项”，用于 utp_set/get_option 与 utp_set/getsockopt
 *   3) 最后一个值 UTP_ARRAY_SIZE 标记枚举结束（不要删除，必须保持末尾）
 */
enum {
	// callback names

	// 防火墙/NAT 探测回调：库请求应用判断指定地址是否位于 NAT/防火墙之后
	UTP_ON_FIREWALL = 0,
	// 接受连接回调：收到对端 SYN 后，库询问应用是否接受该连接
	UTP_ON_ACCEPT,
	// 连接建立回调：出站连接三次握手完成后触发
	UTP_ON_CONNECT,
	// 出错回调：连接出现错误（UTP_ECONNREFUSED/ECONNRESET/ETIMEDOUT 等）
	UTP_ON_ERROR,
	// 数据到达回调：通知应用读取已收到的数据
	UTP_ON_READ,
	// 周期性统计回调：通知应用统计信息（已发送/已接收字节数等）
	UTP_ON_OVERHEAD_STATISTICS,
	// 状态变更回调：UTP_STATE_* 状态变化时触发
	UTP_ON_STATE_CHANGE,
	// 询问接收缓冲区大小：库询问应用当前能接收的最大数据量
	UTP_GET_READ_BUFFER_SIZE,
	// 延迟采样回调：库向应用报告当前测得的端到端延迟（用于 LEDBAT）
	UTP_ON_DELAY_SAMPLE,
	// 询问 UDP MTU：库询问底层 UDP 链路的最大传输单元
	UTP_GET_UDP_MTU,
	// 询问 UDP 协议开销：库询问 UDP+IP 头部的总字节数
	UTP_GET_UDP_OVERHEAD,
	// 询问当前毫秒时间戳（必须实现，否则库无法正常运行）
	UTP_GET_MILLISECONDS,
	// 询问当前微秒时间戳（用于更精细的测量）
	UTP_GET_MICROSECONDS,
	// 询问随机字节：库用于生成初始序列号等需要随机性的场景
	UTP_GET_RANDOM,
	// 日志输出回调：库通过该回调输出诊断/调试/错误日志
	UTP_LOG,
	// 数据发送回调：库通过该回调把 UDP 报文发送到对端
	UTP_SENDTO,

	// context and socket options that may be set/queried

    // 日志级别：普通信息
    UTP_LOG_NORMAL,
    // 日志级别：MTU 探测相关
    UTP_LOG_MTU,
    // 日志级别：详细调试信息
    UTP_LOG_DEBUG,
	// Socket 选项：发送缓冲区大小（字节数）
	UTP_SNDBUF,
	// Socket 选项：接收缓冲区大小（字节数）
	UTP_RCVBUF,
	// Socket 选项：LEDBAT 目标延迟（毫秒）
	UTP_TARGET_DELAY,

	UTP_ARRAY_SIZE,	// must be last
};

// UTP 回调/选项名称的可读字符串数组（按枚举顺序排列）
extern const char *utp_callback_names[];

/*
 * 回调参数结构体
 *
 * 通过 UTP_ON_* 回调的 utp_callback_arguments* 参数传入。
 * 不同的 callback_type 下，部分联合体成员具有不同含义。
 */
typedef struct {
	// 触发该回调所属的 Context
	utp_context *context;
	// 触发该回调所属的 Socket（部分回调下可能为 NULL）
	utp_socket *socket;
	// 数据长度（UTP_ON_READ 中表示 buf 中可读字节数，其他回调语义见具体类型）
	size_t len;
	// 标志位（具体语义取决于回调类型）
	uint32 flags;
	// 当前回调类型（UTP_ON_* 之一）
	int callback_type;
	// 数据缓冲区（UTP_ON_READ 时为对端发来的数据，其他回调可能为 NULL）
	const byte *buf;

	union {
		// 网络地址（UTP_ON_FIREWALL 时传入，库请求判断该地址是否在防火墙后）
		const struct sockaddr *address;
		// 发送方向标志（UTP_ON_READ 时表示是否对端已关闭发送方向，触发 EOF）
		int send;
		// 延迟采样毫秒数（UTP_ON_DELAY_SAMPLE 回调中报告）
		int sample_ms;
		// 错误码（UTP_ON_ERROR 回调中，对应 UTP_ECONNREFUSED 等）
		int error_code;
		// 状态码（UTP_ON_STATE_CHANGE 回调中，对应 UTP_STATE_*）
		int state;
	};

	union {
		// 网络地址长度（与 address 配对使用）
		socklen_t address_len;
		// 通用类型字段
		int type;
	};
} utp_callback_arguments;

// uTP 回调函数签名：所有 UTP_ON_* 回调均使用该原型
// 返回值通常为 0，含义由具体回调决定
typedef uint64 utp_callback_t(utp_callback_arguments *);

/*
 * Context 级统计信息
 *
 * 由 utp_get_context_stats() 返回，反映整个 Context 范围内的聚合数据。
 * 数组按“< 300 / < 600 / < 1200 / < MTU / >= MTU”五档统计小包分布。
 */
// Returned by utp_get_context_stats()
typedef struct {
	// 接收数据包按大小分桶计数（context 全局），
	// 索引分别对应小于 300/600/1200/MTU 字节及大于等于 MTU 的包
	uint32 _nraw_recv[5];	// total packets recieved less than 300/600/1200/MTU bytes fpr all connections (context-wide)
	// 发送数据包按大小分桶计数（context 全局），分桶同上
	uint32 _nraw_send[5];	// total packets sent     less than 300/600/1200/MTU bytes for all connections (context-wide)
} utp_context_stats;

/*
 * Socket 级统计信息
 *
 * 由 utp_get_stats() 返回，反映单个 uTP Socket 的传输统计。
 */
// Returned by utp_get_stats()
typedef struct {
	// 累计已接收的负载字节数（不含协议头/重传）
	uint64 nbytes_recv;	// total bytes received
	// 累计已发送的负载字节数（不含协议头/重传）
	uint64 nbytes_xmit;	// total bytes transmitted
	// 超时重传次数
	uint32 rexmit;		// retransmit counter
	// 快速重传次数（基于重复 ACK 触发的重传）
	uint32 fastrexmit;	// fast retransmit counter
	// 实际发送的数据包总数（含重传）
	uint32 nxmit;		// transmit counter
	// 接收到的数据包总数
	uint32 nrecv;		// receive counter (total)
	// 重复接收到的数据包总数（用于拥塞控制中的快速重传判断）
	uint32 nduprecv;	// duplicate receive counter
	// 当前最佳 MTU 估值（PMTU 探测结果）
	uint32 mtu_guess;	// Best guess at MTU
} utp_socket_stats;

// utp_writev() 一次调用允许的最大 iovec 数量
#define UTP_IOV_MAX 1024

/*
 * I/O 向量结构体
 *
 * 用于 utp_writev()，支持一次写入多个不连续缓冲区（gather write）。
 * 语义与 readv/writev 中的 iovec 类似。
 */
// For utp_writev, to writes data from multiple buffers
struct utp_iovec {
	// 缓冲区起始地址
	void *iov_base;
	// 缓冲区长度（字节）
	size_t iov_len;
};

// ============== Public Functions ==============

/*
 * 初始化一个 uTP Context（库实例）。
 *
 * @param version  API 版本号（当前应传入 UTP_VERSION，库会校验兼容性）
 * @return        成功时返回新创建的 utp_context 指针；失败返回 NULL
 */
utp_context*	utp_init						(int version);

/*
 * 销毁一个 uTP Context，并释放其中所有 Socket。
 * 调用后不能再使用该 ctx 指针。
 *
 * @param ctx  由 utp_init() 创建的 Context
 */
void			utp_destroy						(utp_context *ctx);

/*
 * 注册指定类型的回调函数。
 *
 * 多个回调类型（如 UTP_GET_MILLISECONDS、UTP_SENDTO、UTP_LOG、UTP_GET_UDP_MTU 等）
 * 必须注册后库才能正常工作。
 *
 * @param ctx            目标 Context
 * @param callback_name  回调类型（UTP_ON_* / UTP_GET_* / UTP_SENDTO / UTP_LOG 等）
 * @param proc           回调函数指针，传入 NULL 可取消注册
 */
void			utp_set_callback				(utp_context *ctx, int callback_name, utp_callback_t *proc);

/*
 * 设置 Context 关联的用户自定义数据指针。
 *
 * @param ctx      目标 Context
 * @param userdata 任意指针，会在后续回调中可通过 utp_context_get_userdata 取回
 * @return         之前关联的 userdata
 */
void*			utp_context_set_userdata		(utp_context *ctx, void *userdata);

/*
 * 获取 Context 关联的用户自定义数据指针。
 *
 * @param ctx  目标 Context
 * @return     最近一次 utp_context_set_userdata 设置的指针；未设置则为 NULL
 */
void*			utp_context_get_userdata		(utp_context *ctx);

/*
 * 设置 Context 级选项（如 UTP_LOG_NORMAL/MTU/DEBUG、UTP_SNDBUF/RCVBUF/TARGET_DELAY 等）。
 *
 * @param ctx  目标 Context
 * @param opt  选项名（UTP_* 系列中可配置项）
 * @param val  选项值
 * @return     0 表示成功，-1 表示未知选项
 */
int				utp_context_set_option			(utp_context *ctx, int opt, int val);

/*
 * 获取 Context 级选项的当前值。
 *
 * @param ctx  目标 Context
 * @param opt  选项名
 * @return     选项当前值；未知选项返回 -1
 */
int				utp_context_get_option			(utp_context *ctx, int opt);

/*
 * 将应用从网络上接收到的 UDP 报文送入 uTP 库进行解析与分发。
 *
 * 应用负责从 UDP Socket 收包，然后把每个数据包原始内容交给本函数。
 *
 * @param ctx    目标 Context
 * @param buf    收到的 UDP 负载
 * @param len    buf 长度
 * @param to     收到的报文来源地址（sockaddr_in / sockaddr_in6 等）
 * @param tolen  地址长度
 * @return       1 表示该报文被某个 uTP Socket 接收；0 表示未匹配到任何 Socket
 */
int				utp_process_udp					(utp_context *ctx, const byte *buf, size_t len, const struct sockaddr *to, socklen_t tolen);

/*
 * 通知 uTP 库收到了一个 ICMP 错误报文。
 *
 * 收到对端发来的 ICMP 错误（如端口不可达）时，调用此函数以触发对应 Socket 的
 * 错误回调。
 *
 * @param ctx      目标 Context
 * @param buffer   ICMP 报文原始内容
 * @param len      buffer 长度
 * @param to       ICMP 错误中指示的原始目的地址
 * @param tolen    地址长度
 * @return         非 0 表示成功处理
 */
int				utp_process_icmp_error			(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen);

/*
 * 通知 uTP 库收到了一个 ICMP “需要分片” / “MTU 减小”类型的错误，
 * 以便库根据新的 MTU 调整 PMTU 估算。
 *
 * @param ctx           目标 Context
 * @param buffer        ICMP 报文原始内容
 * @param len           buffer 长度
 * @param to            原始目的地址
 * @param tolen         地址长度
 * @param next_hop_mtu  ICMP 错误中报告的下一跳 MTU
 * @return              非 0 表示成功处理
 */
int				utp_process_icmp_fragmentation	(utp_context *ctx, const byte *buffer, size_t len, const struct sockaddr *to, socklen_t tolen, uint16 next_hop_mtu);

/*
 * 周期性调用以驱动库的定时器（重传超时、延迟采样、ACK 发送等）。
 *
 * 应用应按照合理粒度（例如每 500ms）调用一次本函数。
 *
 * @param ctx  目标 Context
 */
void			utp_check_timeouts				(utp_context *ctx);

/*
 * 触发延迟 ACK（批量 ACK）的发送。
 *
 * 库默认采用延迟 ACK 以减少 ACK 数量，应用可按需周期性调用。
 *
 * @param ctx  目标 Context
 */
void			utp_issue_deferred_acks			(utp_context *ctx);

/*
 * 获取 Context 级的全局统计信息。
 *
 * 返回的指针指向 Context 内部维护的 utp_context_stats，无需释放。
 * 长时间持有指针可能导致库内部状态与读取值不一致，建议临时使用。
 *
 * @param ctx  目标 Context
 * @return     指向内部 utp_context_stats 的指针
 */
utp_context_stats* utp_get_context_stats		(utp_context *ctx);

/*
 * 创建一个新的 uTP Socket。
 *
 * 创建后 Socket 处于未连接状态；出站连接需调用 utp_connect()，
 * 入站连接由 UTP_ON_ACCEPT 回调创建（参见使用流程）。
 *
 * @param ctx  目标 Context
 * @return     新创建的 utp_socket 指针；失败返回 NULL
 */
utp_socket*		utp_create_socket				(utp_context *ctx);

/*
 * 设置 Socket 关联的用户自定义数据指针。
 *
 * @param s        目标 Socket
 * @param userdata 任意指针
 * @return         之前关联的 userdata
 */
void*			utp_set_userdata				(utp_socket *s, void *userdata);

/*
 * 获取 Socket 关联的用户自定义数据指针。
 *
 * @param s  目标 Socket
 * @return   最近一次 utp_set_userdata 设置的指针
 */
void*			utp_get_userdata				(utp_socket *s);

/*
 * 设置 Socket 级选项。
 *
 * @param s    目标 Socket
 * @param opt  选项名（UTP_SNDBUF/RCVBUF/TARGET_DELAY 等）
 * @param val  选项值
 * @return     0 成功；-1 失败
 */
int				utp_setsockopt					(utp_socket *s, int opt, int val);

/*
 * 获取 Socket 级选项的当前值。
 *
 * @param s    目标 Socket
 * @param opt  选项名
 * @return     选项当前值；失败返回 -1
 */
int				utp_getsockopt					(utp_socket *s, int opt);

/*
 * 向指定对端地址发起 uTP 连接（三次握手）。
 *
 * 连接结果通过 UTP_ON_CONNECT / UTP_ON_ERROR 回调异步通知。
 *
 * @param s      目标 Socket
 * @param to     目标地址
 * @param tolen  地址长度
 * @return       0 成功发起；-1 失败
 */
int				utp_connect						(utp_socket *s, const struct sockaddr *to, socklen_t tolen);

/*
 * 向 uTP 连接写入数据。
 *
 * 该调用声明“想要发送”这些字节，并不立即在网络上发送；
 * 库会通过 UTP_ON_READ 等回调（实际由 UTP_SENDTO 触发的写回调）要求应用填充缓冲区。
 * 实际写入网络的字节数受拥塞窗口与接收窗口限制。
 *
 * @param s      目标 Socket
 * @param buf    待发送数据缓冲区
 * @param count  待发送字节数
 * @return       成功入队的字节数；错误返回 -1
 */
ssize_t			utp_write						(utp_socket *s, void *buf, size_t count);

/*
 * 使用 scatter/gather I/O 写入多段不连续数据。
 *
 * @param s           目标 Socket
 * @param iovec       iovec 数组
 * @param num_iovecs  iovec 数量（不得超过 UTP_IOV_MAX）
 * @return            成功入队的总字节数；错误返回 -1
 */
ssize_t			utp_writev						(utp_socket *s, struct utp_iovec *iovec, size_t num_iovecs);

/*
 * 获取对端地址（连接建立后有效）。
 *
 * @param s       目标 Socket
 * @param addr    输出对端地址
 * @param addrlen 输入/输出参数：传入 addr 缓冲区长度，输出实际长度
 * @return        0 成功；-1 失败
 */
int				utp_getpeername					(utp_socket *s, struct sockaddr *addr, socklen_t *addrlen);

/*
 * 通知库应用已消费完 UTP_ON_READ 回调中提供的数据，释放接收缓冲区。
 *
 * 调用此函数后，库可以向应用投递新的 UTP_ON_READ 回调。
 *
 * @param s  目标 Socket
 */
void			utp_read_drained				(utp_socket *s);

/*
 * 获取当前测得的端到端延迟样本（LEDBAT 拥塞控制所用）。
 *
 * @param s       目标 Socket
 * @param ours    [out] 本端测得的延迟
 * @param theirs  [out] 对端测得的延迟
 * @param age     [out] 距上次测量的毫秒数
 * @return        0 成功；非 0 失败
 */
int				utp_get_delays					(utp_socket *s, uint32 *ours, uint32 *theirs, uint32 *age);

/*
 * 获取 Socket 级的统计信息。
 *
 * @param s  目标 Socket
 * @return   指向内部 utp_socket_stats 的指针
 */
utp_socket_stats* utp_get_stats					(utp_socket *s);

/*
 * 获取 Socket 所属的 Context。
 *
 * @param s  目标 Socket
 * @return   对应的 utp_context 指针
 */
utp_context*	utp_get_context					(utp_socket *s);

/*
 * 关闭 Socket 的发送或接收方向（类似 shutdown）。
 *
 * @param s    目标 Socket
 * @param how  SHUT_RD / SHUT_WR / SHUT_RDWR（Win32 下为 SD_RECEIVE/SD_SEND/SD_BOTH）
 */
void			utp_shutdown					(utp_socket *s, int how);

/*
 * 关闭并释放一个 uTP Socket。
 *
 * 与 utp_shutdown 不同，utp_close 会在数据发送完成后彻底销毁 Socket；
 * 在收到 UTP_STATE_DESTROYING 状态之前，应用仍可继续向该 Socket 写入数据。
 *
 * @param s  目标 Socket
 */
void			utp_close						(utp_socket *s);

#ifdef __cplusplus
}
#endif

#endif //__UTP_H__
