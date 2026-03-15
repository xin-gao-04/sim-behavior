#pragma once

#include <atomic>
#include <memory>

#include "sim_bt/runtime/compute_runtime/i_job_handle.hpp"

namespace sim_bt {

class TbbJobHandle : public IJobHandle {
 public:
  TbbJobHandle(uint64_t job_id, CancellationTokenPtr token)
      : job_id_(job_id),
        token_(std::move(token)),
        state_(JobState::kPending) {}

  JobState State() const override {
    return state_.load(std::memory_order_acquire);
  }

  void SetState(JobState s) {
    state_.store(s, std::memory_order_release);
  }

  void Cancel() override {
    token_->Cancel();
    // 若任务尚未开始则直接标记为取消；
    // 若已在运行，worker 会在 IsCancelled() 检查后提前结束。
    JobState expected = JobState::kPending;
    state_.compare_exchange_strong(expected, JobState::kCancelled,
                                   std::memory_order_acq_rel);
  }

  uint64_t JobId() const override { return job_id_; }

  CancellationTokenPtr Token() const override { return token_; }

 private:
  uint64_t                    job_id_;
  CancellationTokenPtr        token_;
  std::atomic<JobState>       state_;
};

}  // namespace sim_bt
