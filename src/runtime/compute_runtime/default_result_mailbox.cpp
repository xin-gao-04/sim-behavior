#include "default_result_mailbox.hpp"

#include <functional>

#include "sim_bt/common/sim_bt_log.hpp"

namespace sim_bt {

void DefaultResultMailbox::Post(JobResult result) {
  SIMBT_LOG_INFO_S("ResultMailbox: post job_id=" << result.job_id
      << " owner=" << result.owner_entity
      << " succeeded=" << result.succeeded);
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

// ┌── Phase 5 TODO [P1] 方案 D — DrainAll 批量化 ─────────────────────────────┐
// │  当前实现对每条 result 单独加锁写 ready_（N 条 → N 次 mu_ 加锁）。        │
// │  优化方案：                                                                │
// │    1. 锁内 swap incoming_ → local（已有，1 次锁）                         │
// │    2. 锁外遍历 local，收集到 vector<pair<uint64_t, JobResult>> batch      │
// │    3. 一次性加锁，batch insert 到 ready_（1 次锁，N 次 insert）           │
// │    4. 锁外遍历 batch 执行 consumer 回调                                   │
// │  总计：2 次加锁（swap + batch insert），替代当前 N+1 次。                  │
// │  consumer 回调中调用 RequestWakeup → 也是 N 次 BtRuntimeImpl::mu_ 加锁，  │
// │  后续可考虑 RequestWakeupBatch(vector<EntityId>) 一次性入队。              │
// └────────────────────────────────────────────────────────────────────────────────┘
void DefaultResultMailbox::DrainAll(
    const std::function<void(JobResult)>& consumer) {
  // 仅在 uvw loop 线程调用，将 incoming_ 移入 ready_ 并逐条回调
  std::queue<JobResult> local;
  {
    std::lock_guard<std::mutex> lock(mu_);
    std::swap(local, incoming_);
  }
  if (!local.empty()) {
    SIMBT_LOG_INFO_S("ResultMailbox: drain " << local.size() << " result(s)");
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

// ┌── Phase 5 TODO [P1] — Discard 精确取消 ────────────────────────────────────┐
// │  当前 Discard 只从 ready_ 移除，incoming_ 中残留的同 job_id 结果            │
// │  会在下次 DrainAll 时被移入 ready_，导致"幽灵结果"。                       │
// │  修复方案：                                                                │
// │    新增 discarded_ids_: std::unordered_set<uint64_t>                       │
// │    Discard() 时同时 discarded_ids_.insert(job_id)                         │
// │    DrainAll() 在移入 ready_ 前检查 discarded_ids_，跳过已丢弃的。         │
// │    Consume() 时也从 discarded_ids_ 中移除对应 id。                        │
// └────────────────────────────────────────────────────────────────────────────────┘
void DefaultResultMailbox::Discard(uint64_t job_id) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_.erase(job_id);
  // incoming_ 中可能还有此 job 的结果，标记为无效（简单丢弃策略）
  // 如需精确取消 incoming_ 中的条目，可改用 deque + 标志位
}

}  // namespace sim_bt
