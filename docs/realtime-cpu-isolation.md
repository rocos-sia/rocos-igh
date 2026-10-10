# 实时主站 CPU 亲和性与内核启动参数

本文记录 ROCOS IgH 主站的 CPU 选择、GRUB 配置、验证与回滚方法。
以下 CPU 编号基于 2026-10-10 对本机的只读检查；更换机器、调整 BIOS
超线程设置或升级内核后，需要重新确认。本文中的配置尚未应用到系统。

## 1. 本机情况与推荐分配

| 项目 | 检查结果 |
|---|---|
| CPU | Intel Core i7-7600U |
| 拓扑 | 1 个插槽，2 个物理核，4 个逻辑 CPU |
| 内核 | `5.15.0-1114-realtime` |
| 内核配置 | `CONFIG_PREEMPT_RT=y`、`CONFIG_NO_HZ_FULL=y`、`CONFIG_RCU_NOCB_CPU=y` |
| 空闲驱动 | `intel_idle` |
| 调频驱动 | `intel_pstate` |
| 调频策略 | `powersave`；EPP 已为 `performance` |
| GRUB 默认参数 | `quiet splash` |
| CPU 隔离 | 尚未配置 |

推荐先按完整物理核分配：

| 物理核 | 逻辑 CPU | 用途 |
|---|---|---|
| Core 0 | `0,2` | 系统服务、桌面、普通中断等后台工作 |
| Core 1 | `1,3` | 隔离；主站周期线程使用 CPU 3，CPU 1 尽量空闲 |

CPU 1 和 CPU 3 是超线程兄弟，共享物理核执行资源。只隔离 CPU 3，
同时让普通任务占用 CPU 1，仍会引入竞争。也不建议将两个繁忙的实时主站
分别放到 1 和 3，并把它们视为两个独立物理核。

这套分配会将普通工作集中到一个物理核，桌面和编译等负载的吞吐量可能下降。
CPU 隔离和线程绑定都不等于完全消除中断、固件或共享硬件资源造成的延迟。

## 2. 查询 CPU 拓扑与当前设置

```bash
# 相同 SOCKET、CORE 的逻辑 CPU 属于同一个物理核
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE

# 本机预期输出 1,3
cat /sys/devices/system/cpu/cpu3/topology/thread_siblings_list

uname -r
cat /proc/cmdline
cat /sys/devices/system/cpu/online
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full

cat /sys/devices/system/cpu/cpuidle/current_driver
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_driver
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
cat /sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference
```

不同内核或调频驱动可能不提供上述部分 sysfs 文件。
若关闭 BIOS 超线程或使用 `nosmt`，必须重新查询在线 CPU 和拓扑，
不能直接沿用本文的 `1,3` 或 `--cpu 3`。

## 3. 建议的 GRUB 起始配置

在 `/etc/default/grub` 中保留原有必需参数，追加下面的实时配置。
本机原值为 `quiet splash`，对应的完整示例为一行：

```bash
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=domain,managed_irq,1,3 irqaffinity=0,2 rcu_nocbs=1,3 intel_idle.max_cstate=1 processor.max_cstate=1 cpufreq.default_governor=performance"
```

| 参数 | 目的与限制 |
|---|---|
| `isolcpus=domain,managed_irq,1,3` | 将 1、3 移出普通调度负载均衡域，并尽可能避开 managed IRQ；不会阻止显式绑定到这些 CPU 的线程 |
| `irqaffinity=0,2` | 设置普通 IRQ 的默认 CPU 亲和性；驱动或 irqbalance 仍可能改变它 |
| `rcu_nocbs=1,3` | 将这些 CPU 的 RCU 回调卸载到线程；仍需核对相关后台线程实际运行位置 |
| `intel_idle.max_cstate=1` | 限制 Intel idle 驱动使用的深度空闲状态，减少退出休眠延迟 |
| `processor.max_cstate=1` | 使用 ACPI idle 驱动时的对应限制 |
| `cpufreq.default_governor=performance` | 请求默认性能策略；启动后的电源管理服务可能覆盖它 |

`isolcpus=domain` 形成的调度域隔离不能在本次启动中直接撤销，需要修改参数
并重启。需要运行时动态调整隔离集合时，应考虑 cpuset。
`managed_irq` 是尽力隔离，不保证每个设备中断都能迁移。

### 节能和调频参数的区别

- `cpufreq.off=1` 关闭调频子系统，不等于固定最高频率；本方案保留
  `intel_pstate` 并请求 `performance`。
- `performance` 仍受温度、功耗和硬件限制，不代表频率绝对恒定。
  `intel_pstate` 的 `powersave` 也不等于一直运行在最低频率。
- `cpuidle.off=1` 禁用 CPUIdle 子系统，不应直接理解为禁止所有硬件空闲状态。
  本方案先限制深度 C-state。
- `intel_idle.max_cstate=0` 会禁用 Intel idle 驱动，可能转用 ACPI idle，
  不表示完全禁止休眠。
- `idle=poll` 会让空闲 CPU 忙轮询，增加功耗和发热；不作为首轮配置。
  持续高温引起的降频也可能影响实时性。

### 可选的第二轮测试：nohz_full

本机内核支持，可在完成基线测量后追加：

```text
nohz_full=1,3
```

它尝试在满足条件时抑制指定 CPU 的周期调度 tick，不会消除所有定时器和
中断。主站每 1 ms 休眠、唤醒并进入内核，收益需要实测，不保证一定更好。
不要把所有在线 CPU 都划入该集合，应保留后台工作 CPU。

## 4. 应用和回滚

在主站停止、设备处于允许停机的状态下操作。先备份；以下备份路径若已存在，
应另选名称，保留最初的可用配置。

```bash
sudo cp -a /etc/default/grub /etc/default/grub.before-rocos-rt
sudoedit /etc/default/grub
sudo update-grub
```

确认 `update-grub` 成功后，在停机窗口重启。不要直接编辑自动生成的
`/boot/grub/grub.cfg`。首次测试应保留本地控制台或其他可恢复启动配置的手段。

正常回滚：

```bash
sudo cp -a /etc/default/grub.before-rocos-rt /etc/default/grub
sudo update-grub
```

随后在停机窗口再次重启。若新配置导致无法正常进入系统，可在 GRUB 菜单
按 `e`，从本次启动的 `linux` 行临时删除新增参数，按界面提示启动；
进入系统后再恢复配置并运行 `update-grub`。

## 5. 重启后验证隔离、调频和中断

```bash
cat /proc/cmdline
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
cat /proc/interrupts
systemctl is-active irqbalance
```

预期隔离列表为 `1,3`，调频策略为 `performance`。若仍为 `powersave`，
应检查电源管理服务是否覆盖了启动策略。若启用了 `nohz_full`，还应检查
`/sys/devices/system/cpu/nohz_full`。

检查时本机 i915 显卡和声卡 IRQ 的有效目标包含 CPU 3，因此不能仅凭
`isolcpus` 已生效就认定 CPU 3 已无普通中断。按设备名称找到当前 IRQ 后读取：

```bash
# 用 /proc/interrupts 中当前查到的编号替换 144
cat /proc/irq/144/smp_affinity_list
cat /proc/irq/144/effective_affinity_list
```

IRQ 编号可能随重启变化；应比较运行一段时间前后的中断计数增量。
若 irqbalance 正在运行，应按本机版本支持的配置方式排除 CPU 1、3，
并核对实际分布。手动调整普通 IRQ 后，也要防止 irqbalance 再次改变它。
managed IRQ 不一定允许通过 `/proc/irq` 手动设置。

EtherCAT 网卡的中断策略需要结合 IgH 网卡驱动和实测决定。不要在未确认
设备归属时批量迁移所有 IRQ，也不要仅凭历史 `enp*` 名称推断当前 EtherCAT
网卡的处理路径。若接收依赖 IRQ 线程，还应核对其 CPU 和实时优先级，
避免周期线程阻碍必需的接收处理。

## 6. 启动主站与测量效果

使用已支持 `--cpu` 的主站程序：

```bash
sudo ./build/rocos_igh_master \
  --config ./config/talon_pdo.yaml \
  --dc on --op-timeout-ms 60000 --cpu 3
```

程序在访问 EtherCAT 设备前通过临时线程试绑 CPU 3 并读回验证，
保留主线程的原有亲和性；进入周期循环前再绑定主线程并读回验证。
即使启动时继承的亲和性为 `0,2`，也可以直接用 `--cpu 3` 绑定隔离核，
无需先执行 `taskset`。显式绑定可以改变 taskset 设置的亲和性，
但不能突破 cpuset 限制；目标 CPU 必须在线且内核允许绑定。

`--cpu` 仅绑定实际运行周期循环的主线程，日志线程保留原有亲和性；
不会自动调整 IRQ、IgH 内核线程或其他进程。省略参数时保留原亲和性。
启动日志会输出 `cyclic thread CPU=3 (affinity verified)`。

```bash
# 用实际主站 PID 替换 1234；主线程 TID 与 PID 相同
taskset -pc 1234
ps -T -p 1234 -o pid,tid,psr,cls,rtprio,comm
```

`taskset` 检查允许集合，`psr` 只是线程最近运行的 CPU。
周期线程应使用 `SCHED_FIFO`，实时优先级为 80。

在相同周期、从站配置和后台负载下，比较修改前后的退出统计：

- `min_us/max_us/avg_us`：相邻周期唤醒间隔，目标周期 1 ms 时关注偏离 1000 µs 的程度。
- `missed_deadlines`：错过并跳过的周期数。
- 同时观察工作计数器、链路状态和设备运行表现，不能只看平均周期。

先做隔离和功耗策略的对照，再单独增加 `nohz_full` 等参数。
每组测试应持续到温度稳定，并覆盖实际后台负载；一次短测试不能证明最坏延迟上界。
这些配置不会改变主站既有的超时跳周期、总线异常检测和 PDO 处理逻辑。

## 7. 参考资料

- [Linux 5.15 内核启动参数](https://www.kernel.org/doc/html/v5.15/admin-guide/kernel-parameters.html)
- [Linux CPU 隔离说明](https://docs.kernel.org/admin-guide/cpu-isolation.html)（新版本文档，具体参数以本机内核为准）
- [Linux 5.15 Intel P-state 驱动](https://www.kernel.org/doc/html/v5.15/admin-guide/pm/intel_pstate.html)
- [CPU Idle 管理](https://docs.kernel.org/admin-guide/pm/cpuidle.html)
- [IRQ 亲和性](https://docs.kernel.org/core-api/irq/irq-affinity.html)
- [项目启动说明](../README.md)
- [EtherCAT 终端诊断命令](ethercat-terminal-commands.md)
