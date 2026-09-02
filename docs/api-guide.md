# IgH EtherCAT Master 主要 API 函数调用流程

本文档基于 [include/ecrt.h](../include/ecrt.h) 整理,介绍 IgH EtherCAT Master 应用接口(`ecrt_*`)中最重要的函数:每个函数的功能、使用时机与注意事项,以及一个典型应用的调用流程。

## 1. 重要概念

### 1.1 主站阶段(phase)

`ec_master` 有三个阶段(`ec_master_phase_t`):

| 阶段 | 含义 |
|------|------|
| `EC_ORPHANED` | 没有以太网设备接入。 |
| `EC_IDLE` | 设备已接入,但主站尚未被激活。**主站自身**负责周期收发帧、扫描总线、应用配置。 |
| `EC_OPERATION` | 应用已调用 `ecrt_master_activate()`,**应用**接管了收发帧的责任。配置被冻结。 |

### 1.2 调用上下文与实时安全性

每个 API 函数都标注了两类约束 `\apiusage{phase, context}`:

- **phase**:`master_idle`(激活前可调用)、`master_op`(激活后调用)、`master_any`(任一阶段均可)。
- **context**:
  - `blocking` —— 会在内核中睡眠,只能在 Linux 进程上下文调用(用户空间或普通内核线程)。
  - `rt_safe` —— 不分配内存、不阻塞,可在实时上下文调用(Xenomai/RTAI 任务、内核 softirq/atomic 上下文)。

**规则**:所有配置类函数都是 `master_idle,blocking`,只能在 `ecrt_master_activate()` **之前**、进程上下文中调用。激活后,只能调用 `master_op` / `rt_safe` 的运行期函数,且不可再改变从站配置。

## 2. 典型调用流程概览

```
┌─────────────────────────────────────────────────────────────────────────┐
│  配置阶段 (master_idle, blocking, 进程上下文)                              │
│                                                                         │
│  1. ecrt_request_master()   请求主站                                       │
│  2. ecrt_master_create_domain()  创建过程数据域                           │
│  3. ecrt_master_slave_config()   获取从站配置对象                          │
│  4. ecrt_slave_config_pdos()     配置 PDO/同步管理器映射                    │
│  5a. ecrt_domain_reg_pdo_entry_list()  批量注册 PDO 条目                │
│  5b. ecrt_slave_config_sdo*() / ecrt_slave_config_*  其它配置(可选)       │
│  6. ecrt_master_activate()   结束配置、进入运行阶段                         │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  运行阶段 (master_op, rt_safe, 实时/周期任务)                             │
│                                                                         │
│  循环:                                                                    │
│    ecrt_master_receive()     收帧并处理返回数据                            │
│    ecrt_domain_process()     处理域数据(检查工作计数器)                    │
│    ecrt_domain_state() / ecrt_master_state()  监控状态(可选)              │
│    读写 ecrt_domain_data() 返回的缓冲区                                    │
│    ecrt_domain_queue()      入队域数据报                                    │
│    ecrt_master_send()       发送所有已入队的数据报                           │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  结束阶段                                                                  │
│  ecrt_release_master()   释放主站(若仍激活,内部先调用 deactivate)          │
└─────────────────────────────────────────────────────────────────────────┘
```

以下按这个流程顺序逐个说明关键函数。

## 3. 配置阶段函数

### 3.1 `ecrt_request_master()` —— 请求主站

```c
ec_master_t *ecrt_request_master(unsigned int master_index);
```

**功能**:请求出编号为 `master_index` 的主站以供独占使用。应用必须第一个调用它才能开始使用 EtherCAT。主站 0 的索引为 0,依次类推;主站数量在加载主站模块时指定。

**用法**:在用户空间,这是 `ecrt_open_master()` + `ecrt_master_reserve()` 的便捷封装。需在非实时、空闲阶段调用。

**参数**:`master_index` 主站索引。

**返回**:成功返回主站指针,失败返回 `NULL`。

**使用时机**:`master_idle,blocking`,应用最开始调用。

---

### 3.2 `ecrt_master_create_domain()` —— 创建域

```c
ec_domain_t *ecrt_master_create_domain(ec_master_t *master);
```

**功能**:创建用于过程数据交换的**域**。域把一组 PDO 条目聚合为连续的逻辑地址空间,并在激活后映射成一段可读写的过程数据缓冲区。至少要有一个域才能交换过程数据。

**用法**:会分配内存,须在激活前、非实时上下文调用。

**参数**:`master` 主站指针。

**返回**:成功返回新域指针,失败返回 `NULL`。

**使用时机**:`master_idle,blocking`。

---

### 3.3 `ecrt_master_slave_config()` —— 获取从站配置对象

```c
ec_slave_config_t *ecrt_master_slave_config(
        ec_master_t *master, uint16_t alias, uint16_t position,
        uint32_t vendor_id, uint32_t product_code);
```

**功能**:为指定的 `(alias, position)` 从站创建(或复用)一个从站配置对象。后续的 PDO、SDO、DC 等配置都将挂到这个对象上。

**地址规则**:
- `alias == 0`:按 `position` 解释为从站在环上的**位置**。
- `alias != 0`:匹配具有该别名(alias)的从站,`position` 为相对该从站起始的环偏移(0 为别名从站本身,1 为它后面的第一个从站)。

**校验**:配置时若找到的从站 vendor_id / product_code 与给定值不匹配,则该从站不会被配置并报错。若不同配置对象指向同一从站,只应用第一个,并发出警告。

**参数**:`master` 主站;`alias`、`position` 从站寻址;`vendor_id`、`product_code` 期望的设备标识(vendor 厂商 ID 与产品码,可查从站文档/ESM)。

**返回**:成功返回从站配置指针,失败返回 `NULL`。

**使用时机**:`master_idle,blocking`。通常对总线上每个需要配置的从站各调用一次。

---

### 3.4 `ecrt_slave_config_pdos()` —— 配置 PDO 与同步管理器

```c
int ecrt_slave_config_pdos(
        ec_slave_config_t *sc,
        unsigned int n_syncs,
        const ec_sync_info_t syncs[]);
```

**功能**:一次性指定完整的 PDO 配置,是 `ecrt_slave_config_sync_manager()`、`ecrt_slave_config_pdo_assign_*()`、`ecrt_slave_config_pdo_mapping_*()` 的便捷封装。主站据此可预分配完整的过程数据——即使从站配置时不在线。

**参数**:`sc` 从站配置;`n_syncs` sync 配置项个数(用 `EC_END` 表示 `0xff` 终止);`syncs` 同步管理器配置数组。

**典型示例**(来自 [examples/user/main.c](../examples/user/main.c)):

```c
// 每个 PDO 内的条目
ec_pdo_entry_info_t el3102_pdo_entries[] = {
    {0x3101, 1,  8},   // 通道1 状态
    {0x3101, 2, 16},   // 通道1 数值
};

// 每个 PDO
ec_pdo_info_t el3102_pdos[] = {
    {0x1A00, 2, el3102_pdo_entries},
};

// 同步管理器(末尾用 0xff 结束)
ec_sync_info_t el3102_syncs[] = {
    {2, EC_DIR_OUTPUT},
    {3, EC_DIR_INPUT, 2, el3102_pdos},
    {0xff}
};

if (ecrt_slave_config_pdos(sc_ana_in, EC_END, el3102_syncs)) {
    // 处理错误
}
```

**终止规则**:处理 `syncs` 时,只要处理项数达到 `n_syncs`,或遇到 `index == 0xff`,即停止。推荐用 `EC_END` 作为 `n_syncs`(表示不限次数直至 `0xff`)。

**返回**:成功 0,失败非零。

**使用时机**:`master_idle,blocking`。注意 PDO 映射区(`0x1600-0x17FF`、`0x1A00-0x1BFF`)和 PDO 分配区(`0x1C10-0x1C2F`)应通过此函数或 `ecrt_slave_config_pdo_*()` 配置,而不是用 `ecrt_slave_config_sdo()`。

---

### 3.5 `ecrt_domain_reg_pdo_entry_list()` —— 批量注册 PDO 条目

```c
int ecrt_domain_reg_pdo_entry_list(
        ec_domain_t *domain,
        const ec_pdo_entry_reg_t *pdo_entry_regs);
```

**功能**:把一组 PDO 条目登记进一个域,从而为它们分配过程数据缓冲区偏移。激活时主站会据此建立 FMMU 和同步管理器映射。

**参数**:`domain` 域;`pdo_entry_regs` 以空结构(或 `index` 为 0 的结构)结尾的注册数组。

**典型示例**:

```c
static const ec_pdo_entry_reg_t domain1_regs[] = {
    {AnaInSlavePos,  Beckhoff_EL3102, 0x3101, 1, &off_ana_in_status},
    {AnaInSlavePos,  Beckhoff_EL3102, 0x3101, 2, &off_ana_in_value},
    {AnaOutSlavePos, Beckhoff_EL4102, 0x3001, 1, &off_ana_out},
    {DigOutSlavePos, Beckhoff_EL2032, 0x3001, 1, &off_dig_out},
    {}   // 终止项
};

ecrt_domain_reg_pdo_entry_list(domain1, domain1_regs);
```

注册后,各条目的偏移量会写入对应的 `unsigned int *` 指针(`off_ana_in_status` 等),用于在运行期定位过程数据。

**返回**:成功 0,失败非零。

**使用时机**:`master_idle,blocking`。也可用 `ecrt_slave_config_reg_pdo_entry()`(按 index/subindex)或 `ecrt_slave_config_reg_pdo_entry_pos()`(按位置)逐个注册。

---

### 3.6 其他可选配置(均在激活前调用)

| 函数 | 功能 |
|------|------|
| `ecrt_slave_config_sdo(index, subindex, data, size)` | 添加一个 SDO 配置,激活时(及之后从站掉电重配置时)下载到从站。不做大小端转换。 |
| `ecrt_slave_config_sdo8/16/32(...)` | 同上,但自动处理大小端。 |
| `ecrt_slave_config_complete_sdo(...)` | 添加一个 complete access 的 SDO 配置。 |
| `ecrt_slave_config_dc(assign, cyc0, shift0, cyc1, shift1)` | 配置分布式时钟(AssignActivate 字、SYNC0/1 周期与偏移)。 |
| `ecrt_slave_config_watchdog(...)` | 配置从站看门狗。 |
| `ecrt_slave_config_state_timeout(from, to, ms)` | 设置从站应用层状态转换超时(如 PREOP→SAFEOP)。 |
| `ecrt_slave_config_idn(drive_no, idn, state, data, size)` | 添加一个 SoE IDN 配置。 |
| `ecrt_slave_config_flag(key, value)` | 添加从站特性标志(如 `AssignToPdi`、`WaitBeforeSAFEOPms`)。 |
| `ecrt_slave_config_create_sdo_request(...)` | 创建 SDO 请求,用于运行期异步读写 SDO。 |
| `ecrt_slave_config_create_voe_handler(...)` | 创建 VoE 处理器,用于厂商自定义 mailbox 协议。 |
| `ecrt_master_select_reference_clock(sc)` | 选择分布式时钟参考时钟(默认选第一个支持 DC 的从站)。 |
| `ecrt_master_application_time(t)` | 设置应用时间,供 DC 同步使用(虽标 `master_op`,但需在运行期周期调用)。 |

---

### 3.7 `ecrt_master_activate()` —— 激活主站,进入运行阶段

```c
int ecrt_master_activate(ec_master_t *master);
```

**功能**:标志配置阶段结束、实时运行开始。内部为域分配内存、计算逻辑 FMMU 地址,并通知主站状态机把配置应用到网络。

**关键注意**:
- **激活后**,实时应用**自己**负责周期调用 `ecrt_master_send()` 和 `ecrt_master_receive()` 来维持网络通信。激活前这两个函数**不可调用**(由主站自身线程负责)。
- 激活会分配内存,不应在实时上下文调用。
- 激活后**不可再改变从站配置**。

**返回**:成功 0,失败 < 0。

**使用时机**:`master_idle,blocking`。

---

## 4. 运行阶段函数(在周期任务中调用)

激活后,应用进入实时周期循环。以 1 ms 周期为例(见 [examples/user/main.c](../examples/user/main.c) 的 `cyclic_task()`):

### 4.1 `ecrt_master_receive()` —— 收帧并处理数据报

```c
int ecrt_master_receive(ec_master_t *master);
```

**功能**:查询网络设备是否收到帧(调用中断服务例程),提取其中的数据报,把结果分发给队列中的数据报对象。超时的数据报会被标记并出队。必须在激活后由应用**周期调用**。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.2 `ecrt_domain_process()` —— 处理域数据

```c
int ecrt_domain_process(ec_domain_t *domain);
```

**功能**:评估域中各数据报的工作计数器,并在必要时输出统计信息。必须在调用 `ecrt_master_receive()`(期望收到域数据报)之后再调用,这样 `ecrt_domain_state()` 才返回上一次过程数据交换的结果。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.3 `ecrt_domain_data()` —— 获取域的进程数据基址

```c
uint8_t *ecrt_domain_data(const ec_domain_t *domain);
```

**功能**:返回域的过程数据内存基址。用户空间需在 `ecrt_master_activate()` 之后调用;内核态若调用了 `ecrt_domain_external_memory()`,则返回外部内存地址。

**上下文**:`master_op,rt_safe`。

**返回**:过程数据内存指针。

**用法**:结合注册时得到的偏移量读写数据:

```c
uint8_t *domain1_pd = ecrt_domain_data(domain1);

// 读 16 位:EC_READ_U16(base + offset)
uint16_t v = EC_READ_U16(domain1_pd + off_ana_in_value);
// 写 8 位:EC_WRITE_U8(base + offset, value)
EC_WRITE_U8(domain1_pd + off_dig_out, 0x06);
```

---

### 4.4 `ecrt_domain_state()` —— 读取域状态(监测过程数据交换)

```c
int ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state);
```

**功能**:把域的当前状态写入 `state`。可用于实时监测过程数据交换是否正常(检查 `working_counter` 与 `wc_state`)。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.5 `ecrt_master_state()` —— 读取主站状态

```c
int ecrt_master_state(const ec_master_t *master, ec_master_state_t *state);
```

**功能**:把主站全局状态写入 `state`(响应从站数量、各从站 AL 状态、链路是否 up 等)。若需冗余链路中每个链路的具体状态,用 `ecrt_master_link_state()`。

**上下文**:`master_any,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.6 `ecrt_slave_config_state()` —— 读取从站配置状态

```c
int ecrt_slave_config_state(const ec_slave_config_t *sc,
                            ec_slave_config_state_t *state);
```

**功能**:写入该从站配置的状态(`al_state`、`online`、`operational` 等)。该状态由主站状态机更新,可能需要几个周期才变化。若要在实时中监测过程数据交换,应优先用 `ecrt_domain_state()`。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.7 `ecrt_domain_queue()` —— 入队域数据报

```c
int ecrt_domain_queue(ec_domain_t *domain);
```

**功能**:把域的数据报(重新)放入主站的数据报队列,以便在下一个 `ecrt_master_send()` 时发送。每次周期任务都应调用(在写完之后、发送之前)。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

### 4.8 `ecrt_master_send()` —— 发送所有已入队的数据报

```c
int ecrt_master_send(ec_master_t *master);
```

**功能**:把队列中所有已排队的数据报打包成帧,交给以太网设备发送。必须在激活后由应用**周期调用**(与 `ecrt_master_receive()` 配对)。

**上下文**:`master_op,rt_safe`。

**返回**:成功 0,否则负错误码。

---

## 5. 运行期异步请求(SDO / SoE / VoE / 寄存器)

除了过程数据,运行期还可用异步请求对象与从站通信(这些也在 `master_op,rt_safe` 上下文调用)。

### 5.1 SDO 请求

创建(激活前,`master_idle,blocking`):

```c
ec_sdo_request_t *ecrt_slave_config_create_sdo_request(
        ec_slave_config_t *sc, uint16_t index, uint8_t subindex, size_t size);
```

运行期使用(激活后):

```c
// 写入:先把数据填入内部缓冲,再调度写
EC_WRITE_U16(ecrt_sdo_request_data(req), 0x1234);
ecrt_sdo_request_write(req);

// 读取:调度读,然后轮询状态
ecrt_sdo_request_read(req);
// ... 每个周期检查 ...
ec_request_state_t st = ecrt_sdo_request_state(req);
if (st == EC_REQUEST_SUCCESS) {
    uint16_t v = EC_READ_U16(ecrt_sdo_request_data(req));
} else if (st == EC_REQUEST_ERROR) {
    // 出错
}
// 仍为 EC_REQUEST_BUSY 时继续等待
```

| 函数 | 功能 |
|------|------|
| `ecrt_sdo_request_index(idx, sub)` | 修改 SDO index/subindex(初始化时已由 create 设置)。 |
| `ecrt_sdo_request_timeout(ms)` | 设置超时(超时后标记失败;0 表示不超时)。 |
| `ecrt_sdo_request_data()` | 返回内部数据缓冲指针,用于读前/写后处理。 |
| `ecrt_sdo_request_data_size()` | 返回当前数据大小。 |
| `ecrt_sdo_request_state()` | 返回请求状态(`EC_REQUEST_BUSY`/`SUCCESS`/`ERROR`/`UNUSED`)。 |
| `ecrt_sdo_request_write()` | 调度一次 SDO 写。 |
| `ecrt_sdo_request_read()` | 调度一次 SDO 读。 |

> 注意:`write()`/`read()` 不能在 `state == EC_REQUEST_BUSY` 时调用。`read()` 进行期间 `ecrt_sdo_request_data()` 的返回值可能因内部重分配而失效。

### 5.2 SoE 请求

Sercos-over-EtherCAT 请求,针对驱动型从站:

```c
ec_soe_request_t *ecrt_slave_config_create_soe_request(
        ec_slave_config_t *sc, uint8_t drive_no, uint16_t idn, size_t size);
```

运行期:用 `ecrt_soe_request_idn()`、`ecrt_soe_request_timeout()`、`ecrt_soe_request_write()`、`ecrt_soe_request_read()`、`ecrt_soe_request_state()`、`ecrt_soe_request_data()` / `ecrt_soe_request_data_size()`,用法与 SDO 请求类似(对应 `EC_HAVE_SOE_REQUESTS`)。

### 5.3 VoE 处理器

厂商自定义(Device-specific)邮箱协议:

```c
ec_voe_handler_t *ecrt_slave_config_create_voe_handler(
        ec_slave_config_t *sc, size_t size);
```

运行期流程:`ecrt_voe_handler_send_header()` → `ecrt_voe_handler_write()` → `ecrt_voe_handler_execute()` 排队;`ecrt_voe_handler_received_header()` → `ecrt_voe_handler_read()` 读取响应。状态用 `ecrt_voe_handler_state()` / `ecrt_voe_handler_execute()` 的返回值判断。

### 5.4 寄存器请求

直接访问处于 EtherCAT 从站的寄存器(用于调试/监控,不用于接管主站功能):

```c
ec_reg_request_t *ecrt_slave_config_create_reg_request(
        ec_slave_config_t *sc, size_t size);
```

运行期:`ecrt_reg_request_write()`、`ecrt_reg_request_read()`、`ecrt_reg_request_state()`、`ecrt_reg_request_data()`(对应 `EC_HAVE_REG_ACCESS`)。

---

## 6. 结束阶段

### `ecrt_release_master()` —— 释放主站

```c
void ecrt_release_master(ec_master_t *master);
```

**功能**:释放主站,使其他应用可用。会释放所有创建的数据结构。若主站仍处于激活状态,内部会先调用 `ecrt_master_deactivate()`。不应在实时上下文调用。

**上下文**:`master_any,blocking`。

> 若只需暂时解除激活、保留后续重新配置,可显式调用 `ecrt_master_deactivate()`(它释放域、从站配置等对象,指针因此失效),之后再 `ecrt_master_activate()` 重新激活。

---

## 7. 主站级 SDO / SoE 直接传输(配置后)

激活后可对任意从站直接发起 SDO / SoE 传输:

```c
// 下载 SDO(blocking)
int ecrt_master_sdo_download(master, slave_pos, index, subindex,
                             data, size, abort_code);

// 上传 SDO
int ecrt_master_sdo_upload(master, slave_pos, index, subindex,
                           target, target_size, size, abort_code);
```

这些在 `master_any` 阶段可用,常用于运行期按需通信(比 SDO 请求对象更直接,但通常非实时安全)。

---

## 8. 分布式时钟(DC)相关

若使用分布式时钟同步:

```c
// 激活前:选择参考时钟、配置各从站 DC(见 3.6)
ecrt_master_select_reference_clock(dc_ref_sc);

// 运行期,每个周期顺序调用:
ecrt_master_application_time(master, app_time);   // 设置应用时间
ecrt_master_sync_reference_clock(master);         // 把参考时钟同步到应用时间
ecrt_master_sync_slave_clocks(master);            // 补偿所有从站时钟漂移
ecrt_master_reference_clock_time(master, &time);   // 读取参考时钟低 32 位
```

- `ecrt_master_application_time()` 需在每个实时周期、固定时刻调用,用于计算从站 SYNC0/1 中断相位。
- `ecrt_master_sync_monitor_queue()` / `ecrt_master_sync_monitor_process()` 可监测 DC 同步精度(所有从站时钟差的上界估计)。
- `ecrt_master_set_send_interval()` 在激活前设置两次 `ecrt_master_send()` 的间隔,帮助主站决定可追加到帧中的数据量。

---

## 9. 函数速查表

### 主站级

| 函数 | 阶段 | 上下文 | 功能 |
|------|------|--------|------|
| `ecrt_request_master` | idle | blocking | 请求主站(首个调用) |
| `ecrt_open_master` | idle | blocking | 用户空间打开主站 |
| `ecrt_master_reserve` | idle | blocking | 保留主站供独占使用 |
| `ecrt_master_create_domain` | idle | blocking | 创建域 |
| `ecrt_master_slave_config` | idle | blocking | 获取从站配置对象 |
| `ecrt_master_select_reference_clock` | idle | blocking | 选择 DC 参考从站 |
| `ecrt_master_activate` | idle | blocking | 结束配置,进入运行阶段 |
| `ecrt_master_deactivate` | op | blocking | 解除激活,释放配置对象 |
| `ecrt_master_set_send_interval` | idle | blocking | 设置发送间隔 |
| `ecrt_master_send` | op | rt_safe | 发送所有已入队数据报 |
| `ecrt_master_receive` | op | rt_safe | 收帧并处理数据报 |
| `ecrt_master_state` | any | rt_safe | 读取主站状态 |
| `ecrt_master_link_state` | any | rt_safe | 读取冗余链路状态 |
| `ecrt_master_application_time` | op | rt_safe | 设置应用时间(DC) |
| `ecrt_master_sync_reference_clock` | op | rt_safe | 同步参考时钟 |
| `ecrt_master_sync_slave_clocks` | op | rt_safe | 补偿从站时钟 |
| `ecrt_master_reference_clock_time` | op | rt_safe | 读参考时钟时间 |
| `ecrt_master_sdo_download/upload` | any | blocking | 直接 SDO 传输 |
| `ecrt_release_master` | any | blocking | 释放主站 |

### 域级

| 函数 | 阶段 | 上下文 | 功能 |
|------|------|--------|------|
| `ecrt_domain_reg_pdo_entry_list` | idle | blocking | 批量注册 PDO 条目 |
| `ecrt_domain_size` | op | rt_safe | 域过程数据大小 |
| `ecrt_domain_data` | op | rt_safe | 域过程数据基址 |
| `ecrt_domain_process` | op | rt_safe | 处理域数据报 |
| `ecrt_domain_queue` | op | rt_safe | 入队域数据报 |
| `ecrt_domain_state` | op | rt_safe | 读取域状态 |

### 从站配置级

| 函数 | 阶段 | 上下文 | 功能 |
|------|------|--------|------|
| `ecrt_slave_config_pdos` | idle | blocking | 配置 PDO/同步管理器 |
| `ecrt_slave_config_sdo` | idle | blocking | 添加 SDO 配置 |
| `ecrt_slave_config_sdo8/16/32` | idle | blocking | 添加定宽 SDO 配置 |
| `ecrt_slave_config_complete_sdo` | idle | blocking | 添加 complete-access SDO 配置 |
| `ecrt_slave_config_dc` | idle | blocking | 配置分布式时钟 |
| `ecrt_slave_config_watchdog` | idle | blocking | 配置看门狗 |
| `ecrt_slave_config_state_timeout` | idle | blocking | 设置状态转换超时 |
| `ecrt_slave_config_idn` | idle | blocking | 添加 SoE IDN 配置 |
| `ecrt_slave_config_flag` | idle | blocking | 添加特性标志 |
| `ecrt_slave_config_state` | op | rt_safe | 读取从站配置状态 |

### 运行期请求对象

| 函数 | 阶段 | 上下文 | 功能 |
|------|------|--------|------|
| `ecrt_slave_config_create_sdo_request` | idle | blocking | 创建 SDO 请求 |
| `ecrt_sdo_request_write/read` | op | rt_safe | 调度 SDO 写/读 |
| `ecrt_sdo_request_state` | op | rt_safe | 查询 SDO 请求状态 |
| `ecrt_slave_config_create_soe_request` | idle | blocking | 创建 SoE 请求 |
| `ecrt_soe_request_write/read` | op | rt_safe | 调度 SoE 写/读 |
| `ecrt_slave_config_create_voe_handler` | idle | blocking | 创建 VoE 处理器 |
| `ecrt_voe_handler_execute/read/write` | op | rt_safe | VoE 读写调度 |
| `ecrt_slave_config_create_reg_request` | idle | blocking | 创建寄存器请求 |
| `ecrt_reg_request_write/read` | op | rt_safe | 调度寄存器读写 |

---

## 10. 参考示例

- [examples/user/main.c](../examples/user/main.c) —— 用户空间实时应用(推荐起点):域创建、从站配置、PDO 注册、激活、周期任务。
- [examples/mini/mini.c](../examples/mini/mini.c) —— 内核模块最小示例,用内核定时器产生周期任务。
- 在线手册详细说明每个函数,见 https://docs.etherlab.org/ethercat/1.6/doxygen/index.html 。
