# Low-Latency Option Gateway & Greeks Engine

一个从零手写的高性能、低延迟期权交易网关核心。代码无依赖任何第三方业务库，全由纯 C++ (C++20) 实现。

## 🚀 核心特性
- **无锁并发架构**：基于 Cache Line 对齐的 SPSC 环形队列，消除多线程锁竞争与伪共享。
- **零拷贝变长协议**：完美解决 TCP 粘包/半包，支持动态内存缓冲区。
- **全生命周期 OMS**：多线程解耦的订单状态机（Pending -> Submitted -> Cancelled）。
- **AVX2 极致计算**：利用 SIMD 指令集手写 Black-Scholes 与 Greeks 批量计算，使用无分支掩码（Masking）处理尾部数据。
- **多核并行**：基于 CPU Affinity 绑核与原子变量，实现组合 Greeks 的多核并行汇总。

## 📊 性能测试数据
*测试环境：Windows 11, Intel Core i7-10700, VS2022 Release (/O2)*
- **单核标量计算（100持仓）**：Avg: 0.38us | P99: 2us
- **多核并行 AVX2（满仓）**：Avg: 5us | P99: 75us (受非实时 OS 调度影响，可通过 Linux 绑核隔离进一步降至 5us 以内)

## 🛠️ 如何编译与运行
1. 使用 Visual Studio 2022 创建两个空项目（导入对应的 cpp 文件）。
2. 确保开启 `C++20` 标准，以及 `Release/x64` 模式。
3. 先运行 `fake_server.exe`，再运行 `gateway.exe`。

## ⚠️ 免责声明
本项目仅供 C++ 并发编程、低延迟架构与 AVX2 向量化技术的学习与交流，不构成任何投资建议。
