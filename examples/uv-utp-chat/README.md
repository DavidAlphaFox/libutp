# uv-utp-chat — libuv + libutp + TBB 多客户端聊天示例

本示例演示 libutp 在**真实异步服务架构**中的正确嵌入方式。现有 `ucat` 是
单连接、阻塞 select 的最小示例；本示例补齐三块拼图：

- **libuv**：替代手写 select/poll，承担 UDP 收发、协议定时器、跨线程唤醒
- **多客户端**：一个 `utp_context` + 一个 UDP 端口天然复用 N 条 uTP 连接
  （库内按 `对端地址 + 连接ID` 哈希路由），server 不需要任何额外分发逻辑
- **TBB**：演示「协议线程 + 计算线程池」的正确分工，而不是把 libutp 变成多线程

应用形态是广播聊天室：server 把每条消息转发给除发送者外的所有客户端——
比 echo 更能体现多客户端互通，且天然产生「一条入站消息扇出为 N 条出站写」
的并发素材给线程池。

---

## 1. 构建与运行

```sh
# 依赖（Debian/Ubuntu）：apt install libuv1-dev libtbb-dev
cmake -S . -B build -DUTP_BUILD_UV_EXAMPLE=ON      # 缺依赖时警告跳过，不影响主工程
cmake --build build -j

# 自检：同进程起 1 server + 3 client，各发 100 条、验证各收齐 200 条广播。
# 已注册为 ctest 用例并接入 CI，是本示例自带的集成测试。
./build/examples/uv-utp-chat/utp_chat_server --self-test

# 手动体验
./build/examples/uv-utp-chat/utp_chat_server -p 9000
./build/examples/uv-utp-chat/utp_chat_client 127.0.0.1 9000 -u alice
./build/examples/uv-utp-chat/utp_chat_client 127.0.0.1 9000 -u bob -n 3   # 一个进程 3 条连接
```

| 参数 | 说明 |
|---|---|
| server `-p 端口` / `-d` / `--self-test` | 监听端口 / libutp 调试日志 / 自检模式 |
| client `<IP> <端口> [-n K] [-u 昵称] [-d]` | K 条并发连接（默认 1），stdin 每行按 round-robin 发到第 i%K 条连接，Ctrl-D 退出 |

---

## 2. 决定一切的约束：libutp 是单线程库

这是整个设计的支点。`utp_context` / `utp_socket` 内部**没有任何锁**
（库采用单线程事件驱动模型），且所有回调（`UTP_ON_READ`、`UTP_SENDTO`…）
都是从 `utp_process_udp` / `utp_check_timeouts` 的调用栈里**同步**发出的，
回调给出的缓冲区在返回后即失效。

因此 TBB 的角色**不是**并行跑协议栈，而是经典的 actor + 计算池模型：

```
        ┌────────────── loop 线程（uv_run，唯一的 utp 线程）──────────────┐
        │                                                                  │
UDP ───▶ uv_udp_recv ─▶ utp_process_udp ─▶ UTP_ON_READ ──┐                │
        │ uv_timer(500ms) ─▶ utp_check_timeouts           │ 分帧 + 拷出    │
        │ uv_check ─▶ utp_issue_deferred_acks             ▼                │
        │                                   task_arena / task_group ───────┼──▶ TBB 工作线程
        │                                                                  │   （格式化/校验，
        │ 广播 utp_write ◀── uv_async ◀── tbb::concurrent_queue ◀──────────┼──  纯计算）
        └──────────────────────────────────────────────────────────────────┘
```

规则一句话：**utp 句柄永远不出 loop 线程；跨线程只传值（连接 id + 字节拷贝）**。

- 入池：`TbbBridge::submit`（`task_arena::execute` + `task_group::run`）
- 回流：worker 把结果推入 `tbb::concurrent_queue` 并 `uv_async_send`
  ——这是 libuv 唯一线程安全的原语；loop 线程在 async 回调里 while-drain
  （`uv_async` 会合并多次唤醒，不能假设 1:1）
- worker 结果只携带**连接 id**（uint64 自增、不复用），回流时按 id 查表，
  连接已死则静默丢弃——因此全程不需要任何跨线程锁

---

## 3. 组件划分

```
examples/uv-utp-chat/
├── CMakeLists.txt             # 依赖探测（缺 libuv/TBB 警告跳过）+ ctest 注册
├── README.md                  # 本文档
├── common/
│   ├── utp_uv.h / .cpp        # UtpUvNode：libutp ↔ libuv 胶水（server/client 共用）
│   ├── worker_pool.h          # TbbBridge：arena + concurrent_queue + uv_async 回流
│   └── protocol.h             # 应用层分帧（4B 小端长度前缀 + 1B 类型）
├── server.cpp                 # 监听、UTP_ON_ACCEPT、TBB 加工后广播、--self-test
└── client.cpp                 # 发起 K 条连接，stdin → 消息，打印收到的广播
```

**`UtpUvNode`（核心胶水，server/client 复用）** 持有：

| 成员 | 职责 |
|---|---|
| `uv_udp_t` | 一个 socket 双向收发；收到报文 → `utp_process_udp` |
| `uv_timer_t`（500ms） | → `utp_check_timeouts`（与库内 `TIMEOUT_CHECK_INTERVAL` 一致，更密无意义） |
| `uv_check_t`（每轮 loop 末尾） | → `utp_issue_deferred_acks`（一个 ACK 确认整批数据包，贴合延迟 ACK 合并语义） |
| `utp_context*` | 回调经 context userdata 取回 `this` 分发到应用钩子 |
| `id → Connection` 表 | worker 回流的查找入口 |

server 与 client 的差别只有 `Config::accept_incoming`：server 的
`UTP_ON_FIREWALL` 返回 0（接受），client 返回 1（拒绝入站）。

**`Connection`（每条 uTP 连接，仅 loop 线程访问）**：自增 `id`、
`utp_socket*`（DESTROYING 后置空）、出站待写队列 `outq`（窗口满时排队，
WRITABLE 续写）、`unconsumed` 在途字节计数（回压用）、应用层昵称与分帧器。

**`TbbBridge<Result>`**：模板化的池↔loop 双向桥，`submit(fn)` 入池、
`on_result` 回流、`shutdown()` 排空。async 句柄 `uv_unref`——它只是唤醒器，
不应阻止 loop 退出。

---

## 4. 关键集成点

### 4.1 出站：`UTP_SENDTO` → `uv_udp_try_send`（最容易做错的点）

`UTP_SENDTO` 给的缓冲区只在回调期间有效，而 `uv_udp_send` 是异步的：

| 方案 | 代价 | 结论 |
|---|---|---|
| `uv_udp_send` + 每包 malloc 拷贝 | 每包一次分配 + 完成回调管理 | 过重 |
| 直接 `sendto()` 系统调用 | 绕过 libuv（UDP 发送本就不阻塞） | 可行，ucat 的做法 |
| **`uv_udp_try_send`** | 同步交内核、零拷贝零分配 | **采用** |

内核缓冲满（EAGAIN）时直接丢弃对 uTP 完全无害——它本来就按丢包设计，
重传会兜底。这正是「UDP 丢包无需应用处理」的教学点。

### 4.2 读侧回压：线程池积压 → uTP 接收窗口（设计亮点）

`UTP_GET_READ_BUFFER_SIZE` 的语义是「应用已收但未消费的字节数」，libutp 用
`opt_rcvbuf − 该值` 通告接收窗口。本示例把它实现为**该连接已派发给 TBB 但
尚未完成的字节数**（`Connection::unconsumed`，server 在 submit 前累加，
worker 结果回流时经 `UtpUvNode::consumed()` 递减并调 `utp_read_drained`
重新开窗），从而得到端到端流控闭环：

> 计算池饱和 → 接收窗口收缩 → 对端发送自动降速。

不加这一条，慢消费场景下回流队列会无限膨胀——这也是 `concurrent_queue`
敢用无界版本的前提。

### 4.3 生命周期：在途任务 vs 连接销毁

时序危险：worker 算完时目标连接可能已收到 `UTP_STATE_DESTROYING`。规则：

- worker **永不持有** `utp_socket*`，只持有 `conn_id`（uint64 自增，不复用）
- 回流时在 loop 线程查 `id → Connection` 表，查不到即静默丢弃
- `DESTROYING` 回调里立即置空 `Connection::sock` 并从表中移除

### 4.4 关闭顺序（必须严格）

1. `begin_shutdown()`：对所有连接 `utp_close`（发 FIN）
2. **继续跑 loop**——FIN 握手需要收发包，uv 句柄此时不能关
3. 连接全部 DESTROYING、表清零后，才关闭 udp/timer/check 句柄
4. `TbbBridge::shutdown()`：`task_group::wait` 排空 → 最后一次 drain → 关 async
5. loop 因无活跃句柄自然退出 → **最后**才 `utp_destroy`
   （它析构期间仍会发 DESTROYING 回调）

### 4.5 应用协议：必须分帧

uTP 和 TCP 一样是**字节流**：`UTP_ON_READ` 的边界与消息边界无关。
协议为 `[4B 小端长度 n][1B 类型 join/text/sys][n−1 字节正文]`，
每连接挂一个 `FrameParser` 重组；长度字段超过 64KB 上限视为坏帧，掐掉连接
（防御恶意长度导致重组缓冲无限增长）。

### 4.6 其他实现细节

- **接收零分配**：单线程 loop 下所有 UDP 报文复用 node 内一块 64KB 缓冲
- **时间/随机数回调不设置**：libutp 的 `UtpCallbacks` 默认实现（平台单调
  时钟）即为所需
- **线程纪律由断言保护**：`UtpUvNode` 记录 loop 线程 id，所有公开方法
  `assert(uv_thread_self() == loop_tid_)`
- **TBB 任务窃取与乱序**：广播按「结果到达序」进行；需要严格 per-sender
  保序时可在协议层加序号或用 `tbb::flow::sequencer_node`（聊天场景不需要）
- client 不使用 TBB：计算型工作在 server 演示，client 保持纯 loop 线程，
  两种形态正好对照

---

## 5. 自检模式（`--self-test`）

同一进程、同一 loop 内创建 1 个 server 节点 + 3 个 client 节点（各自独立的
`utp_context` 与 UDP socket，端口由内核分配）：

1. 每个 client 连上后发 JOIN + 100 条 text
2. server 把每条消息经 TBB 加工（`昵称: 正文` + FNV-1a 演示计算）后广播
3. 每个 client 收齐 (3−1)×100 = 200 条即主动关闭
4. 20ms 轮询器观察「全部收齐且连接清零」→ 按 §4.4 顺序关停全部节点与桥
5. loop 退出后校验计数，30s 看门狗兜底防挂死

该模式注册为 ctest 用例（含 ASan/UBSan 配置）并接入 CI——示例兼任集成测试。

## 6. 已知现象

先关闭一方的 FIN 被确认后立即销毁（uTP 没有 TIME_WAIT），后关闭一方随后发出
的 FIN 可能撞上 RST，于是 server 日志出现 `UTP_ECONNRESET`。这是协议固有的
关闭竞态，无害；主工程的回环集成测试对此有相同的容忍逻辑。

---

## 7. 设计哲学（一句话）

**libuv 拥有线程与 I/O，libutp 拥有连接与可靠性，TBB 只拥有计算；
三者唯一的交汇点是 loop 线程上的两个队列出入口。**
