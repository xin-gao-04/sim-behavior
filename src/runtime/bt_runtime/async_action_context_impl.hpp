#pragma once

#include <atomic>
#include <memory>
#include <optional>

#include "sim_bt/bt_nodes/i_async_action_context.hpp"
#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_executor.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// AsyncActionContextImpl
//
// IAsyncActionContext 的标准实现，聚合 IJobExecutor + IEventLoopRuntime
// + IBtRuntime（用于 RequestWakeup）。
//
// 每个 BT 异步动作节点持有一个 AsyncActionContextImpl 实例，
// 但它们共享同一 JobExecutor、EventLoopRuntime 和 BtRuntime。
// ─────────────────────────────────────────────────────────────────────────────
class AsyncActionContextImpl : public IAsyncActionContext {
 public:
  AsyncActionContextImpl(EntityId owner,
                         std::shared_ptr<IJobExecutor> executor,
                         std::shared_ptr<IEventLoopRuntime> event_loop,
                         std::shared_ptr<IBtRuntime> bt_runtime,
                         SimTimeMs* sim_time_ptr);

  // ── IAsyncActionContext ────────────────────────────────────────────────────

  JobHandlePtr SubmitCpuJob(
      JobPriority priority,
      std::function<void(CancellationTokenPtr, JobResult&)> task) override;

  void CancelJob(uint64_t job_id) override;

  std::optional<JobResult> PeekResult(uint64_t job_id) const override;
  void ConsumeResult(uint64_t job_id) override;

  void StartTimeout(std::chrono::milliseconds timeout) override;
  void CancelTimeout() override;
  bool IsTimedOut() const override;

  void EmitWakeup() override;

  EntityId OwnerEntity() const override;
  SimTimeMs CurrentSimTime() const override;

 private:
  EntityId                              owner_;
  std::shared_ptr<IJobExecutor>         executor_;
  std::shared_ptr<IEventLoopRuntime>    event_loop_;
  std::shared_ptr<IBtRuntime>           bt_runtime_;
  SimTimeMs*                            sim_time_ptr_;

  // 当前活跃的超时计时器句柄
  TimerHandlePtr                        timeout_handle_;
  std::atomic<bool>                     timed_out_{false};

  // 当前活跃的任务句柄（弱引用，避免阻止析构）
  JobHandlePtr                          active_handle_;
};

}  // namespace sim_bt
