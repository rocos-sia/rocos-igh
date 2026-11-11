# ENI 总线配置

主站使用 `--config igh.yaml` 加载 YAML，再通过其中的 `eni:` 读取
`EtherCATConfig/Config` 格式的 ENI XML。`--config` 直接传入 XML 会报错并提示使用 YAML。
解析和校验均在启动期进行，复用现有 `PdoBusConfig`、IgH 配置、PREOP
PDO 写入/读回及 IPC 发布流程，周期线程和共享内存 ABI 无变化。

```bash
./build-master/rocos_igh_master --config config/igh.yaml
# 在配置文件所在目录运行时：
/path/to/rocos_igh_master --config igh.yaml
```

YAML 引用格式如下；相对路径以 YAML 所在目录为基准，与启动目录无关。
`eni` 与原有 `slaves` 互斥。启动参数使用与 rocos-ecm 相同的顶层布局。

```yaml
eni: eni.xml
id: 0
cycle: 1000
cpuidx: null
dc: on
preop_timeout_ms: 10000
op_timeout_ms: 60000
pdo_config_attempts: 5
```

| YAML 顶层字段 | 对应 CLI | 约束及默认行为 |
| --- | --- | --- |
| `eni` | 无；位于 `--config` 指定的 YAML 内 | ENI 路径，与 ECM 一样以配置文件所在目录解析相对路径 |
| `id` | `--master-id` | 非负主站编号，默认 0；同时决定 IgH 和 IPC 主站 ID |
| `cycle` | `--period-us` | μs，至少 1000；省略时使用 ENI 周期，否则默认 1000 |
| `cpuidx` | `--cpu` | Linux 逻辑 CPU 编号；null/省略沿用亲和性，CLI `auto` 可取消 YAML 绑定 |
| `dc` | `--dc` | IgH 专有，on/off；省略时使用 ENI DC 选择 |
| `preop_timeout_ms` | `--preop-timeout-ms` | IgH 专有，正整数，默认 10000 ms |
| `op_timeout_ms` | `--op-timeout-ms` | IgH 专有，正整数，默认 60000 ms |
| `pdo_config_attempts` | `--pdo-config-attempts` | IgH 专有，正整数，默认 5（含首次） |

与 rocos-ecm 统一的字段是 `eni`、`id`、`cycle` 和 `cpuidx`。IgH 的 CPU 亲和性
使用 Linux 逻辑 CPU 编号，不受 ECM SDK 的 32 位 CPU 掩码限制；默认继续沿用进程亲和性，
不采用 ECM 的“缺省 cpuidx=id”策略。没有对应运行能力的 ECM 字段（如 `dcmmode`、
`link`、`instance`、`license`、`state`、`duration` 等）报错，不忽略或模拟其 SDK 语义。
IgH 的 `dc` 控制 DC 启用，不能等同于 ECM 的 DCM 模式选择。

ENI 引用格式无需 `version`；可选填写 `version: 1`。原有内联 `slaves:` PDO 格式仍须
`version: 1`。旧 `master` 块保持兼容，但不能与顶层启动参数混用；旧字段
`master.master_id`、`master.period_us`、`master.cpu` 分别对应新顶层 `id`、`cycle`、`cpuidx`，
IgH 专有字段名称不变。内联 PDO 也可使用这些新的顶层启动参数。

参数优先级为：显式 CLI > YAML 启动参数 > ENI 默认值 > 程序默认值。
ENI `Cyclic/CycleTime` 单位为 μs，缺省时使用 1000 μs；多个 Cyclic 任务不支持。
存在有效 DC 声明时默认启用 DC，可用 `dc: off` 或 `--dc off` 关闭。
启用 DC 时最终运行周期必须等于每个 DC 从站 `CycleTime0`（ns），不自动改写设备时序。
主站编号、CPU、启动超时仍由 YAML 或 CLI 指定。

ENI 已支持与内联 YAML `slaves[].dc: ref/on/off` 对应的从站选择，无需添加新的 YAML 字段：

| ENI 从站声明 | 等效选择 | 主站 DC 开关为 on 时 |
| --- | --- | --- |
| 有效 DC，`ReferenceClock=1` | ref | 配置 DC，并选为参考时钟 |
| 有效 DC，`ReferenceClock=0` | on | 配置 DC |
| 无 `DC` 节点，或 `CycleTime0=0`、`CycleTime1=0`、`ReferenceClock=0` | off | 不配置 DC，保留 PDO 交换 |

有效 DC 需要非零 `CycleTime0`，以及 PS 阶段 `0x0980/0x0981` 启用 SYNC0 的激活写入。
对于不能配置 DC 的传感器，在 ENI 导出工具中关闭该从站 DC，使其输出上述 off 声明。
`config/igh.yaml` 的顶层 `dc: on` 只配置 ENI 中的 ref/on 从站，不会强制开启 off 从站；
`dc: off` 或 `--dc off` 则为全部从站跳过 DC 配置。省略顶层 `dc` 时，
存在有效 DC 声明则默认开启，否则默认关闭。ENI 本身仍须通过解析校验：
存在有效 DC 声明时必须有且仅有一个参考时钟；主站 off 不绕过 ENI 合法性校验。
`eni` 与 `slaves` 互斥，因此不能在 `igh.yaml` 中另加 `slaves[].dc` 覆盖 ENI。

导入规则参考 rocos-ecm 的 `config/eni7.xml`、`config/config.yaml`，以及
ethercat-web-configurator 的 `eni_slaves.py`、`eni_dc.py`、`eni.py`：

| ENI 字段 | IgH 中的用途 |
| --- | --- |
| `Slave/Info/AutoIncAddr` | `position = uint16_t(-AutoIncAddr)`；允许 XML 顺序不同，排序后须从 0 连续且不重复 |
| `Info/Name`、`VendorId`、`ProductCode` | 从站名称与严格身份匹配；ENI 身份非零，不被扫描结果覆盖 |
| `Info/PhysAddr` | 校验站地址非零且唯一；IgH 使用物理位置寻址，由 IgH 自行分配配置地址 |
| `ProcessData/Sm2/Pdo`、`Sm3/Pdo` | 选择活动 PDO 并确定分配顺序；无列表时使用 PDO 的 `Sm` 属性；两者存在时须一致 |
| `RxPdo[@Sm=2]`、`TxPdo[@Sm=3]` | 主站输出/输入；未分配的 ESI PDO 目录项不导入 |
| `Entry/Index`、`SubIndex`、`BitLen`、`Name` | 原有 PDO 描述；索引 0 的填充项保留，无名称时生成名称 |
| `DC/ReferenceClock`、`CycleTime0`、`CycleTime1`、`ShiftTime` | 参考时钟、SYNC0/SYNC1 周期及 SYNC0 偏移；启用时要求唯一参考时钟 |
| `Slave/InitCmds` 的 PS 阶段 DC 激活写入 | 从 `Ado=0x0980` 的两字节或 `0x0981` 的单字节小端 `Data` 提取 `assign_activate`，须启用 SYNC0 |
| `Mailbox/CoE/InitCmds` | 支持 PS 阶段普通标量下载（Ccs=1，1/2/4/8 字节），按原顺序交给现有 PREOP 初始化流程 |

这是将 ENI 声明导入现有 IgH 能力的适配层，不是通用 ENI 执行器：

- 当前支持过程数据 SM2/SM3、单 SyncUnit（Su=0）、字节对齐 PDO。
  多任务、其他过程 SM、非字节对齐条目、超出 ABI 容量等配置报错。
- PDO 映射区 `0x1600..0x1BFF` 和 SM 分配对象 `0x1C12/0x1C13` 的 CoE 下载
  由声明式 PDO 配置和现有校验流程接管；其余应用 SDO 仅支持 PS 普通下载。
  其他转换阶段、CompleteAccess 应用下载或非标量载荷报错。
  不解释 ENI 命令的独立超时/重试设置，使用现有 PREOP 超时和 PDO 重试策略。
- IgH 负责状态机、SM/FMMU 寄存器、DC 初始化和周期帧；ENI 中的原始寄存器命令
  仅用于提取上述 DC 激活值，其余不回放。厂商专用寄存器初始化需求须另行适配。
- 使用 `AutoIncAddr` 的物理遍历顺序和厂商/产品身份匹配；不按 `PreviousPort`
  校验具体分支端口，也不按 RevisionNo/SerialNo 做额外身份约束。
- IgH 自行生成 domain 布局与循环帧；不保留 ENI 的 `Send/Recv/BitStart`、
  `ProcessImage` 绝对偏移或 `Cyclic/Frame` 报文布局。客户端继续读取现有 IPC 元数据。
- 不读取 DTD、不展开外部实体、不联网读取 XML Schema；不声称完成通用 ENI XSD 验证。
  ENI 及引用它的 YAML 均不进行自动 PDO 发现或写回；原有 PDO YAML 的空 entries 行为不变。

仓库的 [单从站 ENI 示例](../config/eni.xml) 裁剪自 rocos-ecm 的七从站示例，
用于演示活动 PDO 选择、填充项和 DC；不能直接代表现场设备。
新增 `eni_config` CTest 覆盖解析、物理顺序、SM 顺序、相对路径、参数覆盖、
DC 周期约束及非法输入。主站构建模式还检查转换后的 IgH PDO 表与预期身份。
这些均为无硬件验证；真实从站的 OP、DC 锁定和 WKC 仍需单独硬件验收。
