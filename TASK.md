# TASK: UtpSocket 数据分组 + 独立函数归位重构

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
