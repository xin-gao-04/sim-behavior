#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "sim_bt/common/result.hpp"
#include "sim_bt/common/types.hpp"
#include "sim_bt/runtime/compute_runtime/i_cancellation_token.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_handle.hpp"
#include "sim_bt/runtime/compute_runtime/i_result_mailbox.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// JobDescriptor
//
// 提交给 IJobExecutor 的任务描述。
// ─────────────────────────────────────────────────────────────────────────────
struct JobDescriptor {
  // 任务执行体：在 TBB worker 线程上调用。
  // token 由 executor 注入，worker 应在热路径中调用 token->IsCancelled()。
  // 返回值由 worker 写入 mailbox（通过 result_out）。
  std::function<void(CancellationTokenPtr token, JobResult& result_out)> task;

  // 调度优先级，决定进入哪个 arena。
  JobPriority priority = JobPriority::kNormal;

  // 提交此 job 的实体 ID，框架自动写入 JobResult::owner_entity，
  // 供 DrainAll consumer 调用 BtRuntime::RequestWakeup 时使用。
  EntityId owner_entity = kInvalidEntityId;

  // 提交者标识（用于日志和调试）。
  std::string submitter_name;
};

// ─────────────────────────────────────────────────────────────────────────────
// IJobExecutor
//
// 对 oneTBB task_arena 的封装层。
//
// 职责：
//   - 维护三个 arena（high / normal / low），对应 JobPriority。
//   - 分配 job_id，创建 ICancellationToken，返回 IJobHandle。
//   - 任务完成后自动将结果写入绑定的 IResultMailbox。
//   - 支持关闭（Shutdown），等待所有 in-flight 任务结束。
//
// 使用约束：
//   - Submit() 是线程安全的，可从 BT tick 线程调用。
//   - 不允许在 TBB worker 内部递归调用 Submit()（会死锁 arena）。
// ─────────────────────────────────────────────────────────────────────────────
class IJobExecutor {
 public:
  virtual ~IJobExecutor() = default;

  // 提交任务，返回任务句柄。失败（executor 已关闭）时返回 nullptr。
  // 任务结果异步写入 Mailbox()；调用者通过 handle->JobId() 检索。
  virtual JobHandlePtr Submit(JobDescriptor descriptor) = 0;

  // 获取绑定的结果邮箱，供 BT 节点查询结果。
  virtual IResultMailbox& Mailbox() = 0;

  // 获取邮箱的 shared_ptr，供 sim_host 接入 DrainAll 通知链。
  virtual std::shared_ptr<IResultMailbox> Mailbox_shared() = 0;

  // 关闭 executor：停止接受新任务，等待所有 in-flight 任务结束。
  // 幂等，线程安全。
  virtual void Shutdown() = 0;

  // 当前各 arena 中排队 + 执行中的任务总数（用于监控）。
  virtual size_t PendingJobCount() const = 0;
};

using JobExecutorPtr = std::shared_ptr<IJobExecutor>;

}  // namespace sim_bt
