# libutp — uTP（Micro Transport Protocol）传输协议库

基于 [LEDBAT][ledbat] 拥塞控制算法实现的 uTP 协议库，在 [BEP-29][bep29] 中作为 BitTorrent 扩展定义。
uTP 在 UDP 之上提供类似 TCP 的可靠、有序字节流传输，同时尽量减少额外延迟，
是 uTorrent 对等连接的主要传输协议。

本项目基于原始 libutp 进行 C++23 现代化重构，使用 CMake 构建系统。

## 项目结构

```
libutp/
├── include/              # 公共 C API 头文件
│   ├── utp.h             # 公共 API 接口
│   └── utp_types.h       # 基础类型定义
├── src/                  # 源代码
│   ├── utp/              # C++23 模块化头文件
│   │   ├── config.hpp    # 编译期常量
│   │   ├── endian.hpp    # 大端字节序模板
│   │   ├── platform.hpp  # 平台相关类型
│   │   ├── wire_format.hpp    # 线路协议数据结构
│   │   ├── sequence_buffer.hpp # 序列号环形缓冲区
│   │   └── delay_history.hpp  # 延迟历史记录
│   ├── utp_api.cpp       # 公共 C API 实现
│   ├── utp_internal.cpp  # 核心协议实现（连接管理、拥塞控制、超时重传）
│   ├── utp_callbacks.cpp # 回调函数
│   ├── utp_packedsockaddr.cpp # 打包的 Socket 地址
│   └── utp_utils.cpp     # 工具函数（时间、MTU）
├── examples/             # 示例程序
│   └── ucat.c            # 简单的 uTP 客户端/服务端
├── tests/                # Catch2 单元测试
├── cmake/                # CMake 模块
└── CMakeLists.txt        # 根构建配置
```

## 接口说明

uTP 的 Socket 接口与传统 Berkeley Socket API 有所不同，
采用事件驱动、回调机制的设计，避免实现 `select()`，方便编写无缓冲的事件驱动代码。

创建 uTP Socket 时注册一组回调函数。其中 `on_read` 是被动回调——
当网络数据到达时触发。发送侧是主动的：调用 `UTP_Write` 声明要发送的字节数，
随后 `on_write` 回调为每个数据包触发，由调用方填充缓冲区。

libutp 接口**非线程安全**，设计用于单线程异步上下文。
如需在多线程环境中使用，需自行添加同步机制。

详见 `include/utp.h`。

## 构建

### 依赖

- CMake 3.25+
- 支持 C++23 的编译器（GCC 13+, Clang 17+, MSVC 2022+）
- Catch2 v3（测试框架，构建时自动获取）

### 编译

```bash
# 配置
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build

# 运行测试
cd build && ctest --output-on-failure
```

### 构建选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `UTP_BUILD_SHARED` | 构建动态库而非静态库 | OFF |
| `UTP_BUILD_EXAMPLES` | 构建示例程序（ucat） | ON |
| `UTP_DEBUG_LOGGING` | 启用详细调试日志 | OFF |
| `UTP_ENABLE_STATS` | 启用统计信息和额外检查 | OFF |
| `BUILD_TESTING` | 构建单元测试 | ON |

### 示例

```bash
cmake --build build --target ucat
./build/examples/ucat <host> <port>
```

## 重构说明

本项目从原始 C89/C++98 代码库进行了以下现代化改造：

- **CMake 构建系统**：替代原始 Makefile 和 VS 项目文件
- **C++23 标准**：使用 `constexpr`、`std::vector`、`std::unordered_map` 等 STL 组件
- **RAII 内存管理**：`OutgoingPacket` 和 `InboundPacket` 使用 `std::vector<uint8_t>` 管理数据，
  消除 `malloc`/`free` 手动内存管理
- **模块化头文件**：`src/utp/` 目录下独立的 C++23 头文件（配置、字节序、线路格式等）
- **单元测试**：Catch2 框架，覆盖字节序转换、线路格式、序列缓冲区、延迟历史等模块
- **公共 C API 保持二进制兼容**：`utp.h` 接口不变

## API 稳定性

libutp API 被视为不稳定。建议与您的应用一起打包，
升级时注意测试。

## 许可证

libutp 基于 [MIT][lic] 许可证发布。

## 相关资料

- [LEDBAT 工作组][ledbat]
- [BEP-29: uTP 协议规范][bep29]
- [拥塞控制机制研究][survey]

[ledbat]: http://datatracker.ietf.org/wg/ledbat/charter/
[bep29]: http://www.bittorrent.org/beps/bep_0029.html
[lic]: http://www.opensource.org/licenses/mit-license.php
[survey]: http://datatracker.ietf.org/doc/draft-ietf-ledbat-survey/
