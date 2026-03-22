#pragma once
//
// nodes.hpp — Squad Patrol 示例：所有 BT 节点定义
//
// 包含以下节点类型（全部 header-only，C++17 inline static）：
//
//   ┌─ 同步条件节点 ──────────────────────────────────────────────────────────┐
//   │  ThreatDetectedCondition   检查实体私有标志 "threat_detected"            │
//   │  SquadAlertCondition       检查编队共享规则 "squad_alert"（示例用）      │
//   └────────────────────────────────────────────────────────────────────────┘
//   ┌─ 异步动作节点 ──────────────────────────────────────────────────────────┐
//   │  ScanZoneAction     提交 TBB 任务扫描区域；发现威胁时设置 EntityContext │
//   │                     标志和 GroupContext 规则                             │
//   │  PatrolMoveAction   提交 TBB 任务模拟移动；完成后更新 patrol_step 计数  │
//   │  ReportThreatAction 通过 CommandBus 派发 "threat_report"；异步等待确认  │
//   └────────────────────────────────────────────────────────────────────────┘
//
// 设计说明：
//   所有节点通过 BT Blackboard 的 "__async_ctx__" / "__sync_ctx__" 键
//   自动获得运行时上下文，与具体实体 ID 隔离，多实体安全复用同一节点类型。
//

#include <any>
#include <atomic>
#include <chrono>
#include <cstdlib>    // std::rand
#include <iostream>
#include <string>
#include <thread>

// ── sim_bt 公共 API ──────────────────────────────────────────────────────────
#include "sim_bt/bt_nodes/async_action_base.hpp"
#include "sim_bt/bt_nodes/condition_base.hpp"
#include "sim_bt/bt_nodes/sync_action_base.hpp"
#include "sim_bt/bt_nodes/i_async_action_context.hpp"
#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/domain/entity/i_entity_context.hpp"
#include "sim_bt/domain/group/i_group_context.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/common/types.hpp"

namespace squad_example {

// ─────────────────────────────────────────────────────────────────────────────
// Blackboard 键名（框架内部约定，与 BtRuntimeImpl 保持一致）
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kBBKeyAsyncCtx = "__async_ctx__";
static constexpr const char* kBBKeySyncCtx  = "__sync_ctx__";

// ─────────────────────────────────────────────────────────────────────────────
// EntityContext 标志键（所有节点共用，统一定义避免拼写错误）
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kFlagThreatDetected = "threat_detected"; // bool
static constexpr const char* kIntPatrolStep      = "patrol_step";     // int64

// ─────────────────────────────────────────────────────────────────────────────
// GroupContext 规则键
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kRuleSquadAlert = "squad_alert"; // bool

// ─────────────────────────────────────────────────────────────────────────────
// 全局仿真统计（由节点递增，由 main.cpp 读取打印）
// ─────────────────────────────────────────────────────────────────────────────
inline std::atomic<int> g_threats_detected{0};   // 累计发现威胁次数
inline std::atomic<int> g_reports_sent{0};        // 累计上报次数
inline std::atomic<int> g_patrols_completed{0};   // 累计完成巡逻次数

// ─────────────────────────────────────────────────────────────────────────────
// 节点可调参数（由 main.cpp 在 Initialize() 后、SpawnEntity() 前设置）
// ─────────────────────────────────────────────────────────────────────────────
inline std::atomic<int> g_scan_duration_ms{200};
inline std::atomic<int> g_patrol_step_ms{300};
inline std::atomic<int> g_report_duration_ms{100};
inline std::atomic<int> g_threat_probability_pct{25};

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数：从 BT Blackboard 获取 ISyncNodeContext 指针
//
// 用途：AsyncActionBase 子类在 OnRunning 中需要读写 EntityContext /
//       GroupContext，可通过此函数获得与同步节点相同的上下文对象。
//
// 线程安全：仅在 BT Tick Domain（主线程）中调用（OnRunning 在主线程执行）。
// ─────────────────────────────────────────────────────────────────────────────
inline sim_bt::ISyncNodeContext* GetSyncCtx(const BT::NodeConfig& cfg) {
    // BtRuntimeImpl::CreateTree() 将 sync_ctx 注入 Blackboard["__sync_ctx__"]
    auto ptr = cfg.blackboard->get<sim_bt::SyncNodeContextPtr>(kBBKeySyncCtx);
    return ptr.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// TBB 任务结果载体（通过 JobResult.payload = std::any 传递）
// ─────────────────────────────────────────────────────────────────────────────

// ScanZoneAction 的扫描结果
struct ScanResult {
    bool   threat_found    = false;   // 是否发现威胁
    float  threat_distance = 0.0f;   // 威胁距离（meter，仅 threat_found=true 时有效）
    int    scan_count      = 0;      // 仿真内部计数（可忽略）
};

// PatrolMoveAction 的移动结果
struct MoveResult {
    int   waypoint_index = 0;   // 已到达的路点序号（从 1 开始累计）
    float pos_x = 0, pos_y = 0; // 到达后的坐标（模拟，XY 平面）
};


// ═════════════════════════════════════════════════════════════════════════════
//  同步条件节点（ConditionBase 子类）
//  执行约束：Check() 必须在微秒内完成，不得阻塞
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ThreatDetectedCondition
//
// 检查本实体的 EntityContext 中是否存在未上报的威胁标志。
//
// 返回 true  → BT::SUCCESS：进入 ReportThreatAction 分支
// 返回 false → BT::FAILURE：Fallback 继续尝试下一分支（正常巡逻）
//
// 该标志由 ScanZoneAction::OnRunning 在扫描结果确认威胁时设置，
// 由 ReportThreatAction::OnRunning 在上报完成后清除。
// ─────────────────────────────────────────────────────────────────────────────
class ThreatDetectedCondition : public sim_bt::ConditionBase {
 public:
    // 构造函数签名必须是 (name, config)，BT.CPP 工厂通过此签名创建节点
    ThreatDetectedCondition(const std::string& name,
                            const BT::NodeConfig& config)
        : sim_bt::ConditionBase(name, config) {}

    // BT.CPP 工厂需要此静态方法声明节点的 Blackboard 端口（此节点无额外端口）
    static BT::PortsList providedPorts() { return {}; }

 protected:
    bool Check() override {
        // Ctx() 返回 ISyncNodeContext&（由 ConditionBase 从 Blackboard 注入）
        // Entity() 返回 IEntityContext&（本实体的私有状态，跨帧持久）
        return Ctx().Entity().GetFlag(kFlagThreatDetected, false);
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// SquadAlertCondition
//
// 检查所属编队的 GroupContext 中是否激活了 "squad_alert" 规则。
// 当编队中任一成员在 ScanZoneAction 中发现威胁时设置此规则。
//
// 用途：在更复杂的树中，可用于让未直接发现威胁的成员也进入警戒行为。
// 本示例的 squad_tree.xml 未使用此节点，但已注册，可按需加入树中。
//
// 返回 true  → 编队处于警戒状态（有成员发现威胁）
// 返回 false → 编队正常（或实体未加入任何编队）
// ─────────────────────────────────────────────────────────────────────────────
class SquadAlertCondition : public sim_bt::ConditionBase {
 public:
    SquadAlertCondition(const std::string& name,
                        const BT::NodeConfig& config)
        : sim_bt::ConditionBase(name, config) {}

    static BT::PortsList providedPorts() { return {}; }

 protected:
    bool Check() override {
        // Group() 返回 IGroupContext*，若实体未加入编队则为 nullptr
        auto* group = Ctx().Group();
        if (!group) return false;
        return group->GetRule(kRuleSquadAlert, false);
    }
};


// ═════════════════════════════════════════════════════════════════════════════
//  异步动作节点（AsyncActionBase 子类）
//  执行模式：OnStart 提交 TBB 任务 → 每帧 OnRunning 轮询结果 → OnHalted 取消
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ScanZoneAction
//
// 模拟传感器扫描巡逻区域（耗时的计算操作在 TBB worker 中异步执行）。
//
// 执行流程：
//   OnStart  → 提交 TBB 扫描任务，启动超时计时器
//   OnRunning（每帧执行）→ 检查 TBB 任务是否完成：
//     - 超时：返回 FAILURE
//     - 未完成：返回 RUNNING（等待下一帧）
//     - 完成且威胁存在：设置 EntityContext["threat_detected"] = true
//                        设置 GroupContext["squad_alert"]    = true
//                        返回 SUCCESS（Sequence 继续执行 PatrolMoveAction）
//     - 完成且安全：返回 SUCCESS
//   OnHalted → 取消 TBB 任务（树被外部中止时调用）
// ─────────────────────────────────────────────────────────────────────────────
class ScanZoneAction : public sim_bt::AsyncActionBase {
 public:
    ScanZoneAction(const std::string& name, const BT::NodeConfig& config)
        : sim_bt::AsyncActionBase(name, config) {}

    static BT::PortsList providedPorts() { return {}; }

 protected:
    // ── OnStart：提交异步扫描任务 ────────────────────────────────────────────
    sim_bt::NodeStatus OnStart() override {
        auto entity_id = OwnerEntity();
        std::cout << "  [" << entity_id << "] ScanZoneAction › 开始扫描...\n";

        // Ctx() = IAsyncActionContext，负责 TBB 任务提交和超时管理
        auto handle = Ctx().SubmitCpuJob(
            sim_bt::JobPriority::kNormal,
            // Lambda 在 TBB worker 线程中执行（可以耗时，不阻塞主线程）
            [](sim_bt::CancellationTokenPtr token, sim_bt::JobResult& out) {
                // 模拟传感器数据处理耗时
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(g_scan_duration_ms.load()));

                // 合作式取消：外部调用 CancelJob 后令牌变为 cancelled
                if (token->IsCancelled()) {
                    out.succeeded     = false;
                    out.error_message = "scan cancelled";
                    return;
                }

                // 随机威胁判定（模拟传感器输出）
                int pct = g_threat_probability_pct.load();
                ScanResult result;
                result.threat_found    = (std::rand() % 100) < pct;
                result.threat_distance = result.threat_found
                    ? static_cast<float>(50 + std::rand() % 250)
                    : 0.0f;
                result.scan_count = std::rand() % 10000;

                out.succeeded = true;
                out.payload   = result;   // std::any 存储自定义类型
            });

        // 保存 job_id，用于 OnRunning 中轮询和 OnHalted 中取消
        job_id_ = handle->JobId();

        // 设置超时保护（扫描时长的 4 倍 + 500ms 余量）
        Ctx().StartTimeout(
            std::chrono::milliseconds(g_scan_duration_ms.load() * 4 + 500));

        return sim_bt::NodeStatus::kRunning;  // 告诉 BT 下帧继续 tick
    }

    // ── OnRunning：每帧轮询 TBB 任务结果（在主线程执行，不阻塞）────────────
    sim_bt::NodeStatus OnRunning() override {
        // 1. 超时检查（优先处理，防止卡死）
        if (Ctx().IsTimedOut()) {
            std::cout << "  [" << OwnerEntity() << "] ScanZoneAction › 超时!\n";
            return sim_bt::NodeStatus::kFailure;
        }

        // 2. 非阻塞查询 TBB 任务结果（通过邮箱 Peek，不移除结果）
        auto result_opt = Ctx().PeekResult(job_id_);
        if (!result_opt.has_value()) {
            return sim_bt::NodeStatus::kRunning;  // 任务仍在处理，等待下一帧
        }

        // 3. 取出结果（从邮箱移除，防止重复消费）
        Ctx().ConsumeResult(job_id_);
        Ctx().CancelTimeout();

        if (!result_opt->succeeded) {
            return sim_bt::NodeStatus::kFailure;
        }

        // 4. 解析扫描结果载荷
        auto scan = std::any_cast<ScanResult>(result_opt->payload);
        auto entity_id = OwnerEntity();

        if (scan.threat_found) {
            std::cout << "  [" << entity_id << "] ScanZoneAction › ⚠  发现威胁! "
                      << "距离 " << static_cast<int>(scan.threat_distance) << "m\n";
            ++g_threats_detected;

            // 5. 在 OnRunning（主线程）中安全地更新状态
            //    通过 GetSyncCtx 从 Blackboard 获取 ISyncNodeContext
            auto* sync_ctx = GetSyncCtx(config());
            if (sync_ctx) {
                // 设置本实体私有标志（下一帧 ThreatDetectedCondition 会检测到）
                sync_ctx->Entity().SetFlag(kFlagThreatDetected, true);

                // 设置编队共享规则（通知其他成员，SquadAlertCondition 可感知）
                auto* group = sync_ctx->Group();
                if (group) {
                    group->SetRule(kRuleSquadAlert, true);
                    std::cout << "  [" << entity_id << "] ScanZoneAction › "
                              << "已通知编队 #" << group->Id() << " 进入警戒\n";
                }
            }
        } else {
            std::cout << "  [" << entity_id << "] ScanZoneAction › ✓ 区域安全\n";
        }

        return sim_bt::NodeStatus::kSuccess;
    }

    // ── OnHalted：外部中止时清理资源 ────────────────────────────────────────
    void OnHalted() override {
        Ctx().CancelJob(job_id_);   // 通知 TBB worker 合作退出
        Ctx().CancelTimeout();
        std::cout << "  [" << OwnerEntity() << "] ScanZoneAction › 已中止\n";
    }

 private:
    uint64_t job_id_ = 0;  // TBB 任务 ID，用于结果查询和取消
};


// ─────────────────────────────────────────────────────────────────────────────
// PatrolMoveAction
//
// 模拟巡逻移动到下一个路点（路线计算在 TBB worker 中异步执行）。
// 完成后更新 EntityContext["patrol_step"] 计数（跨帧持久）。
// ─────────────────────────────────────────────────────────────────────────────
class PatrolMoveAction : public sim_bt::AsyncActionBase {
 public:
    PatrolMoveAction(const std::string& name, const BT::NodeConfig& config)
        : sim_bt::AsyncActionBase(name, config) {}

    static BT::PortsList providedPorts() { return {}; }

 protected:
    sim_bt::NodeStatus OnStart() override {
        auto entity_id = OwnerEntity();

        // 读取当前巡逻步数（EntityContext 跨帧持久化，模拟路点推进）
        int current_step = 0;
        auto* sync_ctx = GetSyncCtx(config());
        if (sync_ctx) {
            current_step = static_cast<int>(
                sync_ctx->Entity().GetInt(kIntPatrolStep, 0));
        }

        std::cout << "  [" << entity_id << "] PatrolMoveAction › "
                  << "移动至路点 #" << (current_step + 1) << "...\n";

        auto handle = Ctx().SubmitCpuJob(
            sim_bt::JobPriority::kLow,  // 移动属于低优先级计算
            [step = current_step](sim_bt::CancellationTokenPtr token,
                                  sim_bt::JobResult& out) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(g_patrol_step_ms.load()));

                if (token->IsCancelled()) {
                    out.succeeded = false;
                    return;
                }

                // 模拟正方形巡逻路线（4×4 网格，每格 100m）
                MoveResult result;
                result.waypoint_index = step + 1;
                result.pos_x = static_cast<float>((step % 4) * 100);
                result.pos_y = static_cast<float>(((step / 4) % 4) * 100);
                out.succeeded = true;
                out.payload   = result;
            });

        job_id_ = handle->JobId();
        Ctx().StartTimeout(
            std::chrono::milliseconds(g_patrol_step_ms.load() * 4 + 500));

        return sim_bt::NodeStatus::kRunning;
    }

    sim_bt::NodeStatus OnRunning() override {
        if (Ctx().IsTimedOut()) {
            std::cout << "  [" << OwnerEntity() << "] PatrolMoveAction › 超时!\n";
            return sim_bt::NodeStatus::kFailure;
        }

        auto result_opt = Ctx().PeekResult(job_id_);
        if (!result_opt.has_value()) {
            return sim_bt::NodeStatus::kRunning;
        }

        Ctx().ConsumeResult(job_id_);
        Ctx().CancelTimeout();

        if (!result_opt->succeeded) {
            return sim_bt::NodeStatus::kFailure;
        }

        auto move = std::any_cast<MoveResult>(result_opt->payload);
        auto entity_id = OwnerEntity();

        // 持久化巡逻步数到 EntityContext（下次 OnStart 时读取）
        auto* sync_ctx = GetSyncCtx(config());
        if (sync_ctx) {
            sync_ctx->Entity().SetInt(kIntPatrolStep, move.waypoint_index);
        }

        std::cout << "  [" << entity_id << "] PatrolMoveAction › "
                  << "到达路点 #" << move.waypoint_index
                  << " 坐标(" << move.pos_x << ", " << move.pos_y << ")\n";

        ++g_patrols_completed;
        return sim_bt::NodeStatus::kSuccess;
    }

    void OnHalted() override {
        Ctx().CancelJob(job_id_);
        Ctx().CancelTimeout();
        std::cout << "  [" << OwnerEntity() << "] PatrolMoveAction › 已中止\n";
    }

 private:
    uint64_t job_id_ = 0;
};


// ─────────────────────────────────────────────────────────────────────────────
// ReportThreatAction
//
// 向指挥中心上报威胁，同时通过 CommandBus 派发 "threat_report" 命令。
//
// 执行流程：
//   OnStart:
//     1. 立即通过 CommandBus.Dispatch() 派发命令（同步，不等待）
//     2. 提交 TBB 任务模拟等待指挥部确认（通信延迟）
//   OnRunning:
//     - 收到确认后清除 EntityContext["threat_detected"] 标志
//     - 清除 GroupContext["squad_alert"]（本例中由上报者负责解除警报）
//   OnHalted:
//     - 取消确认等待任务
// ─────────────────────────────────────────────────────────────────────────────
class ReportThreatAction : public sim_bt::AsyncActionBase {
 public:
    ReportThreatAction(const std::string& name, const BT::NodeConfig& config)
        : sim_bt::AsyncActionBase(name, config) {}

    static BT::PortsList providedPorts() { return {}; }

 protected:
    sim_bt::NodeStatus OnStart() override {
        auto entity_id = OwnerEntity();
        std::cout << "  [" << entity_id << "] ReportThreatAction › 上报威胁!\n";

        // ── 立即通过 CommandBus 派发命令（同步，主线程）─────────────────────
        // CommandBus 将命令路由到已注册的处理器（由 main.cpp 中 RegisterHandler 注册）
        auto* sync_ctx = GetSyncCtx(config());
        if (sync_ctx) {
            sim_bt::ActionCommand cmd;
            cmd.source_entity = entity_id;
            cmd.command_type  = "threat_report";
            cmd.issued_at_ms  = sync_ctx->CurrentSimTime();

            // payload：将 entity_id 编码为 4 字节小端序（示意业务序列化）
            cmd.payload.resize(4);
            for (int i = 0; i < 4; ++i) {
                cmd.payload[i] =
                    static_cast<uint8_t>((entity_id >> (8 * i)) & 0xFF);
            }

            sync_ctx->CommandBus().Dispatch(cmd);
        }

        // ── 提交 TBB 任务：等待指挥部确认（高优先级，威胁响应）────────────
        auto handle = Ctx().SubmitCpuJob(
            sim_bt::JobPriority::kHigh,
            [](sim_bt::CancellationTokenPtr token, sim_bt::JobResult& out) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(g_report_duration_ms.load()));

                if (token->IsCancelled()) {
                    out.succeeded = false;
                    return;
                }
                out.succeeded = true;
            });

        job_id_ = handle->JobId();
        Ctx().StartTimeout(
            std::chrono::milliseconds(g_report_duration_ms.load() * 8 + 1000));

        return sim_bt::NodeStatus::kRunning;
    }

    sim_bt::NodeStatus OnRunning() override {
        if (Ctx().IsTimedOut()) {
            std::cout << "  [" << OwnerEntity() << "] ReportThreatAction › 超时!\n";
            return sim_bt::NodeStatus::kFailure;
        }

        auto result_opt = Ctx().PeekResult(job_id_);
        if (!result_opt.has_value()) {
            return sim_bt::NodeStatus::kRunning;
        }

        Ctx().ConsumeResult(job_id_);
        Ctx().CancelTimeout();

        if (!result_opt->succeeded) {
            return sim_bt::NodeStatus::kFailure;
        }

        auto entity_id = OwnerEntity();
        std::cout << "  [" << entity_id << "] ReportThreatAction › "
                  << "✓ 指挥部已确认，解除实体警报状态\n";

        // ── 清除本实体威胁标志（下帧回到正常巡逻） ──────────────────────────
        auto* sync_ctx = GetSyncCtx(config());
        if (sync_ctx) {
            sync_ctx->Entity().SetFlag(kFlagThreatDetected, false);

            // 清除编队级别警报（由负责上报的实体解除）
            // 实际项目中可能需要多人确认才解除，此处简化为单人解除
            auto* group = sync_ctx->Group();
            if (group) {
                group->SetRule(kRuleSquadAlert, false);
                std::cout << "  [" << entity_id << "] ReportThreatAction › "
                          << "编队 #" << group->Id() << " 警报已解除\n";
            }
        }

        ++g_reports_sent;
        return sim_bt::NodeStatus::kSuccess;
    }

    void OnHalted() override {
        Ctx().CancelJob(job_id_);
        Ctx().CancelTimeout();
        std::cout << "  [" << OwnerEntity() << "] ReportThreatAction › 已中止\n";
    }

 private:
    uint64_t job_id_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// RegisterAllNodes — 一次性注册所有节点类型到 IBtRuntime
//
// 使用方式（在 Initialize() 后、LoadTreeFromXml 前调用）：
//   squad_example::RegisterAllNodes(app.BtRuntime());
// ─────────────────────────────────────────────────────────────────────────────
inline void RegisterAllNodes(sim_bt::IBtRuntime& runtime) {
    // 同步条件节点
    runtime.RegisterNodeType<ThreatDetectedCondition>("ThreatDetectedCondition");
    runtime.RegisterNodeType<SquadAlertCondition>("SquadAlertCondition");

    // 异步动作节点
    runtime.RegisterNodeType<ScanZoneAction>("ScanZoneAction");
    runtime.RegisterNodeType<PatrolMoveAction>("PatrolMoveAction");
    runtime.RegisterNodeType<ReportThreatAction>("ReportThreatAction");
}

}  // namespace squad_example
