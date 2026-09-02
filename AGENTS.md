# ROCOS IgH 开发指引

## 项目定位

- 本仓库实现基于 IgH EtherCAT Master 的 C++ 主站应用，目标周期下限为 1 ms。
- 主站负责周期性 PDO 收发，并通过 POSIX 共享内存和命名信号量向客户端发布数据、接收输出。
- 所有主站相关对象都以同一个非负 `master_id` 隔离；IgH 主站 0/1 分别对应共享内存 `ecm0`/`ecm1`。

## 当前状态

- 当前唯一实现是 [src/shared_memory_config.hpp](src/shared_memory_config.hpp)，包含共享数据结构和主站/客户端 IPC 接口。
- [CMakeLists.txt](CMakeLists.txt)、[cmake/FindEtherCAT.make](cmake/FindEtherCAT.make) 和 [tests](tests) 尚未实现。不要声称已有可用构建目标或测试命令；新增功能时同时补齐相应 CMake 目标和 CTest 测试。
- `.vscode` 中的 ROS 2 设置不是项目依赖依据；除非构建文件明确引入，否则不要添加 ROS 依赖。

## 架构约束

- IgH 配置必须在激活前完成；周期线程遵循 `receive -> domain_process -> 读写 PDO -> domain_queue -> send`。API 阶段和实时安全要求见 [docs/api-guide.md](docs/api-guide.md)。
- 激活后的周期路径不得进行动态内存分配、阻塞式 I/O、日志刷屏或重新配置从站。保持运行期调用为 IgH 标记的 `rt_safe` 操作。
- 使用 `SharedMemoryConfig(master_id)` 创建主站侧 IPC；客户端使用 `SharedMemoryConfig::getInstance(master_id)`。保持每个主站 ID 在 IgH、共享内存、PDO 缓冲区和信号量中的映射一致。
- IPC 命名约定为 `ecm{id}`、`pd_input{id}`、`pd_output{id}` 和 `sync{id}_{0..9}`。新增 IPC 对象也必须包含主站 ID，禁止跨主站共享可变状态。
- 每个周期先把 EtherCAT 输入发布到 `pd_input{id}`，再从 `pd_output{id}` 取得待发送输出；数据就绪后通过现有信号量接口唤醒等待客户端。
- `EcatBus`、`Slave` 和 `PdVar` 是跨进程 ABI。不要随意改变字段顺序、类型、数组上限或旧别名 `EcatConfigMaster`/`EcatConfig`；确需变更时必须同步主站与客户端并增加兼容性验证。
- PDO 访问类型的 `sizeof(T)` 必须与 `PdVar::size` 一致。新增代码先验证从站 ID、变量 ID、偏移和长度，再访问映射内存。
- `EC_SEM_NUM` 同时限制每个主站可分配的等待线程数量。改动线程注册或信号量逻辑时，覆盖上限、重复等待和多主站隔离场景。

## 构建与验证

- 使用现代、目标导向的 CMake，要求 C++17；保持 out-of-source 构建，并通过 CMake 查找 IgH 的 `ecrt.h` 和 `ethercat` 库，不要硬编码本机路径。
- CMake 基础设施完成后，标准验证入口应为 `cmake -S . -B build`、`cmake --build build` 和 `ctest --test-dir build --output-on-failure`。
- 不依赖硬件的测试应使用唯一主站 ID 创建共享内存，验证 PDO 读写、信号量通知、多主站隔离和资源清理。
- 需要已加载 IgH 内核模块、实时权限或真实从站的测试必须单独标记为硬件集成测试，不得默认运行。诊断与设备安全注意事项见 [docs/ethercat-terminal-commands.md](docs/ethercat-terminal-commands.md)。

## 修改原则

- 优先小步修改，保持现有公共 API 和共享内存布局兼容；不要在实现 EtherCAT 功能时顺带引入无关框架。
- 对周期代码的修改必须给出周期超时、工作计数器异常或主站掉线时的行为，并验证不会把阻塞操作带入 1 ms 路径。
- 设备状态变更、SDO 写入和主站重扫属于高风险操作；除非用户明确要求且已确认目标设备，不要自动执行。