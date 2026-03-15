#pragma once

#include <any>
#include <chrono>
#include <functional>
#include <memory>

#include "sim_bt/common/types.hpp"
#include "sim_bt/runtime/compute_runtime/i_cancellation_token.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_handle.hpp"
#include "sim_bt/runtime/compute_runtime/i_result_mailbox.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// IAsyncActionContext
//
// 异步动作节点的运行时上下文 Facade。
//
// 设计原则：
//   BT 节点只依赖此接口，不直接持有 uvw/oneTBB 原始对象。
//   这确保节点可在测试中注入 mock 实现，也使节点代码不受库升级影响。
//
// 典型使用流程（AsyncActionBase 子类中）：
//
//   NodeStatus OnStart() override {
//     auto handle = ctx_->SubmitCpuJob(
//         JobPriority::kNormal,
//         [](CancellationTokenPtr token, JobResult& out) {
//             // TBB worker 线程中执行
//             if (token->IsCancelled()) return;
//             out.succeeded = true;
//             out.payload = MyResult{...};
//         });
//     job_id_ = handle->JobId();
//     ctx_->StartTimeout(std::chrono::milliseconds(100));
//     return NodeStatus::kRunning;
//   }
//
//   NodeStatus OnRunning() override {
//     auto result = ctx_->PeekResult(job_id_);
//     if (!result) return NodeStatus::kRunning;
//     ctx_->ConsumeResult(job_id_);
//     return result->succeeded ? NodeStatus::kSuccess : NodeStatus::kFailure;
//   }
//
//   void OnHalted() override {
//     ctx_->CancelJob(job_id_);
//     ctx_->CancelTimeout();
//   }
// ─────────────────────────────────────────────────────────────────────────────
class IAsyncActionContext {
 public:
  virtual ~IAsyncActionContext() = default;

  // ── CPU 任务提交 ─────────────────────────────────────────────────────────────

  // 提交 CPU 密集型任务到 oneTBB arena。
  // task 在 worker 线程执行，结果自动写入 Mailbox；
  // 由 uvw wakeup → BT re-tick 后在 onRunning 中通过 PeekResult 消费。
  virtual JobHandlePtr SubmitCpuJob(
      JobPriority priority,
      std::function<void(CancellationTokenPtr, JobResult&)> task) = 0;

  // 取消指定任务（通知令牌，worker 合作退出）。
  virtual void CancelJob(uint64_t job_id) = 0;

  // ── 结果查询 ─────────────────────────────────────────────────────────────────

  // 查询指定任务的结果（非消费）。
  virtual std::optional<JobResult> PeekResult(uint64_t job_id) const = 0;

  // 消费并移除指定任务的结果（在 onRunning 成功读取后调用）。
  virtual void ConsumeResult(uint64_t job_id) = 0;

  // ── 超时控制 ─────────────────────────────────────────────────────────────────

  // 启动超时定时器（一次性）。超时后自动将节点状态标记为超时，
  // 并通过 WakeupBridge 触发 re-tick。
  // 若在超时前调用 CancelTimeout()，定时器不触发。
  virtual void StartTimeout(std::chrono::milliseconds timeout) = 0;

  // 取消正在计时的超时定时器，幂等。
  virtual void CancelTimeout() = 0;

  // 当前超时是否已触发（OnRunning 中用于区分超时与正常完成）。
  virtual bool IsTimedOut() const = 0;

  // ── 唤醒 ────────────────────────────────────────────────────────────────────

  // 从任意线程请求 BT runtime re-tick 当前实体（合并式信号）。
  virtual void EmitWakeup() = 0;

  // ── 上下文查询 ───────────────────────────────────────────────────────────────

  virtual EntityId OwnerEntity() const = 0;
  virtual SimTimeMs CurrentSimTime() const = 0;
};

using AsyncActionContextPtr = std::shared_ptr<IAsyncActionContext>;

}  // namespace sim_bt
