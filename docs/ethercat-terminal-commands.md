# IgH EtherCAT Master 终端命令参考

本文基于 IgH EtherCAT Master 1.6.12 `stable-1.6` 分支整理，覆盖
`ethercat` 命令行工具的全部子命令，以及常用的服务、内核模块、权限
和日志诊断命令。不同编译选项可能使部分命令不可用，最终应以本机
`ethercat --help` 输出为准。

## 1. 使用前提

主站服务已启动、内核模块已加载，并且当前用户能够读写字符设备：

```bash
systemctl status ethercat
lsmod | grep '^ec_'
ls -l /dev/EtherCAT*
ethercat master
```

如果普通用户需要访问设备，建议把设备分配给专用用户组，而不是设置为
所有用户可写。以下示例假定用户已经加入 `realtime` 组：

```bash
echo 'KERNEL=="EtherCAT[0-9]*", GROUP="realtime", MODE="0660"' \
    | sudo tee /etc/udev/rules.d/99-EtherCAT.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=EtherCAT
```

重新登录后检查用户组和设备权限：

```bash
id
stat -c '%U %G %a %n' /dev/EtherCAT0
```

> **注意：** `alias`、`crc reset`、`debug`、`download`、`foe_write`、
> `ip`、`reg_write`、`rescan`、`sii_write`、`soe_write` 和 `states`
> 会修改主站或从站状态。执行前应确认从站位置、数据类型和设备手册；
> 生产设备应先停止实时应用并确保机械系统处于安全状态。

## 2. 基本语法与全局选项

```text
ethercat <COMMAND> [OPTIONS] [ARGUMENTS]
```

| 选项 | 含义 |
|---|---|
| `-m, --master <master>` | 选择主站，支持 `0,2`、`1-3`、`-3` 等列表或范围，默认 `-` 表示全部 |
| `-f, --force` | 强制执行命令或确认批量/高风险操作 |
| `-q, --quiet` | 减少输出 |
| `-v, --verbose` | 显示详细输出 |
| `-h, --help` | 显示帮助 |

查看总帮助和子命令帮助：

```bash
ethercat --help
ethercat slaves --help
```

数值可使用十进制、以 `0` 开头的八进制或以 `0x` 开头的十六进制。
子命令可以使用无歧义缩写，但脚本中建议始终写完整名称。

### 2.1 从站选择

许多命令使用以下选项选择从站：

| 选项组合 | 含义 |
|---|---|
| 不指定 `-a` 和 `-p` | 选择全部从站；只允许单从站的命令会报错 |
| 只指定 `-p <pos>` | 按总线绝对位置选择 |
| 只指定 `-a <alias>` | 选择该别名及其后续从站，直到遇到下一个别名 |
| 同时指定 `-a` 和 `-p` | `-p` 表示相对于该别名从站的位置 |

常用示例：

```bash
ethercat slaves -p 0          # 总线上的第 0 个从站
ethercat slaves -a 100 -p 0   # 别名 100 所在的从站
ethercat slaves -a 100 -p 2   # 别名 100 后相对位置 2
```

## 3. 常用场景速查

| 场景 | 命令 |
|---|---|
| 查看主站 | `ethercat master` |
| 列出从站 | `ethercat slaves` |
| 查看指定从站详情 | `ethercat slaves -p 0 -v` |
| 查看 PDO 映射 | `ethercat pdos -p 0` |
| 查看 SDO 字典 | `ethercat sdos -p 0` |
| 读取 SDO | `ethercat upload -p 0 0x1018 1` |
| 写入 SDO | `ethercat download -p 0 -t uint16 0x6040 0 0x0006` |
| 请求从站进入 OP | `ethercat states -p 0 OP` |
| 查看域和工作计数器 | `ethercat domains -v` |
| 查看 CRC 错误 | `ethercat crc` |
| 重新扫描总线 | `ethercat rescan` |
| 查看内核日志 | `journalctl -k -g EtherCAT` |
| 查看服务日志 | `journalctl -u ethercat` |

## 4. 状态与信息命令

### 4.1 `master`：查看主站和网卡信息

```text
ethercat master [OPTIONS]
```

显示主站阶段、链路状态、从站数量、数据报统计和主/备网卡信息。

```bash
ethercat master
ethercat master -m 0
```

### 4.2 `slaves`：列出从站

```text
ethercat slaves [OPTIONS]
```

简要输出包括绝对位置、别名、相对位置、AL 状态、错误标记和设备名。
使用 `-v` 查看厂商 ID、产品码、端口、邮箱和 Distributed Clocks 等详情。

```bash
ethercat slaves
ethercat slaves -p 0 -v
```

### 4.3 `config`：查看应用创建的从站配置

```text
ethercat config [OPTIONS]
```

这里显示的是应用通过 `ecrt_master_slave_config()` 创建的配置，不是单纯
的总线扫描结果。`-v` 会额外显示 PDO、SDO 和 DC 等配置。

```bash
ethercat config
ethercat config -a 0 -p 0 -v
```

### 4.4 `domains`：查看过程数据域

```text
ethercat domains [OPTIONS]
```

显示逻辑基地址、过程数据大小以及当前/期望工作计数器。`-d` 选择域，
`-v` 显示 FMMU、从站配置和过程数据。

```bash
ethercat domains
ethercat domains -d 0 -v
```

工作计数器当前值等于期望值，表示上一周期中该域的 PDO 均成功交换。

### 4.5 `data`：输出域的二进制过程数据

```text
ethercat data [OPTIONS]
```

`-d <index>` 选择域；未指定时会拼接所有域的原始二进制数据。

```bash
ethercat data -d 0 | hexdump -C
ethercat data -d 0 > domain0.bin
```

### 4.6 `version`：查看版本

```bash
ethercat version
```

### 4.7 `graph`：生成总线拓扑图

```text
ethercat graph [DC|CRC]
```

输出 Graphviz DOT。可选参数 `DC` 增加分布式时钟信息，`CRC` 增加链路
错误信息。

```bash
ethercat graph | dot -Tsvg > bus.svg
ethercat graph DC | dot -Tpng > bus-dc.png
```

### 4.8 `crc`：诊断链路 CRC 错误

```text
ethercat crc
ethercat crc reset
```

读取各端口的 CRC、PHY 和转发错误计数。`reset` 会清零错误寄存器，应在
记录原始计数之后执行。

```bash
ethercat crc
ethercat crc reset
```

### 4.9 `eoe`：查看 EoE 统计

```bash
ethercat eoe
```

显示 Ethernet over EtherCAT 的收发统计和速率。只有使用
`--enable-eoe` 编译时，该命令才会注册。

## 5. PDO、SDO 与配置生成

### 5.1 `pdos`：列出 Sync Manager 和 PDO 映射

```text
ethercat pdos [OPTIONS]
```

输出 Sync Manager、已分配 PDO 和 PDO Entry。`-s etherlab` 可输出适合
EtherLab 通用从站块的模板。

```bash
ethercat pdos -p 0
ethercat pdos -p 0 -s etherlab
```

PDO 的 Tx/Rx 方向以从站视角命名：TxPDO 是从站发送给主站的数据。

### 5.2 `cstruct`：生成 C 语言 PDO 配置

```text
ethercat cstruct [OPTIONS]
```

生成可用于 `ecrt_slave_config_pdos()` 的 C 数据结构。

```bash
ethercat cstruct -p 0 > slave0-pdos.c
```

### 5.3 `sdos`：列出 CoE SDO 字典

```text
ethercat sdos [OPTIONS]
```

显示对象、子索引、PREOP/SAFEOP/OP 下的访问权限、数据类型和位宽。
`-q` 只显示对象，不展开子索引。

```bash
ethercat sdos -p 0
ethercat sdos -p 0 -q
```

### 5.4 `upload`：读取 SDO

```text
ethercat upload [OPTIONS] <INDEX> <SUBINDEX>
```

默认从 SDO 字典推断类型。设备不支持 SDO Info 或对象不在字典中时，
必须用 `-t` 指定类型。

```bash
ethercat upload -p 0 0x1018 1
ethercat upload -p 0 -t uint32 0x1018 1
```

### 5.5 `download`：写入 SDO

```text
ethercat download [OPTIONS] <INDEX> <SUBINDEX> <VALUE>
ethercat download [OPTIONS] <INDEX> <VALUE>
```

第二种形式使用 Complete Access。`VALUE` 为 `-` 时从标准输入读取。

```bash
ethercat download -p 0 -t uint16 0x6040 0 0x0006
printf '\x06\x00' | ethercat download -p 0 -t octet_string 0x6040 0 -
```

`upload`、`download`、`reg_read`、`reg_write`、`soe_read` 和
`soe_write` 可用的数据类型包括：

```text
bool
int8 int16 int32 int64
uint8 uint16 uint32 uint64
float double
string octet_string unicode_string
sm8 sm16 sm32 sm64
```

`sm*` 表示符号-数值编码，不是常见的二进制补码。

### 5.6 `xml`：生成从站 XML 信息

```text
ethercat xml [OPTIONS]
```

输出从站信息 XML。可配置 PDO 的输出取决于从站当前配置。

```bash
ethercat xml -p 0 > slave0.xml
```

## 6. 状态、别名与扫描控制

### 6.1 `states`：请求 AL 状态

```text
ethercat states [OPTIONS] <STATE>
```

`STATE` 可取 `INIT`、`PREOP`、`BOOT`、`SAFEOP` 或 `OP`。

```bash
ethercat states -p 0 PREOP
ethercat states OP             # 请求所有从站进入 OP
```

状态请求可能被应用配置、看门狗、通信错误或从站自身错误拒绝。

### 6.2 `alias`：写入从站别名

```text
ethercat alias [OPTIONS] <ALIAS>
```

`ALIAS` 是 16 位无符号数，`0` 表示删除别名。选择多个从站时必须使用
`-f` 明确确认。

```bash
ethercat alias -p 0 100
ethercat alias -p 0 0
```

### 6.3 `rescan`：重新扫描总线

```bash
ethercat rescan
```

主站会丢弃已收集的从站信息并重新读取总线。应用运行期间执行可能影响
配置和过程数据交换。

## 7. ESC 寄存器与 SII EEPROM

### 7.1 `reg_read`：读取 ESC 寄存器

```text
ethercat reg_read [OPTIONS] <ADDRESS> [SIZE]
```

地址和长度均为 16 位无符号数，且地址加长度不能超过 64 KiB。指定
`-t` 后，类型决定读取长度。

```bash
ethercat reg_read -p 0 0x0130 2 | hexdump -C
ethercat reg_read -p 0 -t uint16 0x0130
```

### 7.2 `reg_write`：写入 ESC 寄存器

```text
ethercat reg_write [OPTIONS] <ADDRESS> <DATA>
```

未指定 `-t` 时，`DATA` 是文件路径或 `-`（标准输入）；指定 `-t` 时，
`DATA` 按给定类型解释。`-e` 通过紧急请求发送。

```bash
ethercat reg_write -p 0 -t uint16 0x0120 0x0002
printf '\x02\x00' | ethercat reg_write -p 0 0x0120 -
```

直接写 ESC 寄存器可能破坏状态机、邮箱或同步配置，只应结合 ESC 数据手册
使用。

### 7.3 `sii_read`：读取 SII EEPROM

```text
ethercat sii_read [OPTIONS]
```

默认输出二进制内容，`-v` 输出按 SII 分类解析的文本。

```bash
ethercat sii_read -p 0 > slave0-sii.bin
ethercat sii_read -p 0 -v
```

### 7.4 `sii_write`：写入 SII EEPROM

```text
ethercat sii_write [OPTIONS] <FILENAME>
```

输入必须包含正数个 16 位字。工具会检查有效性和完整性，`-f` 可跳过
检查，但可能使从站无法正常识别。

```bash
ethercat sii_write -p 0 slave0-sii.bin
cat slave0-sii.bin | ethercat sii_write -p 0 -
```

写入前应先用 `sii_read` 备份，并核对厂商 ID、产品码和 EEPROM 容量。

## 8. FoE、SoE 与 EoE

### 8.1 `foe_read`：通过 FoE 读取文件

```text
ethercat foe_read [OPTIONS] <SOURCEFILE>
```

`-o` 指定本地文件；默认输出到标准输出。

```bash
ethercat foe_read -p 0 -o firmware-backup.bin firmware.bin
```

### 8.2 `foe_write`：通过 FoE 写入文件

```text
ethercat foe_write [OPTIONS] <FILENAME>
```

`-o` 指定从站上的目标文件名。输入为 `-` 时，必须同时指定 `-o`。

```bash
ethercat foe_write -p 0 -o firmware.bin firmware-new.bin
cat firmware-new.bin | ethercat foe_write -p 0 -o firmware.bin -
```

固件格式、BOOT 状态要求和升级恢复方式由从站厂商定义。

### 8.3 `soe_read`：读取 SoE IDN

```text
ethercat soe_read [OPTIONS] <IDN>
ethercat soe_read [OPTIONS] <DRIVE> <IDN>
```

驱动器编号范围为 0 到 7，省略时为 0。IDN 可用 16 位数值或
`P-0-150` 格式表示。

```bash
ethercat soe_read -p 0 P-0-150
ethercat soe_read -p 0 -t uint32 1 P-0-150
```

### 8.4 `soe_write`：写入 SoE IDN

```text
ethercat soe_write [OPTIONS] <IDN> <VALUE>
ethercat soe_write [OPTIONS] <DRIVE> <IDN> <VALUE>
```

`-t` 是必选项。

```bash
ethercat soe_write -p 0 -t uint32 P-0-150 1000
```

### 8.5 `ip`：设置 EoE IP 参数

```text
ethercat ip [OPTIONS] <KEY VALUE>...
```

支持 `ip_address`、`mac_address`、`default_gateway`、`dns_address` 和
`hostname`。只有使用 `--enable-eoe` 编译时，该命令才会注册。

```bash
ethercat ip -p 0 ip_address 192.168.10.20/24 \
    default_gateway 192.168.10.1 hostname drive-01
```

## 9. 主站调试命令

### 9.1 `debug`：设置主站调试级别

```text
ethercat debug <LEVEL>
```

| 级别 | 含义 |
|---|---|
| `0` | 关闭调试输出 |
| `1` | 输出部分调试信息 |
| `2` | 输出所有帧内容，数据量很大，谨慎使用 |

```bash
ethercat debug 1
journalctl -k -f
ethercat debug 0
```

调试信息写入内核日志，而不是当前终端。

## 10. 服务与内核模块运维

### 10.1 systemd 服务

```bash
sudo systemctl start ethercat
sudo systemctl stop ethercat
sudo systemctl restart ethercat
systemctl status ethercat
sudo systemctl enable ethercat
sudo systemctl disable ethercat
```

服务内部调用 `ethercatctl` 加载/卸载 `ec_master` 和配置的 EtherCAT
网卡驱动。

### 10.2 `ethercatctl`

```text
ethercatctl [-c path/to/ethercat.conf] {start|stop|restart|status}
```

```bash
sudo ethercatctl start
sudo ethercatctl stop
sudo ethercatctl restart
ethercatctl status
sudo ethercatctl -c /etc/ethercat.conf start
```

主要配置文件通常是 `/etc/ethercat.conf`：

```bash
MASTER0_DEVICE="eth0"
DEVICE_MODULES="generic"
UPDOWN_INTERFACES="eth0"
```

`MASTER0_DEVICE` 也可以填写 MAC 地址。每增加一个连续、非空的
`MASTER<N>_DEVICE` 就会创建一个主站。`DEVICE_MODULES` 指定要加载的
EtherCAT 网卡驱动，例如 `generic`、`igb` 或 `e1000e`。

### 10.3 手动检查模块

一般应通过服务管理模块。以下命令主要用于诊断：

```bash
lsmod | grep '^ec_'
modinfo ec_master
modinfo ec_generic
cat /sys/module/ec_master/parameters/main_devices
```

手动加载示例：

```bash
sudo modprobe ec_master main_devices=00:11:22:33:44:55
sudo modprobe ec_generic
```

手动卸载时必须先停止占用主站的应用，并先卸载网卡模块：

```bash
sudo modprobe -r ec_generic
sudo modprobe -r ec_master
```

原生 EtherCAT 驱动会替代 Linux 标准网卡驱动，直接手动装卸可能影响
普通网络连接，因此生产环境优先使用 `systemctl` 或 `ethercatctl`。

## 11. 日志、设备与故障排查

### 11.1 查看日志

```bash
journalctl -u ethercat -b
journalctl -k -b -g 'EtherCAT|ec_master|ec_'
dmesg -T | grep -i ethercat
```

实时跟踪：

```bash
journalctl -u ethercat -f
journalctl -k -f
```

### 11.2 检查网卡和链路

```bash
ip -br link
ip link show eth0
ethtool eth0
ethercat master
ethercat slaves
```

使用 `ec_generic` 时，网卡必须处于 UP 状态。可在
`/etc/ethercat.conf` 中设置 `UPDOWN_INTERFACES="eth0"`，由
`ethercatctl` 自动管理。

### 11.3 检查字符设备权限

```bash
ls -l /dev/EtherCAT*
stat -c 'owner=%U group=%G mode=%a device=%n' /dev/EtherCAT0
udevadm info --query=all --name=/dev/EtherCAT0
id
```

`MODE="0664"` 只给属主和属组写权限。如果设备仍是 `root:root`，普通
用户只有读权限，调用需要读写访问的 API 时仍会得到
`Permission denied`。规则应同时设置用户所属的 `GROUP`。

### 11.4 主站正常但扫描不到从站

按以下顺序检查：

```bash
systemctl status ethercat
ethercat master
ip -br link
ethercat crc
journalctl -k -b -g EtherCAT
```

重点确认 `/etc/ethercat.conf` 中的网卡名或 MAC 地址正确、对应
EtherCAT 驱动已加载、链路为 UP、网线和从站供电正常，并且该网卡没有
同时被普通网络业务占用。

## 12. 全部 `ethercat` 子命令索引

| 子命令 | 功能 | 类型 |
|---|---|---|
| `alias` | 写入从站别名 | 写操作 |
| `config` | 查看应用创建的从站配置 | 只读 |
| `crc` | 查看或清零链路错误计数 | 读取/写操作 |
| `cstruct` | 生成 C 语言 PDO 配置 | 只读 |
| `data` | 输出域过程数据 | 只读 |
| `debug` | 设置主站调试级别 | 写操作 |
| `domains` | 查看过程数据域 | 只读 |
| `download` | 写入 CoE SDO | 写操作 |
| `eoe` | 查看 EoE 统计 | 只读，可选 |
| `foe_read` | 通过 FoE 读取文件 | 只读 |
| `foe_write` | 通过 FoE 写入文件 | 写操作 |
| `graph` | 生成总线拓扑 DOT | 只读 |
| `ip` | 设置 EoE IP 参数 | 写操作，可选 |
| `master` | 查看主站和网卡信息 | 只读 |
| `pdos` | 查看 Sync Manager 和 PDO | 只读 |
| `reg_read` | 读取 ESC 寄存器 | 只读 |
| `reg_write` | 写入 ESC 寄存器 | 写操作 |
| `rescan` | 重新扫描总线 | 写操作 |
| `sdos` | 查看 CoE SDO 字典 | 只读 |
| `sii_read` | 读取 SII EEPROM | 只读 |
| `sii_write` | 写入 SII EEPROM | 写操作 |
| `slaves` | 查看总线从站 | 只读 |
| `soe_read` | 读取 SoE IDN | 只读 |
| `soe_write` | 写入 SoE IDN | 写操作 |
| `states` | 请求从站 AL 状态 | 写操作 |
| `upload` | 读取 CoE SDO | 只读 |
| `version` | 查看工具版本 | 只读 |
| `xml` | 生成从站 XML 信息 | 只读 |

## 13. 参考入口

```bash
ethercat --help
ethercat <COMMAND> --help
man ethercat
```

源码中的命令注册入口位于 `tool/main.cpp`，每个命令的详细帮助由对应的
`tool/Command*.cpp` 提供；服务控制逻辑位于 `script/ethercatctl`。