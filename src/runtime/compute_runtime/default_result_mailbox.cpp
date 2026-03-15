#include "default_result_mailbox.hpp"

#include <functional>

namespace sim_bt {

void DefaultResultMailbox::Post(JobResult result) {
  VoidCallback cb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    incoming_.push(std::move(result));
    cb = notify_cb_;  // 取出回调引用，在锁外调用
  }
  if (cb) {
    cb();  // 触发 uvw async_handle.send()，不在锁内执行
  }
}

void DefaultResultMailbox::DrainAll(
    const std::function<void(JobResult)>& consumer) {
  // 仅在 uvw loop 线程调用，将 incoming_ 移入 ready_ 并逐条回调
  std::queue<JobResult> local;
  {
    std::lock_guard<std::mutex> lock(mu_);
    std::swap(local, incoming_);
  }
  while (!local.empty()) {
    JobResult r = std::move(local.front());
    local.pop();
    uint64_t id = r.job_id;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ready_[id] = r;  // 存入 ready_ 供后续 Peek
    }
    if (consumer) {
      consumer(r);
    }
  }
}

std::optional<JobResult> DefaultResultMailbox::Peek(uint64_t job_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = ready_.find(job_id);
  if (it == ready_.end()) return std::nullopt;
  return it->second;
}

void DefaultResultMailbox::Consume(uint64_t job_id) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_.erase(job_id);
}

void DefaultResultMailbox::Discard(uint64_t job_id) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_.erase(job_id);
  // incoming_ 中可能还有此 job 的结果，标记为无效（简单丢弃策略）
  // 如需精确取消 incoming_ 中的条目，可改用 deque + 标志位
}

}  // namespace sim_bt
