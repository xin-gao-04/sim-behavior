#pragma once

#include <memory>

#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"
#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_executor.hpp"
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
//   - 加载配置（树定义 XML、arena 参数、adapter 地址等）
//   - 驱动仿真主循环（TickLoop）
//   - 优雅关闭
//
// 第一期实现目标（最小闭环）：
//   - 单实体、单棵树、一个 uvw loop、一个 TBB arena、一个异步动作节点
//   - 验证：能提交 CPU 任务 + 能超时 + 能 halt
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

  // 获取各运行时（供外部注册节点等使用）
  IBtRuntime&          BtRuntime()          { return *bt_runtime_; }
  IEventLoopRuntime&   EventLoopRuntime()   { return *event_loop_; }
  IJobExecutor&        JobExecutor()        { return *job_executor_; }
  ICommandBus&         CommandBus()         { return *command_bus_; }

 private:
  void TickLoop();

  std::shared_ptr<IBtRuntime>        bt_runtime_;
  std::shared_ptr<IEventLoopRuntime> event_loop_;
  std::shared_ptr<IJobExecutor>      job_executor_;
  std::shared_ptr<ICommandBus>       command_bus_;
  std::shared_ptr<IWorldSnapshotProvider> snapshot_provider_;

  SimTimeMs current_sim_time_ = 0;
  bool      stop_requested_   = false;
};

}  // namespace sim_bt
