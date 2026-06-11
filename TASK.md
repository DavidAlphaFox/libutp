# 质量与性能强化（测试兑现 + 类型安全 + 测量驱动优化）

> **状态（2026-06-11 第四次更新）**：两个阶段全部完成并验证（工作区待提交）。
> 验证基线升级：64 个 ctest 用例（Debug + ASan/UBSan 双跑）、libFuzzer 累计
> 160 万+ 次执行零发现、四配置零错误构建、`nm` 28 个公开 C 符号不变、
> ucat 回环 10MB 逐字节一致。

## 阶段 A：测试兑现 + 类型安全 + 工程化（8 项）

| 项 | 内容 |
|----|------|
| 测试基建 | Catch2 改 `find_package` 优先 + FetchContent 兜底（原硬编码 `/tmp/catch2_src` 已坏） |
| 类型安全 | `SequenceBuffer<T>`（`unique_ptr<T>` 持有元素，新增 `take()`），消灭全部 void* 强转与手动 delete |
| 所有权 | `SocketMap` 持 `unique_ptr<UtpSocket>`；`destroy_socket` 为唯一销毁入口（先移出哈希表再析构，杜绝析构回调重入容器）；`UtpSocketKey` memset 改成员初始化；全局 `addrbuf` 改按值临时缓冲 `addrfmt` |
| 文件职责 | `utp_api.cpp` 1147→517 行（纯 C 薄包装+回调适配器）；新建 `utp_context.cpp` 收纳全部 UtpContext 实现 |
| 单元测试 | `test_ledbat.cpp`（13 例）、`test_mtu_discovery.cpp`（8 例） |
| 集成测试 | `test_loopback.cpp`：双 context 内存互递 + 假时钟 + 固定种子故障注入（丢包/乱序/重复），5 场景毫秒级完成 |
| 工程化 | `UTP_SANITIZE` 选项；install + `utpConfig.cmake` + pkg-config；GitHub Actions 矩阵（4 配置 + 符号检查 + 回环 + fuzz 冒烟）；`.clang-format`/`.clang-tidy` |

### 阶段 A 发现并修复的 3 个真实 bug

1. **SACK 位图堆越界读**（上游 libutp 继承）：`selective_ack_bytes` 首轮迭代 `bits == len*8` 读 `mask[len]`，ASan 在丢包回环中捕获；已加上界检查。
2. **C 回调丢失 `args->context`**：回调接口化时 `CFunctionCallbackAdapter::make_args` 漏填 context（违反上游 C API 约定）；适配器现持宿主回指针统一回填。
3. **日志 va_list 误转发**：`UtpContext::log` 把 `va_list` 当可变参数传给 `log_unchecked(...)`，带参日志输出垃圾值；新增 `vlog_unchecked(va_list)` 出口。

## 阶段 B：fuzzing + 测量驱动性能优化

- **libFuzzer harness**（`fuzz/fuzz_process_udp.cpp`，`UTP_BUILD_FUZZERS=ON` 需 Clang）：
  分帧选择子驱动 `process_udp`/ICMP/超时路径，确定性回调保证可复现；
  ASan+UBSan 下累计 160 万+ 次执行、覆盖率驱动语料 2200+，零崩溃零断言。CI 加 60s 冒烟 job。
- **吞吐基准**（`bench/bench_loopback.cpp`，`UTP_BUILD_BENCH=ON`）：双 context 内存回环 +
  全局 operator new 计数。基线 256MB：**2509 MB/s，3005 次堆分配/MB**。
- **perf 定位热点**：`flush_packets` self-time **36%**——每次调用从窗口头线性扫描，
  大窗口下 O(窗口²)；malloc 仅 ~2.5%。
- **优化（flush_scan_start 游标）**：`SendState` 维护"已发送前缀"游标，扫描从第一个
  未发送/待重传包开始；唯一批量置 `need_resend` 的 RTO 路径重置游标回窗口头。
  结果 **2509 → ~4260 MB/s（+70%）**，优化后剖面平坦（无函数 >4%）。
- **池化结论：不做**。优化后 malloc/free 合计仅 ~7%（综合基准，真实场景网络受限），
  数据不支持引入对象池的复杂度。
- 顺手：`LedbatController` 接口方法补全 `override`；`UtpSocket`/`UtpContext` 前置声明
  统一为 `struct`（与 `utp.h` 的 typedef 一致，消除 MSVC ABI 告警）；connect 日志
  format 截断修正。

---

# （历史）面向对象重构（接口化 + 高内聚低耦合）

> **状态（2026-06-11 第三次更新）**：5 个阶段全部完成并验证（已提交）。
> 目标：进一步面向对象（继承/设计模式）+ 每个类高内聚低耦合。
> 取舍原则：只在能真正降低耦合处引入模式，避免"为模式而模式"反增耦合。
> 验证：每阶段均经 Debug / Release / `-DUTP_DEBUG_LOGGING=ON` 三配置零错误构建，
> `nm` 确认 28 个公开 C 符号不变，ucat 回环 100KB + 10MB 逐字节一致。

## 阶段成果

| 阶段 | 内容 | 引入的抽象 |
|------|------|-----------|
| 1 | 删除 utp_socket.cpp 本地 ST_* 枚举改用 `utp::wire::*`；删除 wire_format.hpp 中无人使用、放错层的 `ConnState` | —（消除重复，单一来源） |
| 2 | `MtuDiscovery`/子组件不再持有 `UtpSocket` 反向指针 | **`utp::ILogger`**（日志角色接口，`UtpSocket` 实现）；`MtuDiscovery` 现完全自包含、header-only |
| 3 | 消除 `UtpSocket` ↔ `UtpContext` 的**双向 friend**（5 大耦合热点之首），改依赖倒置 | **`ISocketHost`**（宿主服务接口：时钟/发送/延迟ACK/统计/默认配置/注册表/回调）；`UtpContext` 实现之；`UtpSocket` 经接口访问，新增 `accept_syn/on_reset/on_icmp_*/state/...` 公开 API 取代 friend 直访 |
| 4 | 拥塞控制策略化，算法可替换 | **`ICongestionController`**（Strategy）；`LedbatController` 为默认实现；`UtpSocket::cc_` 改为 `unique_ptr<ICongestionController>` |
| 5 | 连接状态机改 State 模式 | **`IConnectionState`** + 8 个无数据状态单例；`close/shutdown/writev/check_timeouts` 的 switch 改为按 `state_descriptor(state)` 多态分派 |

## 关键设计决策（与"低耦合"目标的取舍）

- **State 模式刻意"有界"**：状态对象是无数据单例，只调用 `UtpSocket` 的一小组公开原语
  （`mark_read_shutdown/request_close/send_fin/...`），**不 friend、不访问私有成员**，
  因此没有重新引入 Phase 3 消除的耦合。`enum conn_.state` 仍是状态唯一真相来源
  （线格式/日志直接用）。每包共享的窗口/ACK 流水线属于**跨状态共享算法**，不随状态
  多态化——这是"在合适粒度用模式"，而非把单体协议逻辑硬拆进状态类（那会让状态类
  反向依赖 socket 内部，违背低耦合）。
- **接口允许虚函数、清晰优先**（按用户选择）：`ISocketHost`/`ICongestionController` 等
  在热路径上有少量虚分派开销，换取可替换性与可测试性。
- **would_log 内联**：移到 `utp_internal.h` 头内联，避免虚函数 `vlog` 在 vtable 所在 TU
  出现跨 TU 未定义引用（Release 链接问题，已修复）。

## 新增文件

- `src/utp/logger.hpp` — `ILogger`
- `src/utp/socket_host.hpp` — `ISocketHost`
- `src/utp/congestion_control.hpp` — `ICongestionController`
- `src/utp/connection_state.hpp` — `IConnectionState`

## 后续可选项（未做，留待评估）

- 回调层（`UtpCallbacks` 虚基类 + `CFunctionCallbackAdapter` 适配器）已是合理的
  策略+适配器，本次保留未动。
- `utp_call_*` 16 个自由转发函数可考虑收敛，但它们服务于 C 边界，价值有限。

---

# （历史）TASK: UtpSocket 数据分组 + 独立函数归位重构

> **状态（2026-06-11 第二次更新）**：遗留工作 #1-#8 已全部修复完毕（工作区待提交）。
> 修复过程中另发现并修复了两个清单外的问题（见文末"修复记录"）。
> 验证：Debug/Release/UTP_DEBUG_LOGGING 三种配置零错误构建；
> `nm` 确认全部 28 个公开 C 符号以 C 链接导出；
> ucat 回环传输 100KB / 10MB 均逐字节一致。

## 目标（原始）

1. 将 UtpSocket 的 ~40 个成员变量归入 4 个纯数据 struct，实现认知分组 — ✅ 已完成
2. 将散落在 utp_api.cpp / utp_process.cpp / utp_socket.cpp 中的独立函数转为成员函数 — ✅ 已补齐
3. C API 层变为薄委托层（1-3 行），保持二进制兼容 — ✅ 已修复并验证

## 约束

- 公共 C API (`include/utp.h`) 二进制兼容不变 — ✅ 已恢复（typedef 改回前置声明 struct，
  三个丢失符号重新以 C 链接导出）
- `LedbatController` 和 `MtuDiscovery` 已提取完毕，保持不动 — 遵守
- 纯数据 struct 无行为方法（协议逻辑天然跨 send/receive 边界）— 遵守

---

## Phase A: 数据 struct 分组 — 已完成（带偏差）

4 个 struct 已定义于 `src/utp_socket.hpp`：`ConnectionId`（含 `target_delay`，`ctx` 为
`UtpContext*` 而非计划中的 `utp_context*`）、`ReceiveState`、`SendState`、`TimingState`。
个别位域字段名带尾下划线（`got_fin_reached_` / `fin_sent_acked_` / `close_requested_` /
`fast_timeout_`），与计划中的命名略有出入。

### 偏差：字段重命名未执行

原计划的"批量字段重命名映射表"（旧名 → `conn_./recv_./send_./timing_.` 新名）**没有执行**。
实际做法是在 UtpSocket 类尾部保留了 26 个引用成员兼容性别名
（`utp_socket.hpp:271-302`，如 `uint16& ack_nr_ = recv_.ack_nr;`），调用点仍大量使用旧名。

代价（审查已确认）：
- 每个 UtpSocket 多 208 字节（26 × 8），拷贝赋值被隐式删除
- 位域无法被引用别名，导致同一函数内两套拼写强制混用
  （如 `utp_process.cpp:418` 的 `send_.fin_sent && cur_window_packets_`）
- 每个字段需要 grep 两个名字

→ 收尾动作见遗留工作 #5。

---

## Phase B: 独立函数 → 成员函数 — 部分完成

### 已完成

| 旧自由函数 | 实际成员函数 |
|-----------|-------------|
| `utp_connect()` | `UtpSocket::connect()` |
| `utp_close()` | `UtpSocket::close()` |
| `utp_shutdown()` | `UtpSocket::shutdown()` |
| `utp_read_drained()` | `UtpSocket::read_drained()` |
| `utp_process_incoming()` | `UtpSocket::process_incoming()`（并进一步分解，见下） |
| `remove_socket_from_ack_list()` | `UtpSocket::remove_from_ack_list()` |
| `utp_register_recv_packet()` | `UtpSocket::register_recv_packet()` |
| `utp_check_timeouts()` | `UtpContext::check_timeouts()` |
| `utp_issue_deferred_acks()` | `UtpContext::issue_deferred_acks()` |
| `utp_process_udp()` | `UtpContext::process_udp()` |
| `utp_process_icmp_fragmentation()` | `UtpContext::process_icmp_fragmentation()` |
| `utp_process_icmp_error()` | `UtpContext::process_icmp_error()` |
| `send_to_addr()` | `UtpContext::send_to_addr_impl()`（名称与计划不同） |
| `utp_register_sent_packet()` | `UtpContext::register_sent_packet()` |

计划外新增：458 行的 `process_incoming` 被分解为私有成员
`parse_packet` / `process_acks` / `advance_send_window` / `deliver_data`（commit 5a0d4be），
引入 `ParsedPacket` 结构体。

### 未按计划执行

| 计划 | 实际 |
|------|------|
| `utp_initialize_socket()` → `UtpSocket::initialize()` | 仍为自由函数，friend 访问私有成员 |
| `utp_writev()` → `UtpSocket::writev()` | 仍为自由函数，friend（且同时是两个类的 friend） |
| `utp_getpeername()` → `UtpSocket::get_peername()` | 仍为自由函数，friend |
| `utp_setsockopt()` → `UtpSocket::set_option()` | 仍为自由函数，且定义签名改为 `UtpSocket*`，**破坏 C ABI** |
| `utp_getsockopt()` → `UtpSocket::get_option()` | 同上 |
| `utp_get_delays()` → `UtpSocket::get_delays()` | 同上 |
| `parse_icmp_payload()` → `UtpContext::find_socket_by_icmp()` | 成员化完成，但保留旧名 `parse_icmp_payload` |

### 偏差：C API 薄委托丢失了 NULL 保护

计划示例为 `if (conn) conn->close();`，实际实现为
`static_cast<UtpSocket*>(s)->close();`——utp_close / utp_connect / utp_shutdown /
utp_read_drained / utp_issue_deferred_acks / utp_check_timeouts / utp_process_udp
均无 NULL 检查（重构前为 `assert(x); if (!x) return ...;`），而 utp_getpeername /
utp_get_userdata 等仍保留检查，契约不一致。→ 遗留工作 #3。

### 计划外改动（commit 2d5a1df / 192b037）

- `include/utp.h` 中 `utp_socket` / `utp_context` 由不透明 struct typedef 改为
  `typedef void`，公开 API 失去编译期类型检查，并直接导致上述 ABI 破坏得以静默编译。
- 访问控制通过逐函数 friend 声明实现（UtpSocket 11 个 + UtpContext 10 个），
  签名已出现漂移（`UtpSocket*` 与 `utp_socket*` 混用；`utp_get_delays(UtpSocket*)`
  出现在 UtpContext 的 friend 表中）。

---

## 修复记录（2026-06-11，原"遗留工作"清单全部完成）

1. ✅ **C ABI 破坏**：`include/utp.h` 句柄 typedef 改回前置声明 struct 后，
   utp_setsockopt / utp_getsockopt / utp_get_delays 定义（utp_socket* == UtpSocket*）
   重新匹配 extern "C" 声明，三个 C 符号恢复导出（`nm` 验证，Debug/Release 均确认）。
2. ✅ **窗口截断回归**：`ParsedPacket::windowsize` 由 `uint16` 改为 `uint32`
   （utp_socket.hpp）。
3. ✅ **C API 包装层 NULL 保护**：utp_close / utp_connect / utp_shutdown /
   utp_read_drained / utp_issue_deferred_acks / utp_check_timeouts /
   utp_process_udp / utp_process_icmp_* 统一恢复 `assert + if (!x) return ...`。
4. ✅ **句柄类型安全**：`typedef void` → `typedef struct UtpSocket utp_socket;`
   / `typedef struct UtpContext utp_context;`（恢复重构前惯用法）。
   由此 utp_socket* 即 UtpSocket*，原 55 处恒等 static_cast 全部删除，
   无需 impl() 助手层。
5. ✅ **字段重命名完成、别名块删除**：26 个引用别名已删，全部调用点
   （431 处，clang 编译器驱动逐点替换）改为 `conn_./recv_./send_./timing_.`
   直接访问；借用旧名的局部变量/参数（send_rst 参数、process_udp /
   send_data 局部量）一并改名避免混淆。
6. ✅ **friend 表收敛**：补齐成员函数 `UtpSocket::initialize/writev/get_peername/
   set_option/get_option/get_delays/get_stats` + 访问器 `context()/userdata()/
   set_userdata()`；`UtpContext::create_socket/set_callback/set_option/get_option`
   + 访问器 `stats()/userdata()/set_userdata()`。C API 全部经由公开成员，
   两个类的 friend 各收敛为一条 `friend class` 互引。
   内部专用的 `utp_initialize_socket` 自由函数已无调用者，删除。
7. ✅ **去重**：新增 `UtpContext::find_socket_for_id()`（RST 与 ICMP 共用三段式查找）；
   新增 `utp::wire::packet_size_bucket()`（从 `packet_size_from_bucket` 推导边界，
   收发统计共用）。
8. ✅ **EOF 回绕比较**：deliver_data 改为比较相对 ack_nr 的回绕距离
   （`seqnr > ((eof_pkt - ack_nr - 1) & SEQ_NR_MASK)`）。

### 清单外发现并修复

9. ✅ **UTP_STATE_CONNECT 回调丢失**（commit a947d4a 引入，比本重构系列更早）：
   回调虚接口化时删掉了"未注册 UTP_ON_CONNECT 则回退触发
   UTP_ON_STATE_CHANGE(UTP_STATE_CONNECT)"的历史语义，导致经典 C API 应用
   （包括 examples/ucat）永远收不到连接建立通知，**主动连接完全不可用**。
   已在 CFunctionCallbackAdapter::on_connect 中恢复回退。
   定位过程：ucat 回环传输冒烟测试失败 → 逐提交二分（fe8bdcc 通过、
   a947d4a 失败）→ 对照该提交 diff 确认删除的分支。
10. ✅ **UTP_DEBUG_LOGGING 构建断裂 + statenames 错位**（30949eb 拆文件时引入）：
    `flagnames/statenames` 原为 utp_socket.cpp 的 static，utp_process.cpp 的
    调试日志引用不到；且 statenames 仍含已删除的 DESTROY_DELAY 条目，
    CS_RESET/CS_DESTROY 的日志标签错一位。已移入 utp_socket.hpp
    （inline constexpr）并修正条目。

### 验证

- 构建：Debug / Release / `-DUTP_DEBUG_LOGGING=ON` 三种配置 0 error
- 符号：`nm libutp.a` 公开 C API 28 个符号全部以 C 链接导出
- 功能：ucat 回环传输 100KB 与 10MB，接收文件与发送文件逐字节一致
  （此前在 HEAD 乃至 a965290 上该测试均失败，见 #9）
- 单元测试未运行：tests/ 依赖的 Catch2（/tmp/catch2_src）在本机不可用

## 风险与注意事项（原文保留，仍然有效）

- 位域分组到不同 struct 后布局变化 → 安全（无 memcpy），但检查 sizeof
- `ida_` 保留在 UtpSocket 顶层（仅 2 字段，不归入任何 struct）
- `duplicate_ack_` 保留在 UtpSocket 顶层（跨 send/recv 边界）
- `target_delay_` 移入 ConnectionId（per-socket 配置）— 已执行
