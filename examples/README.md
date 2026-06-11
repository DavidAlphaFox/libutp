# libutp 示例

| 示例 | 形态 | 演示内容 |
|---|---|---|
| [`ucat`](ucat.c) | 单文件 C 程序，默认构建 | 最小可用集成：单连接、阻塞 select、ICMP 错误队列（netcat 风格管道传输） |
| [`uv-utp-chat`](uv-utp-chat/) | C++23，`-DUTP_BUILD_UV_EXAMPLE=ON` | 生产形态集成：libuv 事件循环、一个 UDP 端口复用 N 条连接、TBB 计算线程池 + 回压闭环、优雅关闭、`--self-test` 自动化验证 |

两个示例覆盖 libutp 嵌入的两端：`ucat` 展示 API 的最小面积，
`uv-utp-chat` 展示真实异步服务里的线程模型与生命周期管理
（设计与架构详见其 [README](uv-utp-chat/README.md)）。
