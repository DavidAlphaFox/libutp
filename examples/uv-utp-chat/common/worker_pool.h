#pragma once

// TbbBridge：TBB 线程池 ↔ libuv loop 线程的双向桥
//
//   入池：submit(fn) —— loop 线程把任务（值捕获：连接 id + 字节拷贝）
//         交给 tbb::task_arena 并发执行；
//   回流：worker 把 fn() 的返回值推入 tbb::concurrent_queue 并 uv_async_send；
//         loop 线程在 async 回调里 while-drain（uv_async 会合并多次唤醒，
//         不能假设 1:1），逐个交给 on_result。
//
// 规则：worker 内禁止调用任何 utp_* / uv_*（uv_async_send 除外，它是
// libuv 唯一线程安全的原语）。utp 句柄永远不进线程池。

#include <functional>
#include <utility>

#include <uv.h>

#include <tbb/concurrent_queue.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>

namespace chat {

template <typename Result>
class TbbBridge {
public:
	TbbBridge(uv_loop_t* loop, std::function<void(Result&&)> on_result)
		: on_result_(std::move(on_result)) {
		uv_async_init(loop, &async_, &TbbBridge::s_drain);
		async_.data = this;
		// async 句柄不应阻止 loop 退出（它只是唤醒器，不代表待办工作）
		uv_unref((uv_handle_t*)&async_);
	}

	// loop 线程调用：fn 在线程池执行，返回的 Result 回流 loop 线程。
	// fn 须为 const 可调用（task_group 经 const 引用调用任务）。
	template <typename F>
	void submit(F&& fn) {
		arena_.execute([&] {
			tg_.run([this, f = std::forward<F>(fn)] {
				q_.push(f());
				uv_async_send(&async_);
			});
		});
	}

	// loop 线程调用：等线程池排空 → 处理收尾结果 → 关闭 async 句柄。
	// 之后不得再 submit。
	void shutdown() {
		arena_.execute([&] { tg_.wait(); });
		drain();
		uv_close((uv_handle_t*)&async_, nullptr);
	}

private:
	static void s_drain(uv_async_t* h) { ((TbbBridge*)h->data)->drain(); }

	void drain() {
		Result r;
		while (q_.try_pop(r)) on_result_(std::move(r));
	}

	tbb::task_arena arena_;       // 默认并发度 = 硬件线程数
	tbb::task_group tg_;
	tbb::concurrent_queue<Result> q_;  // 无界；深度由 uTP 接收窗口回压约束
	uv_async_t async_{};
	std::function<void(Result&&)> on_result_;
};

}  // namespace chat
