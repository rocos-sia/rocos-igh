// =============================================================================
// main.cpp —— rocos_igh_master 主站进程入口
// -----------------------------------------------------------------------------
// 职责：解析命令行参数 → 安装信号处理 → 初始化 EtherCAT 主站与共享内存 IPC
//       → 发布从站配置元数据 → 配置实时调度 → 运行周期任务 → 输出最终统计。
// 本文件是唯一的进程入口，本身不处理 PDO 内容，也不包含从站型号相关逻辑。
// =============================================================================

// 本工程自身头文件：引入各组件接口。
#include "cyclic_task.hpp"          // 绝对时间周期任务（CyclicTask）
#include "ethercat_master.hpp"      // IgH 主站生命周期封装（EthercatMaster）
#include "runtime_options.hpp"      // 命令行参数解析（RuntimeOptions）
#include "shared_memory_config.hpp" // 共享内存 IPC 与跨进程 ABI（SharedMemoryConfig）
#include "slave_config.hpp"         // 编译期从站配置（StaticSlaveConfig）

// 标准库与系统头文件。
#include <array>       // std::array（预触碰栈缓冲区）
#include <cstdlib>     // EXIT_SUCCESS / EXIT_FAILURE
#include <cerrno>      // errno（错误码）
#include <csignal>     // sig_atomic_t / sigaction / SIGINT / SIGTERM
#include <cstring>     // std::strerror
#include <iostream>    // std::cout / std::cerr
#include <sched.h>     // sched_param / SCHED_FIFO / sched_setscheduler
#include <string>      // std::string
#include <sys/mman.h>  // mlockall / MCL_CURRENT / MCL_FUTURE

namespace {

// 全局退出标志：信号处理函数中唯一允许修改的变量。
// 使用 volatile + sig_atomic_t，保证在信号处理器与主循环之间安全传递。
volatile std::sig_atomic_t g_stop_requested = 0;

// 预触碰栈缓冲区大小（64 KiB）：用于把周期线程即将用到的栈页提前加载到内存。
constexpr std::size_t kPrefaultStackBytes = 64U * 1024U;

// 实时线程优先级（SCHED_FIFO 下的静态优先级 80）。
constexpr int kRealtimePriority = 80;

// 信号处理函数：仅把停止标志置 1，不做任何非异步信号安全的操作。
void handleSignal(int) noexcept {
    g_stop_requested = 1; // 通知主循环退出
}

// 安装 SIGINT/SIGTERM 信号处理器，两者都通过 handleSignal 设置停止标志。
bool installSignalHandlers(std::string &error) {
    error.clear(); // 先清空错误信息，保证失败时返回的是本次错误

    struct sigaction action {};        // 信号动作结构体，零初始化
    action.sa_handler = handleSignal;  // 指定处理函数（不使用 sa_sigaction 形式）
    if (sigemptyset(&action.sa_mask) != 0) { // 清空信号掩码，处理期间不屏蔽其他信号
        error = "sigemptyset failed";  // 记录失败原因
        return false;                  // 掩码初始化失败，直接返回
    }

    if (sigaction(SIGINT, &action, nullptr) != 0) { // 为 Ctrl+C（SIGINT）安装处理器
        error = "sigaction SIGINT failed";
        return false;
    }
    if (sigaction(SIGTERM, &action, nullptr) != 0) { // 为 SIGTERM（服务停止）安装处理器
        error = "sigaction SIGTERM failed";
        return false;
    }

    return true; // 两个信号处理器都安装成功
}

// 进入周期循环前完成实时准备：锁定内存、预触碰栈、切换到 SCHED_FIFO 调度。
bool configureRealtime(std::string &error) {
    error.clear();

    // mlockall 锁定当前及未来所有内存页，避免周期线程运行中发生缺页导致的延迟。
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        error = "mlockall failed: " + std::string(std::strerror(errno));
        return false;
    }

    // 预触碰一段 64 KiB 的栈缓冲区：每隔一页（4096B）写一次，触发缺页并锁定。
    std::array<unsigned char, kPrefaultStackBytes> stack{};
    volatile unsigned char *const stack_data = stack.data(); // volatile 防止写被优化掉
    for (std::size_t i = 0; i < stack.size(); i += 4096U) {
        stack_data[i] = 0; // 触碰每一页，确保已映射到物理内存
    }

    // 设置 SCHED_FIFO 实时调度策略与静态优先级，使本进程以实时优先级运行。
    sched_param scheduler{};
    scheduler.sched_priority = kRealtimePriority; // 静态优先级 80
    if (sched_setscheduler(0, SCHED_FIFO, &scheduler) != 0) { // 参数 0 表示当前线程
        error = "sched_setscheduler failed: " + std::string(std::strerror(errno));
        return false;
    }

    return true;
}

// 周期循环结束后输出一行最终统计（非实时路径，允许使用 iostream）。
void printFinalStatistics(const rocos::CycleStatistics &stats, int run_result) {
    std::cout << "final_stats cycles=" << stats.cycles            // 完成的周期数
              << " missed_deadlines=" << stats.missed_deadlines    // 错过的截止期数
              << " min_us=" << stats.minimum_us                    // 最小周期耗时
              << " max_us=" << stats.maximum_us                    // 最大周期耗时
              << " avg_us=" << stats.average_us                    // 平均周期耗时
              << " current_us=" << stats.current_us                // 最近一周期耗时
              << " run_rc=" << run_result << '\n';                 // 运行返回码
}

}  // namespace（匿名命名空间，仅本文件可见）

int main(int argc, char **argv) {
    // —— 第一步：解析命令行参数 ——
    rocos::RuntimeOptions options{}; // 参数解析结果（默认 master_id=0，period_us=1000）
    std::string error;               // 复用同一错误字符串，供各阶段写入失败原因
    if (!rocos::parseRuntimeOptions(argc, argv, options, error)) { // 解析失败（非法选项/取值）
        std::cerr << error << '\n' << rocos::runtimeOptionsUsage() << '\n'; // 打印错误与用法
        return EXIT_FAILURE; // 参数错误，直接退出
    }
    if (options.show_help) { // 用户请求了 --help
        std::cout << rocos::runtimeOptionsUsage() << '\n'; // 只打印用法
        return EXIT_SUCCESS; // 正常退出
    }

    // —— 第二步：安装信号处理器 ——
    if (!installSignalHandlers(error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // —— 第三步：取得编译期从站配置，并校验非空 ——
    rocos::StaticSlaveConfig config = rocos::defaultSlaveConfig(); // 默认映射位置 0 的驱动器
    if (config.slave_count == 0U) { // 空配置意味着没有可驱动的从站
        std::cerr << "no slave configuration compiled" << '\n'; // 报错并说明原因
        return EXIT_FAILURE; // 尚未请求主站/建 IPC/提实时权限，安全退出
    }

    // —— 第四步：初始化 EtherCAT 主站（请求主站、建 domain、配从站、激活）——
    rocos::EthercatMaster master; // RAII 封装一个 IgH 主站
    if (!master.initialize(options.master_id, config, error)) { // 配置或激活失败
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // —— 第五步：创建共享内存 IPC（总线快照 + PDO 缓冲区）——
    rocos::SharedMemoryConfig ipc(static_cast<int>(options.master_id)); // 按 master_id 隔离命名
    if (!ipc.createSharedMemory()) { // 创建 ecm{id} 总线共享内存与信号量
        std::cerr << "failed to create shared memory" << '\n';
        return EXIT_FAILURE;
    }
    if (!ipc.createPdDataMemoryProvider(static_cast<int>(master.inputSize()), // 输入域大小
                                        static_cast<int>(master.outputSize()))) { // 输出域大小
        std::cerr << "failed to create process data memory" << '\n';
        return EXIT_FAILURE;
    }

    // —— 第六步：把从站/PDO 元数据发布到共享内存，供客户端读取 ——
    if (!rocos::publishConfig(*ipc.ecatBus, config, error)) { // 填充 EcatBus 元数据
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    ipc.ecatBus->dt = options.period_us;          // 发布控制周期（微秒）
    ipc.ecatBus->current_state = ECAT_STATE_INIT; // 初始总线状态
    ipc.ecatBus->is_authorized = false;           // 尚未完成健康检查
    ipc.ecatBus->timestamp = 0;                   // 时间戳清零

    // —— 第七步：配置实时环境（锁内存、预触碰、SCHED_FIFO）——
    if (!configureRealtime(error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // —— 第八步：运行周期任务直到收到停止信号 ——
    rocos::CyclicTask cyclic_task(master, ipc, options.period_us); // 绑定主站、IPC 与周期
    const int run_result = cyclic_task.run(g_stop_requested);      // 阻塞运行，直到停止标志置位
    printFinalStatistics(cyclic_task.statistics(), run_result);    // 循环结束后输出统计

    if (run_result != 0) { // 周期任务以错误码结束（如时钟调用失败）
        std::cerr << "cyclic task failed: " << std::strerror(run_result) << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS; // 正常退出；局部对象按逆序析构，释放 IgH 与 IPC 资源
}
