<!--
 Copyright (c) 2026 'Yang Luo, luoyang@sia.cn'
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# rocos-igh

<img src="rocos-igh.png" alt="ROCOS IGH logo" width="267">

Minimal C++ EtherCAT master shell built on IgH EtherCAT Master, with shared-memory IPC for client processes.

The build requires yaml-cpp (`libyaml-cpp-dev` on Debian/Ubuntu). Master-enabled
builds additionally require the IgH `ecrt.h` header and `libethercat` library.
The master executable is enabled by default (`ROCOS_IGH_BUILD_MASTER=ON`).

## Shared memory ABI v2

The shared ABI and IPC implementation come from the pinned Git submodule
[`rocos-shm`](3rdparty/rocos-shm) (`87c436c49777834964a5f1a38d46b4e0c74d34b2`, package 2.0.0 / ABI 2).
Initialize it before building the master or tests:

```bash
git submodule update --init --recursive
```

`src/shared_memory_config.hpp` is a compatibility include only; consumers link
`rocos::shm`. See [the migration and capability contract](docs/shared-memory-v2.md).
All masters, Apps and monitors must be rebuilt together. Old binaries cannot connect.

The shared Web monitor now lives in
[`rocos-shm/tools/shm_web`](https://github.com/rocos-sia/rocos-shm/tree/main/tools/shm_web)
for use by both ECM and IgH. Build it in a separate `rocos-shm` checkout; this
repository no longer keeps a copy. From that checkout:

```bash
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
./tools/shm_web/build/shm_web_server --master-id 0 --port 8484
```

Use the same master ID as the running IgH process, then open `http://127.0.0.1:8484`.
The pinned library revision above predates the tool migration; it still supplies
the matching ABI v2 headers but does not contain the Web tool.

**TC7 配置限制：** `config/tc7_pdo.yaml` 的第 8 个从站含 1-bit GPIO，当前 v2
规范要求拒绝非字节对齐 PDO，因此此配置会在加载时失败。保留设备真实位宽，不能
把 1 改为 8 来绕过校验；需设备提供合规字节映射，或另行扩展协议后再使用。

## Quick build with presets

With CMake 3.20 or newer and Make installed, run from the repository root:

```bash
# Debug: configure, build, and test
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

# Release: configure, build, and test
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

`cmake --preset` configures the build; `cmake --build --preset` compiles it.
Both presets enable the master executable and tests, and keep hardware integration
tests disabled. The executables are `build/debug/rocos_igh_master` and
`build/release/rocos_igh_master`, respectively. Building does not require connected
EtherCAT hardware, but does require the IgH development files. For a nonstandard
IgH installation, set `ETHERCAT_ROOT` to its installation prefix before configuring.

To configure without IgH, use `cmake --preset debug -DROCOS_IGH_BUILD_MASTER=OFF`
(or `release`), then use the same build and test commands. Running the configure
preset again without this override re-enables the master. The explicit-directory
commands below remain available with CMake 3.16 or newer.

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
- `--cpu <non-negative logical CPU ID|auto>`（可选，仅绑定周期线程；`auto` 沿用启动时的亲和性）
- `--preop-timeout-ms <1..4294967295>`：PREOP 等待超时，默认 10000 ms
- `--op-timeout-ms <1..4294967295>`：OP 等待超时，默认 60000 ms
- `--pdo-config-attempts <1..4294967295>`：每个从站 PREOP PDO 配置及读回验证的最大总尝试次数，默认 5（包含首次）
- `--dc <on|off>` (default: `off`)
- `--help`

启动参数也可写入 [config/talon_pdo.yaml](config/talon_pdo.yaml) 顶层的可选 `master` 块：

```yaml
master:
  master_id: 0
  period_us: 1000
  cpu: null
  dc: "off"
  preop_timeout_ms: 10000
  op_timeout_ms: 60000
  pdo_config_attempts: 5
```

优先级为 **命令行显式参数 > YAML `master` 字段 > 程序默认值**。
`--config` 仍必须通过命令行指定，`--help` 不读取配置文件。
省略 `master` 块或其中任意字段时使用程序默认值；旧 PDO 配置仍兼容。
`cpu: null` 保留原有亲和性，数字表示绑定逻辑 CPU；`dc` 使用 `"on"` / `"off"`。
配置只在启动时读取，临时命令行覆盖不会修改文件。例如：

```bash
# 全部使用 YAML 配置
./build-master/rocos_igh_master --config config/talon_pdo.yaml

# 只覆盖周期和 PDO 尝试次数，其余仍使用 YAML 配置
./build-master/rocos_igh_master --config config/talon_pdo.yaml \
    --period-us 2000 --pdo-config-attempts 8

# 临时取消 YAML 中的 CPU 绑定，并关闭 DC
./build-master/rocos_igh_master --config config/talon_pdo.yaml --cpu auto --dc off
```

YAML 中的未知字段、重复字段、非法类型或范围会报错；即便命令行覆盖该字段，
也不会忽略非法 YAML。DC 与周期的组合约束、CPU 可用性按合并后的最终参数校验。

PDO 配置失败时重做当前从站的完整写入和读回验证，已验证的从站不重复配置。
所有从站及尝试仍共用 `--preop-timeout-ms` 的总预算（包括等待 PREOP 和初始化 SDO），
增加尝试次数不会自动延长超时；必要时同时调大该超时。
例如 `--pdo-config-attempts 8 --preop-timeout-ms 30000`。
PREOP 状态丢失或超时立即退出，激活后的 OP/PDO 再验证流程保持不变。

### 每秒运行报告

主站就绪后，已有普通优先级日志线程每约 1 秒输出主站 ID、链路状态、
响应/配置从站数、AL 状态位掩码、`is_authorized`、输入/输出 WKC 及完整性。
每个计时窗口约 1 秒，输出以下 min/avg/max（单位 μs）：

- `Cycle Time`：相邻实际唤醒间隔。
- `Task Duration`：唤醒后到发送及客户端通知完成的执行时间，不含睡眠等待和末尾统计汇总。
- `Receive / Process`：IgH 接收和两个 domain 的处理时间。
- `PDO Copy`：输入发布及输出读取的拷贝时间。
- `Queue / Send`：domain 入队、DC 同步（若启用）及发送时间。

同时显示窗口/累计的 `missed_deadlines`、`wc_error_cycles` 和 `link_down_cycles`。
后两者从就绪后开始累计；`missed_deadlines` 累计值包含启动阶段。
WKC 异常按周期计数，一个周期两个 domain 都异常只记一次；链路异常单独计数。
状态字段为窗口末次快照，窗口内短暂异常仍体现在计数中。
共享内存原有计时字段及退出统计继续使用原来的累计/重置语义，不受报告窗口重置影响。

周期线程仅查询单调时钟、累计统计并发布固定大小的进程内无锁快照，
不为报告分配内存、打印或等待日志线程。日志线程延迟时丢弃新报告窗口，
累计计数继续更新。截止期按本轮执行完成时间推进，超时跳过错过的周期而不补跑；
运行中 WKC 异常或掉线分别通过 `wc_state`、`link_up` 和有效位发布，并继续周期通信；IgH 的 `is_authorized` 固定为 true。

### 周期线程 CPU 亲和性

例如将主站的周期线程绑定到逻辑 CPU 3（编号从 0 开始）：

```bash
sudo ./build/rocos_igh_master --config ./config/talon_pdo.yaml \
    --dc on --op-timeout-ms 60000 --cpu 3
```

`--cpu` 在访问 EtherCAT 设备前通过临时线程试绑并读回验证，不改变主线程的原有亲和性。
显式指定的 CPU 可以位于继承的亲和性集合之外，包括 `isolcpus` 隔离核；
无需先用 `taskset` 扩展集合。内核仍会拒绝离线、cpuset 禁止或权限不足的绑定，
程序也会拒绝超出 `CPU_SETSIZE` 的编号。
日志线程创建后、进入周期循环前，通过 `sched_setaffinity(0, ...)` 绑定实际
运行周期循环的主线程，并读回亲和性确认。失败直接退出，不默默退回未绑定运行。
日志线程保留原有亲和性和调度策略；不修改 IRQ 或 IgH 内核线程的亲和性。

实时准备继续使用 `mlockall(MCL_CURRENT | MCL_FUTURE)`、64 KiB 栈预触碰和
`SCHED_FIFO` 优先级 80，锁内存或调度设置失败则退出。启动时增加
`CLOCK_MONOTONIC` 分辨率检查：查询失败退出，非 1 ns 时给出警告。
分辨率不等于实际唤醒延迟，绑定 CPU 也不等于独占或隔离 CPU。

周期仍使用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`；
亲和性和实时设置仅在初始化执行，不给 1 ms 周期增加系统配置调用、分配或日志。
超时仍跳过错过的周期并累计 `missed_deadlines`，不补跑；启动期总线异常处理、
运行期掉线/WKC 异常通过 v2 诊断字段报告，周期通信继续。

可用 `lscpu -e=CPU,CORE,SOCKET,ONLINE` 查看逻辑核与物理核对应关系，
运行后用 `taskset -pc <主站PID>` 检查主线程绑定结果。退出统计中的
`min_us/max_us/avg_us` 表示相邻周期唤醒间隔，结合 `missed_deadlines` 比较绑定前后抖动。

### 每次启动前关闭 EoE

每次启动主站应用前，先运行 [关闭 EoE 脚本](scripts/disable-eoe.sh)，再启动
`rocos_igh_master`。七轴 Elmo 总线已验证：EoE 邮箱访问会干扰 CoE PDO
Assignment，出现 `Other mailbox protocol response`、`No response` 和
`Failed to assign PDO 0x1607`；关闭 EoE 后多次启动正常。

在仓库根目录执行，先确保 IgH 服务已就绪、从站扫描完成，且此前的主站应用
已经退出。脚本和主站命令的 `--master-id` 必须一致；以下以主站 0 为例：

```bash
# 可选：预览将关闭的 EoE 虚拟接口，不修改接口状态
bash scripts/disable-eoe.sh --master-id 0 --dry-run
```

实际启动时用 `&&` 连接关闭步骤与主站命令，关闭失败时不继续启动：

```bash
sudo bash scripts/disable-eoe.sh --master-id 0 && \
./build-master/rocos_igh_master --config config/pdo.yaml --master-id 0 --period-us 1000
```

脚本只关闭指定主站的 `eoe<ID>s<position>` / `eoe<ID>a<alias>` 虚拟接口，
会中断对应的 EoE/IP 访问，不关闭 EtherCAT 物理网卡。重复执行可以保持接口
关闭；该操作不持久，系统或 IgH 服务重启、接口重建、网络管理器自动拉起接口
后可能失效，因此每次启动都执行此步骤。可用 `ip -br link` 确认目标接口为
DOWN；若被网络管理器重新拉起，需要将这些 EoE 接口设为不受其管理。

脚本提示 `No EoE interfaces found` 时不会修改接口，也会成功退出；若预期
存在 EoE 接口，应先确认主站 ID 和扫描状态，避免接口在执行脚本后才创建。
需要恢复 EoE/IP 访问时，在主站应用停止后使用
`sudo ip link set dev <interface> up` 恢复对应接口。

Distributed Clocks remain disabled unless explicitly enabled. When enabled,
all YAML slaves use the built-in SYNC0 settings (AssignActivate 0x0300, zero shifts,
SYNC1 disabled); exactly one slave must have `dc_ref: true`. Other slaves may set
`dc_ref: false` or omit it and still participate in synchronization:

```bash
sudo bash scripts/disable-eoe.sh --master-id 0 && \
./build-master/rocos_igh_master --config config/device.yaml --master-id 0 --period-us 1000 --dc on
```

The YAML file lists slaves in physical bus order and configures each slave's
RxPDO and TxPDO mappings. Alias is fixed to zero; slave IDs start at zero and
also define ring positions. See the full [PDO configuration](docs/pdo-config.md)
format specification and the checked-in [example](config/pdo.yaml).

Slave identities are not configured in YAML. Startup reads each vendor ID and
product code from SII, prints the resolved mapping, and passes the actual
identity to IgH for strict matching.

## Additional docs

- [主站启动步骤与参数说明](docs/master-startup.md)
- [Architecture](docs/architecture.md)
- [API guide](docs/api-guide.md)
- [EtherCAT terminal commands](docs/ethercat-terminal-commands.md)
- [实时 CPU 隔离与 GRUB 配置](docs/realtime-cpu-isolation.md)


启用 DC 时，激活前设置初始单调时间、激活后立即刷新，以避免 DC 偏移初始化时
尚未收到应用时间。周期 deadline 沿用首次时间的相位，初始化耗时只跳过周期。
此处理针对本机 IgH 1.6 的 APP_TIME 实现，详见 [API 指南](docs/api-guide.md)。

### 启动状态确认

启动时先逐个查询配置中的从站，等待全部处于无错误的 PREOP，连续确认 5 次后才进行 PDO/DC 等配置（轮询间隔 10 ms，默认超时 5000 ms，可用 `--preop-timeout-ms` 配置）。激活成功仅表示配置已提交，不表示从站已经进入 OP。

激活前先按从站顺序执行各自可选的 `init_commands`，全部成功后再按从站顺序通过 SDO 实际写入并读回 PDO 字典，当前从站全部验证通过后才处理下一个从站：先清零 SM2/SM3 的分配数量（`0x1c12:0`、`0x1c13:0`），清零各 PDO 的映射数量，写映射条目并恢复映射数量，再写 PDO 分配条目、恢复分配数量。随后逐项核对两个方向的分配数量、PDO 索引及顺序、映射数量、条目的索引/子索引/位宽及顺序，长度也必须一致。写入、读回或比较失败时，重新执行该从站的完整配置与读回，**默认总共最多尝试 5 次，可通过 `--pdo-config-attempts` 设置**；失败日志包含从站物理位置、对象/子索引、差异或 SDO 返回码/abort code。达到尝试上限仍失败则不激活主站。要求设备支持这些对象的独立子索引读写，不跳过只读或不支持的对象。

`init_commands` 条目包含 `index`、`sub_index`、`Type` 和 `Value`，例如
`{index: 0x2000, sub_index: 0, Type: uint32, Value: 1000}`（仅格式示例）。
支持有符号/无符号 8、16、32、64 位整数，按小端编码；也支持小写 `type`/`value`。
初始化命令按列表顺序执行，只确认 SDO 下载成功应答，不要求对象可读；任何失败立即停止启动。
为避免重复触发设备动作，不随 PDO 重试重放初始化命令。详见
[PREOP SDO 初始化命令](docs/pdo-config.md#preop-sdo-初始化命令)。
`talon_pdo.yaml` 各从站预留空列表，填入实际设备所需字典后生效。

每次 SDO 调用前后检查从站仍处于无错误 PREOP；状态查询失败、离开 PREOP 或耗尽总时限时立即终止，不继续重写。这里使用 IgH 阻塞式 SDO 接口，仅在初始化阶段执行，不进入实时周期线程。`--preop-timeout-ms` 的总预算涵盖 PREOP 稳定等待、初始化 SDO、PDO 配置与所有从站的读回/重试；**IgH 同步 ioctl 内的等待由 IgH 控制，应用不能取消正在执行的调用**，调用返回后才判定超时并拒绝其结果，因此不是可抢占的硬截止时间。

IgH 激活时仍可能再次写入 PDO 配置，PREOP 验证通过不代表业务就绪；保留以下激活后的独立异步读回门禁，防止激活阶段的重写异常被放行。

激活后的启动循环继续按配置周期收发 PDO 和 DC 同步报文，逐个使用 `ecrt_slave_config_state()` 查询从站。全部从站在线、处于 OP 且 `operational` 有效，链路正常、响应从站数量匹配、输入/输出域工作计数器均为 `EC_WC_COMPLETE`，连续确认 5 个周期后，开始异步 PDO 读回验证：

- 对每个从站读取 SM2/SM3 的 `0x1C12/0x1C13` 分配数量和各子索引，确认 PDO 数量、索引及顺序。
- 对每个已配置 PDO 读取子索引 0 的条目数量，再逐项读取 32 位映射值，确认对象索引、子索引、位宽及顺序（包括填充条目）。
- 预期值直接来自本次加载的配置；不把启动时打印的配置或 IgH 配置缓存当作设备读回。使用独立子索引读取，不依赖 Complete Access。
- 所有从站全部验证通过后，再连续确认 5 个健康周期，才宣布就绪、读取客户端输出、发布过程数据并通知客户端。等待期间一直发送清零后的输出域。

每个从站的 SDO 请求和验证表均在激活前创建；启动周期中一次只推进一个异步读取，不动态分配、不阻塞等待、不写 SDO 或重新配置从站。单次读取超时为 1000 ms；读取失败、长度不符、值不一致或超时均停止启动，不跳过不支持读回的设备，不在激活后的周期中自动重试。错误详情在周期任务退出后打印，包含从站物理位置、SM 方向、对象/子索引、期望值和实际值；映射条目不一致时还拆分显示 index/subindex/bits。读取失败没有有效实际值，报告失败原因。

例如，位置 2 的从站第二个 TxPDO 条目位宽错误时：

```text
PDO verification failed at slave[position=2] SM3 TxPDO object=0x1a07:02 mapping entry/order expected=0x60640020 actual=0x60640010 expected{index=0x6064,subindex=0x0,bits=32} actual{index=0x6064,subindex=0x0,bits=16}
```

PREOP 和 OP 均按单调时钟计时，查询及调度耗时计入超时。OP 从启动周期循环开始计时，默认 60000 ms，可用 `--op-timeout-ms` 配置；两个超时均为整个阶段的总时限，包含连续稳定确认时间，不是每个从站单独计时。`--op-timeout-ms` 还包含所有从站的 PDO 读回验证时间；多轴或邮箱较慢的设备可适当增加此值。值必须为正整数毫秒，0 不表示无限等待。状态查询失败或超时会停止启动并返回错误，不会继续处理客户端输出。PDO 验证开始前，链路/OP/WC 不满足时继续等待；验证一旦开始，任何 OP、链路或 WKC 退化都会中止启动，避免沿用从站重新配置前的校验结果。周期超时跳过已错过的周期，不补跑，也不延长启动或单次读取超时。运行期链路/WC 异常通过 `link_up`/`wc_state` 报告，各从站 AL 状态通过 `Slave.al_state` 报告；`is_authorized` 固定为 true。现有周期通信仍继续，不自动执行状态重置。

例如，为多从站总线设置 PREOP 30 秒、OP 60 秒：

```bash
sudo bash scripts/disable-eoe.sh --master-id 0 && \
./build-master/rocos_igh_master --config config/talon_pdo.yaml --master-id 0 --dc on --preop-timeout-ms 30000 --op-timeout-ms 60000
```

当前保持标准 IgH 接口：SAFEOP → OP 由 IgH 自动推进，**不提供“全部停留 SAFEOP、检查后再请求 OP”的屏障**。`initialize()` 完成配置与激活，OP 和 PDO 读回就绪确认在 `CyclicTask::run()` 的启动阶段完成。`pdo_verification_test` 使用模拟 SDO、时钟和总线运行真实启动循环，覆盖 PREOP 写入顺序、逐站读回与可配置尝试上限、状态丢失/超时退出，以及激活后多从站多 PDO、顺序/数量/映射差异、读取错误、超时、健康状态退化、业务隔离及周期内不分配内存，无需硬件；真实从站状态转换仍需单独进行硬件集成验证。

DC 时间沿用 IgH `dc_user` 示例：初始化阶段不设置应用时间，从首个周期起使用目标 deadline 建立相位基准；发送前读取当前 `CLOCK_MONOTONIC` 时间，调用 `ecrt_master_sync_reference_clock_to()` 校准参考时钟。初始化不调用 `ecrt_master_reset()` 请求 INIT，也不在激活后空等状态切换。
