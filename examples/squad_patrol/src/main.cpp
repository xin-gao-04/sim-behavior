// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Squad Patrol 仿真示例
//
// 演示如何用 sim-behavior 构建一个多实体行为树仿真：
//
//   1. 从 JSON 配置文件加载仿真参数（entities、timing、tick policy）
//   2. 从 XML 文件加载行为树定义（支持在不重编译的情况下修改树结构）
//   3. 注册自定义 BT 节点（条件节点 + 异步动作节点）
//   4. 创建实体（每个实体独立的 EntityContext + AsyncActionContext）
//   5. 创建编队（GroupContext 在成员间共享战术状态）
//   6. 注册 CommandBus 处理器，监听实体产出的命令
//   7. 运行固定帧率主循环（20Hz），每帧打印性能统计
//   8. Ctrl+C 优雅停止，打印最终汇总
//
// 运行方式：
//   ./squad_patrol_example                         # 使用默认配置
//   ./squad_patrol_example config/simulation.json  # 指定配置文件
//
// 配置文件位于 config/simulation.json（CMake 会自动复制到构建目录）
// 行为树文件位于 trees/squad_tree.xml
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ── sim_bt 宿主层（内部头，通过 src/ include 路径访问）──────────────────────
#include "sim_host/sim_host_app.hpp"

// ── 本示例的头文件 ──────────────────────────────────────────────────────────
#include "config_loader.hpp"   // LoadConfig()
#include "nodes.hpp"           // 节点定义 + RegisterAllNodes()

// ── 命名空间别名 ─────────────────────────────────────────────────────────────
using namespace sim_bt;
using namespace squad_example;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// 全局停止信号（Ctrl+C 或帧数耗尽后设置）
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop_requested{false};

static void HandleSignal(int /*signum*/) {
    g_stop_requested.store(true);
}

static void ConfigureConsoleForUtf8() {
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// PrintBanner — 启动时显示仿真概述
// ─────────────────────────────────────────────────────────────────────────────
static void PrintBanner(const SimulationConfig& cfg) {
    std::cout << "\n"
              << "╔══════════════════════════════════════════════════════════╗\n"
              << "║          sim-behavior  Squad Patrol 示例               ║\n"
              << "╚══════════════════════════════════════════════════════════╝\n"
              << "\n"
              << "  任务名称 : " << cfg.name << "\n"
              << "  总帧数   : " << cfg.duration_frames << " 帧 (@ 20Hz ≈ "
                                 << cfg.duration_frames * 50 / 1000 << "s)\n"
              << "  Tick策略 : " << cfg.tick_policy << "\n"
              << "  分队     : " << cfg.squad.name
                                 << " (" << cfg.squad.members.size() << " 实体)\n"
              << "  树文件   : " << cfg.tree_file << "\n"
              << "  扫描时长 : " << cfg.scan_duration_ms << "ms\n"
              << "  移动时长 : " << cfg.patrol_step_ms << "ms\n"
              << "  上报时延 : " << cfg.report_duration_ms << "ms\n"
              << "  威胁概率 : " << cfg.threat_probability_pct << "%\n"
              << "\n"
              << "  成员列表：\n";
    for (const auto& m : cfg.squad.members) {
        std::cout << "    EntityId=" << m.id
                  << "  " << m.name
                  << "  (zone=" << m.zone << ")\n";
    }
    std::cout << "\n  按 Ctrl+C 可提前停止\n"
              << "──────────────────────────────────────────────────────────\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// PrintFrameStats — 每 N 帧打印一次帧统计
// ─────────────────────────────────────────────────────────────────────────────
static void PrintFrameStats(int frame, const IBtRuntime::TickStats& stats) {
    std::cout << "\n[帧 " << std::setw(4) << frame << "]"
              << "  sim=" << stats.sim_time_ms << "ms"
              << "  tick=" << stats.duration_us << "μs"
              << "  树总数=" << stats.tree_count
              << "  已跳过=" << stats.skipped_count
              << "  唤醒=" << stats.wakeup_count
              << "  威胁累计=" << g_threats_detected.load()
              << "  上报=" << g_reports_sent.load()
              << "  巡逻=" << g_patrols_completed.load()
              << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// PrintFinalSummary — 仿真结束时打印汇总
// ─────────────────────────────────────────────────────────────────────────────
static void PrintFinalSummary(int frames_run,
                               const SimulationConfig& cfg,
                               const SimHostApp::MemPoolStats& mem) {
    std::cout << "\n"
              << "══════════════════════════════════════════════════════════\n"
              << "  仿真结束\n"
              << "══════════════════════════════════════════════════════════\n"
              << "  运行帧数       : " << frames_run << "\n"
              << "  仿真时间       : " << frames_run * 50 << " ms\n"
              << "  实体数         : " << cfg.squad.members.size() << "\n"
              << "  ─────────────────────────────────────────────────────\n"
              << "  累计发现威胁   : " << g_threats_detected.load() << " 次\n"
              << "  累计上报指挥部 : " << g_reports_sent.load()    << " 次\n"
              << "  累计完成巡逻   : " << g_patrols_completed.load() << " 次\n"
              << "  ─────────────────────────────────────────────────────\n"
              << "  内存池用量     : " << mem.bytes_in_use << " bytes in use\n"
              << "  内存池峰值     : " << mem.bytes_peak   << " bytes peak\n"
              << "  Slab 命中率    : " << mem.slab_hits
                                       << "/" << (mem.slab_hits + mem.slab_misses)
              << "\n"
              << "══════════════════════════════════════════════════════════\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    ConfigureConsoleForUtf8();

    // ── 0. 信号处理：Ctrl+C 优雅退出 ─────────────────────────────────────────
    std::signal(SIGINT,  HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    // ── 1. 加载配置文件 ────────────────────────────────────────────────────────
    std::string config_path = "config/simulation.json";  // 默认路径
    if (argc >= 2) {
        config_path = argv[1];  // 支持命令行指定配置文件
    }

    SimulationConfig cfg;
    try {
        cfg = LoadConfig(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 加载配置失败: " << e.what() << "\n";
        return 1;
    }

    PrintBanner(cfg);

    // ── 2. 初始化仿真宿主 ──────────────────────────────────────────────────────
    //
    // SimHostApp 负责装配所有运行时：
    //   - TbbJobExecutor       (3 优先级 arena：high/normal/low)
    //   - UvwEventLoopRuntime  (libuv event loop，后台线程运行)
    //   - BtRuntimeImpl        (多实体 BT 树 + Phase 5 优化)
    //   - InProcessCommandBus  (进程内同步命令总线)
    // ──────────────────────────────────────────────────────────────────────────
    SimHostApp app;
    {
        auto status = app.Initialize();
        if (!status) {
            std::cerr << "[ERROR] SimHostApp 初始化失败: " << status.message << "\n";
            return 1;
        }
    }
    std::cout << "[INFO] SimHostApp 初始化完成\n";

    // ── 3. 设置 Tick 策略 ──────────────────────────────────────────────────────
    //
    // kSkipIdle: 只 tick 有 RUNNING 节点的树（Active Set 优化），
    //            对空闲树的唤醒通过 Phase 1 wakeup_set 驱动。
    //            适合大规模场景（200+ 实体），空闲实体不消耗 CPU。
    //
    // kTickAll:  每帧 tick 所有树，适合调试或小规模场景（< 50 实体）。
    // ──────────────────────────────────────────────────────────────────────────
    if (cfg.tick_policy == "skip_idle") {
        app.SetTickPolicy(IBtRuntime::TickPolicy::kSkipIdle);
        std::cout << "[INFO] Tick 策略: kSkipIdle (Active Set)\n";
    } else {
        app.SetTickPolicy(IBtRuntime::TickPolicy::kTickAll);
        std::cout << "[INFO] Tick 策略: kTickAll\n";
    }

    // ── 4. 同步节点参数（来自配置文件）──────────────────────────────────────
    //
    // 节点使用 inline static atomic 变量，在此处赋值；
    // 所有后续 SpawnEntity 的实体共享这些参数。
    // ──────────────────────────────────────────────────────────────────────────
    g_scan_duration_ms.store(cfg.scan_duration_ms);
    g_patrol_step_ms.store(cfg.patrol_step_ms);
    g_report_duration_ms.store(cfg.report_duration_ms);
    g_threat_probability_pct.store(cfg.threat_probability_pct);

    // ── 5. 注册自定义 BT 节点 ─────────────────────────────────────────────────
    //
    // 每个节点类型注册一次，所有实体共享同一工厂函数。
    // 节点实例由 BT.CPP 工厂为每个树实例独立创建（实例间无共享状态）。
    // ──────────────────────────────────────────────────────────────────────────
    RegisterAllNodes(app.BtRuntime());
    std::cout << "[INFO] BT 节点注册完成 (5 种类型)\n";

    // ── 6. 加载行为树 XML 定义 ─────────────────────────────────────────────────
    //
    // LoadTreeFromFile 解析 XML 并在 BT.CPP Factory 中注册树模板。
    // 后续 SpawnEntity 通过树 ID "SoldierPatrolTree" 实例化树。
    // ──────────────────────────────────────────────────────────────────────────
    {
        auto status = app.BtRuntime().LoadTreeFromFile(cfg.tree_file);
        if (!status) {
            std::cerr << "[ERROR] 加载树文件失败: " << cfg.tree_file
                      << "\n  " << status.message << "\n";
            return 1;
        }
    }
    std::cout << "[INFO] 树文件加载完成: " << cfg.tree_file << "\n";

    // ── 7. 注册 CommandBus 处理器 ──────────────────────────────────────────────
    //
    // BT 节点（ReportThreatAction）通过 CommandBus.Dispatch() 发出命令。
    // 此处注册的处理器模拟"指挥中心"接收并记录威胁报告。
    //
    // 在真实系统中，可替换为：UDP 发送、消息队列投递、数据库写入等。
    // ──────────────────────────────────────────────────────────────────────────
    app.CommandBus().RegisterHandler(
        "threat_report",
        [](const ActionCommand& cmd) {
            // 解码 entity_id（与 ReportThreatAction::OnStart 中的编码对应）
            uint32_t eid = 0;
            if (cmd.payload.size() >= 4) {
                for (int i = 0; i < 4; ++i) {
                    eid |= static_cast<uint32_t>(cmd.payload[i]) << (8 * i);
                }
            }
            std::cout << "  [CommandBus] 收到 threat_report"
                      << "  来源实体=" << eid
                      << "  仿真时间=" << cmd.issued_at_ms << "ms\n";
        });
    std::cout << "[INFO] CommandBus 处理器注册完成\n";

    // ── 8. 创建实体（SpawnEntity）─────────────────────────────────────────────
    //
    // 每次 SpawnEntity 会为实体创建：
    //   - EntityContextImpl   (私有状态：flag/int/float/target)
    //   - AsyncActionContextImpl (连接 TBB + uvw + BT runtime)
    //   - SyncNodeContextImpl  (连接 EntityContext + CommandBus + GroupContext)
    // 并将它们注入到 BT 树的 Blackboard 中。
    // ──────────────────────────────────────────────────────────────────────────
    std::vector<EntityId> entity_ids;
    for (const auto& member : cfg.squad.members) {
        auto status = app.SpawnEntity(member.id, "SoldierPatrolTree");
        if (!status) {
            std::cerr << "[ERROR] SpawnEntity 失败 id=" << member.id
                      << ": " << status.message << "\n";
            return 1;
        }
        entity_ids.push_back(member.id);
        std::cout << "[INFO] 已生成实体 " << member.id
                  << " (" << member.name << ")\n";
    }

    // ── 9. 创建编队（AssignGroup）────────────────────────────────────────────
    //
    // AssignGroup 创建 GroupContextImpl 并将其注入到所有成员的 SyncNodeContext 中。
    // 之后 ConditionBase / SyncActionBase 节点可通过 Ctx().Group() 访问共享状态。
    //
    // GroupContext 提供：
    //   - SetRule / GetRule       (共享规则标志，如 "squad_alert")
    //   - SetObjectivePosition    (编队目标区坐标)
    //   - SetRallyPoint           (集结点)
    //   - Members / IsMember      (成员查询)
    // ──────────────────────────────────────────────────────────────────────────
    {
        auto status = app.AssignGroup(cfg.squad.id, entity_ids);
        if (!status) {
            std::cerr << "[ERROR] AssignGroup 失败: " << status.message << "\n";
            return 1;
        }
    }
    std::cout << "[INFO] 编队 #" << cfg.squad.id
              << " (" << cfg.squad.name << ") 创建完成，成员数="
              << entity_ids.size() << "\n";

    // ── 10. 主循环（固定 20Hz，手动驱动 TickAll）──────────────────────────────
    //
    // 注意：这里不调用 app.Run()（它会阻塞），而是手动驱动 TickAll，
    // 以便在每帧后读取并打印性能统计。
    //
    // 完整的 wakeup 链路（TBB→uvw→RequestWakeup）由后台线程独立运行，
    // 与主循环的 TickAll 调用解耦：
    //
    //   TBB worker 完成
    //     → mailbox.Post()           (worker 线程)
    //     → notify_cb → PostToLoop() (uvw loop 后台线程)
    //     → DrainAll → RequestWakeup (uvw loop 后台线程)
    //     → wakeup_set_ 入队         (原子操作，线程安全)
    //     → 下一帧 TickAll Phase 1 优先 re-tick 该实体
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n[INFO] 开始仿真主循环 ("
              << cfg.duration_frames << " 帧)...\n"
              << "──────────────────────────────────────────────────────────\n";

    constexpr auto kFrameInterval = std::chrono::milliseconds(50);  // 20Hz
    SimTimeMs sim_time = 0;

    // 每隔多少帧打印一次统计（避免刷屏）
    constexpr int kStatsPrintInterval = 5;

    int frames_run = 0;
    for (int frame = 0; frame < cfg.duration_frames && !g_stop_requested.load();
         ++frame, ++frames_run) {

        auto frame_start = std::chrono::steady_clock::now();

        sim_time += 50;  // 每帧推进 50ms（对应 20Hz）

        // ── 驱动一帧 BT tick ──────────────────────────────────────────────
        //
        // TickAll 内部：
        //   Phase 1: 处理 wakeup_set_（已被 TBB 任务完成唤醒的实体）
        //            在锁外 tick（Lock-Free Tick，Phase 5 优化 A）
        //   Phase 2: 根据策略 tick 其余实体
        //            kSkipIdle: 只遍历 active_set_（Phase 5 优化 B）
        //            kTickAll:  遍历 trees_（所有树）
        // ─────────────────────────────────────────────────────────────────
        app.BtRuntime().TickAll(sim_time);

        // ── 每 N 帧打印统计 ───────────────────────────────────────────────
        if (frame % kStatsPrintInterval == 0) {
            PrintFrameStats(frame, app.LastTickStats());
        }

        // ── 帧率控制：睡眠到下一帧时间点 ────────────────────────────────
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        auto sleep_time = kFrameInterval - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_time);
        }
    }

    std::cout << "──────────────────────────────────────────────────────────\n"
              << "[INFO] 主循环结束，共运行 " << frames_run << " 帧\n";

    // ── 11. 优雅停止 ───────────────────────────────────────────────────────────
    //
    // RequestStop() 执行顺序：
    //   1. stop_requested_ = true
    //   2. event_loop_->Stop()   (停止 uvw loop，等待后台线程退出)
    //   3. job_executor_->Shutdown() (停止 TBB arenas，等待所有 worker 退出)
    //   4. bt_runtime_->Shutdown()   (清空树、active_set_、wakeup_set_)
    // ──────────────────────────────────────────────────────────────────────────
    app.RequestStop();
    std::cout << "[INFO] 仿真已停止\n";

    // ── 12. 打印最终汇总 ───────────────────────────────────────────────────────
    PrintFinalSummary(frames_run, cfg, app.MemoryPoolStats());

    return 0;
}
