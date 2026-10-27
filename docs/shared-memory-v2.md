# IgH 共享内存 ABI v2 迁移

实现依据：[飞书规范 V2.0](https://rocos-sia.feishu.cn/wiki/DQXzwAy0viTXCJkRAEpcrpsXnsg)
及 [rocos-shm](https://github.com/rocos-sia/rocos-shm) 固定提交
`87c436c49777834964a5f1a38d46b4e0c74d34b2`。公共库包版本为 2.0.0，ABI 为 2，
主站产品版本仍从根目录 `VERSION` 编入二进制；三种版本互不替代。

## 构建和部署

先运行 `git submodule update --init --recursive`，再按 README 构建。主站、无硬件
测试和独立 Web 工具都链接 `rocos::shm`。Web 工具已迁至
[`rocos-shm/tools/shm_web`](https://github.com/rocos-sia/rocos-shm/tree/main/tools/shm_web)，
由 ECM 与 IgH 共用；在独立 `rocos-shm` 工作区构建，本仓库不再保留副本。
上述固定子模块提交早于工具迁移，仍用于提供匹配的 ABI v2 公共头文件。`src/shared_memory_config.hpp` 仅转发到
公共库，不再复制结构或生命周期实现；旧 `EcatConfigMaster`/`EcatConfig` 别名保留。

IPC 名称为 `/ecm_v2_{id}`、`/pd_input_v2_{id}`、`/pd_output_v2_{id}`、
`/sync_v2_{id}_{0..9}`。主站持有 owner flock；只清理自己拥有的对象。总线映射为
80 字节 `ShmHeader` 加 `EcatBus`，公共库校验 magic、精确 ABI、结构尺寸、ready、
PDO 长度及描述符边界。零长度 PDO 映射合法，数据指针为空。

v1 二进制不兼容。升级或任一端异常退出后，停止主站和 App，关闭旧映射，部署
使用同一公共库版本重新构建的程序，再先启动主站、后启动 App。不进行双写、
自动桥接或可写客户端热接管。每个总线仅允许一个可写 App，同一 App 的线程共用
一个 `SharedMemoryConfig` 对象；启动管理负责保证该单实例。监控工具用
`connect(true)`，不写输出、不发命令、不消费周期信号量。

## 发布字段与能力

| 信息 | IgH 适配行为 |
|---|---|
| `ecm_version` / `backend_type` | 完整产品版本 / 2（IGH），ready 前固定 |
| `current_state` | 0（未知）；`al_states` 是集合，不冒充单一主站状态 |
| `state_control_supported` | 0，不消费 `request_state` |
| `is_authorized` | true；IgH 不提供该产品许可检查，不能当作健康位 |
| `timestamp` | `CLOCK_REALTIME` 的 Unix 微秒 |
| `heartbeat_mono_ns` | 周期线程更新的 `CLOCK_MONOTONIC` 纳秒，启动等待期间也更新 |
| `connected_slave_num` / `link_up` | 来自 `ecrt_master_state`，失败清对应有效位 |
| `wc_state` | 两个 domain 都 COMPLETE 为 1，否则 2；查询失败为 0 且无效 |
| 从站身份 | 初始化 `ecrt_master_get_slave` 的 vendor/product/revision/serial，`info_valid=0x0f`、`identity_source=1`，零序列号仍可有效 |
| 固件字符串 | 当前未采集，空字符串且 bit4 无效，不能用 revision 代替 |
| 从站运行状态 | rt_safe `ecrt_slave_config_state` 发布 online/AL；离线时 AL 无效，查询失败时运行有效位清零 |
| `al_status_code` | 当前 rt_safe 状态接口不提供，保持无效 |
| DC | 禁用为 0；启用但没有可靠锁定采样为 4（未知），metric/sample 为无效，不声称已同步 |
| `supported_cmds` | 仅 `bit(CMD_RESET_STATS)` |

所有动态标量通过公共库 `atomicLoad`/`atomicStore` 访问，保证单字段不撕裂。
静态名称、身份和映射在 ready 后不改动。不提供整个总线或 PDO 帧的一致快照。
公共头部 ready 表示 ABI 已可连接，业务 OP/PDO 验证门禁仍由启动流程独立处理。

## 邮箱与实时边界

现有普通优先级报告线程同时充当唯一命令处理线程，约 10 ms 轮询一次。线程创建后
才发布 ready。所有协议校验、命令结果发布和 App 本地事务锁使用公共库实现。

`CMD_RESET_STATS` 在业务周期就绪前返回 `STATE_MISMATCH`。就绪后，命令线程递交
进程内原子请求，周期线程在周期边界清零 min/max/avg/current 和统计样本数，发布
零值后确认，命令线程才能返回 SUCCESS。累计错过周期数保留。旧 `resetCycleTime`
字段不再消费。等待期限结束时返回 `OUTCOME_UNKNOWN`，未确认的请求仍保留并阻止
再次提交；随后周期可完成复位。App 侧本地超时须按公共库协议继续 `pollCommand`
消费原结果，不能覆盖邮箱或自动重发。

本次没有启用业务 SDO_READ/WRITE 或 RESET_FAULT，它们返回 `UNSUPPORTED`。
IgH 激活后阻塞 SDO 与周期线程并发、允许访问状态、设备身份重验及设备对象权限
尚未完成硬件验收，因此不声明相应能力。现有初始化阶段的 PREOP SDO 和启动
阶段的 PDO 异步读回保持原用途，不通过新邮箱调用，也不等同于支持业务 SDO。

周期顺序保持 receive → process → 发布输入/读取输出 → 发布诊断 → queue → send
→ notify。新增路径只有固定大小操作、时钟读取、rt_safe 状态查询及无锁原子操作，
没有邮箱轮询、互斥等待、SDO、分配或文件 I/O。周期超时仍跳过错过的 deadline，
不补跑。启动阶段掉线/WKC 异常按原门禁等待或中止；运行期发布异常并继续原有
通信，不自动重配置、清零输出或执行状态切换。时钟/DC 致命错误仍退出周期任务。
退出时撤销 ready、停止并等待命令线程，再销毁 IPC。

## PDO 限制

ABI v2 按规范拒绝非字节对齐 PDO。YAML 非整字节位宽直接报错；自动发现也不会将
不兼容结果写回文件。IgH 注册出的非零 bit_position 会在激活前失败。共享偏移与
原始 domain 字节偏移一致，不再分配位 PDO 的额外字节槽。

`config/tc7_pdo.yaml` 的第 8 个从站含真实 1-bit GPIO，保留配置内容但当前不能启动。
不能把这些位宽直接改为 8；需设备提供字节映射，或另行设计并批准位级协议。

## 验证范围

两种主项目构建均运行公共库 `rocos_shm_v2` 以及项目 CTest：固定布局、ABI 拒绝、
跨进程命令握手、超时消费、主站隔离、零长度 PDO、互斥所有权、等待线程上限与
重复等待、字节 PDO 边界、统计复位确认、静态身份及诊断有效位。模拟 IgH/时钟
测试继续检查启动输出门禁、异常计数、周期超时和周期内零动态分配。

以下 Web 测试现由 `rocos-shm` 仓库维护：后端测试检查真实模式拒绝输出写入、演示模式边界和 HTTP 行为；前端测试
检查只读交互与按有效位显示从站状态。硬件验收仍需验证实际身份/固件、掉站、
DC 偏差及新诊断调用对 1 ms 周期抖动的影响；离线通过不能替代实机验收。
