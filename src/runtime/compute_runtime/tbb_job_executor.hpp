#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include "sim_bt/runtime/compute_runtime/i_job_executor.hpp"
#include "default_result_mailbox.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// TbbJobExecutor
//
// 基于 oneTBB task_arena 的 IJobExecutor 实现。
//
// 维护三个 arena 对应三种 JobPriority：
//   kHigh   → arena_high_   (最大并发较小，高优先级)
//   kNormal → arena_normal_ (路径规划等中优先级)
//   kLow    → arena_low_    (后台预取等低优先级)
//
// 任务完成后结果写入 DefaultResultMailbox，通过 notify_cb 触发 uvw wakeup。
//
// Arena 并发配置（可通过构造参数调整）：
//   high_concurrency   默认 2  （战术决策不需要高并发）
//   normal_concurrency 默认 4
//   low_concurrency    默认 2
// ─────────────────────────────────────────────────────────────────────────────
struct ArenaConfig {
  int high_concurrency   = 2;
  int normal_concurrency = 4;
  int low_concurrency    = 2;
};

class TbbJobExecutor : public IJobExecutor {
 public:
  explicit TbbJobExecutor(ArenaConfig config = {});
  ~TbbJobExecutor() noexcept override;

  // 注入 wakeup 回调（应在 Mailbox 使用前设置，通常由 EventLoopRuntime 注入）
  void SetWakeupCallback(VoidCallback cb);

  // ── IJobExecutor ────────────────────────────────────────────────────────────

  JobHandlePtr Submit(JobDescriptor descriptor) override;
  IResultMailbox& Mailbox() override;
  std::shared_ptr<IResultMailbox> Mailbox_shared() override { return mailbox_; }
  void Shutdown() override;
  size_t PendingJobCount() const override;

 private:
  tbb::task_arena& ArenaFor(JobPriority priority);

  ArenaConfig config_;

  tbb::task_arena arena_high_;
  tbb::task_arena arena_normal_;
  tbb::task_arena arena_low_;

  // 每个 arena 对应一个 task_group 便于 Shutdown 时等待
  tbb::task_group tg_high_;
  tbb::task_group tg_normal_;
  tbb::task_group tg_low_;

  std::shared_ptr<DefaultResultMailbox> mailbox_;

  std::atomic<uint64_t> next_job_id_{1};
  std::atomic<size_t>   pending_count_{0};
  std::atomic<bool>     shutdown_{false};
};

}  // namespace sim_bt
