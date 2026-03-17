#pragma once

#include <memory>
#include <unordered_map>

#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"
#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_executor.hpp"
#include "sim_bt/bt_nodes/i_async_action_context.hpp"
#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/domain/entity/i_entity_context.hpp"
#include "sim_bt/domain/world/i_world_snapshot.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/common/result.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// SimHostApp
//
// 仿真宿主应用（Simulation Host 第一层）。
//
// 职责：
//   - 装配所有运行时：BtRuntime + EventLoopRuntime + JobExecutor
//   - 管理多实体生命周期（SpawnEntity / DespawnEntity）
//   - 每帧刷新 WorldSnapshot，驱动 TickAll
//   - 优雅关闭
//
// 多实体工作流：
//   1. Initialize()           — 启动所有运行时
//   2. BtRuntime().LoadTreeFromXml(xml)  — 注册树定义
//   3. SpawnEntity(id, "TreeName")        — 为每个实体创建完整上下文套件+树
//   4. Run()                  — 进入主循环（阻塞）
// ─────────────────────────────────────────────────────────────────────────────
class SimHostApp {
 public:
  SimHostApp();
  ~SimHostApp();

  // 初始化所有运行时（不启动主循环）
  SimStatus Initialize();

  // 启动仿真主循环，阻塞直到 RequestStop()
  void Run();

  // 从任意线程请求停止主循环（线程安全）
  void RequestStop();

  // ── 多实体管理 ────────────────────────────────────────────────────────────

  // 为实体创建完整上下文套件（EntityCtx + SyncCtx + AsyncCtx）并注册行为树。
  // 须在 Initialize() 之后、Run() 之前调用（或在主循环线程中调用）。
  SimStatus SpawnEntity(EntityId entity_id, const std::string& tree_name);

  // 销毁实体的行为树和上下文（Halt 后清理）。
  void DespawnEntity(EntityId entity_id);

  // ── 获取各运行时（供外部注册节点等使用） ─────────────────────────────────

  IBtRuntime&              BtRuntime()        { return *bt_runtime_; }
  IEventLoopRuntime&       EventLoopRuntime() { return *event_loop_; }
  IJobExecutor&            JobExecutor()      { return *job_executor_; }
  ICommandBus&             CommandBus()       { return *command_bus_; }
  IWorldSnapshotProvider*  SnapshotProvider() { return snapshot_provider_.get(); }

 private:
  void TickLoop();

  std::shared_ptr<IBtRuntime>             bt_runtime_;
  std::shared_ptr<IEventLoopRuntime>      event_loop_;
  std::shared_ptr<IJobExecutor>           job_executor_;
  std::shared_ptr<ICommandBus>            command_bus_;
  std::shared_ptr<IWorldSnapshotProvider> snapshot_provider_;

  // 每个实体的上下文套件（随 SpawnEntity 创建，DespawnEntity 销毁）
  struct EntityBundle {
    std::shared_ptr<IEntityContext>       entity_ctx;
    std::shared_ptr<IAsyncActionContext>  async_ctx;
    std::shared_ptr<ISyncNodeContext>     sync_ctx;
  };
  std::unordered_map<EntityId, EntityBundle> entity_bundles_;

  SimTimeMs current_sim_time_ = 0;
  bool      stop_requested_   = false;
};

}  // namespace sim_bt
