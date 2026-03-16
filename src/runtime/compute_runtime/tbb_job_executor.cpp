#include "tbb_job_executor.hpp"

#include <tbb/task_arena.h>

#include "tbb_job_handle.hpp"
#include "sim_bt/common/sim_bt_log.hpp"

namespace sim_bt {

TbbJobExecutor::TbbJobExecutor(ArenaConfig config)
    : config_(config),
      // 直接以并发数构造；避免 attach{} + initialize() 在已有 TBB arena 时
      // 触发 "Impossible to modify settings of an already initialized task_arena"
      arena_high_(config.high_concurrency),
      arena_normal_(config.normal_concurrency),
      arena_low_(config.low_concurrency),
      mailbox_(std::make_shared<DefaultResultMailbox>()) {}

TbbJobExecutor::~TbbJobExecutor() noexcept {
  Shutdown();
}

void TbbJobExecutor::SetWakeupCallback(VoidCallback cb) {
  mailbox_->SetNotifyCallback(std::move(cb));
}

tbb::task_arena& TbbJobExecutor::ArenaFor(JobPriority priority) {
  switch (priority) {
    case JobPriority::kHigh:   return arena_high_;
    case JobPriority::kNormal: return arena_normal_;
    case JobPriority::kLow:    return arena_low_;
    default:                   return arena_normal_;
  }
}

JobHandlePtr TbbJobExecutor::Submit(JobDescriptor descriptor) {
  if (shutdown_.load(std::memory_order_acquire)) {
    SIMBT_LOG_WARN("TbbJobExecutor::Submit rejected: executor already shut down");
    return nullptr;
  }

  uint64_t job_id = next_job_id_.fetch_add(1, std::memory_order_relaxed);
  auto token = std::make_shared<DefaultCancellationToken>();
  auto handle = std::make_shared<TbbJobHandle>(job_id, token);

  pending_count_.fetch_add(1, std::memory_order_relaxed);

  SIMBT_LOG_INFO_S("TbbJobExecutor: submit job_id=" << job_id
      << " priority=" << static_cast<int>(descriptor.priority)
      << " owner=" << descriptor.owner_entity
      << " pending=" << pending_count_.load(std::memory_order_relaxed));

  tbb::task_arena& arena = ArenaFor(descriptor.priority);
  tbb::task_group& tg    = (descriptor.priority == JobPriority::kHigh   ? tg_high_
                           : descriptor.priority == JobPriority::kNormal ? tg_normal_
                                                                          : tg_low_);

  // 捕获 shared_ptr 以延长生命周期；task 不持有 this 以防悬垂
  auto mailbox      = mailbox_;
  auto task_func    = std::move(descriptor.task);
  auto weak_handle  = std::weak_ptr<TbbJobHandle>(handle);
  EntityId owner_entity = descriptor.owner_entity;

  arena.execute([&tg, job_id, owner_entity, token, mailbox, task_func,
                 weak_handle, pending = &pending_count_]() mutable {
    tg.run([job_id, owner_entity, token, mailbox, task_func, weak_handle, pending]() {
      // handle 可能已被调用方丢弃（fire-and-forget 场景），但结果仍须无条件投递。
      // 状态更新是可选的：只在 handle 仍存活时更新。
      auto h = weak_handle.lock();

      if (token->IsCancelled()) {
        SIMBT_LOG_INFO_S("TbbJobExecutor: job_id=" << job_id
            << " owner=" << owner_entity << " cancelled before start");
        if (h) h->SetState(JobState::kCancelled);
        pending->fetch_sub(1, std::memory_order_relaxed);
        return;
      }

      SIMBT_LOG_INFO_S("TbbJobExecutor: job_id=" << job_id
          << " owner=" << owner_entity << " starting");
      if (h) h->SetState(JobState::kRunning);

      JobResult result;
      result.job_id       = job_id;
      result.owner_entity = owner_entity;  // 框架填充，worker 无需关心
      try {
        task_func(token, result);
        if (!token->IsCancelled()) {
          SIMBT_LOG_INFO_S("TbbJobExecutor: job_id=" << job_id
              << " owner=" << owner_entity << " completed succeeded=" << result.succeeded);
          if (h) h->SetState(JobState::kCompleted);
        } else {
          SIMBT_LOG_INFO_S("TbbJobExecutor: job_id=" << job_id
              << " owner=" << owner_entity << " cancelled mid-task");
          if (h) h->SetState(JobState::kCancelled);
          result.succeeded = false;
          result.error_message = "cancelled";
        }
      } catch (const std::exception& ex) {
        SIMBT_LOG_ERROR_S("TbbJobExecutor: job_id=" << job_id
            << " owner=" << owner_entity << " exception: " << ex.what());
        if (h) h->SetState(JobState::kFailed);
        result.succeeded = false;
        result.error_message = ex.what();
      } catch (...) {
        SIMBT_LOG_ERROR_S("TbbJobExecutor: job_id=" << job_id
            << " owner=" << owner_entity << " unknown exception");
        if (h) h->SetState(JobState::kFailed);
        result.succeeded = false;
        result.error_message = "unknown exception in TBB task";
      }

      pending->fetch_sub(1, std::memory_order_relaxed);
      mailbox->Post(std::move(result));
    });
  });

  return handle;
}

IResultMailbox& TbbJobExecutor::Mailbox() {
  return *mailbox_;
}

void TbbJobExecutor::Shutdown() {
  SIMBT_LOG_INFO_S("TbbJobExecutor: shutdown pending=" << pending_count_.load());
  shutdown_.store(true, std::memory_order_release);
  // 等待所有 in-flight 任务完成
  tg_high_.wait();
  tg_normal_.wait();
  tg_low_.wait();
  SIMBT_LOG_INFO("TbbJobExecutor: shutdown complete");
}

size_t TbbJobExecutor::PendingJobCount() const {
  return pending_count_.load(std::memory_order_relaxed);
}

}  // namespace sim_bt
