#include "tbb_job_executor.hpp"

#include <tbb/task_arena.h>

#include "tbb_job_handle.hpp"

namespace sim_bt {

TbbJobExecutor::TbbJobExecutor(ArenaConfig config)
    : config_(config),
      arena_high_(tbb::task_arena::attach{}),
      arena_normal_(tbb::task_arena::attach{}),
      arena_low_(tbb::task_arena::attach{}),
      mailbox_(std::make_shared<DefaultResultMailbox>()) {
  // 用指定并发数重新初始化各 arena
  arena_high_.initialize(config.high_concurrency);
  arena_normal_.initialize(config.normal_concurrency);
  arena_low_.initialize(config.low_concurrency);
}

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
    return nullptr;
  }

  uint64_t job_id = next_job_id_.fetch_add(1, std::memory_order_relaxed);
  auto token = std::make_shared<DefaultCancellationToken>();
  auto handle = std::make_shared<TbbJobHandle>(job_id, token);

  pending_count_.fetch_add(1, std::memory_order_relaxed);

  tbb::task_arena& arena = ArenaFor(descriptor.priority);
  tbb::task_group& tg    = (descriptor.priority == JobPriority::kHigh   ? tg_high_
                           : descriptor.priority == JobPriority::kNormal ? tg_normal_
                                                                          : tg_low_);

  // 捕获 shared_ptr 以延长生命周期；task 不持有 this 以防悬垂
  auto mailbox   = mailbox_;
  auto task_func = std::move(descriptor.task);
  auto weak_handle = std::weak_ptr<TbbJobHandle>(handle);

  arena.execute([&tg, job_id, token, mailbox, task_func,
                 weak_handle, pending = &pending_count_]() mutable {
    tg.run([job_id, token, mailbox, task_func, weak_handle, pending]() {
      auto h = weak_handle.lock();
      if (!h) return;

      if (token->IsCancelled()) {
        h->SetState(JobState::kCancelled);
        pending->fetch_sub(1, std::memory_order_relaxed);
        return;
      }

      h->SetState(JobState::kRunning);

      JobResult result;
      result.job_id = job_id;
      try {
        task_func(token, result);
        if (!token->IsCancelled()) {
          h->SetState(JobState::kCompleted);
        } else {
          h->SetState(JobState::kCancelled);
          result.succeeded = false;
          result.error_message = "cancelled";
        }
      } catch (const std::exception& ex) {
        h->SetState(JobState::kFailed);
        result.succeeded = false;
        result.error_message = ex.what();
      } catch (...) {
        h->SetState(JobState::kFailed);
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
  shutdown_.store(true, std::memory_order_release);
  // 等待所有 in-flight 任务完成
  tg_high_.wait();
  tg_normal_.wait();
  tg_low_.wait();
}

size_t TbbJobExecutor::PendingJobCount() const {
  return pending_count_.load(std::memory_order_relaxed);
}

}  // namespace sim_bt
