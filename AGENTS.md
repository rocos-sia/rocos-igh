# ROCOS IgH 开发指引

## 项目定位

- 本仓库实现基于 IgH EtherCAT Master 的 C++ 主站应用，目标周期下限为 1 ms。
- 主站负责周期性 PDO 收发，并通过 POSIX 共享内存和命名信号量向客户端发布数据、接收输出。
- 所有主站相关对象都以同一个非负 `master_id` 隔离；IgH 主站 0/1 分别对应共享内存 `ecm_v2_0`/`ecm_v2_1`。

## 当前状态

- 最小主站骨架已实现：进程入口 [src/main.cpp](src/main.cpp)、IgH 生命周期 [src/ethercat_master.cpp](src/ethercat_master.cpp)、绝对时间周期任务 [src/cyclic_task.cpp](src/cyclic_task.cpp)、YAML PDO 解析 [src/pdo_config.cpp](src/pdo_config.cpp)、IgH 配置转换 [src/slave_config.cpp](src/slave_config.cpp)、命令行解析 [src/runtime_options.cpp](src/runtime_options.cpp)，以及 IPC/共享 ABI [src/shared_memory_config.hpp](src/shared_memory_config.hpp)。
- 构建目标已就绪：`rocos_igh_core`（主站模式下静态库、无硬件模式下接口库）、`rocos_igh_master`（可执行）、`pdo_config_test` 和 `shared_memory_config_test`（CTest）。两种构建模式与命令见 [README.md](README.md)。
- 启动时必须通过 `--config` 加载 YAML PDO 配置；格式见 [docs/pdo-config.md](docs/pdo-config.md)。从站按 `id` 顺序映射物理位置，alias 固定为 0，`vendor_id/product_code` 从扫描到的 SII 信息读取，之后仍以实际值交给 IgH 严格匹配。
- `.vscode` 中的 ROS 2 设置不是项目依赖依据；除非构建文件明确引入，否则不要添加 ROS 依赖。

## 架构约束

- IgH 配置必须在激活前完成；周期线程遵循 `receive -> domain_process -> 读写 PDO -> domain_queue -> send`。API 阶段和实时安全要求见 [docs/api-guide.md](docs/api-guide.md)。
- 激活后的周期路径不得进行动态内存分配、阻塞式 I/O、日志刷屏或重新配置从站。保持运行期调用为 IgH 标记的 `rt_safe` 操作。
- 使用 `SharedMemoryConfig(master_id)` 创建主站侧 IPC；客户端优先使用 `connect()` 检查错误，只读工具使用 `connect(true)`；保留 `getInstance(master_id)` 兼容入口。初始化完毕并创建非实时命令线程后才调用 `publishReady()`。保持每个主站 ID 在 IgH、共享内存、PDO 缓冲区和信号量中的映射一致。
- IPC 命名约定为 `ecm_v2_{id}`、`pd_input_v2_{id}`、`pd_output_v2_{id}` 和 `sync_v2_{id}_{0..9}`。新增 IPC 对象也必须包含主站 ID，禁止跨主站共享可变状态。
- 每个周期先把 EtherCAT 输入发布到 `pd_input_v2_{id}`，再从 `pd_output_v2_{id}` 取得待发送输出；数据就绪后通过现有信号量接口唤醒等待客户端。
- `EcatBus`、`Slave` 和 `PdVar` 的 ABI v2 唯一定义来自固定版本子模块 `3rdparty/rocos-shm`，本地旧头文件仅作转发。不要随意改变字段顺序、类型、数组上限或旧别名 `EcatConfigMaster`/`EcatConfig`；确需变更时必须在公共库统一修改，同步主站与客户端并增加兼容性验证。v2 与 v1 不兼容，禁止双写或热接管。
- 动态标量统一通过 `atomicLoad`/`atomicStore` 访问；邮箱只由唯一非实时线程处理。`is_authorized` 在 IgH 固定为 true；状态集合不赋给单一 `current_state`（使用 0），链路/WKC/从站 AL 状态由诊断字段及有效位表达。
- ABI v2 拒绝非字节对齐 PDO；当前 TC7 GPIO 配置因此不可直接启动。仅支持统计复位邮箱命令，SDO/状态控制尚未验收，不声明能力。
- PDO 访问类型的 `sizeof(T)` 必须与 `PdVar::size` 一致。新增代码先验证从站 ID、变量 ID、偏移和长度，再访问映射内存。
- `EC_SEM_NUM` 同时限制每个主站可分配的等待线程数量。改动线程注册或信号量逻辑时，覆盖上限、重复等待和多主站隔离场景。

## 构建与验证

- 使用现代、目标导向的 CMake，要求 C++17；保持 out-of-source 构建，并通过 [cmake/FindEtherCAT.cmake](cmake/FindEtherCAT.cmake) 查找 IgH 的 `ecrt.h` 和 `ethercat` 库，不要硬编码本机路径。
- 标准验证入口（`ROCOS_IGH_BUILD_MASTER` 控制两种模式）：无硬件模式 `cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF && cmake --build build && ctest --test-dir build --output-on-failure`；主站模式 `cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON && cmake --build build-master && ctest --test-dir build-master --output-on-failure`。
- 不依赖硬件的测试应使用唯一主站 ID 创建共享内存，验证 PDO 读写、信号量通知、多主站隔离和资源清理。
- 需要已加载 IgH 内核模块、实时权限或真实从站的测试必须单独标记为硬件集成测试，不得默认运行。诊断与设备安全注意事项见 [docs/ethercat-terminal-commands.md](docs/ethercat-terminal-commands.md)。

## 修改原则

- 优先小步修改，保持现有公共 API 和共享内存布局兼容；不要在实现 EtherCAT 功能时顺带引入无关框架。
- 对周期代码的修改必须给出周期超时、工作计数器异常或主站掉线时的行为，并验证不会把阻塞操作带入 1 ms 路径。
- 设备状态变更、SDO 写入和主站重扫属于高风险操作；除非用户明确要求且已确认目标设备，不要自动执行。

## 版本与 Git 提交

- 每次准备创建 Git 提交或发布版本时，必须先读取并执行项目技能 [rocos-version](.agents/skills/rocos-version/SKILL.md)。包括文档、配置、测试和构建修改；仅编辑文件时不递增版本，不因本规则自行创建提交。
- 根目录 `VERSION` 是项目版本号的唯一来源；CMake 从中读取完整版本和数字核心。PDO YAML 的 `version: 1` 是配置格式版本，不随项目版本更新。
- 根据兼容性确定 patch/minor/major；根据用户明确指令确定内部测试或正式发布。指令未明确渠道时，先给出兼容性判断与候选版本，再询问用户；已有适用指令时不重复询问。
- 每次新提交必须包含版本更新。内部测试使用 `X.Y.Z-rc.N`；正式 patch/minor/major 发布使用 `X.Y.Z`，提交成功后在该提交创建 `vX.Y.Z` Git tag。具体递增、校验和重试规则见技能。
