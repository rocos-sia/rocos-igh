# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

基于 IgH EtherCAT Master 的最小 C++17 主站运行时：每个 `rocos_igh_master` 进程独占一个 IgH 主站，以 1 ms 为设计目标周期收发 PDO，并通过 POSIX 共享内存和命名信号量向客户端进程发布输入、接收输出。从站与 PDO 在启动时从必选的 YAML 配置文件加载。

## 构建与测试

存在两种构建模式，由 `ROCOS_IGH_BUILD_MASTER` 选项控制（默认 ON）。

无硬件模式（不依赖 IgH 开发文件，仅构建 IPC 库与测试）：

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

主站模式（需要 `ecrt.h` 与 `libethercat`）：

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master --output-on-failure
```

只运行单个测试：

```bash
ctest --test-dir build -R shared_memory_config --output-on-failure
```

运行主站可执行：

```bash
./build-master/rocos_igh_master --config config/pdo.yaml --master-id 0 --period-us 1000
```

## 架构

- 构建目标：`rocos_igh_core`（主站模式下静态库、无硬件模式下接口库）、`rocos_igh_master`（可执行，仅主站模式）、`pdo_config_test` 和 `shared_memory_config_test`（CTest 测试可执行，无外部测试框架，用自定义 `CHECK` 宏断言，失败返回非零）。`ROCOS_IGH_BUILD_MASTER` 宏同时作为编译期开关隔离 IgH 相关代码。
- 组件与职责（详见 [docs/architecture.md](docs/architecture.md)）：
  - [src/main.cpp](src/main.cpp) — 进程入口：解析参数、安装 `SIGINT`/`SIGTERM` 处理、按固定顺序初始化、实时设置、运行与退出。
  - [src/runtime_options.hpp](src/runtime_options.hpp) — 严格命令行解析（必选 `--config`，以及 `--master-id`、`--period-us`、`--help`），无 EtherCAT 副作用。
  - [src/pdo_config.hpp](src/pdo_config.hpp) — 硬件无关的 YAML PDO 配置解析与校验。
  - [src/slave_config.hpp](src/slave_config.hpp) — 动态配置到 IgH 指针视图的转换、校验、格式化输出与元数据发布。
  - [src/ethercat_master.hpp](src/ethercat_master.hpp) — IgH 主站 RAII 生命周期：request → 两个 domain → 从站配置 → PDO 注册 → activate；周期方法 `receiveAndProcess`/`queueAndSend`/`readState`。
  - [src/cyclic_task.hpp](src/cyclic_task.hpp) — 绝对时间实时循环（`clock_nanosleep(TIMER_ABSTIME)`）、定长 memcpy、周期统计、信号量通知。
  - [src/shared_memory_config.hpp](src/shared_memory_config.hpp) — 跨进程 IPC：主站侧 `SharedMemoryConfig(master_id)` 创建、客户端侧 `SharedMemoryConfig::getInstance(master_id)` 连接。
- 周期数据流（`CyclicTask::run`）：`receive → 处理两个 domain → 输入复制到 pd_input / 输出从 pd_output 复制回 domain → 读状态 → queue → send → 通知信号量`。
- 双 domain 设计：输入、输出各一个 domain，各自形成连续缓冲区，周期内直接定长 `memcpy`，避免逐变量复制。

## 关键约束

- **ABI 兼容**：`EcatBus`、`Slave`、`PdVar` 是跨进程 ABI，不要改变字段顺序/类型/数组上限；`EcatConfigMaster`/`EcatConfig` 是 `SharedMemoryConfig` 的旧别名，须保留。`PdVar::size` 必须等于对应 PDO 的 `sizeof(T)`。
- **实时路径**：激活后的周期循环禁止动态内存分配、阻塞 I/O、日志、SDO、从站重配、互斥锁；只允许 IgH 标记 `rt_safe` 的调用。`mlockall`、内存预触碰、`SCHED_FIFO`（优先级 80）都在进入循环前完成。
- **master_id 隔离**：同一个非负 `master_id` 贯穿 IgH 主站索引与全部 IPC 命名；命名约定为 `ecm{id}`、`pd_input{id}`、`pd_output{id}`、`sync{id}_{0..9}`（POSIX 调用需补前导 `/`，由 `toPosixName` 处理）。禁止跨主站共享可变状态。
- **周期下限**：`period_us` 最小为 `1000`（1 ms 是设计目标，非普通 Linux 下的确定性保证）。
- **YAML PDO 配置**：从站 `id` 从 0 连续递增并映射物理位置，alias 固定为 0；RxPDO 固定使用 SM2，TxPDO 固定使用 SM3；身份从 SII 读取。PDO 条目要求字节对齐（`bit_length % 8 == 0`、`bit_position == 0`）。完整格式见 [docs/pdo-config.md](docs/pdo-config.md)。
- **测试隔离**：无硬件测试用 `10000 + getpid() % 10000` 生成唯一主站 ID 避免并行冲突；`EC_SEM_NUM`（10）同时限制每主站等待线程数。

## 相关文档

- [docs/architecture.md](docs/architecture.md) — 设计决策与完整架构
- [docs/api-guide.md](docs/api-guide.md) — IgH API 阶段与实时安全约束
- [docs/ethercat-terminal-commands.md](docs/ethercat-terminal-commands.md) — 硬件诊断命令与安全注意
- [README.md](README.md) — 构建与运行说明
