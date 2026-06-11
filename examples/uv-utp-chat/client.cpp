// uv-utp-chat 客户端
//
// 演示一个进程在同一个 UDP socket / 同一个 UtpContext 上并发持有 K 条
// uTP 连接（-n K，默认 1）：libutp 按 (对端地址, 连接ID) 路由，应用侧
// 不需要任何分发逻辑。stdin 每行消息按 round-robin 发到第 i%K 条连接。
//
// 客户端不使用 TBB：计算型工作（消息加工）在服务端演示；客户端保持
// 纯 loop 线程，正好对照两种形态。

#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include <uv.h>

#include "common/protocol.h"
#include "common/utp_uv.h"

using namespace chat;

namespace {

struct ClientApp {
	std::unique_ptr<UtpUvNode> node;
	std::string name;
	int total_conns = 0;
	uint64_t rr = 0;            // round-robin 计数
	std::vector<uint64_t> conn_ids;

	// stdin（tty 或管道，二者都是 uv_stream_t）
	uv_tty_t tty{};
	uv_pipe_t pipe{};
	uv_stream_t* stdin_stream = nullptr;
	std::string linebuf;
	bool quitting = false;

	void send_line(std::string_view line) {
		if (conn_ids.empty()) return;
		// 轮转选一条连接发送：演示多连接复用
		for (size_t tries = 0; tries < conn_ids.size(); ++tries) {
			Connection* c = node->find(conn_ids[rr++ % conn_ids.size()]);
			if (c && c->connected && !c->closing) {
				node->write(*c, encode(MsgType::text, line));
				return;
			}
		}
	}

	void quit() {
		if (quitting) return;
		quitting = true;
		if (stdin_stream) {
			uv_read_stop(stdin_stream);
			uv_close((uv_handle_t*)stdin_stream, nullptr);
			stdin_stream = nullptr;
		}
		node->begin_shutdown();
	}
};

void start_stdin(ClientApp& app, uv_loop_t* loop) {
	const uv_handle_type t = uv_guess_handle(0);
	if (t == UV_TTY) {
		uv_tty_init(loop, &app.tty, 0, 1);
		app.stdin_stream = (uv_stream_t*)&app.tty;
	} else if (t == UV_NAMED_PIPE) {
		uv_pipe_init(loop, &app.pipe, 0);
		uv_pipe_open(&app.pipe, 0);
		app.stdin_stream = (uv_stream_t*)&app.pipe;
	} else {
		std::println("[client] stdin 类型不支持交互（重定向文件请改用管道），仅接收消息");
		return;
	}
	app.stdin_stream->data = &app;

	uv_read_start(app.stdin_stream,
		[](uv_handle_t*, size_t, uv_buf_t* buf) {
			static char b[4096];
			*buf = uv_buf_init(b, sizeof(b));
		},
		[](uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
			auto& app = *(ClientApp*)s->data;
			if (nread < 0) {  // EOF / 错误：优雅退出
				app.quit();
				return;
			}
			app.linebuf.append(buf->base, (size_t)nread);
			size_t pos;
			while ((pos = app.linebuf.find('\n')) != std::string::npos) {
				std::string line = app.linebuf.substr(0, pos);
				app.linebuf.erase(0, pos + 1);
				if (!line.empty()) app.send_line(line);
			}
		});
}

}  // namespace

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IOLBF, 0);  // 重定向到文件/管道时输出仍实时可见
	const char* host = nullptr;
	uint16_t port = 0;
	int nconns = 1;
	std::string name = "guest";
	bool debug = false;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) nconns = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-u") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "-d")) debug = true;
		else if (!host) host = argv[i];
		else if (!port) port = (uint16_t)atoi(argv[i]);
	}
	if (!host || !port || nconns < 1) {
		std::println(stderr, "用法: {} <服务器IP> <端口> [-n 连接数] [-u 昵称] [-d]", argv[0]);
		return 64;
	}

	sockaddr_in server_addr{};
	if (uv_ip4_addr(host, port, &server_addr) != 0) {
		std::println(stderr, "无效地址: {}:{}", host, port);
		return 64;
	}

	uv_loop_t* loop = uv_default_loop();
	ClientApp app;
	app.name = name;
	app.total_conns = nconns;

	sockaddr_in bind_addr{};
	uv_ip4_addr("0.0.0.0", 0, &bind_addr);

	app.node = std::make_unique<UtpUvNode>(
		loop,
		UtpUvNode::Config{ .bind_addr = bind_addr,
		                   .accept_incoming = false,
		                   .debug_log = debug },
		UtpUvNode::Hooks{
			.on_connect = [&app](Connection& c) {
				std::println("[client] {} 已连接 (conn#{})", c.name, c.id);
				app.node->write(c, encode(MsgType::join, c.name));
			},
			.on_read = [](Connection& c, std::span<const uint8_t> d) {
				c.parser.feed(d);
				while (auto f = c.parser.next()) {
					if (f->type == MsgType::text)
						std::println("{}", f->text());
					else if (f->type == MsgType::sys)
						std::println("* {}", f->text());
				}
			},
			.on_eof = [&app](Connection& c) {
				std::println("[client] conn#{} 对端关闭", c.id);
				app.node->close(c);
			},
			.on_error = [](Connection& c, int err) {
				std::println("[client] conn#{} 错误: {}", c.id,
				             utp_error_code_names[err]);
			},
			.on_destroy = [&app](Connection& c) {
				// 注意：此刻 c 仍在表中（count 含将亡连接）
				if (app.node->connection_count() == 1) {
					std::println("[client] 全部连接已断开，退出");
					app.quit();
				}
			},
		});

	for (int k = 0; k < nconns; ++k) {
		Connection* c = app.node->connect(server_addr);
		c->name = nconns == 1 ? name : std::format("{}#{}", name, k + 1);
		app.conn_ids.push_back(c->id);
	}

	start_stdin(app, loop);
	std::println("[client] {} 条连接 → {}:{}，输入消息回车发送，Ctrl-D 退出",
	             nconns, host, port);
	uv_run(loop, UV_RUN_DEFAULT);
	return 0;
}
