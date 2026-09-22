<!--
 Copyright (c) 2026 'Yang Luo, luoyang@sia.cn'
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# rocos-igh

Minimal C++ EtherCAT master shell built on IgH EtherCAT Master, with shared-memory IPC for client processes.

The build requires yaml-cpp (`libyaml-cpp-dev` on Debian/Ubuntu). Master-enabled
builds additionally require the IgH `ecrt.h` header and `libethercat` library.

## Build and test (hardware-free)

Use this mode when IgH development files are not installed. It builds the IPC-focused library and tests.

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Build and test (master enabled)

Use this mode when `ecrt.h` and `libethercat` are available on the machine.

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master --output-on-failure
```

## Runtime entrypoint

The executable is `rocos_igh_master` and supports:

- `--config <PDO YAML path>` (required)
- `--master-id <non-negative integer>`
- `--period-us <integer >= 1000>`
- `--preop-timeout-ms <1..4294967295>`：PREOP 等待超时，默认 5000 ms
- `--op-timeout-ms <1..4294967295>`：OP 等待超时，默认 10000 ms
- `--dc <on|off>` (default: `off`)
- `--help`

```bash
./build-master/rocos_igh_master --config config/pdo.yaml --master-id 0 --period-us 1000
```

Distributed Clocks remain disabled unless explicitly enabled. Enabling DC also
requires device-specific `dc` blocks in the YAML configuration, including
exactly one reference slave:

```bash
./build-master/rocos_igh_master --config config/device.yaml --period-us 1000 --dc on
```

The YAML file lists slaves in physical bus order and configures each slave's
RxPDO and TxPDO mappings. Alias is fixed to zero; slave IDs start at zero and
also define ring positions. See the full [PDO configuration](docs/pdo-config.md)
format specification and the checked-in [example](config/pdo.yaml).

Slave identities are not configured in YAML. Startup reads each vendor ID and
product code from SII, prints the resolved mapping, and passes the actual
identity to IgH for strict matching.

## Additional docs

- [Architecture](docs/architecture.md)
- [API guide](docs/api-guide.md)
- [EtherCAT terminal commands](docs/ethercat-terminal-commands.md)


启用 DC 时，激活前设置初始单调时间、激活后立即刷新，以避免 DC 偏移初始化时
尚未收到应用时间。周期 deadline 沿用首次时间的相位，初始化耗时只跳过周期。
此处理针对本机 IgH 1.6 的 APP_TIME 实现，详见 [API 指南](docs/api-guide.md)。

### 启动状态确认

启动时先逐个查询配置中的从站，等待全部处于无错误的 PREOP，连续确认 5 次后才进行 PDO/DC 等配置（轮询间隔 10 ms，默认超时 5000 ms，可用 `--preop-timeout-ms` 配置）。激活成功仅表示配置已提交，不表示从站已经进入 OP。

激活后的启动循环继续按配置周期收发 PDO 和 DC 同步报文，逐个使用 `ecrt_slave_config_state()` 查询从站。全部从站在线、处于 OP 且 `operational` 有效，链路正常、响应从站数量匹配、输入/输出域工作计数器均为 `EC_WC_COMPLETE`，连续确认 5 个周期后才读取客户端输出、发布过程数据并通知客户端。启动期间发送清零后的输出域；PREOP 和 OP 确认成功均打印日志。

PREOP 和 OP 均按单调时钟计时，查询及调度耗时计入超时。OP 从启动周期循环开始计时，默认 10000 ms，可用 `--op-timeout-ms` 配置；两个超时均为整个阶段的总时限，包含连续稳定确认时间，不是每个从站单独计时。值必须为正整数毫秒，0 不表示无限等待。状态查询失败或超时会停止启动并返回错误，不会继续处理客户端输出。链路掉线或工作计数器不完整会清除连续成功计数；周期超时跳过已错过的周期，不补跑，也不延长启动超时。运行期链路/WC/汇总 OP 状态异常会使共享总线的 `is_authorized` 为 false，现有周期通信仍继续，不自动执行状态重置。

例如，为多从站总线设置 PREOP 30 秒、OP 60 秒：

```bash
./build-master/rocos_igh_master --config config/talon_pdo.yaml --dc on --preop-timeout-ms 30000 --op-timeout-ms 60000
```

当前保持标准 IgH 接口：SAFEOP → OP 由 IgH 自动推进，**不提供“全部停留 SAFEOP、检查后再请求 OP”的屏障**。`initialize()` 完成配置与激活，OP 就绪确认在 `CyclicTask::run()` 的启动阶段完成。启动状态测试使用模拟查询，无需硬件；真实从站状态转换仍需单独进行硬件集成验证。

DC 时间沿用 IgH `dc_user` 示例：初始化阶段不设置应用时间，从首个周期起使用目标 deadline 建立相位基准；发送前读取当前 `CLOCK_MONOTONIC` 时间，调用 `ecrt_master_sync_reference_clock_to()` 校准参考时钟。初始化不调用 `ecrt_master_reset()` 请求 INIT，也不在激活后空等状态切换。
