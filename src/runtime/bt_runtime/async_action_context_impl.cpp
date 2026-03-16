#include "async_action_context_impl.hpp"

#include "sim_bt/common/sim_bt_log.hpp"

namespace sim_bt {

AsyncActionContextImpl::AsyncActionContextImpl(
    EntityId owner,
    std::shared_ptr<IJobExecutor> executor,
    std::shared_ptr<IEventLoopRuntime> event_loop,
    std::shared_ptr<IBtRuntime> bt_runtime,
    SimTimeMs* sim_time_ptr)
    : owner_(owner),
      executor_(std::move(executor)),
      event_loop_(std::move(event_loop)),
      bt_runtime_(std::move(bt_runtime)),
      sim_time_ptr_(sim_time_ptr) {}

JobHandlePtr AsyncActionContextImpl::SubmitCpuJob(
    JobPriority priority,
    std::function<void(CancellationTokenPtr, JobResult&)> task) {
  timed_out_.store(false, std::memory_order_relaxed);

  JobDescriptor desc;
  desc.priority     = priority;
  desc.owner_entity = owner_;  // 框架自动携带，DrainAll 后可直接 RequestWakeup
  desc.task         = std::move(task);

  SIMBT_LOG_INFO_S("AsyncActionCtx: entity=" << owner_
      << " submitting cpu job priority=" << static_cast<int>(priority));

  active_handle_ = executor_->Submit(std::move(desc));

  // 任务完成后，ResultMailbox 的 notify_cb 会触发 uvw wakeup，
  // uvw loop 回调中再通知 BtRuntime re-tick 此实体。
  return active_handle_;
}

void AsyncActionContextImpl::CancelJob(uint64_t job_id) {
  if (active_handle_ && active_handle_->JobId() == job_id) {
    active_handle_->Cancel();
  }
}

std::optional<JobResult> AsyncActionContextImpl::PeekResult(uint64_t job_id) const {
  return executor_->Mailbox().Peek(job_id);
}

void AsyncActionContextImpl::ConsumeResult(uint64_t job_id) {
  executor_->Mailbox().Consume(job_id);
}

void AsyncActionContextImpl::StartTimeout(std::chrono::milliseconds timeout) {
  SIMBT_LOG_INFO_S("AsyncActionCtx: entity=" << owner_
      << " timeout started " << timeout.count() << "ms");
  timed_out_.store(false, std::memory_order_relaxed);
  EntityId owner = owner_;
  auto bt = bt_runtime_;
  auto& timed_out_ref = timed_out_;
  timeout_handle_ = event_loop_->StartOneShotTimer(timeout, [owner, bt, &timed_out_ref]() {
    SIMBT_LOG_WARN_S("AsyncActionCtx: entity=" << owner << " timeout fired");
    timed_out_ref.store(true, std::memory_order_release);
    if (bt) bt->RequestWakeup(owner);
  });
}

void AsyncActionContextImpl::CancelTimeout() {
  if (timeout_handle_) {
    // Timer handle 的 Cancel() 须在 loop 线程上调用
    // 通过 PostToLoop 保证线程安全
    auto handle = timeout_handle_;
    event_loop_->PostToLoop([handle]() {
      handle->Cancel();
    });
    timeout_handle_.reset();
  }
}

bool AsyncActionContextImpl::IsTimedOut() const {
  return timed_out_.load(std::memory_order_acquire);
}

void AsyncActionContextImpl::EmitWakeup() {
  if (bt_runtime_) {
    bt_runtime_->RequestWakeup(owner_);
  }
}

EntityId AsyncActionContextImpl::OwnerEntity() const {
  return owner_;
}

SimTimeMs AsyncActionContextImpl::CurrentSimTime() const {
  return sim_time_ptr_ ? *sim_time_ptr_ : 0;
}

}  // namespace sim_bt
