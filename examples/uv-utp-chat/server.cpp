// uv-utp-chat 服务端：libuv 事件循环 + libutp 可靠传输 + TBB 计算线程池
//
// 职责分工（见 README.md）：
//   libuv  拥有线程与 I/O —— UDP 收发、定时器、跨线程唤醒
//   libutp 拥有连接与可靠性 —— 多客户端复用同一 UDP 端口，按连接 ID 路由
//   TBB    只拥有计算 —— 消息格式化/校验在线程池完成，经 uv_async 回流
//
// 数据流：
//   UTP_ON_READ → 分帧 → [text 帧] 派发 TBB（连接 id + 字节拷贝）
//                → worker 格式化 "昵称: 内容" + FNV 校验
//                → 回流 loop 线程 → 广播给除发送者外的所有连接
//   线程池积压字节数经 UTP_GET_READ_BUFFER_SIZE 收缩接收窗口（端到端回压）。
//
// --self-test：同进程同 loop 内起 1 server + 3 client，各发 100 条消息，
// 验证每个 client 收到其余两人的 200 条广播后干净退出（兼任 CI 集成测试）。

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include <uv.h>

#include "common/protocol.h"
#include "common/utp_uv.h"
#include "common/worker_pool.h"

using namespace chat;

namespace {

sockaddr_in make_addr(const char* ip, uint16_t port) {
	sockaddr_in a{};
	uv_ip4_addr(ip, port, &a);
	return a;
}

// FNV-1a：worker 里的演示性计算（真实服务这里是解析/压缩/落盘等重活）
uint64_t fnv1a(std::span<const uint8_t> d) {
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
	return h;
}

// worker → loop 线程的回流载荷：目标用连接 id 表达，绝不携带 utp 句柄
struct Job {
	uint64_t origin;              // 发送者连接 id（回压记账 + 广播排除）
	size_t consumed;              // 本次消费的入站字节数
	std::vector<uint8_t> frame;   // 已编码好的广播帧
};

// ── 聊天服务 ────────────────────────────────────────────────

class ChatServer {
public:
	ChatServer(uv_loop_t* loop, uint16_t port, bool debug)
		: bridge_(loop, [this](Job&& j) { deliver(std::move(j)); }),
		  node_(loop,
		        { .bind_addr = make_addr("0.0.0.0", port),
		          .accept_incoming = true,
		          .debug_log = debug },
		        hooks()) {}

	UtpUvNode& node() { return node_; }
	TbbBridge<Job>& bridge() { return bridge_; }

private:
	UtpUvNode::Hooks hooks() {
		return {
			.on_accept = [this](Connection& c) {
				std::println("[server] conn#{} 接入", c.id);
			},
			.on_read = [this](Connection& c, std::span<const uint8_t> d) {
				c.parser.feed(d);
				while (auto f = c.parser.next()) handle_frame(c, *f);
				if (c.parser.broken()) node_.close(c);  // 坏帧：掐掉
			},
			.on_eof = [this](Connection& c) {
				if (!c.name.empty())
					broadcast(encode(MsgType::sys, c.name + " 离开了"), c.id);
				node_.close(c);
			},
			.on_error = [](Connection& c, int err) {
				std::println("[server] conn#{} 错误: {}", c.id,
				             utp_error_code_names[err]);
			},
			.on_destroy = [](Connection& c) {
				std::println("[server] conn#{} 销毁", c.id);
			},
		};
	}

	void handle_frame(Connection& c, Frame& f) {
		switch (f.type) {
		case MsgType::join:
			c.name = std::string(f.text());
			std::println("[server] conn#{} 昵称 {}", c.id, c.name);
			broadcast(encode(MsgType::sys, c.name + " 加入了"), c.id);
			break;
		case MsgType::text: {
			// 回压记账：这些字节进入线程池流水线，窗口随之收缩
			c.unconsumed += f.body.size();
			bridge_.submit([origin = c.id, name = c.name,
			                body = std::move(f.body)]() -> Job {
				// ―― 线程池内：纯计算，不碰 utp/uv ――
				std::string text = name + ": ";
				text.append((const char*)body.data(), body.size());
				(void)fnv1a(body);  // 演示性计算负载
				return { .origin = origin,
				         .consumed = body.size(),
				         .frame = encode(MsgType::text, text) };
			});
			break;
		}
		default:
			break;  // 客户端不该发 sys，忽略
		}
	}

	// worker 结果回流（loop 线程）：此刻连接集合可能已变化，按当前表广播
	void deliver(Job&& j) {
		broadcast(std::move(j.frame), j.origin);
		node_.consumed(j.origin, j.consumed);  // 重新开窗（连接已死则忽略）
	}

	void broadcast(std::vector<uint8_t> frame, uint64_t except) {
		node_.for_each([&](Connection& c) {
			if (c.id != except && !c.name.empty())
				node_.write(c, frame);  // 拷贝：每连接独立写队列
		});
	}

	TbbBridge<Job> bridge_;
	UtpUvNode node_;
};

// ── 自检模式：同 loop 内 1 server + 3 client ────────────────

struct TestClient {
	std::unique_ptr<UtpUvNode> node;
	std::string name;
	int sent = 0, got = 0;
	bool done = false;
};

constexpr int kClients = 3;
constexpr int kMsgs = 100;  // 每客户端发送条数；期望各收 (kClients-1)*kMsgs

int run_self_test(bool debug) {
	uv_loop_t* loop = uv_default_loop();
	ChatServer server(loop, 0, debug);
	const uint16_t port = server.node().local_port();
	std::println("[self-test] server 127.0.0.1:{}", port);

	TestClient clients[kClients];
	for (int i = 0; i < kClients; ++i) {
		TestClient& tc = clients[i];
		tc.name = std::format("c{}", i + 1);
		tc.node = std::make_unique<UtpUvNode>(
			loop,
			UtpUvNode::Config{ .bind_addr = make_addr("127.0.0.1", 0),
			                   .accept_incoming = false,
			                   .debug_log = false },
			UtpUvNode::Hooks{
				.on_connect = [&tc](Connection& c) {
					tc.node->write(c, encode(MsgType::join, tc.name));
					for (int m = 0; m < kMsgs; ++m) {
						tc.node->write(c, encode(MsgType::text,
							std::format("msg {} from {}", m, tc.name)));
						++tc.sent;
					}
				},
				.on_read = [&tc](Connection& c, std::span<const uint8_t> d) {
					c.parser.feed(d);
					while (auto f = c.parser.next()) {
						if (f->type != MsgType::text) continue;
						if (++tc.got == (kClients - 1) * kMsgs && !tc.done) {
							tc.done = true;
							tc.node->close(c);  // 收齐即下线
						}
					}
				},
				.on_eof = [&tc](Connection& c) { tc.node->close(c); },
				.on_error = [&tc](Connection& c, int err) {
					std::println("[self-test] {} 错误: {}", tc.name,
					             utp_error_code_names[err]);
				},
			});
		tc.node->connect(make_addr("127.0.0.1", port));
	}

	// 收尾驱动：全部客户端收齐且连接清零 → 关停所有节点与桥
	struct Finisher {
		ChatServer* server;
		TestClient* clients;
		uv_timer_t poll{}, watchdog{};
		bool finished = false;
	} fin{ &server, clients };

	uv_timer_init(loop, &fin.poll);
	fin.poll.data = &fin;
	uv_timer_start(&fin.poll, [](uv_timer_t* h) {
		auto& fin = *(Finisher*)h->data;
		for (int i = 0; i < kClients; ++i) {
			if (!fin.clients[i].done ||
			    fin.clients[i].node->connection_count() != 0)
				return;
		}
		if (fin.server->node().connection_count() != 0) {
			fin.server->node().begin_shutdown();
			return;  // 等服务端连接销毁
		}
		fin.finished = true;
		for (int i = 0; i < kClients; ++i) fin.clients[i].node->begin_shutdown();
		fin.server->node().begin_shutdown();
		fin.server->bridge().shutdown();
		uv_timer_stop(&fin.poll);
		uv_close((uv_handle_t*)&fin.poll, nullptr);
		uv_timer_stop(&fin.watchdog);
		uv_close((uv_handle_t*)&fin.watchdog, nullptr);
	}, 20, 20);

	uv_timer_init(loop, &fin.watchdog);
	uv_timer_start(&fin.watchdog, [](uv_timer_t*) {
		std::println(stderr, "[self-test] FAIL: 30s 超时");
		std::exit(2);
	}, 30'000, 0);

	uv_run(loop, UV_RUN_DEFAULT);

	bool pass = fin.finished;
	for (int i = 0; i < kClients; ++i) {
		std::println("[self-test] {}: sent={} got={} (期望 {})",
		             clients[i].name, clients[i].sent, clients[i].got,
		             (kClients - 1) * kMsgs);
		pass = pass && clients[i].got == (kClients - 1) * kMsgs;
	}
	std::println("[self-test] {}", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IOLBF, 0);  // 重定向到文件/管道时日志仍实时可见
	uint16_t port = 9000;
	bool debug = false, self_test = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d")) debug = true;
		else if (!strcmp(argv[i], "--self-test")) self_test = true;
		else {
			std::println(stderr, "用法: {} [-p 端口] [-d] [--self-test]", argv[0]);
			return 64;
		}
	}

	if (self_test) return run_self_test(debug);

	uv_loop_t* loop = uv_default_loop();
	ChatServer server(loop, port, debug);
	std::println("[server] 监听 UDP 0.0.0.0:{}（uTP），Ctrl-C 退出", port);
	uv_run(loop, UV_RUN_DEFAULT);
	return 0;
}
