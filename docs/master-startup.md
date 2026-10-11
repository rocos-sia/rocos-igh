# rocos_igh_master 启动步骤

本文使用 `config/talon_pdo.yaml`，在 IgH 主站 0 上启用 DC，周期为 1 ms，
将周期线程绑定到逻辑 CPU 3。所有相对路径均以仓库根目录为基准。

启动会配置真实从站的 PDO 并推进 EtherCAT 状态。操作前确认目标主站、
从站接线与配置匹配，设备处于允许启动的状态，且没有其他程序占用同一主站。

## 1. 进入项目目录并编译

本机项目路径如下；其他机器按实际位置调整：

```bash
cd /home/think/Documents/GitHub/rocos-igh

cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build -j4

./build/rocos_igh_master --help
```

主站构建需要 C++17 编译器、CMake、yaml-cpp，以及 IgH 的 `ecrt.h` 和
`libethercat`。`--help` 应包含 `--cpu` 参数；该命令不会访问 EtherCAT 设备。
本文统一使用 `build` 目录；若实际构建到 `build-master`，应同步替换后续路径。

## 2. 检查 IgH 服务、从站和 CPU

```bash
systemctl status ethercat --no-pager
sudo ethercat master -m 0
sudo ethercat slaves -m 0
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
```

确认主站 0 的链路与从站扫描正常，物理从站数量、顺序和型号与
`config/talon_pdo.yaml` 匹配。当前文件包含位置 0～6 共七个从站。

如果服务尚未启动，在确认 EtherCAT 网卡配置正确且没有其他应用运行后，
执行 `sudo systemctl start ethercat`，再重新检查。不要在运行中的总线上
通过重启服务、重扫或强制切换从站状态来代替排查。

CPU 3 必须在线，且允许通过内核亲和性接口绑定（受 cpuset 和权限限制），
无需预先包含在启动进程继承的亲和性集合中。本机 CPU 1、3 是同一
物理核的超线程兄弟；若已按 [实时 CPU 隔离与 GRUB 配置](realtime-cpu-isolation.md)
隔离 `1,3`，仍需要在主站命令中使用 `--cpu 3`。
绑定参数不会自动配置 GRUB、中断亲和性或 CPU 隔离。

## 3. 确认 EoE 接口已关闭

本机七轴总线曾出现 EoE 干扰 CoE/SDO 配置的情况。主站应用停止且不需要
通过 EoE/IP 调试驱动器时，检查接口：

```bash
ip -br link
```

若已配置 NetworkManager 永久忽略 EoE，且主站 0 对应的 EoE 接口保持 DOWN，
可直接进入下一步。否则使用项目脚本关闭主站 0 的 EoE 虚拟接口：

```bash
# 可选：先预览
bash scripts/disable-eoe.sh --master-id 0 --dry-run

# 实际关闭后检查
sudo bash scripts/disable-eoe.sh --master-id 0
ip -br link
```

脚本仅匹配主站 0 的 `eoe0s<位置>` / `eoe0a<别名>` 接口，不关闭物理网卡。
如果接口又被自动拉起，需要调整网络管理器配置。脚本显示
`No EoE interfaces found` 不代表总线已准备好，应结合从站扫描结果判断。

## 4. 启动主站

完成上述检查后，在仓库根目录运行：

```bash
sudo ./build/rocos_igh_master \
  --config ./config/talon_pdo.yaml \
  --dc on --op-timeout-ms 60000 --cpu 3
```

反斜杠 `\` 必须是该行的最后一个字符，后面不要加空格或注释。
也可以写为一行：

```bash
sudo ./build/rocos_igh_master --config ./config/talon_pdo.yaml --dc on --op-timeout-ms 60000 --cpu 3
```

如需将临时关闭 EoE 和启动连接为一次操作，使用以下命令代替上面的启动命令：

```bash
sudo bash scripts/disable-eoe.sh --master-id 0 && \
sudo ./build/rocos_igh_master \
  --config ./config/talon_pdo.yaml \
  --dc on --op-timeout-ms 60000 --cpu 3
```

两个命令分别使用 `sudo`；关闭步骤失败时，`&&` 会阻止继续启动。

### 参数说明

| 参数 | 本次设置及含义 |
|---|---|
| `sudo` | 提供设备访问、实时调度和内存锁定所需权限；系统资源限制仍可能导致失败 |
| `--config ./config/talon_pdo.yaml` | 必填，加载主站参数、从站 PDO/DC 和可选初始化 SDO 配置；路径相对于当前工作目录 |
| `--dc on` | 启用 Distributed Clocks；YAML 中必须恰有一个从站设置 `dc_ref: true`，其他从站为 false 或省略 |
| `--op-timeout-ms 60000` | OP 启动阶段总预算为 60 秒，包含稳定确认和 PDO 异步读回验证 |
| `--cpu 3` | 将周期线程绑定到逻辑 CPU 3；编号从 0 开始，不是“第 3 个物理核” |
| `--master-id 0` | 本次省略，默认主站 0，对应 `ecm0`、`pd_input0`、`pd_output0` 等 IPC |
| `--period-us 1000` | 本次省略，默认周期 1000 µs，即 1 ms；当前最小支持值为 1000 |
| `--preop-timeout-ms 10000` | 本次省略，默认 PREOP 总预算为 10 秒，包含等待、初始化 SDO、PDO 配置和读回重试 |

两个超时参数分别作用于不同阶段，不是每个从站各自拥有完整超时；值必须为
正整数。IgH 同步 SDO 调用不能被 PREOP 预算直接取消，返回后才检查是否超时。
若日志表明 PREOP 阶段预算不足，应单独调整 `--preop-timeout-ms`；
增加 OP 超时不能修复 PREOP 阶段的 SDO 失败。

需要在 PREOP 配置额外字典时，填写各从站的 `init_commands`，格式见
[PDO 配置规范](pdo-config.md#preop-sdo-初始化命令)。列表默认为空；命令成功仅确认
SDO 下载应答，不要求初始化对象可读。命令失败立即终止启动，不随 PDO 重试重放；
其耗时与 PDO 配置共用 PREOP 总预算，已完成的写入不会自动回滚。

## 5. 确认启动成功

启动过程会等待 PREOP 稳定，先按顺序写入各从站可选的 `init_commands`，
全部成功后逐站配置并读回 PDO，随后进入周期通信，
等待 OP、工作计数器和激活后的 PDO 读回验证通过。

确认日志中出现 CPU 绑定结果：

```text
[Realtime] cyclic thread CPU=3 (affinity verified)
```

业务就绪应以以下提示为准：

```text
[master 0] 全部从站就绪，可以开始工作（全部 RxPDO/TxPDO 分配及映射读回一致，OP、链路及输入/输出工作计数器连续 5 次检查正常）
```

仅看到 `PREOP stable confirmations complete`、主站激活成功或某个从站进入 OP，
都不代表完整启动验证已经通过。启动就绪也不等于驱动器已经完成业务侧使能。

需要检查时，在另一个终端运行以下只读命令：

```bash
sudo ethercat slaves -m 0
sudo ethercat domains -m 0 -v
sudo journalctl -k -b -g EtherCAT -n 100 --no-pager

# 将 1234 替换为实际主站进程 PID
taskset -pc 1234
ps -T -p 1234 -o pid,tid,psr,cls,rtprio,comm
```

主线程应绑定 CPU 3，使用 `SCHED_FIFO`，实时优先级为 80。
日志线程保留原有亲和性；`psr` 仅表示线程最近运行的 CPU。

## 6. 正常停止与查看周期统计

先按业务流程停止设备运动并使设备进入安全状态，再在前台终端按 `Ctrl+C`。
程序处理 SIGINT 后退出周期循环并输出统计、释放资源。SIGTERM 也可用于正常
退出；不要把结束主站进程当作设备急停或受控停机的替代方案。

退出时关注 `final_stats`：

| 字段 | 含义 |
|---|---|
| `cycles` | 统计的周期数 |
| `missed_deadlines` | 已错过并跳过的周期数，不补跑历史周期 |
| `min_us/max_us/avg_us/current_us` | 相邻周期唤醒间隔，单位 µs；目标为 1000 µs |
| `run_rc` | 周期任务返回码，正常停止为 0 |

比较实时性时应保持从站配置、周期和后台负载一致，同时观察最大周期、
错过的周期数和总线健康状态，不只比较平均值。

## 7. 常见启动失败

| 现象 | 排查方向 |
|---|---|
| `unknown option: --cpu` | 正在运行旧二进制；重新编译并确认使用的构建目录 |
| CPU 不在 `allowed CPU mask` 内 | 旧版本限制；重新编译后可直接绑定继承集合之外的隔离核，无需先用 taskset 扩展集合 |
| `sched_setaffinity CPU ... failed` | 查询在线 CPU、cpuset 或服务的 AllowedCPUs 限制，以及亲和性设置权限 |
| `mlockall failed` / `sched_setscheduler failed` | 检查启动权限、锁内存限制和实时调度权限 |
| 请求主站失败 | 检查 IgH 服务、主站 ID、设备权限及是否已有应用占用 |
| SDO `rc=-5`，内核同时出现 EoE/CoE 邮箱错误 | 检查 EoE 是否重新启用；结合内核日志判断，不能仅凭 `-5` 认定原因 |
| PREOP 或 OP 超时 | 按日志所处阶段检查状态、PDO、DC、链路及工作计数器；不要只增加超时 |

更多说明：

- [PDO 配置格式](pdo-config.md)
- [实时 CPU 隔离与 GRUB 配置](realtime-cpu-isolation.md)
- [EtherCAT 终端诊断命令](ethercat-terminal-commands.md)
- [项目 README](../README.md)
